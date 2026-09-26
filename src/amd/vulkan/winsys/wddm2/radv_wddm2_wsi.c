/*
 * Copyright © 2026 Valve Corporation
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

#include "radv_wddm2_wsi.h"
#include "radv_wddm2_winsys.h"

#define COBJMACROS
#include <unknwn.h>
#include <directx/d3d12.h>

#include "radv_device.h"
#include "vk_dxgi.h"
#include "wsi_common.h"

static void *
radv_wddm2_wsi_get_d3d12_device(VkDevice _device)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(device->ws);

   if (!ws->wsi.d3d12_device) {
      ws->wsi.d3d12_device =
         vk_dxgi_create_d3d12_device(ws->adapter_luid);
   }

   return ws->wsi.d3d12_device;
}

static void *
radv_wddm2_wsi_get_d3d12_command_queue(VkDevice _device)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(device->ws);

   if (!ws->wsi.d3d12_queue) {
      ID3D12Device *d3d12_device = (ID3D12Device *)radv_wddm2_wsi_get_d3d12_device(_device);      D3D12_COMMAND_QUEUE_DESC desc = {};
      ID3D12Device_CreateCommandQueue(d3d12_device, &desc,
                                      &IID_ID3D12CommandQueue, (void **)&ws->wsi.d3d12_queue);
   }

   return ws->wsi.d3d12_queue;
}

static bool
radv_wddm2_wsi_needs_blits(VkDevice _device)
{
   return true;
}

void
radv_wddm2_wsi_init(struct radeon_winsys *ws, struct wsi_device *wsi)
{
   wsi->win32.get_d3d12_device = radv_wddm2_wsi_get_d3d12_device;
   wsi->win32.get_d3d12_command_queue = radv_wddm2_wsi_get_d3d12_command_queue;
   wsi->win32.requires_blits = radv_wddm2_wsi_needs_blits;
}

void
radv_wddm2_wsi_finish(struct radv_wddm2_winsys *ws)
{
   if (ws->wsi.d3d12_queue) {
      ID3D12CommandQueue_Release((ID3D12CommandQueue *)ws->wsi.d3d12_queue);
      ws->wsi.d3d12_queue = NULL;
   }
   if (ws->wsi.d3d12_device) {
      ID3D12Device_Release((ID3D12Device *)ws->wsi.d3d12_device);
      ws->wsi.d3d12_device = NULL;
   }
}