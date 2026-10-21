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
   bool cache_hosted;
   struct bc250_host host;

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
   /* R1-c (BD-201): no VA heaps of our own any more. MapGpuVirtualAddress places every ordinary range
    * and ReserveGpuVirtualAddress every reservation, within the RADV_WDDM2_*_HEAP_START bounds, which
    * stay as the MinimumAddress/MaximumAddress of those calls (radv_wddm2_bo.h). */

   struct radv_winsys_bo_list global_bo_list;
   struct radv_winsys_bo_log bo_log;

   /* Deferred destruction (radv_wddm2_bo.c, BD-201), R v2: an ordinary BO is released at its destroy
    * and the OS defers what has to be deferred (DestroyAllocation2 with flags 0 keeps the allocation
    * and its API-allocated range until the commands queued before the request finish). A BO that owns
    * a range of its own (radv_wddm2_bo_owns_va) is held here until the progress values captured at its
    * destroy are reached. BC250_DEFERRED_DESTROY=0 turns the hold off. All fields below the lock are
    * guarded by it; count, abandoned and refused_count are also read without it as a hint, and
    * enabled and cap_bytes are set once at creation. */
   struct {
      simple_mtx_t lock;
      bool enabled;
      /* R7: a cap never forces a release; it only reports. Held bytes above this write a line, at most
       * one per doubling of cap_reports; nothing is waited for and nothing is released early. 0: no
       * cap, no line. */
      uint64_t cap_bytes;
      uint64_t cap_reports;      /* times the held bytes were found over the cap */
      uint64_t report_cap_reports;
      uint32_t next_serial;      /* the last tracker serial handed out */
      struct list_head trackers; /* struct radv_wddm2_tracker: one per queue with a progress fence */
      /* BD-212: a queue that went while a value it published had not retired leaves its tracker
       * listed, with the fence and value page its queue keeps, until the value retires: a destroy in
       * between still records a wait for that work. abandoned counts them now (atomic, so a drain
       * point looks without the lock), reaped counts those released. */
      uint32_t abandoned;
      uint64_t reaped;
      struct list_head entries;  /* struct radv_wddm2_deferred_bo, oldest first */
      uint32_t count;            /* BOs held now */
      uint64_t bytes;            /* their size */
      /* R7: the VA ranges those held BOs keep, and the ranges given up unfreed (R2/R5). */
      uint64_t held_va_bytes;
      uint64_t held_va_32bit;
      uint64_t leaked_va_bytes;
      uint64_t leaked_ranges;
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
      uint64_t retries;          /* allocations retried after waiting for held BOs */
      uint64_t oom_waits;        /* CPU waits for the oldest held range after a failed allocation */
      uint64_t sweeps;           /* create-time sweeps: an allocation or a VA reservation that failed */
      /* R7: a DestroyAllocation2 the OS refused. The allocation is still the ICD's, so it is counted
       * and the drain points and teardown retry it, through refused_link; nothing is ever written
       * down as "no allocations held". */
      struct list_head refused;  /* struct radv_wddm2_bo, through refused_link */
      uint32_t refused_count;    /* atomic */
      uint64_t refused_bytes;
      uint64_t refused_total;    /* refusals seen, retries included */
      uint64_t refused_retries;  /* DestroyAllocation2 calls a retry made */
      uint64_t refused_next_ns;  /* the next retry */
      uint64_t refused_interval_ns;
      /* A FreeGpuVirtualAddress the OS refused, of a range only this driver frees
       * (radv_wddm2_bo_owns_va). The range is still the ICD's, so the BO is kept whole, through
       * refused_link, and the drain points and teardown retry the free, every refused_interval_ns;
       * teardown leaks and counts what is still refused then (radv_wddm2_bo.c). va_refused_bytes is the
       * address space those BOs still own. */
      struct list_head va_refused; /* struct radv_wddm2_bo, through refused_link */
      uint32_t va_refused_count;   /* atomic */
      uint64_t va_refused_bytes;
      uint64_t va_refused_total;   /* refusals seen, retries included */
      uint64_t va_refused_retries; /* destroys a retry made */
      uint64_t va_refused_next_ns; /* the next retry */
      /* R6 (BD-201): repair signals of a published progress value whose submission or signal failed,
       * and those that could not be sent, which leave that value unproven for ever. Atomic: the
       * submission path writes them without the lock. */
      uint64_t progress_repairs, progress_repairs_failed;
   } deferred;

   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];
   struct {
      void *d3d12_device;
      void *d3d12_queue;
      /* BC-250 present route (radv_wddm2_wsi_route.h). The lock serialises the lazy creation of
       * the D3D12 device and queue; a zeroed SRWLOCK is a valid unlocked lock. */
      void *lock; /* SRWLOCK, kept as a pointer-sized field to stay free of windows.h here */
      int route;
      bool d3d12_tried;
      const char *d3d12_failure; /* why the D3D12 device or queue could not be made, or NULL */
   } wsi;
};

static inline struct radv_wddm2_winsys *
radv_wddm2_winsys(struct radeon_winsys *base)
{
   return (struct radv_wddm2_winsys *)base;
}

#endif /* RADV_WDDM2_WINSYS_H */
