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
#include "util/u_atomic.h"

#define RADV_WDDM2_HEAP_START          0x0000000200000000ull
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

   void *map;
   uint32_t handle;
   uint32_t resource_handle; /* nonzero for an opened shared WDDM resource */

   struct radeon_bo_metadata md;

   /* Deferred destruction (radv_wddm2_bo.c, BD-201). */
   bool destroyed;                 /* buffer_destroy ran: the BO waits for the GPU */
   /* R2/R5: this BO owns a GPU VA range (radv_wddm2_bo_owns_va) that was given up without being freed,
    * because no proof of retirement was ever established for it (teardown, or no entry at the destroy).
    * ws->deferred.leaked_ranges counts it. */
   bool va_leaked;
   /* The range is no longer this driver's to free: freed already, or leaked. A destroy retried after a
    * refused DestroyAllocation2 must not free it twice. */
   bool va_gone;
   struct list_head refused_link;  /* ws->deferred.refused, while a refused DestroyAllocation2 waits for
                                    * a retry (R7) */
};

static inline struct radv_wddm2_bo *
radv_wddm2_bo(struct radeon_winsys_bo *bo)
{
   return (struct radv_wddm2_bo *)bo;
}

struct radv_wddm2_queue;
void radv_wddm2_sparse_groups_clear(struct radv_wddm2_queue *queue);

void radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws);

/* The progress of one queue as deferred destruction sees it (radv_wddm2_bo.c, BD-201). The queue's
 * submitter publishes, before each submission, the progress value that submission will signal; a
 * destroyed BO then waits for the value published on every queue at the time of its destroy. */
struct radv_wddm2_tracker {
   struct list_head link;       /* ws->deferred.trackers */
   const uint64_t *value_map;   /* the queue's progress fence; NULL once the queue's work retired */
   uint64_t published;          /* the largest value submitted or being submitted; atomic */
   uint32_t refs;               /* the queue while attached, each held BO that waits on it, each CPU waiter */
   uint32_t waiters;            /* CPU waits on the fence now: a retired detach waits for them */
   uint32_t fence;              /* the progress fence's handle, for CPU waits */
   uint32_t serial;             /* unique in the winsys, never 0 */
   uint32_t context;            /* for the lines only */
   bool attached;
   /* BD-212: the queue went while a value it published had not retired. The tracker stays listed and
    * referenced until the value retires (radv_wddm2_tracker_reap_locked), so a destroy in between
    * still records a wait for that work. */
   bool abandoned;
   /* R4: every value this queue published was proved retired before its value page went, so the
    * tracker answers every later question without the page (radv_wddm2_tracker_page_done_locked). */
   bool retired_done;
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

/* A bounded CPU wait for value on a progress fence (radv_wddm2_cs.c); false if it failed or timed out. */
bool radv_wddm2_fence_wait_value(struct radv_wddm2_winsys *ws, uint32_t fence, const uint64_t *value_map,
                                 uint64_t value);

/* Destroys the held BOs whose waits retired. Makes no host call when none did. */
void radv_wddm2_deferred_drain(struct radv_wddm2_winsys *ws);
/* Teardown: CPU-waits for every held BO's work, destroys every held BO (one whose wait failed too) and
 * writes the summary line. */
void radv_wddm2_deferred_finish(struct radv_wddm2_winsys *ws);

/* BD-201: while this counter is positive, that many allocations of a deferred-destroy entry fail, and
 * each failure decrements it. Only the winsys host tests write it; the driver never does, and no knob
 * reaches it. It is here so that a test can prove that the destroy path needs no allocation. */
extern int32_t radv_wddm2_deferred_entry_fail;

#endif /* RADV_WDDM2_BO_H */
