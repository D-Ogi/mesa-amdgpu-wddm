/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_wddm2_monitored_fence.h"

#include <assert.h>
#include <inttypes.h>

#include "util/os_time.h"
#include "util/u_atomic.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/eventfd.h>
#endif

/* Windows headers conflict with Wayland and XLib headers */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "vk_async_event.h"
#include "vk_device.h"
#include "vk_wddm2_dispatch_table.h"
#include "vk_log.h"
#include "vk_queue.h"
#include "vk_util.h"

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#include "d3dkmthk.h"

typedef struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
  ULONG           Length;
  HANDLE          RootDirectory;
  PUNICODE_STRING ObjectName;
  ULONG           Attributes;
  PVOID           SecurityDescriptor;
  PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES;


static struct vk_wddm2_monitored_fence *
to_wddm2_monitored_fence(struct vk_sync *sync)
{
   assert(vk_sync_type_is_wddm2_monitored_fence(sync->type));
   return container_of(sync, struct vk_wddm2_monitored_fence, base);
}

static VkResult
NTSTATUS_to_VkResult(struct vk_device *device,
                     NTSTATUS status)
{
   switch (status) {
   case STATUS_SUCCESS:
      return VK_SUCCESS;
   case STATUS_NO_MEMORY:
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   case STATUS_DEVICE_REMOVED:
      return vk_device_set_lost(device, "Received STATUS_DEVICE_REMOVED");
   default:
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "Unknown NTSTATUS: 0x%x", status);
   }
}

static VkResult
vk_wddm2_monitored_fence_init(struct vk_device *device,
                              struct vk_sync *sync,
                              uint64_t initial_value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   NTSTATUS status;

   /* An exportable semaphore (VkExportSemaphoreCreateInfo) arrives here with
    * VK_SYNC_IS_SHAREABLE only; the runtime sets VK_SYNC_IS_SHARED after the
    * first successful export. D3DKMTShareObjects needs NtSecuritySharing at
    * creation time, so the object must be created shareable now, otherwise
    * the export below has no NT handle to duplicate (vkd3d-proton's
    * D3D12_FENCE export then failed with VK_ERROR_UNKNOWN).
    */
   const bool shareable = (sync->flags & VK_SYNC_IS_SHAREABLE) != 0;

   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {
      .hDevice = device->wddm2_handle,
      .Info = {
         .Type = D3DDDI_MONITORED_FENCE,
         .Flags = {
            .Shared = shareable,
            .NtSecuritySharing = shareable,
            /* This gets us 64-bit fences */
            .NoGPUAccess = false,
         },
         .MonitoredFence = {
            .InitialFenceValue = initial_value,
            .EngineAffinity = 1,
         },
      }
   };
   status = BC250_WDDM_CALL(&device->bc250_host, CreateSynchronizationObject2, &create);
   if (unlikely(!NT_SUCCESS(status)))
       return NTSTATUS_to_VkResult(device, status);

   fence->handle = create.hSyncObject;
   fence->value_map = create.Info.MonitoredFence.FenceValueCPUVirtualAddress;
#ifdef _WIN32
   if (shareable && !device->bc250_host.dispatch) {
      OBJECT_ATTRIBUTES oa = { sizeof(OBJECT_ATTRIBUTES) };
      status = WDDM2_DISPATCH(ShareObjects(
         1,
         &fence->handle,
         &oa,
         D3DDDI_SYNC_OBJECT_ALL_ACCESS,
         &fence->shared_handle
      ));
      if (unlikely(!NT_SUCCESS(status))) {
         const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
            .hSyncObject = fence->handle,
         };
         BC250_WDDM_CALL(&device->bc250_host, DestroySynchronizationObject, &destroy);
         fence->handle = 0;
         fence->shared_handle = NULL;
         return NTSTATUS_to_VkResult(device, status);
      }
   }
#endif

   return VK_SUCCESS;
}

static void
vk_wddm2_monitored_fence_finish(struct vk_device *device,
                                struct vk_sync *sync)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

#ifdef _WIN32
   if (fence->shared_handle) {
      ASSERTED BOOL ok = CloseHandle(fence->shared_handle);
      assert(ok);
   }
#endif

   const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
      .hSyncObject = fence->handle,
   };
   ASSERTED NTSTATUS status = BC250_WDDM_CALL(&device->bc250_host, DestroySynchronizationObject, &destroy);
   assert(NT_SUCCESS(status));
}

static VkResult
vk_wddm2_monitored_fence_signal(struct vk_device *device,
                                struct vk_sync *sync,
                                uint64_t value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   if (!bc250_host_fence_valid(&device->bc250_host,p_atomic_read(fence->value_map)))
      return vk_device_set_lost(device,"Hosted fence lost before signal");
   assert(value > p_atomic_read(fence->value_map));

   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal = {
      .hDevice = device->wddm2_handle,
      .ObjectCount = 1,
      .ObjectHandleArray = &fence->handle,
      .FenceValueArray = &value,
      .Flags = {
         .AllowFenceRewind = true,
      },
   };
   NTSTATUS status = BC250_WDDM_CALL(&device->bc250_host, SignalSynchronizationObjectFromCpu, &signal);
   if (unlikely(!NT_SUCCESS(status))) {
      return NTSTATUS_to_VkResult(device, status);
   }

   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_get_value(struct vk_device *device,
                                   struct vk_sync *sync,
                                   uint64_t *value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   *value = p_atomic_read(fence->value_map);
   if (!bc250_host_fence_valid(&device->bc250_host,*value))
      return vk_device_set_lost(device,"Hosted fence value invalid");
   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_wait_many(struct vk_device *device,
                                   uint32_t wait_count,
                                   const struct vk_sync_wait *waits,
                                   enum vk_sync_wait_flags wait_flags,
                                   uint64_t abs_timeout_ns)
{
   VkResult result;
   NTSTATUS status;

   /* Quick poll all the fences ourselves.  We may not have to call into the
    * kernel at all.
    */
   uint32_t ready = 0;
   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_wddm2_monitored_fence *fence =
         to_wddm2_monitored_fence(waits[i].sync);

      uint64_t value=p_atomic_read(fence->value_map);
      if (!bc250_host_fence_valid(&device->bc250_host,value))
         return vk_device_set_lost(device,"Hosted fence lost while polling");
      if (value >= waits[i].wait_value) ready++;
   }
   if (ready == wait_count || ((wait_flags & VK_SYNC_WAIT_ANY) && ready > 0))
      return VK_SUCCESS;

   if (abs_timeout_ns == 0)
      return VK_TIMEOUT;

   uint64_t now_ns = os_time_get_nano();
   if (abs_timeout_ns <= now_ns)
      return VK_TIMEOUT;

   /* A NULL hAsyncEvent makes D3DKMT wait indefinitely. Always request
    * notification so finite Vulkan deadlines remain under our control.
    */
   HANDLE async_event = 0;
   result = vk_async_event_create(&async_event);
   if (unlikely(result != VK_SUCCESS))
      return result;

   STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
   STACK_ARRAY(uint64_t, wait_values, wait_count);

   for (uint64_t i = 0; i < wait_count; i++) {
      handles[i] = to_wddm2_monitored_fence(waits[i].sync)->handle,
      wait_values[i] = waits[i].wait_value;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = device->wddm2_handle,
      .ObjectCount = wait_count,
      .ObjectHandleArray = handles,
      .FenceValueArray = wait_values,
      .Flags = {
         .WaitAny = (wait_flags & VK_SYNC_WAIT_ANY) != 0,
      },
      .hAsyncEvent = async_event,
   };
   status = BC250_WDDM_CALL(&device->bc250_host, WaitForSynchronizationObjectFromCpu, &wait);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(wait_values);

   if (unlikely(!NT_SUCCESS(status))) {
      result = NTSTATUS_to_VkResult(device, status);
      goto fail_close_event;
   }

   for (;;) {
      now_ns = os_time_get_nano();
      uint64_t remaining = abs_timeout_ns > now_ns ? abs_timeout_ns - now_ns : 0;
      result = vk_async_event_wait(async_event, remaining);
      if (result != VK_TIMEOUT || os_time_get_nano() >= abs_timeout_ns)
         break;
      /* Retry early/interrupted waits and timeouts capped by the OS API. */
   }

   if (result == VK_SUCCESS) {
      for (uint32_t i=0;i<wait_count;++i) {
         struct vk_wddm2_monitored_fence *fence=to_wddm2_monitored_fence(waits[i].sync);
         if (!bc250_host_fence_valid(&device->bc250_host,p_atomic_read(fence->value_map))) {
            result=vk_device_set_lost(device,"Hosted fence lost after wake");
            break;
         }
      }
      if (result==VK_SUCCESS) result=vk_wddm2_check_device_status(device);
   }

fail_close_event:
   vk_async_event_close(async_event);

   return result;
}

#ifdef _WIN32
static VkResult
vk_wddm2_monitored_fence_import_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    void *handle,
                                                   const wchar_t *name)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   bool shared = (sync->flags & VK_SYNC_IS_SHARED) != 0;
   NTSTATUS status;
   
   assert(name == NULL);

   D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {
      .hNtHandle = (HANDLE) handle,
      .hDevice = device->wddm2_handle,
      .Flags = {
         .Shared = shared,
         .NtSecuritySharing = shared,
         /* This gets us 64-bit fences */
         .NoGPUAccess = true,
      },
   };

   status = BC250_WDDM_CALL(&device->bc250_host, OpenSyncObjectFromNtHandle2, &open);
   if (unlikely(!NT_SUCCESS(status)))
      return NTSTATUS_to_VkResult(device, status);

   vk_wddm2_monitored_fence_finish(device, sync);

   fence->handle = open.hSyncObject;
   fence->shared_handle = handle;
   fence->value_map = open.MonitoredFence.FenceValueCPUVirtualAddress;

   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_export_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    void **handle)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   if (!fence->shared_handle)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "monitored fence was not created shareable");

   HANDLE process = GetCurrentProcess();
   BOOL ok = DuplicateHandle(process, (HANDLE)fence->shared_handle,
                             process, (HANDLE *)handle, 0,
                             false, DUPLICATE_SAME_ACCESS);
   if (!ok)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "DuplicateHandle failed");

   return VK_SUCCESS;
}
#endif

const struct vk_sync_type vk_wddm2_monitored_fence_type = {
   .size = sizeof(struct vk_wddm2_monitored_fence),
   .features = VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = vk_wddm2_monitored_fence_init,
   .finish = vk_wddm2_monitored_fence_finish,
   .signal = vk_wddm2_monitored_fence_signal,
   .get_value = vk_wddm2_monitored_fence_get_value,
   .wait_many = vk_wddm2_monitored_fence_wait_many,
   .export_win32_handle = vk_wddm2_monitored_fence_export_opaque_win32_handle,
   .import_win32_handle = vk_wddm2_monitored_fence_import_opaque_win32_handle,
};

const struct vk_sync_type vk_wddm2_monitored_fence_hosted_type = {
   .size = sizeof(struct vk_wddm2_monitored_fence),
   .features = VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = vk_wddm2_monitored_fence_init,
   .finish = vk_wddm2_monitored_fence_finish,
   .signal = vk_wddm2_monitored_fence_signal,
   .get_value = vk_wddm2_monitored_fence_get_value,
   .wait_many = vk_wddm2_monitored_fence_wait_many,
};

VkResult
vk_wddm2_check_device_status(struct vk_device *device)
{
   if (device->bc250_host.dispatch)
      return bc250_host_check_status(&device->bc250_host)<0 ?
         vk_device_set_lost(device,"Hosted runtime reports device loss") : VK_SUCCESS;
   NTSTATUS status;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = device->wddm2_handle,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   status = BC250_WDDM_CALL(&device->bc250_host, GetDeviceState, &get_state);
   if (unlikely(!NT_SUCCESS(status))) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "D3DKMTGetDeviceState failed");
   }

   switch (get_state.ExecutionState) {
   case D3DKMT_DEVICEEXECUTION_ACTIVE:
      return VK_SUCCESS;
   case D3DKMT_DEVICEEXECUTION_RESET:
      return vk_device_set_lost(device, "Device was reset");
   case D3DKMT_DEVICEEXECUTION_HUNG:
      return vk_device_set_lost(device, "Device is hung");
   case D3DKMT_DEVICEEXECUTION_STOPPED:
      return vk_device_set_lost(device, "Device is stopped");
   case D3DKMT_DEVICEEXECUTION_ERROR_OUTOFMEMORY:
      return vk_device_set_lost(device, "Device ran out of memory");
   case D3DKMT_DEVICEEXECUTION_ERROR_DMAFAULT:
      return vk_device_set_lost(device, "Device DMA fault");
   case D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT:
      get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
      status = BC250_WDDM_CALL(&device->bc250_host, GetDeviceState, &get_state);
      if (unlikely(!NT_SUCCESS(status))) {
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "D3DKMTGetDeviceState failed");
      }
      D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

      if (fault.FaultedVirtualAddress) {
         return vk_device_set_lost(device, "Device page fault at 0x%"PRIx64,
                                   fault.FaultedVirtualAddress);
      }

      return vk_device_set_lost(device, "Unknown device page fault");
   default:
      return vk_device_set_lost(device, "Unknown device error");
   }
}

VkResult
vk_wddm2_monitored_fence_gpu_wait_many(struct vk_queue *queue,
                                       uint32_t context_handle,
                                       uint32_t wait_count,
                                       const struct vk_sync_wait *waits)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
   STACK_ARRAY(uint64_t, wait_values, wait_count);

   for (uint32_t i = 0; i < wait_count; i++) {
      handles[i] = to_wddm2_monitored_fence(waits[i].sync)->handle;
      wait_values[i] = waits[i].wait_value;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU gpu_wait = {
      .hContext = context_handle,
      .ObjectCount = wait_count,
      .ObjectHandleArray = handles,
      .MonitoredFenceValueArray = wait_values,
   };
   NTSTATUS status = BC250_WDDM_CALL(&queue->base.device->bc250_host, WaitForSynchronizationObjectFromGpu, &gpu_wait);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(wait_values);

   if (unlikely(!NT_SUCCESS(status))) {
      return vk_queue_set_lost(queue,
         "D3DKMTWaitForSynchronizationObjectFromGpu failed");
   }

   return VK_SUCCESS;
}

VkResult
vk_wddm2_monitored_fence_gpu_signal_many(struct vk_queue *queue,
                                         uint32_t context_handle,
                                         uint32_t signal_count,
                                         const struct vk_sync_signal *signals)
{
   if (signal_count == 0)
      return VK_SUCCESS;

   STACK_ARRAY(D3DKMT_HANDLE, handles, signal_count);
   STACK_ARRAY(uint64_t, signal_values, signal_count);

   for (uint32_t i = 0; i < signal_count; i++) {
      struct vk_wddm2_monitored_fence *fence =
         to_wddm2_monitored_fence(signals[i].sync);

      if (!bc250_host_fence_valid(&queue->base.device->bc250_host,p_atomic_read(fence->value_map))) {
         STACK_ARRAY_FINISH(handles);
         STACK_ARRAY_FINISH(signal_values);
         return vk_queue_set_lost(queue,"Hosted fence lost before GPU signal");
      }
      assert(signals[i].signal_value > p_atomic_read(fence->value_map));

      handles[i] = fence->handle;
      signal_values[i] = signals[i].signal_value;
   }

   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU gpu_signal = {
      .hContext = context_handle,
      .ObjectCount = signal_count,
      .ObjectHandleArray = handles,
      .MonitoredFenceValueArray = signal_values,
   };
   NTSTATUS status = BC250_WDDM_CALL(&queue->base.device->bc250_host, SignalSynchronizationObjectFromGpu, &gpu_signal);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(signal_values);

   if (unlikely(!NT_SUCCESS(status))) {
      return vk_queue_set_lost(queue,
         "D3DKMTSignalSynchronizationObjectFromGpu failed");
   }

   return VK_SUCCESS;
}
