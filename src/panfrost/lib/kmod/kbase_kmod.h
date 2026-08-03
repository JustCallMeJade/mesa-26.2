/*
 * Copyright © 2024 Mesa kbase backend contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "pan_kmod.h"
#include <stdint.h>
#include "kbase_uapi.h"
#include "kbase_csf_uapi.h"
#include "mali_base_csf_kernel.h"
#include "mali_kbase_csf_registers.h"

#if defined(__GNUC__) || defined(__clang__)
#define memory_barrier() __sync_synchronize()
#else
#include <stdatomic.h>
#define memory_barrier() atomic_thread_fence(memory_order_seq_cst)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * kbase_kmod_ops - pan_kmod backend for Arm mali_kbase kernel driver.
 *
 * Use when the kernel exposes /dev/mali0 via mali_kbase (Android OEM/MTK/Samsung)
 * instead of upstream Panfrost/Panthor DRM.
 */
extern const struct pan_kmod_ops kbase_kmod_ops;

struct kbase_kmod_dev {
   struct pan_kmod_dev base;
   struct {
      uint32_t product_id;
      uint32_t major_rev;
      uint32_t minor_rev;
      uint64_t cycle_freq;
   } gpu_info;
   bool allow_cs_groups;
};

/**
 * struct base_external_resource - external dma-buf resource for job submission.
 */
struct base_external_resource;

/**
 * kbase_kmod_job_submit - Submit a JM job chain directly via mali_kbase.
 *
 * @dev:       pan_kmod_dev created with kbase_kmod_ops
 * @jc:        GPU VA of the first job descriptor in the chain
 * @core_req:  BASE_JD_REQ_* bitmask (FS=fragment, CS=compute, T=tiler, V=vertex...)
 * @bos:       array of BOs touched by this job (for bo_wait() tracking)
 * @nbo:       number of BOs in the array
 * @ext_res:   external dma-buf resource descriptors (may be NULL)
 * @next_res:  number of external resources
 *
 * Returns the atom_number assigned (non-zero) on success, 0 on failure.
 * The caller can later call pan_kmod_bo_wait() on any BO in @bos and it will
 * block until this atom (and all others touching that BO) have completed.
 */
uint64_t kbase_kmod_job_submit(struct pan_kmod_dev *dev,
                               uint64_t jc, uint32_t core_req,
                               struct pan_kmod_bo **bos, uint32_t nbo,
                               struct base_external_resource *ext_res,
                               uint32_t next_res);

uint64_t
kbase_bo_gpu_va(const struct pan_kmod_bo *bo);

// New (version 1.14 plus) uapi
union kbase_ioctl_cs_tiler_heap_init_24 {
   struct {
      uint32_t chunk_size;       /* 0x00 */
      uint32_t initial_chunks;   /* 0x04 */
      uint32_t max_chunks;       /* 0x08 */
      uint16_t target_in_flight; /* 0x0C */
      uint8_t  group_id;          /* 0x0E */
      uint8_t  padding;           /* 0x0F */
      uint64_t heap_ctx_gpu_va;  /* 0x10 */
   } in;
   struct {
      uint64_t gpu_heap_va;      /* 0x00 */
      uint64_t first_chunk_va;   /* 0x08 */
   } out;
};

#define KBASE_IOCTL_CS_TILER_HEAP_INIT_24 \
   _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init_24)

int
kbase_ioctl(int fd, unsigned long req, void *arg);

struct kbase_kmod_bo {
   struct pan_kmod_bo base;
   uint64_t gpu_va;
   void    *cpu_ptr;   /* MAP_FAILED when unmapped */
   bool     exported;
   int      dmabuf_fd;
};

#ifndef KBASE_PAGE_SIZE
#define KBASE_PAGE_SIZE 4096
#endif

static inline volatile uint64_t *
kbase_user_cs_insert_ptr(void *db_page, uint32_t subqueue)
{
   uintptr_t base = (uintptr_t)db_page + KBASE_PAGE_SIZE;
   uintptr_t offset = (subqueue * CS_USER_INPUT_BLOCK_SIZE) + CS_INSERT_LO;
   return (volatile uint64_t *)(base + offset);
}

static inline volatile uint64_t *
kbase_user_cs_extract_ptr(void *db_page, uint32_t subqueue)
{
   uintptr_t base = (uintptr_t)db_page + KBASE_PAGE_SIZE * 2;
   uintptr_t offset = (subqueue * CS_USER_INPUT_BLOCK_SIZE) + CS_EXTRACT_LO;
   return (volatile uint64_t *)(base + offset);
}

static inline bool
kbase_user_cs_active(void *db_page, uint32_t subqueue)
{
   uintptr_t base = (uintptr_t)db_page + KBASE_PAGE_SIZE * 2;
   uintptr_t offset = (subqueue * CS_USER_INPUT_BLOCK_SIZE) + CS_ACTIVE;
   volatile uint32_t *active_reg = (volatile uint32_t *)(base + offset);
   memory_barrier();
   return (*active_reg & CS_ACTIVE_HW_ACTIVE_MASK) != 0;
}

static inline void
kbase_ring_user_doorbell(void *db_page)
{
   volatile uint32_t *db_reg = (volatile uint32_t *)((uint8_t *)db_page + DB_BLK_DOORBELL);
   memory_barrier();
   *db_reg = 1;
   memory_barrier();
}

#ifdef __cplusplus
} /* extern "C" */
#endif
