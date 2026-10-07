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

/* The witness's BO classes (radv_wddm2_bo.c), from the creation priority and flags. */
#define RADV_WDDM2_BO_CLASSES 8
/* In-flight destroys the witness logs one by one, with their stacks. */
#define RADV_WDDM2_WITNESS_LINES 64

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

   struct radeon_info gpu_info;

   bool debug_all_bos;
   bool debug_log_bos;
   bool chain_ib;
   bool dump_ibs;
   bool bc250_trace_submits; /* optional hot-path diagnostic output */
   /* BD-096: the D3DDDI_MAKERESIDENT_FLAGS.Value of every MakeResident of this winsys, from the memory
    * overflow policy (radv_wddm2_mem_overflow.h), read once at winsys creation. */
   unsigned make_resident_flags;
   bool bc250; /* caps blob was ours; allocate/context/submit use the BC2* contract */
   /* bc250 submission of several IBs: false (the default) writes IB2 calls into the gather
    * slot, true copies every IB into it (BC250_IB_NOCOPY=0, or BC250_IB_DWORDS set). */
   bool bc250_gather_copy;
   unsigned long bc250_ib_dwords_cap; /* BC250_IB_DWORDS, read once; 0 when unset */
   /* Submissions whose IB1 holds a copied IB although the IB2 calls are on. */
   uint64_t bc250_inline_submits;
   /* The submit path (radv_wddm2_cs.c), both read once at creation (radv_wddm2_bo_init_functions):
    * BC250_GATHER_SLOTS, the gather ring of each queue (4..BC250_GATHER_SLOTS_MAX, 16 by default),
    * and BC250_SUBMIT_COALESCE: merge the progress signal into the application's signal call of the
    * same submission, and leave out GPU waits the CPU already sees complete (both by default). */
   /* The host's copy of the knobs it decided, taken at creation; present is 0 without a policy. */
   struct bc250_host_policy_values bc250_policy;
   unsigned bc250_gather_slots;
   bool bc250_merge_signals;
   bool bc250_drop_waits;
   /* BC250_PROGRESS_FENCE (read with the knobs above): true ("gpu", the default) lets each queue's IB1
    * write its progress value with a RELEASE_MEM to the fence's FenceValueGPUVirtualAddress, so the
    * progress fence takes no kernel signal; false ("kernel") signals it through the kernel, as before.
    * A queue without a usable GPU address, or with BC250_IB_DWORDS set, uses the kernel for its life. */
   bool bc250_progress_gpu;
   /* Counters of the submit path for the periodic summary, since creation; atomic. */
   struct {
      uint64_t submits;           /* bc250 submissions with an IB */
      uint64_t progress_separate; /* progress signals in a call of their own */
      uint64_t progress_merged;   /* progress signals carried by the application's signal call */
      uint64_t progress_gpu;      /* progress values written by the IB1's RELEASE_MEM: no kernel signal */
      uint64_t kernel_queues;     /* queues bound with the GPU write asked for but signalled by the kernel */
      uint64_t signal_calls;      /* application signal calls (signal-only submissions too) */
      uint64_t signal_objects;    /* application fences in them */
      uint64_t wait_objects;      /* application waits handed to the winsys */
      uint64_t wait_dropped;      /* of them, complete on the CPU's read: no GPU wait */
      uint64_t wait_calls;        /* WaitForSynchronizationObjectFromGpu calls made */
      uint64_t wait_skipped;      /* calls left out, every wait of the submission being complete */
      uint64_t gather_waits;      /* CPU waits for a gather slot to retire before its reuse */
      uint64_t gather_wait_ns, gather_wait_max_ns;
   } submit_stats;

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

   /* Deferred destruction (radv_wddm2_bo.c): a destroyed BO keeps its allocation, its VA and its
    * residency until every queue's work that was in flight at the destroy has retired, as the amdgpu
    * kernel keeps a freed BO until its fences signal. BC250_DEFERRED_DESTROY=0 turns it off. All
    * fields below the lock are guarded by it; count is also read without it as a hint, enabled,
    * witness and cap_bytes are set once at creation. */
   struct {
      simple_mtx_t lock;
      bool enabled;
      /* The witness: destroys of BOs whose last submission had not retired, by class
       * (BC250_DEFERRED_WITNESS=0 turns it off). */
      bool witness;
      uint64_t cap_bytes;        /* held bytes above this CPU-wait for the oldest held BO; 0: no cap */
      uint32_t next_serial;      /* the last tracker serial handed out */
      uint64_t released;         /* held BOs destroyed since the winsys was created */
      uint64_t cap_waits;        /* CPU waits for the oldest held BO over the cap */
      uint64_t cap_wait_ns, cap_wait_max_ns;
      uint64_t cap_failed;       /* cap waits that failed or released nothing */
      uint64_t report_cap_waits;
      uint64_t oom_waits;        /* CPU waits for the oldest held BO after a failed allocation */
      uint64_t destroys[RADV_WDDM2_BO_CLASSES];  /* the witness: every non-borrowed destroy */
      uint64_t held_by_class[RADV_WDDM2_BO_CLASSES];
      uint64_t in_flight[RADV_WDDM2_BO_CLASSES]; /* its last submission had not retired */
      uint64_t destroys_total, in_flight_total, in_flight_32bit, in_flight_bytes;
      uint64_t report_destroys, report_in_flight;
      uint32_t in_flight_logged; /* in-flight lines written, at most RADV_WDDM2_WITNESS_LINES */
      uint64_t stale_names;      /* BOs named by a submission after their destroy; atomic */
      uint32_t stale_logged;     /* atomic */
      /* Destroyed BO structs wait here, oldest first, before reuse: a stale pointer in a command
       * stream's BO set then names a BO struct, never freed memory. */
      simple_mtx_t pool_lock;
      struct list_head pool;
      uint32_t pool_count;
      struct list_head trackers; /* struct radv_wddm2_tracker: one per queue with a progress fence */
      struct list_head entries;  /* struct radv_wddm2_deferred_bo, oldest first */
      uint32_t count;            /* BOs held now */
      uint64_t bytes;            /* their size */
      uint64_t total;            /* BOs deferred since the winsys was created */
      uint64_t total_bytes;
      uint64_t immediate;        /* destroys that found nothing in flight */
      uint32_t peak_count;
      uint64_t peak_bytes;
      uint64_t max_hold_ns;      /* longest time a BO was held */
      uint32_t report_count;     /* the next count, bytes or hold that prints a line */
      uint64_t report_bytes;
      uint64_t report_hold_ns;
      uint64_t report_total;     /* the next total that prints a progress line (a killed process prints no summary) */
      uint64_t forced;          /* destroyed unretired at teardown */
      uint64_t retries;          /* allocations retried after waiting for held BOs */
      uint64_t borrowed;         /* host imports destroyed: the host's, never held or witnessed; atomic */
      /* BD-045: owned BOs whose CPU lock the host would not release at destroy. They keep the lock, the
       * VA, the allocation and the byte charge; the drain points retry them (radv_wddm2_bo.c). */
      struct list_head locked;   /* struct radv_wddm2_bo, through pool_link */
      uint32_t locked_count;     /* atomic */
      uint64_t locked_bytes;
      uint64_t locked_next_ns;   /* the next retry */
      uint64_t locked_interval_ns;
      uint64_t unlock_failed;    /* Unlock2 calls that failed; atomic */
   } deferred;

   /* The periodic summary (radv_wddm2_bo.c): every BC250_DEFERRED_SUMMARY_S seconds (30 by default, 0
    * off) a submission writes the deferred-destroy and submit-path counters to the log, each line only
    * when its counters changed since the last one. next_ns is the deadline, 0 when off; atomic. */
   struct {
      simple_mtx_t lock;   /* the writer's: one summary at a time, and the snapshots below */
      uint64_t interval_ns;
      uint64_t next_ns;
      uint64_t start_ns;
      uint32_t lines;      /* summaries written */
      /* The counters of the last lines written, in the order radv_wddm2_bo.c collects them. */
      uint64_t deferred_snapshot[48];
      uint64_t submit_snapshot[16];
   } summary;

   /* BC250_DRAW_STATS=1: the RADV_DRAW_STATS counters of every command buffer ended since the winsys was
    * created (atomic), written as a third summary line (radv_wddm2_bo.c); snapshot is the last line's. */
   struct {
      uint64_t totals[RADV_DRAW_STAT_COUNT];
      uint64_t snapshot[RADV_DRAW_STAT_COUNT];
   } draw_stats;

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
