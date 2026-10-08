/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_WDDM2_WINSYS_H
#define RADV_WDDM2_WINSYS_H

#include "radv_wddm2_mem_precheck.h"
#include "util/list.h"
#include "util/bc250_host_bootstrap.h"
#include "util/simple_mtx.h"
#include "util/vma.h"
#include "vk_wddm2_dispatch_table.h"
#include "ac_gpu_info.h"
#include "radv_winsys_bo.h"
#include "radv_radeon_winsys.h"
#include "vk_sync_binary.h"

struct vk_sync_type;


struct radv_wddm2_winsys {
   struct radeon_winsys base;

   uint32_t refcount;
   const void *cache_key;
   struct bc250_host host;

   struct radeon_info gpu_info;

   bool debug_all_bos;
   bool debug_log_bos;
   bool chain_ib;
   bool dump_ibs;
   bool bc250_trace_submits; /* optional hot-path diagnostic output */
   bool bc250; /* caps blob was ours; allocate/context/submit use the BC2* contract */

   uint32_t adapter_h;
   LUID adapter_luid;
   uint32_t device_h;
   uint32_t paging_queue_h;
   uint32_t paging_fence_h;

   /* Real zero backing for the low SMEM view, shared for the winsys lifetime. */
   struct {
      simple_mtx_t lock;
      struct radeon_winsys_bo *bo;
   } null_prt;
   simple_mtx_t heap_mtx;
   struct util_vma_heap heap;
   struct util_vma_heap _32bit_heap;
   struct util_vma_heap replay_heap;

   struct radv_winsys_bo_list global_bo_list;
   struct radv_winsys_bo_log bo_log;

   /* C70 (BD-096): what this process holds, and the limits that say when one more device-local
    * allocation cannot be made resident by any eviction. radv_wddm2_mem_precheck.h holds the rule. */
   struct {
      simple_mtx_t lock;
      struct radv_wddm2_mem_limits limits;
      uint64_t local_bytes;    /* device-local bytes this winsys holds */
      uint64_t nonlocal_bytes; /* host-visible bytes this winsys holds */
      uint64_t refusals;
      bool enabled;
      bool logged;             /* the first refusal is logged, the rest are counted */
   } mem;

   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];
   struct {
      void *d3d12_device; 
      void *d3d12_queue;
   } wsi;
};

static inline struct radv_wddm2_winsys *
radv_wddm2_winsys(struct radeon_winsys *base)
{
   return (struct radv_wddm2_winsys *)base;
}

/* C70 (BD-096), radv_wddm2_winsys.c: the gate before a kernel allocation, and the count of what
 * this winsys holds. Only a device-local allocation can be refused. */
bool radv_wddm2_mem_admit(struct radv_wddm2_winsys *ws, uint64_t size, bool device_local);
void radv_wddm2_mem_account(struct radv_wddm2_winsys *ws, uint64_t size, bool device_local,
                            bool add);

#endif /* RADV_WDDM2_WINSYS_H */
