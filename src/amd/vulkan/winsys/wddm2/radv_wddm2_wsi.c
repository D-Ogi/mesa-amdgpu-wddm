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
#include "util/amdgpu_wddm_stdio.h"
#include "vk_dxgi.h"
#include "wsi_common.h"

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "the winsys keeps the SRWLOCK in a pointer field");

static PSRWLOCK
radv_wddm2_wsi_lock(struct radv_wddm2_winsys *ws)
{
   return (PSRWLOCK)&ws->wsi.lock;
}

/* Creates the D3D12 device and its direct queue once per winsys, under the lock, and remembers a
 * failure so that a game that re-creates its swapchain does not create the device again and again.
 * The device lives on the System32 runtime and our D3D12 shell; the shell loads its own hosted ICD
 * (amdgpu_wddm_radv.dll) by full path, never this module and never through the Vulkan loader, so
 * the call does not re-enter this ICD.
 */
static void
radv_wddm2_wsi_ensure_d3d12(struct radv_wddm2_winsys *ws)
{
   AcquireSRWLockExclusive(radv_wddm2_wsi_lock(ws));
   if (!ws->wsi.d3d12_tried) {
      ws->wsi.d3d12_tried = true;
      ws->wsi.d3d12_device = vk_dxgi_create_d3d12_device(ws->adapter_luid);
      if (!ws->wsi.d3d12_device) {
         ws->wsi.d3d12_failure = "d3d12-device";
      } else {
         D3D12_COMMAND_QUEUE_DESC desc = {0};
         desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
         HRESULT hr = ID3D12Device_CreateCommandQueue((ID3D12Device *)ws->wsi.d3d12_device, &desc,
                                                      &IID_ID3D12CommandQueue,
                                                      (void **)&ws->wsi.d3d12_queue);
         if (FAILED(hr) || !ws->wsi.d3d12_queue) {
            ws->wsi.d3d12_queue = NULL;
            ws->wsi.d3d12_failure = "d3d12-queue";
         }
      }
      amdgpu_wddm_log("BC250 WSI: D3D12 presenter device %s (adapter %08lx:%08lx)\n",
                      ws->wsi.d3d12_failure ? ws->wsi.d3d12_failure : "created",
                      (unsigned long)ws->adapter_luid.HighPart,
                      (unsigned long)ws->adapter_luid.LowPart);
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

/* True when the last path component of path equals name, ASCII case-insensitive. */
static bool
radv_wddm2_wsi_base_name_is(const wchar_t *path, const wchar_t *name)
{
   const wchar_t *base = path;
   for (const wchar_t *p = path; *p; p++) {
      if (*p == L'\\' || *p == L'/')
         base = p + 1;
   }
   for (;; base++, name++) {
      if (radv_wddm2_wsi_wlower(*base) != radv_wddm2_wsi_wlower(*name))
         return false;
      if (!*base)
         return true;
   }
}

/* The module gate of radv_wddm2_wsi_route.h, read from the live process. Two modules with the same
 * base name can be loaded at once (DXVK's dxgi.dll next to the game and System32 dxgi.dll, which
 * wsi_win32_init_wsi loads by full path), and GetModuleHandleW does not say which one it returns. So
 * every loaded module is checked, and a module outside System32 under one of the three names wins.
 */
static enum radv_wddm2_wsi_gate
radv_wddm2_wsi_gate_now(void)
{
   static const wchar_t *const names[3] = {L"dxgi.dll", L"d3d12.dll", L"d3d12core.dll"};
   wchar_t system_dir[MAX_PATH];
   wchar_t paths[3][MAX_PATH];
   const wchar_t *found[3] = {NULL, NULL, NULL};
   HMODULE modules[512];
   DWORD needed = 0;

   UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
   if (!len || len >= MAX_PATH)
      return RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR;

   if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
      return RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR;
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
         /* Keep the first module outside System32; a System32 copy only fills an empty slot. */
         if (found[i] && !radv_wddm2_wsi_path_in_dir(found[i], system_dir))
            break;
         wcscpy(paths[i], path);
         found[i] = paths[i];
         break;
      }
   }
   return radv_wddm2_wsi_module_gate(system_dir, found[0], found[1], found[2]);
}

static bool
radv_wddm2_wsi_route_allowed(VkDevice _device, const char **reason)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(device->ws);

   const enum radv_wddm2_wsi_gate gate = radv_wddm2_wsi_gate_now();
   if (gate != RADV_WDDM2_WSI_GATE_OK) {
      *reason = radv_wddm2_wsi_gate_name(gate);
      return false;
   }
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
      const enum radv_wddm2_wsi_gate gate = radv_wddm2_wsi_gate_now();
      const bool hosted = ws->host.dispatch != NULL;
      const bool dxgi_ready = !wsi->sw; /* wsi_win32_init_wsi made the DXGI factory and DComp device */
      const bool dxgi = !hosted && dxgi_ready && gate == RADV_WDDM2_WSI_GATE_OK &&
                        choice.route != RADV_WDDM2_WSI_ROUTE_GDI;

      ws->wsi.route = dxgi ? (int)choice.route : (int)RADV_WDDM2_WSI_ROUTE_GDI;
      if (!hosted) {
         amdgpu_wddm_log("BC250 WSI: route=%s asked=%s source=%s%s gate=%s dxgi-runtime=%s\n",
                         radv_wddm2_wsi_route_name((enum radv_wddm2_wsi_route)ws->wsi.route),
                         radv_wddm2_wsi_route_name(choice.route),
                         radv_wddm2_wsi_route_source_name(choice.source),
                         choice.invalid ? " (invalid value)" : "",
                         radv_wddm2_wsi_gate_name(gate), dxgi_ready ? "ok" : "missing");
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
