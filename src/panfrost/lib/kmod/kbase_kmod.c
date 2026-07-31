/*
 * Copyright © 2024 Mesa kbase backend contributors
 * SPDX-License-Identifier: MIT
 *
 * kbase_kmod.c - pan_kmod backend for the Arm mali_kbase kernel driver.
 * Matches DDK r38+ (Mali-G615/G715 Valhall / CSF generation)
 *
 * Based on https://github.com/Vtgamer998/MESA-KMOD/blob/main/kbase_kmod.c
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"
#include "util/macros.h"
#include "util/os_mman.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#include "pan_kmod_backend.h"
#include "kbase_kmod.h"
#include "mali_base_kernel.h"
#include "kbase_uapi.h"
#include "kbase_csf_uapi.h"

/* ============================================================
 * mali_kbase ioctl definitions (Type 0x80)
 * ============================================================ */

struct kbase_ioctl_mem_share {
   uint64_t gpu_va;
   int32_t  out_fd;
   uint32_t _pad;
};
#define KBASE_IOCTL_MEM_SHARE \
   _IOWR(KBASE_IOCTL_TYPE, 0x33, struct kbase_ioctl_mem_share)

#define KBASE_SYNC_TO_DEVICE 0
#define KBASE_SYNC_TO_CPU    1

/* ============================================================
 * Internal Structures
 * ============================================================ */

const struct pan_kmod_ops kbase_kmod_ops;

struct kbase_kmod_dev {
   struct pan_kmod_dev base;
   struct {
      uint32_t product_id;
      uint32_t major_rev;
      uint32_t minor_rev;
      uint64_t cycle_freq;
   } gpu_info;
};

struct kbase_kmod_vm {
   struct pan_kmod_vm base;
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;
   uint64_t gpu_va;
   void    *cpu_ptr;   /* MAP_FAILED when unmapped */
   bool     exported;
   int      dmabuf_fd; /* -1 unless exported/imported */
};

/* ============================================================
 * Helper Functions
 * ============================================================ */

int
kbase_ioctl(int fd, unsigned long req, void *arg)
{
   return ioctl(fd, req, arg);
}

static bool
kbase_parse_gpuprops(const uint8_t *buf, size_t len, uint32_t key, uint64_t *out)
{
   size_t off = 0;
   while (off + 4 <= len) {
      uint32_t kr;
      memcpy(&kr, buf + off, 4);
      off += 4;
      uint32_t sc = kr & 3, pid = kr >> 2;
      size_t vs;
      switch (sc) {
      case 0: vs = 1; break;
      case 1: vs = 2; break;
      case 2: vs = 4; break;
      default: vs = 8; break;
      }
      if (off + vs > len) return false;
      uint64_t v = 0;
      memcpy(&v, buf + off, vs);
      off += vs;
      if (pid == key) {
         *out = v;
         return true;
      }
   }
   return false;
}

static uint64_t
kbase_query_gpuprop(int fd, uint32_t prop_id)
{
   struct kbase_ioctl_get_gpuprops req = {0};
   int ret = kbase_ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (ret < 0) return 0;

   uint8_t *buf = calloc(1, (size_t)ret);
   if (!buf) return 0;

   req.buffer = (uintptr_t)buf;
   req.size = (uint32_t)ret;
   uint64_t val = 0;
   if (kbase_ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req) >= 0)
      kbase_parse_gpuprops(buf, (size_t)ret, prop_id, &val);

   free(buf);
   return val;
}

/* ============================================================
 * Device Operations
 * ============================================================ */

static void
kbase_dev_query_props(struct kbase_kmod_dev *kd)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct pan_kmod_dev_props *p = &kd->base.props;
   int fd = kd->base.fd;
   memset(p, 0, sizeof(*p));

   p->pgsize_bitmap = PAN_PGSIZE_4K | PAN_PGSIZE_2M;

   uint32_t pid = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_PRODUCT_ID);
   uint32_t maj = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MAJOR_REVISION);
   uint32_t min = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MINOR_REVISION);
   uint32_t var = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_VERSION_STATUS);

   p->gpu_id = (pid << 16) | ((var & 0xf) << 12) | ((maj & 0xf) << 8) | (min & 0xff);
   p->gpu_variant = var;

   mesa_logi("gpu_id=%lx gpu_variant=%u", p->gpu_id, p->gpu_variant);

   p->shader_present = kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_SHADER_PRESENT);

   p->tiler_features = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_TILER_FEATURES);
   p->mem_features   = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_MEM_FEATURES);
   p->mmu_features   = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_MMU_FEATURES);

   p->texture_features[0] = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0);
   p->texture_features[1] = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1);
   p->texture_features[2] = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2);
   p->texture_features[3] = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3);

   p->max_threads_per_core =
      (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS);
   if (!p->max_threads_per_core) p->max_threads_per_core = 256;

   p->max_threads_per_wg =
      (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE);
   if (!p->max_threads_per_wg) p->max_threads_per_wg = p->max_threads_per_core;

   uint32_t tf = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_RAW_THREAD_FEATURES);
   p->max_tasks_per_core     = MAX2(tf >> 24, 1);
   p->num_registers_per_core = tf & 0xffff;
   if (!p->num_registers_per_core)
      p->num_registers_per_core = p->max_threads_per_core * 32;

   p->max_tls_instance_per_core = p->max_threads_per_core;

   p->supported_bo_flags =
      PAN_KMOD_BO_FLAG_EXECUTABLE | PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
      PAN_KMOD_BO_FLAG_NO_MMAP    | PAN_KMOD_BO_FLAG_GPU_UNCACHED;
   p->is_io_coherent = false;
   p->allowed_group_priorities_mask =
      PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW    |
      PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM |
      PAN_KMOD_GROUP_ALLOW_PRIORITY_HIGH;

   kd->gpu_info.product_id = pid;
   kd->gpu_info.major_rev  = maj;
   kd->gpu_info.minor_rev  = min;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags, const struct pan_kmod_driver * _,
                      const struct pan_kmod_allocator *allocator)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_ioctl_version_check vc = { .major = 11, .minor = 38 };
   if (kbase_ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) < 0) {
      mesa_loge("kbase: VERSION_CHECK failed (err=%d)", errno);
   }

   struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
   if (kbase_ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) < 0) {
      mesa_logw("kbase: SET_FLAGS failed (err=%d)", errno);
   }

   struct kbase_kmod_dev *kd = pan_kmod_alloc(allocator, sizeof(*kd));
   if (!kd) {
      mesa_loge("kbase: Out of memory");
      return NULL;
   }

   struct pan_kmod_driver fv = {
      .version.major = vc.major,
      .version.minor = vc.minor
   };
   pan_kmod_dev_init(&kd->base, fd, flags, &fv, &kbase_kmod_ops, allocator);
   kbase_dev_query_props(kd);

   return &kd->base;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_dev *kd = container_of(dev, struct kbase_kmod_dev, base);
   pan_kmod_dev_cleanup(dev);
   pan_kmod_free(dev->allocator, kd);
}

static struct pan_kmod_va_range
kbase_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev)
{
   (void)dev;
   /* Reserved 32MB low memory; 48-bit address space */
   return (struct pan_kmod_va_range){
      .start = 32ull << 20,
      .size  = (1ull << 48) - (32ull << 20),
   };
}

/* ============================================================
 * Buffer Object Operations
 * ============================================================ */

static uint64_t
pan_flags_to_kbase(uint32_t f)
{
   uint64_t k = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                BASE_MEM_SAME_VA;
   if (f & PAN_KMOD_BO_FLAG_EXECUTABLE)     k |= BASE_MEM_PROT_GPU_EX;
   if (f & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) k |= BASE_MEM_GROW_ON_GPF;
   if (f & PAN_KMOD_BO_FLAG_GPU_UNCACHED)  k |= BASE_MEM_UNCACHED_GPU;
   return k;
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev, struct pan_kmod_vm *vm,
                    uint64_t size, uint32_t flags)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_bo *kbo = pan_kmod_dev_alloc(dev, sizeof(*kbo));
   if (!kbo) return NULL;

   uint64_t pages = DIV_ROUND_UP(size, 4096);
   uint64_t kflags = pan_flags_to_kbase(flags);
   uint64_t cookie = 0;
   int ret = -1;

   union kbase_ioctl_mem_alloc_ex a_ex = {
      .in = {
         .va_pages      = pages,
         .commit_pages  = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? 0 : pages,
         .extension     = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? pages : 0,
         .flags         = kflags,
         .fixed_address = 0,
         .extra         = {0, 0, 0},
      },
   };

   ret = kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC_EX, &a_ex);
   if (ret == 0) {
      cookie = a_ex.out.gpu_va; /* Output cookie/address is at offset 0x08 in .out */
   } else {
      union kbase_ioctl_mem_alloc a = {
         .in = {
            .va_pages     = pages,
            .commit_pages = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? 0 : pages,
            .extension    = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? pages : 0,
            .flags        = kflags,
         }
      };

      ret = kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &a);
      if (ret == 0) {
         cookie = a.out.gpu_va;
      }
   }

   if (ret < 0 || !cookie) {
      mesa_loge("kbase: MEM_ALLOC size=%"PRIu64" failed (err=%d)", size, errno);
      pan_kmod_dev_free(dev, kbo);
      return NULL;
   }

   kbo->cpu_ptr = MAP_FAILED;

   /* mmap the returned cookie to acquire identical CPU/GPU virtual address */
   if (flags & PAN_KMOD_BO_FLAG_NO_MMAP) {
      kbo->gpu_va = cookie;
   } else {
      void *p = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                     dev->fd, (off_t)cookie);

      if (p != MAP_FAILED) {
         kbo->gpu_va  = (uintptr_t)p;
         kbo->cpu_ptr = p;
      } else {
         mesa_loge("kbase: mmap failed for cookie 0x%"PRIx64" (err=%d)", cookie, errno);
         struct kbase_ioctl_mem_free mf = { .gpu_addr = cookie };
         kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_FREE, &mf);
         pan_kmod_dev_free(dev, kbo);
         return NULL;
      }
   }

   kbo->exported  = false;
   kbo->dmabuf_fd = -1;

   pan_kmod_bo_init(&kbo->base, dev, vm, pages * 4096, flags,
                    (uint32_t)(kbo->gpu_va & 0xFFFFFFFF));
   return &kbo->base;
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *bo)
{
   mesa_logi("%s @ %d: handle=%p", __func__, __LINE__, bo);
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);

   pan_kmod_bo_cleanup(bo);

   mesa_logi("%s @ %d: gpuva=%lx", __func__, __LINE__, kbo->gpu_va);
   if (kbo->dmabuf_fd >= 0) {
      close(kbo->dmabuf_fd);
      kbo->dmabuf_fd = -1;
   }

   if (kbo->cpu_ptr != NULL && kbo->cpu_ptr != MAP_FAILED) {
      munmap(kbo->cpu_ptr, bo->size);
      kbo->cpu_ptr = MAP_FAILED;
   }

   if (kbo->gpu_va != 0 && !(bo->flags & PAN_KMOD_BO_FLAG_IMPORTED)) {
      struct kbase_ioctl_mem_free mf = { .gpu_addr = kbo->gpu_va };
      if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FREE, &mf) < 0) {
         mesa_logw("kbase: MEM_FREE 0x%"PRIx64" failed (err=%d, %s)",
                     kbo->gpu_va, errno, strerror(errno));
      }
   }

   pan_kmod_dev_free(bo->dev, kbo);
}

static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size)
{
   int dfd = (int)handle;
   mesa_logi("kbase_kmod_bo_import: handle=%d", dfd);

   struct kbase_kmod_bo *kbo = pan_kmod_dev_alloc(dev, sizeof(*kbo));
   if (!kbo) return NULL;

   static const uint64_t import_flags_try[] = {
      BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_COHERENT_LOCAL,

      // Uncached fallback
      BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_COHERENT_LOCAL | BASE_MEM_UNCACHED_GPU,

      // Basic fallback
      BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR,

      // Device-local fallback
      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_COHERENT_LOCAL,
   };

   int ret = -1;
   union kbase_ioctl_mem_import mi;

   for (unsigned i = 0; i < 4; i++) {
      memset(&mi, 0, sizeof(mi));
      mi.in.flags   = import_flags_try[i];
      mi.in.phandle = (uint64_t)(uintptr_t)&dfd;
      mi.in.type    = BASE_MEM_IMPORT_TYPE_UMM; /* 2 */

      ret = kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_IMPORT, &mi);
      if (ret == 0)
         break;
   }

   if (ret < 0) {
      mesa_loge("kbase: MEM_IMPORT failed for fd %d (err=%d)", dfd, errno);
      pan_kmod_dev_free(dev, kbo);
      return NULL;
   }

   uint64_t gpu_va = mi.out.gpu_va;
   kbo->gpu_va    = gpu_va;
   kbo->cpu_ptr   = MAP_FAILED;
   kbo->exported  = false;
   kbo->dmabuf_fd = dup(dfd);

   uint64_t imported_size = mi.out.va_pages * 4096;
   if (!imported_size && size > 0)
      imported_size = size;

   mesa_logi("%s: imported_size=%lu", __func__, imported_size);

   pan_kmod_bo_init(&kbo->base, dev, NULL, imported_size,
                    PAN_KMOD_BO_FLAG_IMPORTED,
                    handle); // handles are fds instead of VAs
   return &kbo->base;
}

static int
kbase_kmod_bo_export(struct pan_kmod_bo *bo, int unused_fd)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   (void)unused_fd;

   if (kbo->exported && kbo->dmabuf_fd >= 0) return kbo->dmabuf_fd;
   if ((bo->flags & PAN_KMOD_BO_FLAG_IMPORTED) && kbo->dmabuf_fd >= 0) {
      kbo->exported = true;
      return kbo->dmabuf_fd;
   }

   /* Mark memory shareable */
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = BASE_MEM_IMPORT_SHARED,
      .mask   = BASE_MEM_IMPORT_SHARED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0)
      mesa_logw("kbase: MEM_FLAGS_CHANGE for export failed (err=%d)", errno);

   /* Acquire dma-buf handle via MEM_SHARE */
   struct kbase_ioctl_mem_share ms = { .gpu_va = kbo->gpu_va, .out_fd = -1 };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_SHARE, &ms) == 0 && ms.out_fd >= 0) {
      kbo->dmabuf_fd = ms.out_fd;
      kbo->exported  = true;
      bo->flags |= PAN_KMOD_BO_FLAG_EXPORTED;
      return kbo->dmabuf_fd;
   }

   mesa_loge("kbase: MEM_SHARE ioctl failed (err=%d)", errno);
   return -1;
}

static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   return (off_t)kbo->gpu_va;
}

static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   util_dynarray_foreach(&dev->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync) {
      struct kbase_kmod_bo *kbo = container_of(sync->bo, struct kbase_kmod_bo, base);
      if (kbo->cpu_ptr == MAP_FAILED) continue;

      struct kbase_ioctl_mem_sync ks = {
         .handle    = kbo->gpu_va,
         .user_addr = (uint64_t)(uintptr_t)kbo->cpu_ptr + sync->start,
         .size      = sync->size,
         .type      = (sync->type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH)
                        ? KBASE_SYNC_TO_DEVICE : KBASE_SYNC_TO_CPU,
      };
      if (kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_SYNC, &ks) < 0)
         mesa_logw("kbase: MEM_SYNC failed (err=%d)", errno);
   }
   return 0;
}

static bool
kbase_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                   bool for_read_only_access)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   (void)bo; (void)timeout_ns; (void)for_read_only_access;
   return true;
}

static void
kbase_kmod_bo_make_evictable(struct pan_kmod_bo *bo)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = BASE_MEM_DONT_NEED,
      .mask   = BASE_MEM_DONT_NEED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0)
      mesa_logw("kbase: make_evictable failed (err=%d)", errno);
}

static bool
kbase_kmod_bo_make_unevictable(struct pan_kmod_bo *bo)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = 0,
      .mask   = BASE_MEM_DONT_NEED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0) {
      mesa_logw("kbase: make_unevictable failed (err=%d)", errno);
      return false;
   }
   return true;
}

/* ============================================================
 * Virtual Memory (VM) Operations
 * ============================================================ */

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   (void)va_start; (void)va_range;
   struct kbase_kmod_vm *vm = pan_kmod_dev_alloc(dev, sizeof(*vm));
   if (!vm) return NULL;

   pan_kmod_vm_init(&vm->base, dev, 0, flags | PAN_KMOD_VM_FLAG_AUTO_VA);
   return &vm->base;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   pan_kmod_dev_free(vm->dev, vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   (void)vm; (void)mode;
   for (uint32_t i = 0; i < op_count; i++) {
      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct kbase_kmod_bo *kbo =
            container_of(ops[i].map.bo, struct kbase_kmod_bo, base);
         if (ops[i].va.start == PAN_KMOD_VM_MAP_AUTO_VA)
            ops[i].va.start = kbo->gpu_va + ops[i].map.bo_offset;
      }
   }
   return 0;
}

/* ============================================================
 * Timestamp Operation
 * ============================================================ */

static uint64_t
kbase_query_timestamp(const struct pan_kmod_dev *dev)
{
   mesa_logi("%s @ %d", __func__, __LINE__);
   union kbase_ioctl_get_cpu_gpu_timeinfo ti = {
      .in = { .request_flags = BASE_TIMEINFO_CYCLE_COUNTER_FLAG }
   };
   if (kbase_ioctl(dev->fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &ti) == 0)
      return ti.out.cycle_counter;

   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t
kbase_bo_gpu_va(const struct pan_kmod_bo *bo)
{
   const struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   return kbo->gpu_va;
}

/* ============================================================
 * Ops Table
 * ============================================================ */

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create              = kbase_kmod_dev_create,
   .dev_destroy             = kbase_kmod_dev_destroy,
   .dev_query_user_va_range = kbase_kmod_dev_query_user_va_range,

   .bo_alloc                = kbase_kmod_bo_alloc,
   .bo_free                 = kbase_kmod_bo_free,
   .bo_import               = kbase_kmod_bo_import,
   .bo_export               = kbase_kmod_bo_export,
   .bo_get_mmap_offset      = kbase_kmod_bo_get_mmap_offset,
   .flush_bo_map_syncs      = kbase_kmod_flush_bo_map_syncs,
   .bo_wait                 = kbase_kmod_bo_wait,

   .bo_make_evictable       = kbase_kmod_bo_make_evictable,
   .bo_make_unevictable     = kbase_kmod_bo_make_unevictable,

   .vm_create               = kbase_kmod_vm_create,
   .vm_destroy              = kbase_kmod_vm_destroy,
   .vm_bind                 = kbase_kmod_vm_bind,

   .query_timestamp         = kbase_query_timestamp,
   .bo_set_label            = NULL,
};
