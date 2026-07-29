/*
 * Copyright © 2024 Mesa kbase backend contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "pan_kmod.h"
#include <stdint.h>

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

/**
 * struct base_external_resource - external dma-buf resource for job submission.
 */
struct base_external_resource {
   uint64_t ext_resource; /* gpu_va | access (bit 0: 0=non-exclusive, 1=exclusive) */
};

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


#define KBASE_IOCTL_TYPE 0x80

/* 120-byte extended struct for DDK >= 11.24 / v2.0 (0xc078803f, ioctl #63) */
struct kbase_ioctl_cs_queue_group_create_ex {
   uint64_t compute_core_mask;  /* 0x00 - Out: handle at 0x04 */
   uint64_t fragment_core_mask; /* 0x08 */
   uint64_t tiler_core_mask;    /* 0x10 */
   uint8_t  max_compute_cores;  /* 0x18 */
   uint8_t  max_fragment_cores; /* 0x19 */
   uint8_t  max_tiler_cores;    /* 0x1a */
   uint8_t  priority;           /* 0x1b */
   uint8_t  padding[4];         /* 0x1c */
   uint8_t  reserved[88];       /* 0x20 - 0x77 */
};
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_EX \
   _IOWR(KBASE_IOCTL_TYPE, 63, struct kbase_ioctl_cs_queue_group_create_ex)

/* 32-byte legacy struct (0xc020802a, ioctl #42) */
struct kbase_ioctl_cs_queue_group_create {
   uint64_t compute_core_mask;  /* 0x00 */
   uint64_t fragment_core_mask; /* 0x08 */
   uint64_t tiler_core_mask;    /* 0x10 */
   uint8_t  max_compute_cores;  /* 0x18 */
   uint8_t  max_fragment_cores; /* 0x19 */
   uint8_t  max_tiler_cores;    /* 0x1a */
   uint8_t  priority;           /* 0x1b */
   uint32_t group_handle;       /* 0x1c - Out */
};
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE \
   _IOWR(KBASE_IOCTL_TYPE, 42, struct kbase_ioctl_cs_queue_group_create)

/* KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE (0x4008802b, ioctl #43) */
struct kbase_ioctl_cs_queue_group_terminate {
   uint64_t group_handle;
};
#define KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE \
   _IOW(KBASE_IOCTL_TYPE, 43, struct kbase_ioctl_cs_queue_group_terminate)

int
kbase_ioctl(int fd, unsigned long req, void *arg);

#ifdef __cplusplus
} /* extern "C" */
#endif
