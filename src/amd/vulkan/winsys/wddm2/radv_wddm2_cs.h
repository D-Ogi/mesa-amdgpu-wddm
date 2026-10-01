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

#ifndef RADV_WDDM2_CS_H
#define RADV_WDDM2_CS_H

#include "radv_wddm2_winsys.h"
#include "radv_radeon_winsys.h"

#include <stdint.h>
#include "util/u_dynarray.h"

struct vk_wddm2_fence {
   uint32_t handle;
   uint64_t wait_value;
   uint64_t *value_map;
};

/* The gather ring of a queue: ws->bc250_gather_slots slots in use (BC250_GATHER_SLOTS, 16 by default,
 * 4 to BC250_GATHER_SLOTS_MAX), the array sized for the largest. */
#define BC250_GATHER_SLOTS_MIN     4u
#define BC250_GATHER_SLOTS_DEFAULT 16u
#define BC250_GATHER_SLOTS_MAX     32u
struct bc250_gather_slot {
   struct radeon_winsys_bo *bo;
   uint8_t *map;
   uint64_t retire_value;
};

struct bc250_submit_ib; /* radv_wddm2_cs.c */

struct radv_wddm2_queue {
   enum amd_ip_type hw_ip;
   uint32_t context_h;
   uint32_t handle;
   uint32_t queue_id; /* KMD identifier */
   struct vk_wddm2_fence vm_fence;
   struct util_dynarray sparse_ops;
   bool sparse_batch_active;
   /* Each queue owns its packed IB and an unconditional retirement fence. The packed IB
    * is the IB1 the KMD launches: IB2 calls of the submitted IBs, or their copies. */
   struct radv_wddm2_winsys *bc250_ws;
   struct bc250_gather_slot bc250_gather[BC250_GATHER_SLOTS_MAX];
   unsigned bc250_gather_index;
   bool bc250_submit_failed;
   struct bc250_submit_ib *bc250_ibs;
   unsigned bc250_ib_capacity;
   struct vk_wddm2_fence bc250_progress;
   /* Nonzero: the GPU writes bc250_progress (BC250_PROGRESS_FENCE=gpu), this is its
    * FenceValueGPUVirtualAddress, and every IB1 of the queue ends with that write; the kernel never
    * signals this fence. Zero: the kernel signals it after each IB1. Fixed from the bind to the release,
    * so that one fence never takes both writers. */
   uint64_t bc250_progress_va;
   /* The largest bc250_progress value read at a submit: the invariant check of
    * radv_wddm2_cs_submit (a progress read never goes back). Zeroed with the queue. */
   uint64_t bc250_last_observed;
   /* The progress tracker of deferred destruction, while bc250_progress exists (radv_wddm2_bo.h). */
   struct radv_wddm2_tracker *bc250_tracker;
   /* context_h came from the embedder's queue (BC250_HOST_CREATE_QUEUE_CONTEXT)
    * and goes back through it, with the cookie it was bound with: NULL for
    * the engine's internal queue. */
   bool bc250_queue_context;
   void *bc250_queue_cookie;
};

struct radv_wddm2_ctx {
   struct radv_wddm2_winsys *ws;

   /* Hosted queue binding (draft). A bindable context belongs to one
    * VkQueue and holds kernel objects only while bound; unbound, it makes
    * no host call at all. Binding fills per_ip[AMD_IP_GFX] only. */
   bool bindable;
   bool bound;
   /* A bind or unbind could not release everything: per_ip[AMD_IP_GFX]
    * still owns what it names, the context is never bound again, and its
    * destruction makes no host call. unretired: its work did not retire. */
   bool kept;
   bool unretired;
   enum radeon_ctx_priority priority;

   struct radv_wddm2_queue ace_queue;

   struct {
      struct radv_wddm2_queue queue;
      struct vk_wddm2_fence last_submission;
   } per_ip[AMD_NUM_IP_TYPES];
};

static inline struct radv_wddm2_ctx *
radv_wddm2_ctx(struct radeon_winsys_ctx *base)
{
   return (struct radv_wddm2_ctx *)base;
}

/* Outside the embedder's bind scope a bindable context reaches no host. */
static inline bool
radv_wddm2_ctx_unbound(const struct radv_wddm2_ctx *ctx)
{
   return ctx->bindable && !ctx->bound;
}

void radv_wddm2_cs_init_functions(struct radv_wddm2_winsys *ws);

struct radv_wddm2_bo;
/* The deferred-destruction witness's stamp of every BO one command stream names (radv_wddm2_bo.c). */
void radv_wddm2_witness_cs(struct ac_cmdbuf *cs, uint32_t serial, uint64_t value, uint32_t *stale,
                           struct radv_wddm2_bo **first);

#endif /* RADV_WDDM2_CS_H */
