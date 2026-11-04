/* SPDX-License-Identifier: MIT
 *
 * Inert kernel boundary for wddm_fence_pending_test.c (BD-200). Included
 * twice by the generated unit: stage 1 before the production fence structure
 * (types), stage 2 after it (the fence table, the kernel and the host calls).
 * Real: c11 threads, util/u_atomic.h, util/timespec.h, util/stack_array.h,
 * util/bc250_host_bootstrap.h, vk_sync.h. Mocked: D3DKMT, vk_async_event,
 * os_time_get_nano, the vk_log/vk_device error paths.
 */
#if WDDM_PENDING_STAGE == 1

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/timespec.h"
#include "util/u_atomic.h"
#include "util/stack_array.h"
#include "util/bc250_host_bootstrap.h"
#include "vk_sync.h"

typedef LONG NTSTATUS;
#undef STATUS_NO_MEMORY
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define STATUS_DEVICE_REMOVED ((NTSTATUS)0xC00002B6L)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define WDDM2_DISPATCH(call) mock_##call

typedef UINT D3DKMT_HANDLE;
typedef struct {
   D3DKMT_HANDLE hDevice;
   UINT ObjectCount;
   const D3DKMT_HANDLE *ObjectHandleArray;
   const UINT64 *FenceValueArray;
   struct { UINT AllowFenceRewind : 1; } Flags;
} D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU;
typedef struct {
   D3DKMT_HANDLE hDevice;
   UINT ObjectCount;
   const D3DKMT_HANDLE *ObjectHandleArray;
   const UINT64 *FenceValueArray;
   HANDLE hAsyncEvent;
   struct { UINT WaitAny : 1; } Flags;
} D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU;
typedef enum {
   D3DKMT_DEVICESTATE_EXECUTION = 1,
} D3DKMT_DEVICESTATE_TYPE;
typedef enum {
   D3DKMT_DEVICEEXECUTION_ACTIVE = 1,
   D3DKMT_DEVICEEXECUTION_STOPPED = 4,
} D3DKMT_DEVICEEXECUTION_STATE;
typedef struct {
   D3DKMT_HANDLE hDevice;
   D3DKMT_DEVICESTATE_TYPE StateType;
   D3DKMT_DEVICEEXECUTION_STATE ExecutionState;
} D3DKMT_GETDEVICESTATE;

struct vk_device {
   D3DKMT_HANDLE wddm2_handle;
   struct bc250_host bc250_host;
};

static volatile LONG checks, failures;
static const char *group = "startup";
#define CHECK(c) do { InterlockedIncrement(&checks); if (!(c)) { InterlockedIncrement(&failures); \
   printf("FAIL CHECK %s line %u: %s\n", group, __LINE__, #c); } } while (0)

static volatile LONG lost_calls;
static VkResult
mock_set_lost(struct vk_device *device)
{
   (void)device;
   InterlockedIncrement(&lost_calls);
   return VK_ERROR_DEVICE_LOST;
}
#define vk_device_set_lost(device, ...) mock_set_lost(device)
#define vk_errorf(device, result, ...) (result)
#define vk_error(device, result) (result)

int64_t
os_time_get_nano(void)
{
   static LARGE_INTEGER frequency;
   LARGE_INTEGER now;
   if (!frequency.QuadPart)
      QueryPerformanceFrequency(&frequency);
   QueryPerformanceCounter(&now);
   return (int64_t)((double)now.QuadPart * 1e9 / (double)frequency.QuadPart);
}

#elif WDDM_PENDING_STAGE == 2

const struct vk_sync_type vk_wddm2_monitored_fence_type = {
   .size = sizeof(struct vk_wddm2_monitored_fence),
};

static inline bool
vk_sync_type_is_wddm2_monitored_fence(const struct vk_sync_type *type)
{
   return type == &vk_wddm2_monitored_fence_type;
}

struct mock_fence {
   struct vk_wddm2_monitored_fence fence;
   uint64_t value;
};

#define MOCK_FENCES 64
static struct mock_fence mock_fences[MOCK_FENCES];
static unsigned mock_fence_count;
static volatile bool host_lost;
static volatile LONG kernel_waits, cpu_signals, host_reported_lost, unexpected_calls;

/* The kernel wait in flight, read by the async event mock. */
static D3DKMT_HANDLE kwait_handles[8];
static uint64_t kwait_values[8];
static UINT kwait_count;
static bool kwait_any;

static struct mock_fence *
new_fence(enum vk_sync_flags flags, bool imported)
{
   assert(mock_fence_count < MOCK_FENCES);
   struct mock_fence *f = &mock_fences[mock_fence_count++];
   memset(f, 0, sizeof(*f));
   f->fence.base.type = &vk_wddm2_monitored_fence_type;
   f->fence.base.flags = VK_SYNC_IS_TIMELINE | flags;
   f->fence.handle = mock_fence_count; /* index + 1 */
   f->fence.value_map = &f->value;
#ifdef FENCE_HAS_IMPORTED_FLAG
   f->fence.shared_handle_imported = imported;
#else
   (void)imported; /* a revision before BD-200 on a line without the field */
#endif
   return f;
}

static void
reset_mock(void)
{
   printf("case %s\n", group);
   mock_fence_count = 0;
   host_lost = false;
   kernel_waits = cpu_signals = host_reported_lost = 0;
   kwait_count = 0;
}

static uint64_t
mock_value(D3DKMT_HANDLE handle)
{
   if (handle == 0 || handle > mock_fence_count)
      return 0;
   return p_atomic_read(&mock_fences[handle - 1].value);
}

static NTSTATUS
mock_SignalSynchronizationObjectFromCpu(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *signal)
{
   (void)signal;
   /* The signal stays queued (behind a GPU wait, for example): the fence
    * value does not move. */
   InterlockedIncrement(&cpu_signals);
   return STATUS_SUCCESS;
}

static NTSTATUS
mock_WaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *wait)
{
   InterlockedIncrement(&kernel_waits);
   if (!wait->hAsyncEvent || wait->ObjectCount > ARRAY_SIZE(kwait_handles))
      return STATUS_NOT_IMPLEMENTED;
   for (UINT i = 0; i < wait->ObjectCount; i++) {
      kwait_handles[i] = wait->ObjectHandleArray[i];
      kwait_values[i] = wait->FenceValueArray[i];
   }
   kwait_any = wait->Flags.WaitAny;
   kwait_count = wait->ObjectCount;
   return STATUS_SUCCESS;
}

static NTSTATUS
mock_GetDeviceState(D3DKMT_GETDEVICESTATE *state)
{
   if (state->StateType != D3DKMT_DEVICESTATE_EXECUTION)
      return STATUS_NOT_IMPLEMENTED;
   state->ExecutionState = host_lost ? D3DKMT_DEVICEEXECUTION_STOPPED
                                     : D3DKMT_DEVICEEXECUTION_ACTIVE;
   return STATUS_SUCCESS;
}

static int32_t
mock_host_dispatch(void *userdata, uint32_t operation, void *argument)
{
   (void)userdata;
   switch (operation) {
   case BC250_HOST_CHECK_STATUS:
      return host_lost ? -1 : 0;
   case BC250_HOST_REPORT_LOST:
      InterlockedIncrement(&host_reported_lost);
      return 0;
   case BC250_HOST_SignalSynchronizationObjectFromCpu:
      return mock_SignalSynchronizationObjectFromCpu(argument);
   case BC250_HOST_WaitForSynchronizationObjectFromCpu:
      return mock_WaitForSynchronizationObjectFromCpu(argument);
   case BC250_HOST_GetDeviceState:
      return mock_GetDeviceState(argument);
   default:
      InterlockedIncrement(&unexpected_calls);
      return STATUS_NOT_IMPLEMENTED;
   }
}

/* A hosted device (the D3D12 route) and a kernel device (the system route). */
static struct vk_device device = {
   .wddm2_handle = 7,
   .bc250_host = {.dispatch = mock_host_dispatch},
};
static struct vk_device kdevice = {.wddm2_handle = 8};

static VkResult
vk_wddm2_check_device_status(struct vk_device *dev)
{
   (void)dev;
   return host_lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static bool
kwait_satisfied(void)
{
   UINT ready = 0;
   for (UINT i = 0; i < kwait_count; i++)
      ready += mock_value(kwait_handles[i]) >= kwait_values[i];
   return kwait_count && (ready == kwait_count || (kwait_any && ready));
}

static VkResult
vk_async_event_create(HANDLE *event)
{
   *event = (HANDLE)(uintptr_t)1;
   return VK_SUCCESS;
}

static VkResult
vk_async_event_wait(HANDLE event, uint64_t rel_timeout_ns)
{
   (void)event;
   const uint64_t end = os_time_get_nano() + MIN2(rel_timeout_ns, 10000000000ull);
   do {
      if (kwait_satisfied())
         return VK_SUCCESS;
      Sleep(1);
   } while ((uint64_t)os_time_get_nano() < end);
   return VK_TIMEOUT;
}

static void
vk_async_event_close(HANDLE event)
{
   (void)event;
   kwait_count = 0;
}

#else
#error "WDDM_PENDING_STAGE must be 1 or 2"
#endif
