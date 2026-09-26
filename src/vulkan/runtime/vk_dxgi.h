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

#ifndef VK_DXGI_H
#define VK_DXGI_H

#include "vk_dx_adapter_info.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef VkResult (*vk_dxgi_adapter_cb)(const struct vk_dx_adapter_info *info,
                                       void *adapter, void *user_data);

VkResult vk_dxgi_adapter_foreach(vk_dxgi_adapter_cb func, void *user_data);

void *vk_dxgi_find_adapter(LUID adapter_luid);     /* IDXGIAdapter1* */
void *vk_dxgi_create_d3d12_device(LUID adapter_luid); /* ID3D12Device* */
void *vk_dxgi_create_command_queue(void *device);  /* ID3D12CommandQueue* */
HANDLE vk_dxgi_share_device_resource(void *device, void *resource); /* ID3D12Device*, ID3D12Resource* */

#ifdef __cplusplus
}
#endif

#endif /* VK_DXGI_H */
