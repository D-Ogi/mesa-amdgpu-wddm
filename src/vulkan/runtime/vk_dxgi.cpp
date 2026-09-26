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

#include "vk_dxgi.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <directx/d3d12.h>

static IDXGIFactory4 *
vk_dxgi_get_factory(bool debug)
{
   HMODULE dxgi_mod = LoadLibraryA("DXGI.DLL");
   if (!dxgi_mod) {
      return NULL;
   }

   typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT flags, REFIID riid, void **ppFactory);
   PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2;

   CreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(dxgi_mod, "CreateDXGIFactory2");
   if (!CreateDXGIFactory2) {
      return NULL;
   }

   UINT flags = 0;
   if (debug)
      flags |= DXGI_CREATE_FACTORY_DEBUG;

   IDXGIFactory4 *factory;
   HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));
   if (FAILED(hr)) {
      return NULL;
   }

   return factory;
}

VkResult
vk_dxgi_adapter_foreach(vk_dxgi_adapter_cb func, void *user_data)
{   IDXGIFactory4 *factory = vk_dxgi_get_factory(false);
   if (!factory)
      return VK_ERROR_INITIALIZATION_FAILED;

   IDXGIAdapter1 *adapter = NULL;
   VkResult result = VK_SUCCESS;

   for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
      DXGI_ADAPTER_DESC1 dxgi_desc;
      adapter->GetDesc1(&dxgi_desc);

      struct vk_dx_adapter_info info = {};
      info.physical_adapter_index = i;
      info.adapter_luid = dxgi_desc.AdapterLuid;
      info.vendor_id = dxgi_desc.VendorId;
      info.device_id = dxgi_desc.DeviceId;
      info.subsys_id = dxgi_desc.SubSysId;
      info.revision = dxgi_desc.Revision;
      info.shared_system_memory = dxgi_desc.SharedSystemMemory;
      info.dedicated_system_memory = dxgi_desc.DedicatedSystemMemory;
      info.dedicated_video_memory = dxgi_desc.DedicatedVideoMemory;
      info.is_warp = (dxgi_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      WideCharToMultiByte(CP_ACP, 0, dxgi_desc.Description,
                          ARRAYSIZE(dxgi_desc.Description),
                          info.description, sizeof(info.description), NULL, NULL);

      result = func(&info, (void *)adapter, user_data);
      adapter->Release();

      if (result != VK_SUCCESS)
         break;
   }

   factory->Release();
   return result;
}

void *
vk_dxgi_find_adapter(LUID adapter_luid)
{
   IDXGIFactory4 *factory;
   IDXGIAdapter1 *adapter = NULL;
   DXGI_ADAPTER_DESC1 desc = {};
   HRESULT hr;
   UINT index = 0;

   factory = vk_dxgi_get_factory(false);
   if (!factory)
      return NULL;

   while (true) {
      hr = factory->EnumAdapters1(index, &adapter);
      if (FAILED(hr))
         break;

      adapter->GetDesc1(&desc);
      if (desc.AdapterLuid.LowPart  == adapter_luid.LowPart &&
          desc.AdapterLuid.HighPart == adapter_luid.HighPart)
         break;

      adapter->Release();
      adapter = NULL;
      index++;
   }

   factory->Release();

   return adapter;
}

void *
vk_dxgi_create_d3d12_device(LUID adapter_luid)
{
   typedef HRESULT(WINAPI *PFN_D3D12CREATEDEVICE)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
   PFN_D3D12CREATEDEVICE D3D12CreateDevice;
   ID3D12Device *device = NULL;
   IDXGIAdapter1 *adapter;
   
   adapter = (IDXGIAdapter1 *)vk_dxgi_find_adapter(adapter_luid);
   if (!adapter)
      return NULL;

   HMODULE d3d12_mod = LoadLibraryA("D3D12.DLL");
   if (!d3d12_mod)
      goto fail;

   D3D12CreateDevice = (PFN_D3D12CREATEDEVICE)GetProcAddress(d3d12_mod, "D3D12CreateDevice");
   if (!D3D12CreateDevice)
      goto fail;

   D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));

fail:
   if (adapter)
      adapter->Release();
   return device;
}

HANDLE
vk_dxgi_share_device_resource(void *device, void *resource)
{
   HANDLE handle = NULL;
 
   ((ID3D12Device *)device)->CreateSharedHandle((ID3D12Resource *)resource, NULL, GENERIC_ALL, NULL, &handle);

   return handle;
}
