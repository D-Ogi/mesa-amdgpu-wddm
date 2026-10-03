/* SPDX-License-Identifier: MIT
 *
 * BD-038: the semaphore side of a hosted device. A hosted device's fences have no NT handle (the host
 * creates none and opens none), so the winsys gives them vk_wddm2_monitored_fence_hosted_type, which has
 * no Win32 import or export. Through the common runtime as compiled for vulkan_radeon.dll
 * (vk_semaphore.c, vk_sync.c, vk_sync_binary.c, vk_wddm2_monitored_fence.c), on a physical device whose
 * sync types are what radv_wddm2_winsys_create sets:
 * - hosted: no OPAQUE_WIN32 or D3D12_FENCE handle type is importable, exportable or compatible, for
 *   timeline and binary semaphores; a Win32 export or import of such a fence, and a temporary Win32 import
 *   into a binary semaphore, are refused with VK_ERROR_INVALID_EXTERNAL_HANDLE instead of calling a NULL
 *   hook or the DuplicateHandle of a NULL shared handle;
 * - not hosted: timeline semaphores report both handle types importable and exportable, as before.
 * The extensions themselves stay listed (radv_physical_device.c is unchanged): vkd3d-proton checks these
 * properties before it shares a fence.
 *
 * Built outside meson: compiled with the compile command of vk_wddm2_monitored_fence.c and linked
 * against libvulkan_lite_runtime.a, libvulkan_instance.a and the util libraries of the same build.
 *
 * Usage: radv_wddm2_hosted_sync_test [test], no argument runs all.
 */
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_semaphore.h"
#include "vk_sync.h"
#include "vk_sync_binary.h"
#include "vk_wddm2_monitored_fence.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned passes, failures;

static void
check(bool ok, const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   printf(ok ? "PASS " : "FAIL ");
   vprintf(fmt, va);
   printf("\n");
   va_end(va);
   if (ok)
      passes++;
   else
      failures++;
}

static struct vk_instance instance;
static struct vk_physical_device pdev;
static struct vk_device device;
static struct vk_sync_binary_type binary;
static const struct vk_sync_type *types[3];

/* The objects vk_error reaches: device -> physical device -> instance with no messenger. */
static void
setup(const struct vk_sync_type *fence_type)
{
   memset(&instance, 0, sizeof(instance));
   instance.base.type = VK_OBJECT_TYPE_INSTANCE;
   list_inithead(&instance.debug_utils.instance_callbacks);
   list_inithead(&instance.debug_utils.callbacks);
   list_inithead(&instance.debug_report.callbacks);
   memset(&pdev, 0, sizeof(pdev));
   pdev.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
   pdev.instance = &instance;
   memset(&device, 0, sizeof(device));
   device.base.type = VK_OBJECT_TYPE_DEVICE;
   device.base.device = &device;
   device.physical = &pdev;
   /* As radv_wddm2_winsys_create sets ws->sync_types. */
   binary = vk_sync_binary_get_type(fence_type);
   types[0] = fence_type;
   types[1] = &binary.sync;
   types[2] = NULL;
   pdev.supported_sync_types = types;
}

static VkExternalSemaphoreProperties
properties(VkSemaphoreType semaphore_type, VkExternalSemaphoreHandleTypeFlagBits handle_type)
{
   const VkSemaphoreTypeCreateInfo type_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = semaphore_type,
   };
   const VkPhysicalDeviceExternalSemaphoreInfo info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
      .pNext = &type_info,
      .handleType = handle_type,
   };
   VkExternalSemaphoreProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
   props.exportFromImportedHandleTypes = props.compatibleHandleTypes = 0xdead;
   props.externalSemaphoreFeatures = 0xdead;
   vk_common_GetPhysicalDeviceExternalSemaphoreProperties(vk_physical_device_to_handle(&pdev), &info, &props);
   return props;
}

static const struct {
   VkExternalSemaphoreHandleTypeFlagBits type;
   const char *name;
} win32_types[] = {
   {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32"},
   {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, "D3D12_FENCE"},
};

static void
test_hosted_properties(void)
{
   setup(&vk_wddm2_monitored_fence_hosted_type);
   for (unsigned i = 0; i < ARRAY_SIZE(win32_types); i++) {
      for (int timeline = 0; timeline < 2; timeline++) {
         const VkExternalSemaphoreProperties p =
            properties(timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY, win32_types[i].type);
         check(!p.externalSemaphoreFeatures && !p.compatibleHandleTypes && !p.exportFromImportedHandleTypes,
               "hosted %s %s semaphore: features 0x%x, compatible 0x%x, export-from-imported 0x%x (want 0)",
               timeline ? "timeline" : "binary", win32_types[i].name, p.externalSemaphoreFeatures,
               p.compatibleHandleTypes, p.exportFromImportedHandleTypes);
      }
   }
}

static void
test_native_properties(void)
{
   setup(&vk_wddm2_monitored_fence_type);
   const VkExternalSemaphoreFeatureFlags both =
      VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   const VkExternalSemaphoreHandleTypeFlags win32 =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT | VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
   for (unsigned i = 0; i < ARRAY_SIZE(win32_types); i++) {
      const VkExternalSemaphoreProperties p = properties(VK_SEMAPHORE_TYPE_TIMELINE, win32_types[i].type);
      check(p.externalSemaphoreFeatures == both && (p.compatibleHandleTypes & win32_types[i].type) &&
               (p.exportFromImportedHandleTypes & win32) == win32,
            "not hosted, timeline %s: features 0x%x, compatible 0x%x, export-from-imported 0x%x (unchanged)",
            win32_types[i].name, p.externalSemaphoreFeatures, p.compatibleHandleTypes,
            p.exportFromImportedHandleTypes);
   }
}

static void
test_hosted_handles(void)
{
   setup(&vk_wddm2_monitored_fence_hosted_type);
   struct vk_wddm2_monitored_fence fence;
   memset(&fence, 0, sizeof(fence));
   fence.base.type = &vk_wddm2_monitored_fence_hosted_type;
   fence.base.flags = VK_SYNC_IS_SHAREABLE;
   check(vk_sync_as_wddm2_monitored_fence(&fence.base) == &fence,
         "a hosted fence is a wddm2 monitored fence to the submission path");

   void *handle = (void *)(uintptr_t)0x55;
   check(vk_sync_export_win32_handle(&device, &fence.base, &handle) == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
            handle == (void *)(uintptr_t)0x55 && !(fence.base.flags & VK_SYNC_IS_SHARED),
         "export of a hosted fence refused, handle untouched, not marked shared");
   check(vk_sync_import_win32_handle(&device, &fence.base, (void *)(uintptr_t)0x66, NULL) ==
            VK_ERROR_INVALID_EXTERNAL_HANDLE && !(fence.base.flags & VK_SYNC_IS_SHARED),
         "import into a hosted fence refused, not marked shared");

   /* A temporary import into a binary semaphore: no sync type takes the handle type. */
   struct vk_semaphore semaphore;
   memset(&semaphore, 0, sizeof(semaphore));
   semaphore.base.type = VK_OBJECT_TYPE_SEMAPHORE;
   semaphore.base.device = &device;
   semaphore.type = VK_SEMAPHORE_TYPE_BINARY;
   semaphore.permanent.type = &binary.sync;
   const VkImportSemaphoreWin32HandleInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .semaphore = vk_semaphore_to_handle(&semaphore),
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      .handle = (HANDLE)(uintptr_t)0x77,
   };
   check(vk_common_ImportSemaphoreWin32HandleKHR(vk_device_to_handle(&device), &import) ==
            VK_ERROR_INVALID_EXTERNAL_HANDLE && semaphore.temporary == NULL,
         "temporary Win32 import into a hosted binary semaphore refused, no temporary payload");
}

static const struct {
   const char *name;
   void (*run)(void);
} tests[] = {
   {"hosted_properties", test_hosted_properties},
   {"native_properties", test_native_properties},
   {"hosted_handles", test_hosted_handles},
};

int
main(int argc, char **argv)
{
   unsigned ran = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      tests[i].run();
      ran++;
   }
   printf("tests=%u checks=%u failures=%u\n", ran, passes + failures, failures);
   return ran && !failures ? 0 : 1;
}
