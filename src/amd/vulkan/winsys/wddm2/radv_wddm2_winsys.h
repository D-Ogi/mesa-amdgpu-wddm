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

#include "util/list.h"
#include "util/u_atomic.h"
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

   /* Live physical BO bytes, partitioned like the amdgpu winsys. */
   uint64_t allocated_vram;
   uint64_t allocated_vram_vis;
   uint64_t allocated_gtt;

   uint32_t refcount;
   const void *cache_key;
   struct bc250_host host;
   bool adapter_query;

   /* Atomic and sticky for this KMT device/host identity. Creation errors
    * cannot return DEVICE_LOST; the device status hook consumes this. */
   int allocation_device_removed;

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

   /* Deferred destruction (radv_wddm2_bo.c, BD-201): a destroyed BO keeps its allocation, its VA and
    * its residency until every queue's work that was in flight at the destroy has retired, as the
    * amdgpu kernel keeps a freed BO until its fences signal. BC250_DEFERRED_DESTROY=0 turns it off in
    * the lab flavour. All fields below the lock are guarded by it; count and starved_count are also
    * read without it as a hint, and enabled and cap_bytes are set once at creation. */
   struct {
      simple_mtx_t lock;
      bool enabled;
      uint64_t cap_bytes;        /* held bytes above this CPU-wait for the oldest held BO; 0: no cap */
      uint32_t next_serial;      /* the last tracker serial handed out */
      struct list_head trackers; /* struct radv_wddm2_tracker: one per queue with a progress fence */
      struct list_head entries;  /* struct radv_wddm2_deferred_bo, oldest first */
      uint32_t count;            /* BOs held now */
      uint64_t bytes;            /* their size */
      uint64_t total;            /* BOs deferred since the winsys was created */
      uint64_t total_bytes;
      uint64_t released;         /* held BOs destroyed since the winsys was created */
      uint64_t immediate;        /* destroys that found nothing in flight */
      uint32_t peak_count;
      uint64_t peak_bytes;
      uint64_t max_hold_ns;      /* longest time a BO was held */
      uint32_t report_count;     /* the next count, bytes or hold that prints a line */
      uint64_t report_bytes;
      uint64_t report_hold_ns;
      uint64_t report_total;
      uint64_t forced;           /* destroyed unretired at teardown */
      uint64_t retries;          /* allocations retried after waiting for held BOs */
      uint64_t oom_waits;        /* CPU waits for the oldest held BO after a failed allocation */
      uint64_t cap_waits;        /* CPU waits for the oldest held BO over the cap */
      uint64_t cap_wait_ns, cap_wait_max_ns;
      uint64_t cap_failed;       /* cap waits that failed or released nothing */
      uint64_t report_cap_waits;
      /* BD-201: a destroy that could not allocate its entry waits for the published values itself,
       * with no allocation. A BO whose wait failed or timed out keeps its allocation, its VA and its
       * residency here, through starved_link: the drain points and teardown retry it. */
      struct list_head starved;  /* struct radv_wddm2_bo, through starved_link */
      uint32_t starved_count;    /* atomic */
      uint64_t starved_bytes;
      uint64_t starved_total;    /* destroys that found no entry */
      uint64_t starved_waits;    /* CPU waits those destroys made */
      uint64_t starved_wait_ns;
      uint64_t starved_failed;   /* of them, the waits that failed: the BO went on the list */
   } deferred;

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

#endif /* RADV_WDDM2_WINSYS_H */
