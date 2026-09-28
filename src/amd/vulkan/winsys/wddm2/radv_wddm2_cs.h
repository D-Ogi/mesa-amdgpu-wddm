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

#define BC250_GATHER_SLOTS 7u
struct bc250_gather_slot {
   struct radeon_winsys_bo *bo;
   uint8_t *map;
   uint64_t retire_value;
};

struct radv_wddm2_queue {
   enum amd_ip_type hw_ip;
   uint32_t context_h;
   uint32_t handle;
   uint32_t queue_id; /* KMD identifier */
   struct vk_wddm2_fence vm_fence;
   struct util_dynarray sparse_ops;
   bool sparse_batch_active;
   /* Each queue owns its packed IB and an unconditional retirement fence. */
   struct radv_wddm2_winsys *bc250_ws;
   struct bc250_gather_slot bc250_gather[BC250_GATHER_SLOTS];
   unsigned bc250_gather_index;
   bool bc250_submit_failed;
   struct radv_winsys_ib *bc250_ibs;
   unsigned bc250_ib_capacity;
   struct vk_wddm2_fence bc250_progress;
};

struct radv_wddm2_ctx {
   struct radv_wddm2_winsys *ws;

   /* Hosted queue binding (draft). A bindable context belongs to one
    * VkQueue and holds kernel objects only while bound; unbound, it makes
    * no host call at all. */
   bool bindable;
   bool bound;

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

#endif /* RADV_WDDM2_CS_H */
