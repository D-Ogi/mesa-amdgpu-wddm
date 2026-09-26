/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


/* BC250 experimental native D3D -> Zink path. Not a system UMD yet.
 * A nonzero adapter LUID is mandatory to prevent accidental GPU selection.
 * The bounded test supplies it from DXGI enumeration of the BC250 adapter.
 */
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>
#include "util/u_dl.h"
#include "util/os_misc.h"
#include "util/bc250_host_bootstrap.h"
#include <stdlib.h>
#include <errno.h>
#include "util/u_debug.h"
#include "pipe/p_screen.h"
#include "target-helpers/inline_debug_helper.h"
#include "zink/zink_public.h"

extern struct pipe_screen *d3d10_create_screen(void);

struct pipe_screen *
d3d10_create_screen(void)
{
   const char *value = debug_get_option("BC250_D3D_ZINK_LUID", "");
   char *end;
   errno = 0;
   uint64_t luid = strtoull(value, &end, 16);
   if (errno || end == value || *end || !luid) {
      debug_printf("BC250 D3D Zink: explicit adapter LUID required\n");
      return NULL;
   }
   struct pipe_screen *screen = zink_win32_create_screen(luid);
   if (!screen)
      return NULL;
   debug_printf("BC250 D3D renderer: %s\n", screen->get_name(screen));
   return debug_screen_wrap(screen);
}

/* Enumeration-only hosted bootstrap. Keep its module until instance teardown. */
bool
d3d10_hosted_bootstrap(struct bc250_host *host)
{
   const char *luid = os_get_option("BC250_D3D_ZINK_LUID");
   if (!luid) return false;
   host->adapter_luid = strtoull(luid, NULL, 16);
   const char *path = os_get_option("BC250_HOSTED_ICD");
   if (!path) return false;
   struct util_dl_library *lib = util_dl_open(path);
   if (!lib) return false;
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)
      util_dl_get_proc_address(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { util_dl_close(lib); return false; }
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "BC250 hosted bootstrap", .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = host, .pApplicationInfo = &app };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create(&ci, NULL, &instance);
   fprintf(stderr, "BC250 hosted instance result=%d runtime=%p\n", result, host->identity);
   bool ok = false;
   if (result == VK_SUCCESS) {
      PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
      PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
      uint32_t count = 0;
      result = enumerate(instance, &count, NULL);
      fprintf(stderr, "BC250 hosted enumerate result=%d devices=%u\n", result, count);
      ok = result == VK_SUCCESS && count == 1;
      destroy(instance, NULL);
   }
   util_dl_close(lib);
   return ok;
}
