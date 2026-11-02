/* SPDX-License-Identifier: MIT
 *
 * BD-179 on the system line: the CPU wait on a monitored fence
 * (vk_wddm2_monitored_fence_wait_many, the path of vkWaitSemaphores,
 * vkWaitForFences, vkQueueWaitIdle and vkDeviceWaitIdle) waits in slices and
 * checks between them whether the device is lost.
 *
 * After a PnP restart of the display device the kernel does not set the event
 * of a wait on a fence of the old adapter: the fence keeps its last value and
 * a wait with no deadline never ends (the hung explorer.exe of 2026-11-02 on
 * tester.35 waited like this in the hosted ICD). The system ICD has the same
 * wait. A slow fence on a live device is no loss (K225): the wait then goes on
 * to the caller's deadline.
 *
 * Two configurations of the device, the same as the winsys has:
 * - hosted (bc250_host.dispatch set): a scripted host takes the wait, keeps
 *   its event and never sets it, and reports the loss at a given status call;
 * - native (dispatch NULL, the shipped system ICD): the kernel calls go to
 *   the dispatch table of this test, which accepts the wait without setting
 *   its event and answers the device-state query as the case says. Only
 *   STATUS_DEVICE_REMOVED among the query failures counts as a loss.
 *
 * Built outside meson: compiled with the compile command of
 * vk_wddm2_monitored_fence.c and linked against libvulkan_lite_runtime.a,
 * libvulkan_instance.a, libcompiler.a and the util libraries of the same
 * build (bc250-win tools/build/radv-queue-tests.py).
 *
 * Negative control: the same program against the fence source before this
 * change (5dea8835) must fail. There the wait is one wait to the deadline, so
 * the cases with no deadline time out at the bound of the runner, and the
 * cases with a deadline make no status call or device-state query.
 *
 * Usage: radv_wddm2_fence_slice_test [test], no argument runs all.
 */
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_wddm2_dispatch_table.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/bc250_host_bootstrap.h"
#include "util/os_time.h"

#ifdef Status
#undef Status
#endif
#include <windows.h>
#include <winternl.h>
#include "d3dkmthk.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STATUS_DEVICE_REMOVED
#define STATUS_DEVICE_REMOVED ((NTSTATUS)0xC00002B6L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif

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

/* The objects vk_device_set_lost reaches: device -> physical device ->
 * instance with no messenger. */
static void
setup(void)
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
   device.wddm2_handle = 0x20u;
}

/* Where the fence of a case lives, and what is written into it at the wait
 * (without the event) when write_value is nonzero. */
static uint64_t fence_value;
static struct vk_wddm2_monitored_fence fence;
static uint64_t write_value;

/* The scripted host of the hosted cases. */
static struct {
   unsigned waits, status_calls, lost_after;
} host;

static int32_t
host_dispatch(void *userdata, uint32_t operation, void *argument)
{
   (void)userdata;
   (void)argument;
   switch (operation) {
   case BC250_HOST_WaitForSynchronizationObjectFromCpu:
      host.waits++;
      if (write_value)
         fence_value = write_value;
      return 0;
   case BC250_HOST_CHECK_STATUS:
      host.status_calls++;
      return host.lost_after && host.status_calls >= host.lost_after ? (int32_t)0xc00002b6 : 0;
   case BC250_HOST_REPORT_LOST:
      return (int32_t)0xc00002b6;
   default:
      return (int32_t)0xc00000bb;
   }
}

/* The kernel of the native cases. BC250_WDDM_CALL takes the native path only
 * while host.dispatch is NULL. */
static struct {
   bool in_use;
   NTSTATUS fail_status;  /* what a failing GetDeviceState returns */
   unsigned fail_from;    /* queries from this number on fail; 0: none */
   D3DKMT_DEVICEEXECUTION_STATE state; /* what an answering query reports */
   unsigned queries, waits;
} k;

static NTSTATUS APIENTRY
test_kernel_GetDeviceState(D3DKMT_GETDEVICESTATE *arg)
{
   k.queries++;
   if (k.fail_from && k.queries >= k.fail_from)
      return k.fail_status;
   arg->ExecutionState = k.state;
   return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY
test_kernel_WaitForSynchronizationObjectFromCpu(CONST D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *arg)
{
   (void)arg;
   k.waits++;
   if (write_value)
      fence_value = write_value;
   return STATUS_SUCCESS;
}

static struct vk_wddm2_dispatch_table test_table;

struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   if (!k.in_use) {
      fprintf(stderr, "native dispatch table requested by a hosted case: test setup error\n");
      abort();
   }
   return &test_table;
}

/* One wait for value 2 on a fence that reads 1, with slices of 20 ms. */
static VkResult
slice_wait(bool hosted, uint64_t timeout_ns, uint64_t value_at_wait, uint64_t *elapsed_ms)
{
   static int identity;
   setup();
   if (hosted) {
      device.bc250_host.sType = BC250_HOST_STYPE;
      device.bc250_host.identity = &identity;
      device.bc250_host.dispatch = host_dispatch;
   } else {
      test_table.GetDeviceState = test_kernel_GetDeviceState;
      test_table.WaitForSynchronizationObjectFromCpu = test_kernel_WaitForSynchronizationObjectFromCpu;
      k.in_use = true;
   }
   write_value = value_at_wait;
   fence_value = 1;
   memset(&fence, 0, sizeof(fence));
   fence.base.type = &vk_wddm2_monitored_fence_type;
   fence.handle = 0x40000700u;
   fence.value_map = &fence_value;
   const struct vk_sync_wait wait = {
      .sync = &fence.base,
      .stage_mask = ~(VkPipelineStageFlags2)0,
      .wait_value = 2,
   };
   const uint64_t slice = vk_wddm2_monitored_fence_wait_slice_ns;
   vk_wddm2_monitored_fence_wait_slice_ns = 20000000ull;
   const uint64_t start = os_time_get_nano();
   const uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : start + timeout_ns;
   VkResult result = vk_wddm2_monitored_fence_type.wait_many(&device, 1, &wait, VK_SYNC_WAIT_COMPLETE, deadline);
   *elapsed_ms = (os_time_get_nano() - start) / 1000000;
   vk_wddm2_monitored_fence_wait_slice_ns = slice;
   return result;
}

static void
test_hosted_stopped_adapter(void)
{
   memset(&host, 0, sizeof(host));
   host.lost_after = 4;
   uint64_t ms = 0;
   /* The case of the dump: no deadline, the event never comes. The host
    * reports the loss at its fourth status call: the poll before the wait,
    * then the poll and the liveness check of the first slice, then the poll
    * of the second slice. */
   VkResult result = slice_wait(true, UINT64_MAX, 0, &ms);
   check(result == VK_ERROR_DEVICE_LOST && host.waits == 1 && host.status_calls == 4 && ms < 5000,
         "hosted, no deadline, event never set, host lost at its 4th status call: result %d (want %d), "
         "waits %u, status calls %u (want 4), %llu ms",
         result, VK_ERROR_DEVICE_LOST, host.waits, host.status_calls, (unsigned long long)ms);
   check(vk_device_is_lost_no_report(&device), "hosted: the device is marked lost");
}

static void
test_hosted_live_device(void)
{
   uint64_t ms = 0;
   /* K225: a healthy host, a 150 ms deadline: several slices, then VK_TIMEOUT
    * and no loss. */
   memset(&host, 0, sizeof(host));
   VkResult result = slice_wait(true, 150000000ull, 0, &ms);
   check(result == VK_TIMEOUT && host.status_calls >= 3 && ms >= 140 && !vk_device_is_lost_no_report(&device),
         "hosted, live, 150 ms deadline: result %d (want %d), status calls %u (want >= 3), %llu ms, lost %d",
         result, VK_TIMEOUT, host.status_calls, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));

   /* A fence that reaches its value while the event stays unset: the first
    * slice reads it and the wait succeeds, with no deadline. */
   memset(&host, 0, sizeof(host));
   result = slice_wait(true, UINT64_MAX, 2, &ms);
   check(result == VK_SUCCESS && host.waits == 1 && ms < 5000 && !vk_device_is_lost_no_report(&device),
         "hosted, fence reaches its value with no event: result %d (want 0), waits %u, %llu ms, lost %d",
         result, host.waits, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));
}

static void
test_native_device_removed(void)
{
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_ACTIVE;
   k.fail_status = STATUS_DEVICE_REMOVED;
   k.fail_from = 2;
   uint64_t ms = 0;
   /* The system ICD on a stopped adapter: no deadline, the event never comes,
    * the second device-state query answers STATUS_DEVICE_REMOVED. */
   VkResult result = slice_wait(false, UINT64_MAX, 0, &ms);
   check(result == VK_ERROR_DEVICE_LOST && k.waits == 1 && k.queries == 2 && ms < 5000,
         "native, no deadline, event never set, query 2 STATUS_DEVICE_REMOVED: result %d (want %d), "
         "waits %u, queries %u (want 2), %llu ms",
         result, VK_ERROR_DEVICE_LOST, k.waits, k.queries, (unsigned long long)ms);
   check(vk_device_is_lost_no_report(&device), "native, removed: the device is marked lost");
}

static void
test_native_stopped_state(void)
{
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_STOPPED;
   uint64_t ms = 0;
   /* The query answers, and the device does not execute any more. */
   VkResult result = slice_wait(false, UINT64_MAX, 0, &ms);
   check(result == VK_ERROR_DEVICE_LOST && k.waits == 1 && k.queries == 1 && ms < 5000,
         "native, no deadline, execution state STOPPED: result %d (want %d), waits %u, queries %u (want 1), "
         "%llu ms",
         result, VK_ERROR_DEVICE_LOST, k.waits, k.queries, (unsigned long long)ms);
   check(vk_device_is_lost_no_report(&device), "native, stopped: the device is marked lost");
}

static void
test_native_live_device(void)
{
   uint64_t ms = 0;
   /* K225 on the native path: an active device, a 150 ms deadline. */
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_ACTIVE;
   VkResult result = slice_wait(false, 150000000ull, 0, &ms);
   check(result == VK_TIMEOUT && k.queries >= 3 && ms >= 140 && !vk_device_is_lost_no_report(&device),
         "native, active, 150 ms deadline: result %d (want %d), queries %u (want >= 3), %llu ms, lost %d",
         result, VK_TIMEOUT, k.queries, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));

   /* A query that fails for another reason than a removed device is no
    * evidence of a loss: the wait goes on to its deadline. */
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_ACTIVE;
   k.fail_status = STATUS_INVALID_PARAMETER;
   k.fail_from = 1;
   result = slice_wait(false, 150000000ull, 0, &ms);
   check(result == VK_TIMEOUT && k.queries >= 3 && ms >= 140 && !vk_device_is_lost_no_report(&device),
         "native, every query STATUS_INVALID_PARAMETER, 150 ms deadline: result %d (want %d), queries %u "
         "(want >= 3), %llu ms, lost %d",
         result, VK_TIMEOUT, k.queries, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));

   /* A fence that reaches its value with no event: the first slice reads it,
    * the wait succeeds, and the check after the wake reads an active device. */
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_ACTIVE;
   result = slice_wait(false, UINT64_MAX, 2, &ms);
   check(result == VK_SUCCESS && k.waits == 1 && ms < 5000 && !vk_device_is_lost_no_report(&device),
         "native, fence reaches its value with no event: result %d (want 0), waits %u, %llu ms, lost %d",
         result, k.waits, (unsigned long long)ms, (int)vk_device_is_lost_no_report(&device));
}

static const struct {
   const char *name;
   void (*run)(void);
} tests[] = {
   {"hosted_stopped_adapter", test_hosted_stopped_adapter},
   {"hosted_live_device", test_hosted_live_device},
   {"native_device_removed", test_native_device_removed},
   {"native_stopped_state", test_native_stopped_state},
   {"native_live_device", test_native_live_device},
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
