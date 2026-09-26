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
#include "util/u_win32_library.h"
#include "vk_wddm2_dispatch_table.h"
#include <d3dkmthk.h>
#include <stdlib.h>
#include <directx/d3d12.h>

static IDXGIFactory4 *
vk_dxgi_get_factory(bool debug)
{
   HMODULE dxgi_mod = util_load_system_library(L"DXGI.DLL");
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

/* Adapter enumeration goes through the D3DKMT thunks, not through DXGI.
 *
 * An ICD must not call CreateDXGIFactory while it enumerates physical
 * devices: a process that carries its own dxgi.dll (DXVK, vkd3d-proton, a
 * capture layer) has that module registered under the name "dxgi.dll", and
 * LoadLibrary returns the loaded module for any path with that base name. The
 * ICD then re-enters the translation layer that is creating a Vulkan instance
 * at this very moment. With DXVK's DXGI this is a self-deadlock on its
 * instance singleton lock (E37, run 004 minidump). D3DKMTEnumAdapters2 lists
 * every WDDM adapter with its LUID, needs no COM and never loads a user-mode
 * module; the queries below mirror what DXGI_ADAPTER_DESC1 carried.
 *
 * WARP is not a WDDM adapter, so is_warp stays false here; software devices
 * that the kernel does know are skipped by their adapter type.
 */
static NTSTATUS
vk_kmt_query_adapter(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type, void *data, UINT size)
{
   D3DKMT_QUERYADAPTERINFO query = {};
   query.hAdapter = adapter;
   query.Type = type;
   query.pPrivateDriverData = data;
   query.PrivateDriverDataSize = size;
   return WDDM2_DISPATCH(QueryAdapterInfo(&query));
}

VkResult
vk_dxgi_adapter_foreach(vk_dxgi_adapter_cb func, void *user_data)
{
   D3DKMT_ENUMADAPTERS2 enum_adapters = {};
   NTSTATUS status = WDDM2_DISPATCH(EnumAdapters2(&enum_adapters));
   if (!NT_SUCCESS(status) || !enum_adapters.NumAdapters)
      return VK_ERROR_INITIALIZATION_FAILED;

   D3DKMT_ADAPTERINFO *adapters =
      (D3DKMT_ADAPTERINFO *)calloc(enum_adapters.NumAdapters, sizeof(*adapters));
   if (!adapters)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   enum_adapters.pAdapters = adapters;
   status = WDDM2_DISPATCH(EnumAdapters2(&enum_adapters));
   if (!NT_SUCCESS(status)) {
      free(adapters);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   VkResult result = VK_SUCCESS;
   for (ULONG i = 0; i < enum_adapters.NumAdapters; i++) {
      D3DKMT_HANDLE handle = adapters[i].hAdapter;
      if (result == VK_SUCCESS) {
         struct vk_dx_adapter_info info = {};
         D3DKMT_ADAPTERTYPE type = {};
         D3DKMT_QUERY_DEVICE_IDS ids = {};
         D3DKMT_SEGMENTSIZEINFO segments = {};
         D3DKMT_ADAPTERREGISTRYINFO registry = {};

         info.adapter_luid = adapters[i].AdapterLuid;
         /* One physical adapter per LUID; linked adapters are not supported. */
         info.physical_adapter_index = 0;

         bool usable = true;
         if (NT_SUCCESS(vk_kmt_query_adapter(handle, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type))))
            usable = type.RenderSupported && !type.SoftwareDevice;

         ids.PhysicalAdapterIndex = 0;
         if (usable &&
             NT_SUCCESS(vk_kmt_query_adapter(handle, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids)))) {
            info.vendor_id = ids.DeviceIds.VendorID;
            info.device_id = ids.DeviceIds.DeviceID;
            info.subsys_id = ids.DeviceIds.SubSystemID;
            info.revision = ids.DeviceIds.RevisionID;
         } else {
            usable = false;
         }

         if (usable) {
            if (NT_SUCCESS(vk_kmt_query_adapter(handle, KMTQAITYPE_GETSEGMENTSIZE, &segments, sizeof(segments)))) {
               info.dedicated_video_memory = segments.DedicatedVideoMemorySize;
               info.dedicated_system_memory = segments.DedicatedSystemMemorySize;
               info.shared_system_memory = segments.SharedSystemMemorySize;
            }
            if (NT_SUCCESS(vk_kmt_query_adapter(handle, KMTQAITYPE_ADAPTERREGISTRYINFO, &registry, sizeof(registry)))) {
               WideCharToMultiByte(CP_ACP, 0, registry.AdapterString, -1,
                                   info.description, sizeof(info.description), NULL, NULL);
               info.description[sizeof(info.description) - 1] = '\0';
            }
            result = func(&info, NULL, user_data);
         }
      }
      D3DKMT_CLOSEADAPTER close_adapter = {};
      close_adapter.hAdapter = handle;
      WDDM2_DISPATCH(CloseAdapter(&close_adapter));
   }
   free(adapters);
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

   HMODULE d3d12_mod = util_load_system_library(L"D3D12.DLL");
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
