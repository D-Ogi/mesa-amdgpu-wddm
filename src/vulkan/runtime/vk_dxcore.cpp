/*
 * Copyright ?? Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.??? IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_dxcore.h"
#ifndef _WIN32
#include <wsl/winadapter.h>
#endif

#include <directx/dxcore.h>
#include <dxguids/dxguids.h>

#include "util/u_dl.h"
#include "util/log.h"

VkResult
vk_dxcore_adapter_foreach(vk_dxcore_adapter_cb func, void *user_data)
{
   IDXCoreAdapterFactory *factory;
   IDXCoreAdapterList *list;
   VkResult result = VK_SUCCESS;

   util_dl_library *dxcore = util_dl_open(UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT);
   if (!dxcore) {
      mesa_loge("Failed to load DXCore\n");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   using PFNDXCoreCreateAdapterFactory = HRESULT (APIENTRY*)(REFIID, void **);
   PFNDXCoreCreateAdapterFactory create_func = (PFNDXCoreCreateAdapterFactory)util_dl_get_proc_address(dxcore, "DXCoreCreateAdapterFactory");
   if (!create_func) {
      mesa_loge("Failed to load DXCoreCreateAdapterFactory\n");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (FAILED(create_func(IID_PPV_ARGS(&factory)))) {
      mesa_loge("Failed to create DXCore adapter factory\n");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (FAILED(factory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, IID_PPV_ARGS(&list)))) {
      factory->Release();
      mesa_loge("Failed to create DXCore adapter list\n");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   for (uint32_t i = 0; i < list->GetAdapterCount(); ++i) {
      struct vk_dx_adapter_info info = {};
      IDXCoreAdapter *adapter;
      DXCoreHardwareID hardware_id;
      bool is_hardware;

      if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&adapter)))) {
         mesa_loge("Failed to get DXCore adapter %i\n", i);
         continue;
      }

      if (FAILED(adapter->GetProperty(DXCoreAdapterProperty::HardwareID, &hardware_id)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, &info.dedicated_video_memory)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, &info.shared_system_memory)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::DedicatedSystemMemory, &info.dedicated_system_memory)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, &info.adapter_luid)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::IsHardware, &is_hardware)) ||
          FAILED(adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, sizeof(info.description), info.description))) {
         adapter->Release();
         mesa_loge("Failed to retrieve DXCore adapter properties for adapter %i\n", i);
         continue;
      }

      info.physical_adapter_index = i;
      info.vendor_id = hardware_id.vendorID;
      info.device_id = hardware_id.deviceID;
      info.subsys_id = hardware_id.subSysID;
      info.revision = hardware_id.revision;
      info.is_warp = !is_hardware;

      result = func(&info, (IUnknown *)adapter, user_data);
      adapter->Release();

      if (result != VK_SUCCESS)
         break;
   }

   list->Release();
   factory->Release();

   return result;
}
