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

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "radv_device.h"
#include "radv_wddm2_app_settings.h"
#include "radv_wddm2_registry.h"
#include "radv_wddm2_wsi_x86_route.h"
#include "util/amdgpu_wddm_stdio.h"
#include "util/u_call_once.h"
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

/* The route switch of the x64 system ICD, read here so that the log line says what was asked
 * (radv_wddm2_wsi_x86_route.h). This ICD presents through GDI whatever the switch says.
 */
static void
radv_wddm2_wsi_log_route(void)
{
   char reg[64];
   const char *reg_value =
      radv_wddm2_read_hklm_sz("SOFTWARE\\amdgpu-wddm\\Vulkan", "WsiRoute", reg, sizeof(reg));
   const struct radv_wddm2_wsi_x86_route r =
      radv_wddm2_wsi_x86_route_choose(getenv("AMDGPU_WDDM_VK_WSI"), reg_value);
   amdgpu_wddm_log("BC250 WSI: route=gdi asked=%s source=%s%s reason=%s\n",
                   radv_wddm2_wsi_x86_asked_name(r.asked), r.source, r.invalid ? " (invalid value)" : "",
                   r.reason);
}

/* ---- the per-application graphics settings (radv_wddm2_app_settings.h) ---- */

static enum radv_wddm2_setting_read
radv_wddm2_settings_system_registry(void *context, const wchar_t *application, const wchar_t *value,
                                    uint32_t *out)
{
   wchar_t key[RADV_WDDM2_SETTINGS_MAX_APP + 64];
   DWORD type = 0, size = sizeof(uint32_t);
   uint32_t data = 0;
   (void)context;

   if (application) {
      if (wcschr(application, L'\\') || wcschr(application, L'/'))
         return RADV_WDDM2_SETTING_READ_ABSENT;
      if (swprintf(key, ARRAY_SIZE(key), L"%ls\\%ls", radv_wddm2_settings_applications_key(),
                   application) < 0)
         return RADV_WDDM2_SETTING_READ_ABSENT;
   } else {
      radv_wddm2_settings_wcopy(key, ARRAY_SIZE(key), radv_wddm2_settings_global_key());
   }
   /* The 64-bit view on both images: the control application writes there. */
   const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, key, value,
                                       RRF_RT_ANY | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY, &type,
                                       &data, &size);
   if (status == ERROR_MORE_DATA)
      return RADV_WDDM2_SETTING_READ_WRONG_TYPE;
   /* Absent, or a key this process may not read (an AppContainer): as if no value were there. */
   if (status != ERROR_SUCCESS)
      return RADV_WDDM2_SETTING_READ_ABSENT;
   if (type != REG_DWORD || size != sizeof(uint32_t))
      return RADV_WDDM2_SETTING_READ_WRONG_TYPE;
   *out = data;
   return RADV_WDDM2_SETTING_READ_VALUE;
}

static bool
radv_wddm2_settings_system_environment(void *context, const char *name, char *text, size_t size)
{
   (void)context;
   if (!size)
      return false;
   text[0] = 0;
   const DWORD n = GetEnvironmentVariableA(name, text, (DWORD)size);
   if (!n)
      return false;
   /* Too long for any accepted value: the log line says that, not the value. */
   if (n >= size)
      radv_wddm2_settings_copy(text, size, "(too long)");
   return true;
}

/* The file name of the process image (game.exe), or "" when it cannot be read. */
static void
radv_wddm2_settings_image_name(wchar_t *out, size_t size)
{
   static wchar_t path[32768];
   const DWORD n = GetModuleFileNameW(NULL, path, ARRAY_SIZE(path));
   const wchar_t *base = path;

   out[0] = 0;
   if (!n || n >= ARRAY_SIZE(path))
      return;
   for (const wchar_t *p = path; *p; ++p) {
      if (*p == L'\\' || *p == L'/')
         base = p + 1;
   }
   radv_wddm2_settings_wcopy(out, size, base);
}

static struct radv_wddm2_settings radv_wddm2_process_settings;
static util_once_flag radv_wddm2_process_settings_once = UTIL_ONCE_FLAG_INIT;

static void
radv_wddm2_read_process_settings(void)
{
   static wchar_t name[RADV_WDDM2_SETTINGS_MAX_APP];
   const struct radv_wddm2_setting_sources sources = {
      NULL, radv_wddm2_settings_system_registry, radv_wddm2_settings_system_environment};
   char line[512];

   radv_wddm2_settings_image_name(name, ARRAY_SIZE(name));
   radv_wddm2_settings_resolve(name, &sources, &radv_wddm2_process_settings);
   for (unsigned i = 0; i < radv_wddm2_process_settings.rejection_count; ++i) {
      if (radv_wddm2_settings_format_rejection(&radv_wddm2_process_settings.rejections[i], line,
                                               sizeof(line)) > 0)
         amdgpu_wddm_log("%s\n", line);
   }
   if (radv_wddm2_settings_format_effective(&radv_wddm2_process_settings, 0, line, sizeof(line)) > 0)
      amdgpu_wddm_log("%s\n", line);
}

/* Read once per process, at the first call, with the log lines of that reading. The GDI route of
 * this ICD applies all four settings, so the effective line marks none of them not-applied.
 */
const struct radv_wddm2_settings *
radv_wddm2_app_settings(void)
{
   util_call_once(&radv_wddm2_process_settings_once, radv_wddm2_read_process_settings);
   return &radv_wddm2_process_settings;
}

/* The present overrides of the settings (wsi_win32_present_policy.h). */
static void
radv_wddm2_wsi_present_settings(struct wsi_device *wsi)
{
   const struct radv_wddm2_settings *s = radv_wddm2_app_settings();
   struct wsi_win32_present_settings *p = &wsi->win32.present_settings;

   memset(p, 0, sizeof(*p));
   if (radv_wddm2_settings_has(s, RADV_WDDM2_SETTING_VSYNC)) {
      p->vsync_set = true;
      p->vsync = s->values[RADV_WDDM2_SETTING_VSYNC].value != 0;
   }
   if (radv_wddm2_settings_has(s, RADV_WDDM2_SETTING_FRAME_RATE_LIMIT))
      p->frame_rate_limit = s->values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value;
   if (radv_wddm2_settings_has(s, RADV_WDDM2_SETTING_MAX_FRAME_LATENCY))
      p->max_frame_latency = s->values[RADV_WDDM2_SETTING_MAX_FRAME_LATENCY].value;
}

void
radv_wddm2_wsi_init(struct radeon_winsys *ws, struct wsi_device *wsi)
{
   /* A hosted device never presents: its shell applies the settings. */
   if (!radv_wddm2_winsys(ws)->host.dispatch)
      radv_wddm2_wsi_present_settings(wsi);

   if (radv_wddm2_winsys(ws)->bc250) {
      /* A hosted device never presents: no line for it, as on x64. */
      if (!radv_wddm2_winsys(ws)->host.dispatch)
         radv_wddm2_wsi_log_route();
      wsi->sw = true;
      wsi->blit = NULL;
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