/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#ifndef RADV_WDDM2_BO_H
#define RADV_WDDM2_BO_H

#include "radv_wddm2_winsys.h"
#include "util/macros.h"
#include "util/os_time.h"

#define RADV_WDDM2_HEAP_START         0x0000000200000000ull
#define RADV_WDDM2_32BIT_HEAP_START    0x0000000100000000ull
/* Both sparse views must stay below the GFX10 canonical-address hole (bit 47).
 * Ordinary, imported and capture/replay allocations all keep control bit 46 clear.
 * Linux reference: amdgpu_kms.c AMDGPU_INFO_DEV_INFO VA limits; E33 high-VA controls.
 */
#define RADV_WDDM2_PRT_CONTROL_BIT    46
#define RADV_WDDM2_PRT_CONTROL_MASK   (1ull << RADV_WDDM2_PRT_CONTROL_BIT)
#define RADV_WDDM2_REPLAY_HEAP_START   (1ull << 45)

struct radv_wddm2_bo {
   struct radeon_winsys_bo base;
   struct radv_wddm2_winsys *ws;

   enum radeon_bo_flag flags;

   /* Entire OS reservation; base.va may be an aligned interior address. */
   uint64_t reserved_va;
   uint64_t reserved_size;
   uint64_t sparse_high_va;
   bool emulate_sparse_residency;
   bool borrowed;
   /* A borrowed allocation whose host answers Lock2 and Unlock2 (BC250_HOST_IMPORT_CPU_MAP). */
   bool host_mappable;

   void *map;
   uint32_t handle;
   uint32_t resource_handle; /* nonzero for an opened shared WDDM resource */

   struct radeon_bo_metadata md;

   /* Deferred destruction and its witness (radv_wddm2_bo.c). A submission stamps every BO it names
    * with its queue's tracker serial and the progress value it signals; a destroy reads the stamp. */
   uint64_t last_use_value;
   uint32_t last_use_serial;  /* 0: no submission named the BO */
   uint8_t priority;          /* the RADV priority at creation, for the witness's class */
   bool destroyed;            /* buffer_destroy ran: the struct is held or waits in the pool */
   struct list_head pool_link; /* ws->deferred.pool, once the struct waits for reuse */
};

static inline struct radv_wddm2_bo *
radv_wddm2_bo(struct radeon_winsys_bo *bo)
{
   return (struct radv_wddm2_bo *)bo;
}

struct radv_wddm2_queue;
void radv_wddm2_sparse_groups_clear(struct radv_wddm2_queue *queue);

/* The progress of one queue as deferred destruction sees it (radv_wddm2_bo.c). The queue's
 * submitter publishes, before each submission, the progress value that submission will signal;
 * a destroyed BO then waits for the value published on every queue at the time of its destroy.
 */
struct radv_wddm2_tracker {
   struct list_head link;       /* ws->deferred.trackers */
   const uint64_t *value_map;   /* the queue's progress fence; NULL once the queue's work retired */
   uint64_t published;          /* the largest value submitted or being submitted; atomic */
   uint32_t refs;               /* the queue while attached, each held BO that waits on it, each CPU waiter */
   uint32_t waiters;            /* CPU waits on the fence now: a retired detach waits for them */
   uint32_t fence;              /* the progress fence's handle, for CPU waits */
   uint32_t serial;             /* unique in the winsys, never 0: the witness's stamp names the queue by it */
   uint32_t context;            /* for the lines only */
   bool attached;
};

struct radv_wddm2_tracker *radv_wddm2_tracker_attach(struct radv_wddm2_winsys *ws, uint32_t fence,
                                                     const uint64_t *value_map, uint32_t context);
/* retired: every value the queue signalled has retired, and its fence may go after this call.
 * Otherwise the fence stays with the device and held BOs keep waiting on it. */
void radv_wddm2_tracker_detach(struct radv_wddm2_winsys *ws, struct radv_wddm2_tracker *tracker, bool retired);

static inline void
radv_wddm2_tracker_publish(struct radv_wddm2_tracker *tracker, uint64_t value)
{
   if (tracker)
      p_atomic_set(&tracker->published, value);
}

/* The witness stamp of one BO a submission names. stale counts BOs already destroyed (a submission
 * that names freed memory); first keeps the first of them. */
static inline void
radv_wddm2_witness_stamp(struct radv_wddm2_bo *bo, uint32_t serial, uint64_t value, uint32_t *stale,
                         struct radv_wddm2_bo **first)
{
   if (unlikely(bo->destroyed) && !(*stale)++)
      *first = bo;
   bo->last_use_value = value;
   bo->last_use_serial = serial;
}

/* Counts stale BOs a submission on context named and logs the first few (radv_wddm2_cs.c). */
void radv_wddm2_witness_stale(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *first, uint32_t stale,
                              uint32_t context);

/* A bounded CPU wait for value on a progress fence (radv_wddm2_cs.c); false if it failed or timed out. */
bool radv_wddm2_fence_wait_value(struct radv_wddm2_winsys *ws, uint32_t fence, const uint64_t *value_map,
                                 uint64_t value);

/* One line of the winsys log (the log file always, stderr with AMDGPU_WDDM_LOG), in the format and
 * file of the periodic summary. The wait provenance of radv_wddm2_cs.c writes its lines through it. */
void radv_wddm2_winsys_line(const char *format, ...) PRINTFLIKE(1, 2);

/* Destroys the held BOs whose waits retired. Makes no host call when none did. */
void radv_wddm2_deferred_drain(struct radv_wddm2_winsys *ws);
/* Teardown: CPU-waits for every held BO's work, destroys every held BO (one whose wait failed too) and
 * writes the finish lines. */
void radv_wddm2_deferred_finish(struct radv_wddm2_winsys *ws);
/* Frees the BO structs the pool keeps; after radv_wddm2_deferred_finish. */
void radv_wddm2_bo_pool_finish(struct radv_wddm2_winsys *ws);

/* The periodic summary (radv_wddm2_bo.c). due is the deadline the caller read; final writes both lines
 * now, whatever changed (teardown). */
void radv_wddm2_summary_write(struct radv_wddm2_winsys *ws, uint64_t now, uint64_t due, bool final);

/* At every submission: one clock read (QueryPerformanceCounter, no kernel call), and the lines only
 * once the period has passed. */
static inline void
radv_wddm2_summary_tick(struct radv_wddm2_winsys *ws)
{
   const uint64_t due = p_atomic_read(&ws->summary.next_ns);
   if (!due)
      return;
   const uint64_t now = os_time_get_nano();
   if (unlikely(now >= due))
      radv_wddm2_summary_write(ws, now, due, false);
}

void radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws);

#endif /* RADV_WDDM2_BO_H */
