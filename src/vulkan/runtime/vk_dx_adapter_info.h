/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef VK_DX_ADAPTER_INFO_H
#define VK_DX_ADAPTER_INFO_H

#include <wsl/winadapter.h>
#include <vulkan/vulkan_core.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_dx_adapter_info {
   LUID adapter_luid;
   uint32_t physical_adapter_index;
   uint32_t vendor_id;
   uint32_t device_id;
   uint32_t subsys_id;
   uint32_t revision;
   uint64_t shared_system_memory;
   uint64_t dedicated_system_memory;
   uint64_t dedicated_video_memory;
   bool is_warp;
   char description[128];
};

#ifdef __cplusplus
}
#endif

#endif /* VK_DX_ADAPTER_INFO_H */
