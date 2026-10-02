/*
 * Copyright © 2021 Intel Corporation
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
#ifndef VK_DXGI_FENCE_H
#define VK_DXGI_FENCE_H

#include "vk_sync.h"

#include "util/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(_WINDEF)
typedef void *HANDLE;
#endif

struct vk_queue;

extern const struct vk_sync_type vk_wddm2_monitored_fence_type;

struct vk_wddm2_monitored_fence {
   struct vk_sync base;

   uint32_t handle;
#ifdef _WIN32
   HANDLE shared_handle;
#endif
   uint64_t *value_map;

   /* Where this fence was last signalled from, for the BC250 winsys's wait provenance and its wait
    * elision (radv_wddm2_cs.c). The signalling submission writes the record, a later submission reads
    * it; signal_gen is a seqlock, odd while the three fields are being written, so a reader that sees
    * an odd generation or a generation that changed across its read calls the fence's provenance
    * unknown and keeps the kernel wait. Set to zero by init and by an import (a different kernel
    * object): no record, hence unknown. The epoch is the signalling queue's binding
    * (radv_wddm2_queue::bc250_epoch), never a D3DKMT context handle, which the kernel recycles.
    */
   uint32_t signal_gen;
   uint64_t signal_epoch;
   uint64_t signal_value;
   uint64_t signal_progress;
};

static inline bool
vk_sync_type_is_wddm2_monitored_fence(const struct vk_sync_type *type)
{
   return type == &vk_wddm2_monitored_fence_type;
}

static inline struct vk_wddm2_monitored_fence *
vk_sync_as_wddm2_monitored_fence(struct vk_sync *sync)
{
   if (!vk_sync_type_is_wddm2_monitored_fence(sync->type))
      return NULL;

   return container_of(sync, struct vk_wddm2_monitored_fence, base);
}

VkResult vk_wddm2_check_device_status(struct vk_device *device);

VkResult
vk_wddm2_monitored_fence_gpu_wait_many(struct vk_queue *queue,
                                       uint32_t context_handle,
                                       uint32_t wait_count,
                                       const struct vk_sync_wait *waits);
VkResult
vk_wddm2_monitored_fence_gpu_signal_many(struct vk_queue *queue,
                                         uint32_t context_handle,
                                         uint32_t signal_count,
                                         const struct vk_sync_signal *signals);

#ifdef __cplusplus
}
#endif

#endif /* VK_DXGI_FENCE_H */
