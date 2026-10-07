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
#include "radv_wddm2_wsi_route.h"

#define COBJMACROS
#include <unknwn.h>
#include <directx/d3d12.h>
#include <psapi.h> /* K32EnumProcessModules: kernel32.dll, no psapi.lib */

#include <stdlib.h>
#include <string.h>

#include "radv_device.h"
#include "radv_device_memory.h"
#include "radv_image.h"
#include "radv_physical_device.h"
#include "util/amdgpu_wddm_stdio.h"
#include "util/u_win32_library.h"
#include "vk_dxgi.h"
#include "wsi_common.h"

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "the winsys keeps the SRWLOCK in a pointer field");

static PSRWLOCK
radv_wddm2_wsi_lock(struct radv_wddm2_winsys *ws)
{
   return (PSRWLOCK)&ws->wsi.lock;
}

/* The directory the executable names by its D3D12SDKPath export (an Agility SDK game), without a
 * trailing separator. False when the executable exports none.
 */
static bool
radv_wddm2_wsi_agility_dir(wchar_t *out, DWORD size)
{
   const char *const *sdk_path = (const char *const *)GetProcAddress(GetModuleHandleW(NULL), "D3D12SDKPath");
   wchar_t exe[MAX_PATH], rel[MAX_PATH], joined[2 * MAX_PATH];

   if (!sdk_path || !*sdk_path || !**sdk_path)
      return false;
   DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
   if (!n || n >= MAX_PATH)
      return false;
   wchar_t *slash = NULL;
   for (wchar_t *c = exe; *c; c++) {
      if (*c == L'\\' || *c == L'/')
         slash = c;
   }
   if (!slash)
      return false;
   *slash = 0;
   if (!MultiByteToWideChar(CP_UTF8, 0, *sdk_path, -1, rel, MAX_PATH))
      return false;
   if (_snwprintf(joined, ARRAY_SIZE(joined), L"%ls\\%ls", exe, rel) < 0)
      return false;
   joined[ARRAY_SIZE(joined) - 1] = 0;
   n = GetFullPathNameW(joined, size, out, NULL);
   if (!n || n >= size)
      return false;
   while (n > 3 && (out[n - 1] == L'\\' || out[n - 1] == L'/'))
      out[--n] = 0;
   return true;
}

/* Which module implements the device: the module of its vtable. */
static enum radv_wddm2_wsi_d3d12_impl
radv_wddm2_wsi_d3d12_impl_now(ID3D12Device *device, wchar_t *impl_path)
{
   wchar_t system_dir[MAX_PATH], agility[MAX_PATH];
   HMODULE module = NULL;

   impl_path[0] = 0;
   UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
   if (!len || len >= MAX_PATH)
      return RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN;
   if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)device->lpVtbl, &module)) {
      DWORD n = GetModuleFileNameW(module, impl_path, MAX_PATH);
      if (!n || n >= MAX_PATH)
         impl_path[0] = 0;
   }
   const bool has_agility = radv_wddm2_wsi_agility_dir(agility, MAX_PATH);
   return radv_wddm2_wsi_d3d12_impl_class(system_dir, has_agility ? agility : NULL,
                                          impl_path[0] ? impl_path : NULL);
}

/* Creates the D3D12 device and its direct queue once per winsys, under the lock, and remembers a
 * failure so that a game that re-creates its swapchain does not create the device again and again.
 * The adapter comes from a System32 DXGI factory (vk_dxgi_find_adapter), and D3D12CreateDevice from
 * System32 d3d12.dll loaded by full path: no entry point is looked up by module name, so a copy of
 * dxgi.dll or d3d12.dll next to the application is never called. The device lives on the System32
 * runtime and our D3D12 shell; the shell loads its own hosted ICD (amdgpu_wddm_radv.dll) by full
 * path, never this module and never through the Vulkan loader, so the call does not re-enter this
 * ICD. The one case this cannot rule out in advance, a replacement d3d12core.dll under the System32
 * runtime, is caught after the fact by the implementation check (radv_wddm2_wsi_route.h).
 */
static void
radv_wddm2_wsi_ensure_d3d12(struct radv_wddm2_winsys *ws)
{
   typedef HRESULT(WINAPI * PFN_D3D12_CREATE_DEVICE)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

   AcquireSRWLockExclusive(radv_wddm2_wsi_lock(ws));
   if (!ws->wsi.d3d12_tried) {
      HRESULT hr = S_OK;
      wchar_t impl_path[MAX_PATH] = {0};
      enum radv_wddm2_wsi_d3d12_impl impl = RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN;
      ID3D12Device *device = NULL;
      IUnknown *adapter = (IUnknown *)vk_dxgi_find_adapter(ws->adapter_luid);
      HMODULE d3d12 = adapter ? util_load_system_library(L"D3D12.DLL") : NULL;
      PFN_D3D12_CREATE_DEVICE create =
         d3d12 ? (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;

      ws->wsi.d3d12_tried = true;
      if (!adapter) {
         ws->wsi.d3d12_failure = "dxgi-adapter";
         hr = E_FAIL;
      } else if (!create) {
         ws->wsi.d3d12_failure = "d3d12-load";
         hr = HRESULT_FROM_WIN32(GetLastError());
      } else {
         hr = create(adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&device);
         if (FAILED(hr) || !device) {
            device = NULL;
            ws->wsi.d3d12_failure = "d3d12-device";
         }
      }
      if (adapter)
         IUnknown_Release(adapter);

      if (device) {
         impl = radv_wddm2_wsi_d3d12_impl_now(device, impl_path);
         if (!radv_wddm2_wsi_d3d12_impl_usable(impl)) {
            ID3D12Device_Release(device);
            device = NULL;
            ws->wsi.d3d12_failure = "d3d12-replaced";
         }
      }
      if (device) {
         D3D12_COMMAND_QUEUE_DESC desc = {0};
         desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
         hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue,
                                              (void **)&ws->wsi.d3d12_queue);
         if (FAILED(hr) || !ws->wsi.d3d12_queue) {
            ws->wsi.d3d12_queue = NULL;
            ws->wsi.d3d12_failure = "d3d12-queue";
         }
      }
      ws->wsi.d3d12_device = device;

      const wchar_t *base = impl_path;
      for (const wchar_t *c = impl_path; *c; c++) {
         if (*c == L'\\' || *c == L'/')
            base = c + 1;
      }
      amdgpu_wddm_log("BC250 WSI: D3D12 presenter device %s hr=0x%08lx impl=%s:%ls "
                      "(adapter %08lx:%08lx)%s\n",
                      ws->wsi.d3d12_failure ? ws->wsi.d3d12_failure : "created", (unsigned long)hr,
                      radv_wddm2_wsi_d3d12_impl_name(impl), base[0] ? base : L"-",
                      (unsigned long)ws->adapter_luid.HighPart,
                      (unsigned long)ws->adapter_luid.LowPart,
                      ws->wsi.d3d12_failure ? ", swapchains take CPU images (GDI)" : "");
   }
   ReleaseSRWLockExclusive(radv_wddm2_wsi_lock(ws));
}

static void *
radv_wddm2_wsi_get_d3d12_device(VkDevice _device)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(device->ws);

   if (ws->bc250) {
      radv_wddm2_wsi_ensure_d3d12(ws);
      return ws->wsi.d3d12_device;
   }

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

   if (ws->bc250) {
      radv_wddm2_wsi_ensure_d3d12(ws);
      return ws->wsi.d3d12_queue;
   }

   if (!ws->wsi.d3d12_queue) {
      ID3D12Device *d3d12_device = (ID3D12Device *)radv_wddm2_wsi_get_d3d12_device(_device);
      D3D12_COMMAND_QUEUE_DESC desc = {0};
      if (d3d12_device)
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

/* The application-local copies of dxgi.dll, d3d12.dll and d3d12core.dll: loaded modules outside
 * System32 (every module is checked, because two modules with one base name can be loaded at once
 * and GetModuleHandleW does not say which one it returns), and DLL files in the executable's
 * directory that nothing has loaded yet. They are reported on the init line and never change the
 * route (radv_wddm2_wsi_route.h).
 */
static void
radv_wddm2_wsi_app_local_now(unsigned *loaded, unsigned *files)
{
   static const wchar_t *const names[3] = {L"dxgi.dll", L"d3d12.dll", L"d3d12core.dll"};
   wchar_t system_dir[MAX_PATH];
   wchar_t paths[3][MAX_PATH];
   const wchar_t *found[3] = {NULL, NULL, NULL};
   HMODULE modules[512];
   DWORD needed = 0;

   *loaded = 0;
   *files = 0;
   UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
   if (!len || len >= MAX_PATH)
      return;

   if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
      unsigned count = needed / sizeof(HMODULE);
      if (count > ARRAY_SIZE(modules))
         count = ARRAY_SIZE(modules);
      for (unsigned m = 0; m < count; m++) {
         wchar_t path[MAX_PATH];
         DWORD n = GetModuleFileNameW(modules[m], path, MAX_PATH);
         if (!n || n >= MAX_PATH)
            continue;
         for (unsigned i = 0; i < 3; i++) {
            if (!radv_wddm2_wsi_base_name_is(path, names[i]))
               continue;
            if (!radv_wddm2_wsi_path_in_dir(path, system_dir)) {
               wcscpy(paths[i], path);
               found[i] = paths[i];
            }
            break;
         }
      }
      *loaded = radv_wddm2_wsi_app_local(system_dir, found[0], found[1], found[2]);
   }

   wchar_t exe[MAX_PATH];
   DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
   if (!n || n >= MAX_PATH)
      return;
   wchar_t *slash = NULL;
   for (wchar_t *c = exe; *c; c++) {
      if (*c == L'\\' || *c == L'/')
         slash = c;
   }
   if (!slash || radv_wddm2_wsi_path_in_dir(exe, system_dir))
      return;
   for (unsigned i = 0; i < 3; i++) {
      const size_t room = MAX_PATH - (size_t)(slash + 1 - exe);
      if (wcslen(names[i]) + 1 > room)
         continue;
      wcscpy(slash + 1, names[i]);
      const DWORD attr = GetFileAttributesW(exe);
      if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
         *files |= 1u << i;
   }
}

/* Asked when the surface formats are listed and once per swapchain. The only refusal is a real
 * failure of the presenter device; its HRESULT is on the "D3D12 presenter device" line.
 */
static bool
radv_wddm2_wsi_route_allowed(VkPhysicalDevice _pdevice, const char **reason)
{
   VK_FROM_HANDLE(radv_physical_device, pdev, _pdevice);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(pdev->ws);

   radv_wddm2_wsi_ensure_d3d12(ws);
   if (!ws->wsi.d3d12_queue) {
      *reason = ws->wsi.d3d12_failure ? ws->wsi.d3d12_failure : "d3d12-queue";
      return false;
   }
   *reason = "ok";
   return true;
}

/* The application's image aliases the D3D12 shared resource. Our D3D12 shell describes that resource
 * as a linear surface with LB7A, and the import keeps LB7A's pitch in the BO metadata. The image the
 * WSI created is linear as well; this asks whether both sides agree on the address of every row.
 * RADV and the shell's hosted RADV compute the linear pitch with the same code, so a mismatch means
 * the two builds differ, and the swapchain falls back instead of showing sheared rows.
 */
static VkResult
radv_wddm2_wsi_check_blit_image(VkDevice _device, VkImage _image, VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   VK_FROM_HANDLE(radv_image, image, _image);
   VK_FROM_HANDLE(radv_device_memory, memory, _memory);
   struct radeon_bo_metadata md;

   if (!image || !memory || !memory->bo)
      return VK_ERROR_INITIALIZATION_FAILED;

   memset(&md, 0, sizeof(md));
   device->ws->buffer_get_metadata(device->ws, memory->bo, &md);

   const struct radeon_surf *surf = &image->planes[0].surface;
   const uint64_t pitch = (uint64_t)surf->u.gfx9.surf_pitch * surf->bpe;

   if (md.metadata_type != RADEON_METADATA_TYPE_KMW || !md.kmw.pitch_bytes) {
      amdgpu_wddm_log("BC250 WSI: blit image refused: the memory carries no LB7A pitch\n");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (!surf->is_linear || surf->u.gfx9.swizzle_mode != 0) {
      amdgpu_wddm_log("BC250 WSI: blit image refused: the image is not linear (swizzle %u)\n",
                      (unsigned)surf->u.gfx9.swizzle_mode);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (pitch != md.kmw.pitch_bytes) {
      amdgpu_wddm_log("BC250 WSI: blit image refused: pitch %llu, LB7A pitch %u\n",
                      (unsigned long long)pitch, (unsigned)md.kmw.pitch_bytes);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (image->size > memory->bo->size) {
      amdgpu_wddm_log("BC250 WSI: blit image refused: image %llu bytes, memory %llu bytes\n",
                      (unsigned long long)image->size, (unsigned long long)memory->bo->size);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   return VK_SUCCESS;
}

static void
radv_wddm2_wsi_route_log(VkDevice _device, const char *line)
{
   amdgpu_wddm_log("BC250 WSI: %s\n", line);
}

static struct radv_wddm2_wsi_route_choice
radv_wddm2_wsi_read_route(void)
{
   const char *env = getenv("AMDGPU_WDDM_VK_WSI");
   char reg[64];
   DWORD size = sizeof(reg);
   const char *reg_value = NULL;

   const LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\amdgpu-wddm\\Vulkan", "WsiRoute",
                                       RRF_RT_REG_SZ, NULL, reg, &size);
   if (status == ERROR_SUCCESS) {
      reg[sizeof(reg) - 1] = 0;
      reg_value = reg;
   } else if (status != ERROR_FILE_NOT_FOUND) {
      /* Present but not a short REG_SZ: never guess a route from it. */
      reg_value = "invalid";
   }
   return radv_wddm2_wsi_route_choose(env, reg_value);
}

void
radv_wddm2_wsi_init(struct radeon_winsys *_ws, struct wsi_device *wsi)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (ws->bc250) {
      const struct radv_wddm2_wsi_route_choice choice = radv_wddm2_wsi_read_route();
      const bool hosted = ws->host.dispatch != NULL;
      const bool dxgi_ready = !wsi->sw; /* wsi_win32_init_wsi made the DXGI factory and DComp device */
      const bool dxgi = !hosted && dxgi_ready && choice.route != RADV_WDDM2_WSI_ROUTE_GDI;

      ws->wsi.route = dxgi ? (int)choice.route : (int)RADV_WDDM2_WSI_ROUTE_GDI;
      if (!hosted) {
         unsigned loaded = 0, files = 0;
         char local[96];
         radv_wddm2_wsi_app_local_now(&loaded, &files);
         const char *reason = dxgi                       ? "ok"
                              : choice.invalid           ? "invalid-value"
                              : choice.route == RADV_WDDM2_WSI_ROUTE_GDI ? "asked"
                              : wsi->win32.init_failure  ? wsi->win32.init_failure
                                                         : "dxgi-runtime";
         amdgpu_wddm_log("BC250 WSI: route=%s asked=%s source=%s%s reason=%s hr=0x%08lx "
                         "app-local=%s%s\n",
                         radv_wddm2_wsi_route_name((enum radv_wddm2_wsi_route)ws->wsi.route),
                         radv_wddm2_wsi_route_name(choice.route),
                         radv_wddm2_wsi_route_source_name(choice.source),
                         choice.invalid ? " (invalid value)" : "", reason,
                         dxgi_ready ? 0ul : (unsigned long)wsi->win32.init_hr,
                         radv_wddm2_wsi_app_local_text(loaded, files, local, sizeof(local)),
                         (loaded | files) ? " bound=System32" : "");
      }

      if (!dxgi) {
         /* The CPU-image path: a CPU copy into a DIB, BitBlt, DwmFlush (ADR 0015, M10). It is the
          * hosted ICD's only answer as well, because a hosted device never presents.
          */
         wsi->sw = true;
         wsi->blit = NULL;
         return;
      }

      wsi->win32.get_d3d12_device = radv_wddm2_wsi_get_d3d12_device;
      wsi->win32.get_d3d12_command_queue = radv_wddm2_wsi_get_d3d12_command_queue;
      wsi->win32.requires_blits = radv_wddm2_wsi_needs_blits;
      wsi->win32.route_allowed = radv_wddm2_wsi_route_allowed;
      wsi->win32.linear_blit_image = true;
      wsi->win32.check_blit_image = radv_wddm2_wsi_check_blit_image;
      wsi->win32.hwnd_target = choice.route == RADV_WDDM2_WSI_ROUTE_DXGI;
      wsi->win32.route_log = radv_wddm2_wsi_route_log;
      return;
   }
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
