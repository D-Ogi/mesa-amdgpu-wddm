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

#define RADV_WDDM2_HEAP_START          0x0000000200000000ull
#define RADV_WDDM2_32BIT_HEAP_START    0x0000000100000000ull
#define RADV_WDDM2_REPLAY_HEAP_START   0x0000500000000000ull

struct radv_wddm2_bo {
   struct radeon_winsys_bo base;
   struct radv_wddm2_winsys *ws;

   enum radeon_bo_flag flags;

   void *map;
   uint32_t handle;

   struct radeon_bo_metadata md;
};

static inline struct radv_wddm2_bo *
radv_wddm2_bo(struct radeon_winsys_bo *bo)
{
   return (struct radv_wddm2_bo *)bo;
}

void radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws);

#endif /* RADV_WDDM2_BO_H */
