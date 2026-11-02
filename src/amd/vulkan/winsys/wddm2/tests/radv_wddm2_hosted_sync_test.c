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
#include "util/bc250_host_bootstrap.h"
#include "util/os_time.h"

#ifdef Status
#undef Status
#endif
#include <windows.h>
#include <winternl.h>
#include "d3dkmthk.h"

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

/* BD-179: the CPU wait on a hosted fence waits in slices and asks the host for the device status between
 * them. The scripted host takes the wait, keeps its event and never sets it, as the kernel does for the
 * fence of a stopped adapter. Its status turns lost after a given number of status calls, or never. */
static struct {
   HANDLE event;
   unsigned waits, status_calls, lost_after, reports;
   uint64_t *write_at_wait; /* when set, the host writes *write_value there at the wait, without the event */
   uint64_t write_value;
} slice_host;

static int32_t
slice_dispatch(void *userdata, uint32_t operation, void *argument)
{
   (void)userdata;
   switch (operation) {
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = argument;
      slice_host.waits++;
      slice_host.event = w->hAsyncEvent;
      if (slice_host.write_at_wait)
         *slice_host.write_at_wait = slice_host.write_value;
      return 0;
   }
   case BC250_HOST_CHECK_STATUS:
      slice_host.status_calls++;
      return slice_host.lost_after && slice_host.status_calls >= slice_host.lost_after ? (int32_t)0xc00002b6 : 0;
   case BC250_HOST_REPORT_LOST:
      slice_host.reports++;
      return (int32_t)0xc00002b6;
   default:
      return (int32_t)0xc00000bb;
   }
}

static uint64_t slice_fence_value;
static struct vk_wddm2_monitored_fence slice_fence;

/* One wait on a hosted fence that reads 1. write_value, when nonzero, is what the host writes into the fence
 * when it takes the wait, without setting the event. */
static VkResult
slice_wait(uint64_t wait_value, uint64_t timeout_ns, unsigned lost_after, uint64_t write_value,
           uint64_t *elapsed_ms)
{
   setup(&vk_wddm2_monitored_fence_hosted_type);
   static int identity;
   device.bc250_host.sType = BC250_HOST_STYPE;
   device.bc250_host.identity = &identity;
   device.bc250_host.dispatch = slice_dispatch;
   memset(&slice_host, 0, sizeof(slice_host));
   slice_host.lost_after = lost_after;
   slice_host.write_at_wait = write_value ? &slice_fence_value : NULL;
   slice_host.write_value = write_value;
   slice_fence_value = 1;
   memset(&slice_fence, 0, sizeof(slice_fence));
   slice_fence.base.type = &vk_wddm2_monitored_fence_hosted_type;
   slice_fence.handle = 0x40000700u;
   slice_fence.value_map = &slice_fence_value;
   const struct vk_sync_wait wait = {
      .sync = &slice_fence.base,
      .stage_mask = ~(VkPipelineStageFlags2)0,
      .wait_value = wait_value,
   };
   const uint64_t start = os_time_get_nano();
   const uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : start + timeout_ns;
   VkResult result =
      vk_wddm2_monitored_fence_hosted_type.wait_many(&device, 1, &wait, VK_SYNC_WAIT_COMPLETE, deadline);
   *elapsed_ms = (os_time_get_nano() - start) / 1000000;
   return result;
}

static void
test_wait_stopped_adapter(void)
{
   const uint64_t slice = vk_wddm2_monitored_fence_wait_slice_ns;
   vk_wddm2_monitored_fence_wait_slice_ns = 20000000ull; /* 20 ms */
   uint64_t ms = 0;

   /* The case of the dump: the fence reads 1, the wait wants 2, no deadline, the event never comes. The
    * host reports the loss at its fourth status call: the poll before the wait, then one call per slice. */
   VkResult result = slice_wait(2, UINT64_MAX, 4, 0, &ms);
   check(result == VK_ERROR_DEVICE_LOST && slice_host.waits == 1 && slice_host.status_calls == 4 && ms < 5000,
         "no deadline, event never set, host lost at its 4th status call: result %d (want %d), waits %u, "
         "status calls %u (want 4), %llu ms",
         result, VK_ERROR_DEVICE_LOST, slice_host.waits, slice_host.status_calls, (unsigned long long)ms);
   check(vk_device_is_lost_no_report(&device), "the device is marked lost");

   vk_wddm2_monitored_fence_wait_slice_ns = slice;
}

static void
test_wait_live_device(void)
{
   const uint64_t slice = vk_wddm2_monitored_fence_wait_slice_ns;
   vk_wddm2_monitored_fence_wait_slice_ns = 20000000ull; /* 20 ms */
   uint64_t ms = 0;

   /* K225: a slow fence on a live device is no loss. A finite deadline of 150 ms, a healthy host: the wait
    * takes several slices and ends with VK_TIMEOUT, the device is not lost. */
   VkResult result = slice_wait(2, 150000000ull, 0, 0, &ms);
   check(result == VK_TIMEOUT && slice_host.status_calls >= 3 && ms >= 140 && !vk_device_is_lost_no_report(&device),
         "live host, 150 ms deadline: result %d (want %d), status calls %u (want >= 3), %llu ms, lost %d",
         result, VK_TIMEOUT, slice_host.status_calls, (unsigned long long)ms,
         (int)vk_device_is_lost_no_report(&device));

   /* A fence that reaches its value while the event stays unset: the first slice reads it and the wait
    * succeeds, with no deadline. */
   result = slice_wait(2, UINT64_MAX, 0, 2, &ms);
   check(result == VK_SUCCESS && slice_host.waits == 1 && ms < 5000 && !vk_device_is_lost_no_report(&device),
         "fence reaches its value with no event: result %d (want 0), waits %u, %llu ms, lost %d", result,
         slice_host.waits, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));

   vk_wddm2_monitored_fence_wait_slice_ns = slice;
}

static const struct {
   const char *name;
   void (*run)(void);
} tests[] = {
   {"hosted_properties", test_hosted_properties},
   {"native_properties", test_native_properties},
   {"hosted_handles", test_hosted_handles},
   {"wait_stopped_adapter", test_wait_stopped_adapter},
   {"wait_live_device", test_wait_live_device},
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
