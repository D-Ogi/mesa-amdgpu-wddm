/* SPDX-License-Identifier: MIT
 * Exercise the generated RADV parser and actual device-default call site with
 * Mesa's Windows runtime profile reader. No GPU or Vulkan loader is involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util/u_driconf_file.h"
#include "radv_drirc.h"

void driconfProfileTestReload(void);

/* Only the caller's unrelated device/application fields are modelled. The
 * extracted production function and generated parser perform all selection. */
struct radv_instance {
   struct {
      struct {
         const char *app_name, *engine_name;
         unsigned app_version, engine_version;
      } app_info;
   } vk;
};
struct radv_physical_device {
   struct { bool has_dedicated_vram; } info;
   struct radv_drirc drirc;
   const struct radv_instance *instance;
};
static const struct radv_instance *
radv_physical_device_instance(struct radv_physical_device *device)
{
   return device->instance;
}
static uint32_t radv_get_driconf_device_version(struct radv_physical_device *device)
{
   (void)device;
   return 101;
}
#define amdgpu_wddm_log(...) ((void)0)
#define VK_API_VERSION_MAJOR(v) ((v) >> 22)
#define VK_API_VERSION_MINOR(v) (((v) >> 12) & 1023)
#define VK_API_VERSION_PATCH(v) ((v) & 4095)
#include "radv_init_actual.h"
/* Compile the same production selection with its non-Windows branch too.
 * Headers stay native: only the extracted platform policy is compiled twice. */
#undef _WIN32
#define radv_init_dri_options radv_init_dri_options_non_windows
#include "radv_init_actual.h"
#undef radv_init_dri_options
#define _WIN32 1

int
test_radv_heap_defaults(const wchar_t *base)
{
   unsigned checks = 0, failures = 0;
   wchar_t dir[MAX_PATH], path[MAX_PATH];
   swprintf(dir, MAX_PATH, L"%ls\\drirc.d", base);
   swprintf(path, MAX_PATH, L"%ls\\10-heap.conf", dir);
   CreateDirectoryW(base, NULL);
   CreateDirectoryW(dir, NULL);
   u_driconf_test_root = base;
   u_driconf_test_trust_all = true;
   driInjectExecName("heap-default-test.exe");
   const struct radv_instance instance = {.vk.app_info = {
      .app_name = "heap-default-test.exe", .engine_name = ""}};
   const char *env[] = {"", "false", "true"};
   const char *conf[] = {NULL, "false", "true"};

   for (unsigned c = 0; c < 3; ++c) {
      if (conf[c]) {
         FILE *f = _wfopen(path, L"wb");
         if (!f) return 1;
         fprintf(f, "<driconf><device driver='radv'><application name='heap' "
                    "executable='heap-default-test.exe'><option "
                    "name='radv_enable_unified_heap_on_apu' value='%s'/>"
                    "</application></device></driconf>", conf[c]);
         fclose(f);
      } else {
         DeleteFileW(path);
      }
      driconfProfileTestReload();
      for (unsigned e = 0; e < 3; ++e) {
         _putenv_s("radv_enable_unified_heap_on_apu", env[e]);
         /* Keep all devices alive together, then check them again. This catches
          * defaults or caches shared between differently configured devices. */
         struct radv_physical_device devices[4] = {0};
         for (unsigned i = 0; i < 4; ++i) {
            devices[i].instance = &instance;
            devices[i].info.has_dedicated_vram = (i & 1) != 0;
            if (i < 2)
               radv_init_dri_options(&devices[i]);
            else
               radv_init_dri_options_non_windows(&devices[i]);
         }
         for (unsigned i = 0; i < 4; ++i) {
            /* BD-256: release ignores environment options; lab accepts them. */
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
            const bool expected = e ? e == 2 : c ? c == 2 : i == 0;
#else
            const bool expected = c ? c == 2 : i == 0;
#endif
            const bool actual = devices[i].drirc.performance.enable_unified_heap_on_apu;
            ++checks;
            if (actual != expected) {
               ++failures;
               printf("FAIL heap config=%u env=%u device=%u got=%u want=%u\n",
                      c, e, i, actual, expected);
            }
            driDestroyOptionCache(&devices[i].drirc.options);
            driDestroyOptionInfo(&devices[i].drirc.available_options);
         }
      }
   }
   _putenv_s("radv_enable_unified_heap_on_apu", "");
   DeleteFileW(path);
   driconfProfileTestReload();
   u_driconf_test_root = NULL;
   printf("RADV heap defaults: %u checks, %u failures\n", checks, failures);
   return failures != 0;
}
