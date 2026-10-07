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

#ifndef RADV_WDDM2_WINSYS_PUBLIC_H
#define RADV_WDDM2_WINSYS_PUBLIC_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "util/bitset.h"

struct bc250_host;
struct bc250_host_policy_values;
struct radeon_winsys;
struct vk_dx_adapter_info;

VkResult radv_wddm2_winsys_create(const struct vk_dx_adapter_info *adapter_info,
                                  const BITSET_WORD *debug_flags, const struct bc250_host *host,
                                  const struct bc250_host_policy_values *policy, bool adapter_query,
                                  struct radeon_winsys **winsys);

void radv_wddm2_query_allocated(struct radeon_winsys *ws, uint64_t *vram,
                                uint64_t *vram_vis, uint64_t *gtt);

/* BD-096: the process's video memory budget, local plus non-local (QueryVideoMemoryInfo). */
bool radv_wddm2_query_budget(struct radeon_winsys *ws, uint64_t *budget);

#endif /* RADV_WDDM2_WINSYS_PUBLIC_H */
