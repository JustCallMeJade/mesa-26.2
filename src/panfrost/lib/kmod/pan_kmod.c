/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>

#include "util/cache_ops.h"
#include "util/u_memory.h"
#include "util/macros.h"
#include "pan_kmod.h"
#include "pan_kmod_backend.h"
#include "kbase_kmod.h"

extern const struct pan_kmod_ops panfrost_kmod_ops;
extern const struct pan_kmod_ops panthor_kmod_ops;
extern const struct pan_kmod_ops kbase_kmod_ops;

static const struct {
   const char *name;
   const struct pan_kmod_ops *ops;
} drivers[] = {
   {
      "panfrost",
      &panfrost_kmod_ops,
   },
   {
      "panthor",
      &panthor_kmod_ops,
   },
   {
      "mali_kbase",
      &kbase_kmod_ops,
   },
};

static void *
default_zalloc(const struct pan_kmod_allocator *allocator, size_t size,
               UNUSED bool transient)
{
   return os_calloc(1, size);
}

static void
default_free(const struct pan_kmod_allocator *allocator, void *data)
{
   os_free(data);
}

static const struct pan_kmod_allocator default_allocator = {
   .zalloc = default_zalloc,
   .free = default_free,
};

struct pan_kmod_dev *
pan_kmod_dev_create(int fd, uint32_t flags,
                    const struct pan_kmod_allocator *allocator)
{
   mesa_logi("in pan_kmod_dev_create: %d", fd);
   if (!allocator)
         allocator = &default_allocator;

   drmVersionPtr version = drmGetVersion(fd);

   if (!version) {
      mesa_logi("in pan_kmod_dev_create version: %p", kbase_kmod_ops.dev_create);
      struct pan_kmod_driver fake_ver = {
      };

      return kbase_kmod_ops.dev_create(fd, flags, &fake_ver, allocator);
   }

   struct pan_kmod_dev *dev = NULL;

   if (!version)
      return NULL;

   if (!allocator)
      allocator = &default_allocator;

   const char *drv_name = version->name;
   const struct pan_kmod_driver drv_info = {
      .version = {
         .major = version->version_major,
         .minor = version->version_minor,
      },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(drivers); i++) {
      if (!strcmp(drivers[i].name, drv_name)) {
         const struct pan_kmod_ops *ops = drivers[i].ops;

         dev = ops->dev_create(fd, flags, &drv_info, allocator);
         break;
      }
   }

   drmFreeVersion(version);
   return dev;
}

void
pan_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   dev->ops->dev_destroy(dev);
}

struct pan_kmod_bo *
pan_kmod_bo_alloc(struct pan_kmod_dev *dev, struct pan_kmod_vm *exclusive_vm,
                  uint64_t size, uint32_t flags)
{
   struct pan_kmod_bo *bo;

   bo = dev->ops->bo_alloc(dev, exclusive_vm, size, flags);
   if (!bo)
      return NULL;

   /* We intentionally don't take the lock when filling the sparse array,
    * because we just created the BO, and haven't exported it yet, so
    * there's no risk of imports racing with our BO insertion.
    */
   struct pan_kmod_bo **slot =
      util_sparse_array_get(&dev->handle_to_bo.array, bo->handle);

   if (!slot) {
      mesa_loge("failed to allocate slot in the handle_to_bo array");
      bo->dev->ops->bo_free(bo);
      return NULL;
   }

   // TODO(leegao): look into this
   // assert(*slot == NULL);
   *slot = bo;
   return bo;
}

int
pan_kmod_bo_export(struct pan_kmod_bo *bo)
{
   PAN_TRACE_FUNC(PAN_TRACE_LIB_KMOD);

   int fd;

   if (bo->dev->ops == &kbase_kmod_ops) {
      fd = bo->dev->ops->bo_export(bo, -1);
      if (fd < 0) return -1;
      bo->flags |= PAN_KMOD_BO_FLAG_EXPORTED;
      return fd;
   }

   if (drmPrimeHandleToFD(bo->dev->fd, bo->handle, DRM_CLOEXEC | DRM_RDWR,
                          &fd)) {
      mesa_loge("drmPrimeHandleToFD() failed (err=%d)", errno);
      return -1;
   }

   if (bo->dev->ops->bo_export && bo->dev->ops->bo_export(bo, fd)) {
      close(fd);
      return -1;
   }

   bo->flags |= PAN_KMOD_BO_FLAG_EXPORTED;
   return fd;
}

void
pan_kmod_bo_put(struct pan_kmod_bo *bo)
{
   if (!bo)
      return;

   int32_t refcnt = p_atomic_dec_return(&bo->refcnt);

   assert(refcnt >= 0);

   if (refcnt)
      return;

   struct pan_kmod_dev *dev = bo->dev;

   simple_mtx_lock(&dev->handle_to_bo.lock);

   mesa_logi("%s @ %d", __FUNCTION__, __LINE__);
   /* If some import took a ref on this BO while we were trying to acquire the
    * lock, skip the destruction.
    */
   if (!p_atomic_read(&bo->refcnt)) {
      struct pan_kmod_bo **slot = (struct pan_kmod_bo **)util_sparse_array_get(
         &dev->handle_to_bo.array, bo->handle);

      assert(slot);
      *slot = NULL;
      bo->dev->ops->bo_free(bo);
   }

   simple_mtx_unlock(&dev->handle_to_bo.lock);
}

static inline size_t
get_dmabuf_size(int fd)
{
   struct stat st;
   if (fstat(fd, &st) == 0 && st.st_size > 0)
      return (size_t)st.st_size;

   mesa_logi("fstat failed: errno=%d", errno);

   off_t sz = lseek(fd, 0, SEEK_END);
   if (sz != (off_t)-1 && sz > 0) {
      lseek(fd, 0, SEEK_SET);
      return (size_t)sz;
   }

   return 0;
}

struct pan_kmod_bo *
pan_kmod_bo_import(struct pan_kmod_dev *dev, int fd)
{
   struct pan_kmod_bo *bo = NULL;
   struct pan_kmod_bo **slot;

   simple_mtx_lock(&dev->handle_to_bo.lock);

   uint32_t handle;
   if (dev->ops == &kbase_kmod_ops) {
      handle = (uint32_t)fd; // handle is the fd itself for kbase
   } else {
      int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
      if (ret)
         goto err_unlock;
   }

   slot = util_sparse_array_get(&dev->handle_to_bo.array, handle);
   if (!slot)
      goto err_close_handle;

   if (*slot) {
      bo = *slot;

      p_atomic_inc(&bo->refcnt);
   } else {
      size_t size = get_dmabuf_size(fd);
      if ((size == 0 || size == (size_t)-1) && dev->ops != &kbase_kmod_ops) { // kbase does not need the size?
         mesa_loge("invalid dmabuf size: %lu, errno=%d", size, errno);
         goto err_close_handle;
      }

      bo = dev->ops->bo_import(dev, handle, size);
      if (!bo)
         goto err_close_handle;

      *slot = bo;
   }

   assert(p_atomic_read(&bo->refcnt) > 0);

   simple_mtx_unlock(&dev->handle_to_bo.lock);

   return bo;

err_close_handle:
   if (dev->ops != &kbase_kmod_ops)
      drmCloseBufferHandle(dev->fd, handle);

err_unlock:
   simple_mtx_unlock(&dev->handle_to_bo.lock);

   return NULL;
}

void
pan_kmod_flush_bo_map_syncs_locked(struct pan_kmod_dev *dev)
{
   ASSERTED int ret = dev->ops->flush_bo_map_syncs(dev);
   assert(!ret);

   util_dynarray_foreach(&dev->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync)
      sync->bo->has_pending_deferred_syncs = false;

   util_dynarray_clear(&dev->pending_bo_syncs.array);
}

void
pan_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   if (dev->props.is_io_coherent)
      return;

   /* Barrier to make sure all flush/invalidate requests are effective. */
   if (p_atomic_xchg(&dev->pending_bo_syncs.user_cache_ops_pending, false))
      util_post_flush_inval_fence();

   /* This can be racy, but that's fine, because we expect a future call to
    * pan_kmod_flush_bo_map_syncs() if new ops are being added while we check
    * this value.
    */
   if (!util_dynarray_num_elements(&dev->pending_bo_syncs.array,
                                   struct pan_kmod_deferred_bo_sync))
      return;

   simple_mtx_lock(&dev->pending_bo_syncs.lock);
   pan_kmod_flush_bo_map_syncs_locked(dev);
   simple_mtx_unlock(&dev->pending_bo_syncs.lock);
}

/* Arbitrary limit for now. Pick something bigger or make it configurable if it
 * becomes problematic.
 */
#define MAX_PENDING_SYNC_OPS 4096

void
pan_kmod_queue_bo_map_sync(struct pan_kmod_bo *bo, uint64_t bo_offset,
                           void *cpu_ptr, uint64_t range,
                           enum pan_kmod_bo_sync_type type)
{
   struct pan_kmod_dev *dev = bo->dev;

   /* Nothing to do if the buffer is IO coherent or if the BO is not mapped
    * cacheable.
    */
   if (!(bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP) ||
       (bo->flags & PAN_KMOD_BO_FLAG_IO_COHERENT))
      return;

   /* If we have userspace cache flushing ops, use them instead of trapping
    * through to the kernel.
    */
   if (pan_kmod_can_sync_bo_map_from_userland(dev)) {
      /* Pre-flush needs to be executed before each flush/inval operation, but
       * we can batch the post flush/inval fence. util_pre_flush_fence() being
       * a NOP on aarch64, it's effectively free there, but we keep it here for
       * clarity (not sure we care about Mali on x86 to be honest :D).
       */
      util_pre_flush_fence();

      if (type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH)
         util_flush_range_no_fence(cpu_ptr, range);
      else
         util_flush_inval_range_no_fence(cpu_ptr, range);

      /* The util_pre_flush_inval_fence() is inserted by
       * pan_kmod_flush_bo_map_syncs() to avoid unnecessary serialization when
       * flush/invalidate operations are batched.
       */
      p_atomic_set(&dev->pending_bo_syncs.user_cache_ops_pending, true);
      return;
   }

   simple_mtx_lock(&dev->pending_bo_syncs.lock);

   /* If we reach the limit, flush the pending ops before queuing new ones. */
   if (util_dynarray_num_elements(&dev->pending_bo_syncs.array,
                                  struct pan_kmod_deferred_bo_sync) >=
       MAX_PENDING_SYNC_OPS)
      pan_kmod_flush_bo_map_syncs_locked(dev);

   uint64_t start = bo_offset & ~((uint64_t)util_cache_granularity() - 1);
   uint64_t end = ALIGN_POT(bo_offset + range, util_cache_granularity());

   struct pan_kmod_deferred_bo_sync new_sync = {
      .bo = bo,
      .start = start,
      .size = end - start,
      .type = type,
   };

   bo->has_pending_deferred_syncs = true;
   util_dynarray_append(&dev->pending_bo_syncs.array, new_sync);

   simple_mtx_unlock(&dev->pending_bo_syncs.lock);
}

void *
pan_kmod_bo_mmap(struct pan_kmod_bo *bo, int prot, int flags, void *host_addr)
{
   PAN_TRACE_FUNC(PAN_TRACE_LIB_KMOD);

   off_t mmap_offset;

   /* Don't bother trying an mmap() if it's not allowed. */
   if (bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP)
      return MAP_FAILED;

   if (bo->dev->ops == &kbase_kmod_ops) {
      // kbase gpu va is always mapped to cpu va
      struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
      if (kbo->cpu_ptr)
         return kbo->cpu_ptr;
   }

   mmap_offset = bo->dev->ops->bo_get_mmap_offset(bo);
   if (mmap_offset < 0)
      return MAP_FAILED;

   host_addr =
      os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);
   if (host_addr == MAP_FAILED)
      mesa_loge("mmap(..., size=%" PRIu64 ", prot=%d, flags=0x%x) failed: %s",
                bo->size, prot, flags, strerror(errno));

   return host_addr;
}
