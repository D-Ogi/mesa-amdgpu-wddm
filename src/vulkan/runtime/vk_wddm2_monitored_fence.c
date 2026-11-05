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
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "c11/threads.h"
#include "util/os_time.h"
#include "util/timespec.h"
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

/* Opt-in diagnostics for shared-fence admission. No object names, handles or
 * virtual addresses are logged. Keep the input flags distinct from returned
 * mapping presence, including every failed attempt.
 */
static void
vk_wddm2_fence_trace(const char *operation, NTSTATUS status, uint32_t flags,
                    uint32_t access, uint32_t affinity, bool cpu, bool gpu)
{
   const char *trace = getenv("MESA_WDDM_FENCE_TRACE");
   if (trace && strcmp(trace, "1") == 0) {
      unsigned long pid = 0;
#ifdef _WIN32
      pid = (unsigned long)GetCurrentProcessId();
#endif
      fprintf(stderr, "wddm-fence: pid=%lu op=%s status=0x%08x flags=0x%08x "
              "access=0x%08x affinity=0x%x cpu-map=%u gpu-map=%u\n",
              pid, operation, (unsigned)status,
              flags, access, affinity, cpu, gpu);
      fflush(stderr);
   }
}


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

   /* The payload must be shareable before export parameters are applied.
    * Create the NT handle only after its security and name are known.
    */
   const bool shareable = (sync->flags & VK_SYNC_IS_SHAREABLE) != 0;

   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {
      .hDevice = device->wddm2_handle,
      .Info = {
         .Type = D3DDDI_MONITORED_FENCE,
         .Flags = {
            .Shared = shareable,
            .NtSecuritySharing = shareable,
            /* Shared timelines use scheduler packets on both APIs. This mode
             * keeps their 64-bit CPU value but needs no GPU VA. In particular,
             * a D3D12 runtime may first open an exported payload in this mode.
             * Private queue fences keep their existing mapped allocation.
             */
            .NoGPUAccess = shareable,
         },
         .MonitoredFence = {
            .InitialFenceValue = initial_value,
            .EngineAffinity = 1,
         },
      }
   };
   const uint32_t create_flags = create.Info.Flags.Value;
   status = BC250_WDDM_CALL(&device->bc250_host, CreateSynchronizationObject2, &create);
   vk_wddm2_fence_trace("create-monitored", status, create_flags, 0, 1,
                       create.Info.MonitoredFence.FenceValueCPUVirtualAddress != NULL,
                       create.Info.MonitoredFence.FenceValueGPUVirtualAddress != 0);
   if (unlikely(!NT_SUCCESS(status)))
       return NTSTATUS_to_VkResult(device, status);

   fence->handle = create.hSyncObject;
   fence->value_map = create.Info.MonitoredFence.FenceValueCPUVirtualAddress;
   fence->pending_value = initial_value;
#ifdef _WIN32
   fence->shared_handle = NULL;
   fence->export_access = GENERIC_ALL;
   fence->shared_handle_inheritable = false;
   fence->shared_handle_imported = false;
   fence->export_access_explicit = false;
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
   vk_wddm2_fence_trace("destroy", status, 0, 0, 0, false, false);
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

   vk_wddm2_monitored_fence_note_submitted(fence, value);
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

/* The time between two liveness checks of a CPU wait (BD-179). The host tests
 * shorten it; nothing else writes it. */
uint64_t vk_wddm2_monitored_fence_wait_slice_ns = 1000000000ull;

/* Read the fences of a wait: VK_SUCCESS when the wait is satisfied,
 * VK_NOT_READY when it is not, VK_ERROR_DEVICE_LOST when a fence reads the
 * lost-device value or the host reports the loss.
 */
static VkResult
vk_wddm2_monitored_fence_poll(struct vk_device *device,
                              uint32_t wait_count,
                              const struct vk_sync_wait *waits,
                              enum vk_sync_wait_flags wait_flags)
{
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
   return VK_NOT_READY;
}

/* Whether a CPU wait must stop after a slice that ended without its event.
 * A hosted device asks its host, which knows whether the adapter of the
 * device is still in the system (BD-179). A kernel device reads its
 * execution state. A query that fails for another reason than a removed
 * device is no evidence of a loss: the wait continues.
 */
static bool
vk_wddm2_wait_slice_device_lost(struct vk_device *device)
{
   if (device->bc250_host.dispatch)
      return bc250_host_check_status(&device->bc250_host) < 0;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = device->wddm2_handle,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   NTSTATUS status = BC250_WDDM_CALL(&device->bc250_host, GetDeviceState, &get_state);
   if (!NT_SUCCESS(status))
      return status == STATUS_DEVICE_REMOVED;
   return get_state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE;
}

/* BD-200: the wait for VK_SYNC_WAIT_PENDING, after vk_wddm2_check_device_status. */
static bool
vk_wddm2_monitored_fence_pending_known(uint32_t wait_count,
                                       const struct vk_sync_wait *waits);
static VkResult
vk_wddm2_monitored_fence_wait_pending(struct vk_device *device,
                                      uint32_t wait_count,
                                      const struct vk_sync_wait *waits,
                                      enum vk_sync_wait_flags wait_flags,
                                      uint64_t abs_timeout_ns);

static VkResult
vk_wddm2_monitored_fence_wait_many(struct vk_device *device,
                                   uint32_t wait_count,
                                   const struct vk_sync_wait *waits,
                                   enum vk_sync_wait_flags wait_flags,
                                   uint64_t abs_timeout_ns)
{
   VkResult result;
   NTSTATUS status;

   /* BD-200: VK_SYNC_WAIT_PENDING asks for the submission of the signal, not
    * for its completion (vk_sync.h). The threaded and on-demand submit modes
    * use it to hold a submission until its waits are in the kernel's hands;
    * a wait for completion there makes the CPU wait for the GPU on every
    * submission (Quake II RTX 64 fps against 73, the revert 3b660ba0). This
    * process knows every signal of a payload that no other process can
    * reach. For a payload that another process can signal, this process
    * cannot see the submission, so the wait below is for completion, which
    * implies it.
    */
   if ((wait_flags & VK_SYNC_WAIT_PENDING) &&
       vk_wddm2_monitored_fence_pending_known(wait_count, waits))
      return vk_wddm2_monitored_fence_wait_pending(device, wait_count, waits,
                                                   wait_flags, abs_timeout_ns);

   /* Quick poll all the fences ourselves.  We may not have to call into the
    * kernel at all.
    */
   result = vk_wddm2_monitored_fence_poll(device, wait_count, waits,
                                          wait_flags);
   if (result != VK_NOT_READY)
      return result;

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

   /* BD-179: wait in slices. The kernel does not set the event of a wait on
    * the fence of a stopped adapter: after a PnP restart of the display
    * device (pnputil /restart-device, a driver install) the fences of the old
    * device keep their last value, and a wait for a later value never ends.
    * In the dump of the hung explorer.exe (2026-11-02, tester.35) the
    * wallpaper's device release waited here with no bound for a graphics
    * fence that read 1, for value 2, while it held the composition lock of
    * the desktop window. So after each slice that ends without the event,
    * read the fences again and ask whether the device is lost. A slow fence
    * on a live device is no loss (K225): the wait then continues to the
    * caller's deadline.
    */
   for (;;) {
      now_ns = os_time_get_nano();
      if (now_ns >= abs_timeout_ns) {
         result = VK_TIMEOUT;
         break;
      }
      result = vk_async_event_wait(async_event,
                                   MIN2(abs_timeout_ns - now_ns,
                                        vk_wddm2_monitored_fence_wait_slice_ns));
      if (result != VK_TIMEOUT)
         break;
      result = vk_wddm2_monitored_fence_poll(device, wait_count, waits,
                                             wait_flags);
      if (result != VK_NOT_READY)
         break;
      if (vk_wddm2_wait_slice_device_lost(device)) {
         result = vk_device_set_lost(device,
            "Device lost during a fence wait (stopped adapter, BD-179)");
         break;
      }
      result = VK_TIMEOUT;
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
vk_wddm2_fence_object_name(struct vk_device *device, const wchar_t *name,
                          UNICODE_STRING *object_name)
{
   *object_name = (UNICODE_STRING){0};
   if (!name)
      return VK_SUCCESS;

   bool global = wcsncmp(name, L"Global\\", 7) == 0;
   if (global)
      name += 7;
   else if (wcsncmp(name, L"Local\\", 6) == 0)
      name += 6;
   if (!*name)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "unsupported semaphore namespace");

   DWORD session = 0;
   if (!global && !ProcessIdToSessionId(GetCurrentProcessId(), &session))
      return vk_errorf(device, VK_ERROR_UNKNOWN, "cannot resolve semaphore session");

   wchar_t prefix[64];
   int prefix_len = session == 0 ?
      swprintf(prefix, ARRAY_SIZE(prefix), L"\\BaseNamedObjects\\") :
      swprintf(prefix, ARRAY_SIZE(prefix), L"\\Sessions\\%lu\\BaseNamedObjects\\",
               (unsigned long)session);
   if (prefix_len < 0)
      return vk_error(device, VK_ERROR_UNKNOWN);

   size_t name_len = 0;
   const size_t max_chars = UINT16_MAX / sizeof(wchar_t) - 1;
   while (name_len <= max_chars && name[name_len]) {
      if (name[name_len] == L'\\')
         return vk_errorf(device, VK_ERROR_UNKNOWN, "unsupported semaphore namespace");
      name_len++;
   }
   if (name_len > max_chars - (size_t)prefix_len)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "semaphore name too long");

   size_t chars = (size_t)prefix_len + name_len;
   wchar_t *buffer = malloc((chars + 1) * sizeof(wchar_t));
   if (!buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   memcpy(buffer, prefix, (size_t)prefix_len * sizeof(wchar_t));
   memcpy(buffer + prefix_len, name, (name_len + 1) * sizeof(wchar_t));
   object_name->Length = (USHORT)(chars * sizeof(wchar_t));
   object_name->MaximumLength = (USHORT)((chars + 1) * sizeof(wchar_t));
   object_name->Buffer = buffer;
   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_set_win32_export_params(struct vk_device *device,
                                                struct vk_sync *sync,
                                                const void *security_attributes,
                                                uint32_t access,
                                                const wchar_t *name)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   if (device->bc250_host.dispatch || fence->shared_handle)
      return vk_error(device, VK_ERROR_UNKNOWN);

   const SECURITY_ATTRIBUTES *sa = security_attributes;
   UNICODE_STRING object_name;
   VkResult result = vk_wddm2_fence_object_name(device, name, &object_name);
   if (result != VK_SUCCESS)
      return result;

   OBJECT_ATTRIBUTES oa = {
      .Length = sizeof(OBJECT_ATTRIBUTES),
      .ObjectName = name ? &object_name : NULL,
      .Attributes = 0,
      .SecurityDescriptor = sa ? sa->lpSecurityDescriptor : NULL,
   };
   HANDLE shared_handle = NULL;
   NTSTATUS status = WDDM2_DISPATCH(ShareObjects(
      1, &fence->handle, &oa, access, &shared_handle));
   vk_wddm2_fence_trace("share-named-or-attributes", status, oa.Attributes,
                       access, 0, false, false);
   free(object_name.Buffer);
   if (!NT_SUCCESS(status))
      return vk_error(device, status == STATUS_NO_MEMORY ?
                      VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN);

   assert(fence->shared_handle == NULL);
   fence->shared_handle = shared_handle;
   fence->export_access = access;
   fence->export_access_explicit = true;
   fence->shared_handle_inheritable = sa && sa->bInheritHandle;
   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_import_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    void *handle,
                                                   const wchar_t *name)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   NTSTATUS status;
   
   HANDLE owned_handle = NULL;
   if (name) {
      if (device->bc250_host.dispatch)
         return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      UNICODE_STRING object_name;
      VkResult result = vk_wddm2_fence_object_name(device, name, &object_name);
      if (result != VK_SUCCESS)
         return result == VK_ERROR_OUT_OF_HOST_MEMORY ? result :
                vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      OBJECT_ATTRIBUTES oa = {
         .Length = sizeof(OBJECT_ATTRIBUTES),
         .ObjectName = &object_name,
      };
      D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME named = {
         /* Match the full-access name-open used by D3D12. Wait/signal rights
          * alone can open the NT handle but fail the subsequent KMT import.
          * The object's DACL still controls access; export rights are separate.
          */
         .dwDesiredAccess = D3DDDI_SYNC_OBJECT_ALL_ACCESS,
         .pObjAttrib = &oa,
      };
      status = WDDM2_DISPATCH(OpenSyncObjectNtHandleFromName(&named));
      vk_wddm2_fence_trace("open-name", status, oa.Attributes,
                          named.dwDesiredAccess, 0, false, false);
      free(object_name.Buffer);
      if (!NT_SUCCESS(status))
         return vk_error(device, status == STATUS_NO_MEMORY ?
                         VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INVALID_EXTERNAL_HANDLE);
      owned_handle = named.hNtHandle;
   } else {
      HANDLE process = GetCurrentProcess();
      if (!DuplicateHandle(process, (HANDLE)handle, process, &owned_handle,
                           0, false, DUPLICATE_SAME_ACCESS)) {
         DWORD error = GetLastError();
         vk_wddm2_fence_trace("duplicate-import-win32-error", (NTSTATUS)error,
                             DUPLICATE_SAME_ACCESS, 0, 0, false, false);
         return vk_error(device, error == ERROR_NOT_ENOUGH_MEMORY ||
                         error == ERROR_OUTOFMEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY :
                         VK_ERROR_INVALID_EXTERNAL_HANDLE);
      }
      vk_wddm2_fence_trace("duplicate-import", STATUS_SUCCESS,
                          DUPLICATE_SAME_ACCESS, 0, 0, false, false);
   }

   D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {
      .hNtHandle = owned_handle,
      .hDevice = device->wddm2_handle,
      .Flags = {
         /* Import opens an NT-shared payload even when the old destination
          * payload was private. vk_sync sets IS_SHARED only after success.
          */
         .Shared = true,
         .NtSecuritySharing = true,
         /* Packet-only access guarantees a 64-bit CPU value. */
         .NoGPUAccess = true,
      },
   };

   /* A diagnostic control reproduces the pre-fix flags on this exact object
    * and device. It never installs its payload and is disabled by default.
    * Enable the trace as well to retain its raw status.
    */
   const char *probe_enabled = getenv("MESA_WDDM_FENCE_IMPORT_PROBE");
   if (probe_enabled && strcmp(probe_enabled, "1") == 0) {
      D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 probe = open;
      probe.Flags.Shared = (sync->flags & VK_SYNC_IS_SHARED) != 0;
      probe.Flags.NtSecuritySharing = probe.Flags.Shared;
      const uint32_t probe_flags = probe.Flags.Value;
      NTSTATUS probe_status = BC250_WDDM_CALL(&device->bc250_host, OpenSyncObjectFromNtHandle2, &probe);
      vk_wddm2_fence_trace("open-nt-legacy-probe", probe_status, probe_flags, 0, 0,
                          probe.MonitoredFence.FenceValueCPUVirtualAddress != NULL,
                          probe.MonitoredFence.FenceValueGPUVirtualAddress != 0);
      if (NT_SUCCESS(probe_status) && probe.hSyncObject) {
         const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = { .hSyncObject = probe.hSyncObject };
         probe_status = BC250_WDDM_CALL(&device->bc250_host, DestroySynchronizationObject, &destroy);
         vk_wddm2_fence_trace("destroy-legacy-probe", probe_status, 0, 0, 0, false, false);
         if (!NT_SUCCESS(probe_status)) {
            CloseHandle(owned_handle);
            return NTSTATUS_to_VkResult(device, probe_status);
         }
      }
   }

   const uint32_t packet_flags = open.Flags.Value;
   status = BC250_WDDM_CALL(&device->bc250_host, OpenSyncObjectFromNtHandle2, &open);
   vk_wddm2_fence_trace("open-nt-packet", status, packet_flags, 0, 0,
                       open.MonitoredFence.FenceValueCPUVirtualAddress != NULL,
                       open.MonitoredFence.FenceValueGPUVirtualAddress != 0);
   if (status == STATUS_INVALID_PARAMETER) {
      /* Some shared objects cannot be reopened with NoGPUAccess. Windows'
       * D3D12 runtime retries this refusal with the mapped form too. Do not
       * retry access failures, invalid handles, OOM or device removal. Clear
       * the outputs first: a failed attempt is not a valid mapping witness.
       */
      open = (D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2) {
         .hNtHandle = owned_handle,
         .hDevice = device->wddm2_handle,
         .Flags = { .Shared = true, .NtSecuritySharing = true },
      };
      const uint32_t mapped_flags = open.Flags.Value;
      status = BC250_WDDM_CALL(&device->bc250_host, OpenSyncObjectFromNtHandle2, &open);
      vk_wddm2_fence_trace("open-nt-mapped", status, mapped_flags, 0, 0,
                          open.MonitoredFence.FenceValueCPUVirtualAddress != NULL,
                          open.MonitoredFence.FenceValueGPUVirtualAddress != 0);
   }
   if (unlikely(!NT_SUCCESS(status))) {
      CloseHandle(owned_handle);
      if (status == STATUS_DEVICE_REMOVED)
         return vk_device_set_lost(device, "Shared monitored fence import lost the device");
      return vk_error(device, status == STATUS_NO_MEMORY ?
                      VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   /* Legacy non-monitored objects can open successfully without a CPU value.
    * They cannot implement this sync type. Preserve the old payload on refusal.
    */
   if (!open.hSyncObject || !open.MonitoredFence.FenceValueCPUVirtualAddress ||
       ((uintptr_t)open.MonitoredFence.FenceValueCPUVirtualAddress & 7u)) {
      const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
         .hSyncObject = open.hSyncObject,
      };
      NTSTATUS cleanup = open.hSyncObject ?
         BC250_WDDM_CALL(&device->bc250_host, DestroySynchronizationObject, &destroy) :
         STATUS_INVALID_HANDLE;
      vk_wddm2_fence_trace("destroy-rejected-import", cleanup, 0, 0, 0, false, false);
      CloseHandle(owned_handle);
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   vk_wddm2_monitored_fence_finish(device, sync);

   fence->handle = open.hSyncObject;
   fence->shared_handle = owned_handle;
   fence->shared_handle_imported = true;
   fence->value_map = open.MonitoredFence.FenceValueCPUVirtualAddress;

   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_export_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    void **handle)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   if (!fence->shared_handle) {
      if (device->bc250_host.dispatch)
         return vk_error(device, VK_ERROR_UNKNOWN);
      OBJECT_ATTRIBUTES oa = { .Length = sizeof(OBJECT_ATTRIBUTES) };
      HANDLE exported = NULL;
      NTSTATUS status = WDDM2_DISPATCH(ShareObjects(
         1, &fence->handle, &oa, fence->export_access, &exported));
      vk_wddm2_fence_trace("share-unnamed", status, 0, fence->export_access,
                          0, false, false);
      if (!NT_SUCCESS(status))
         return vk_error(device, status == STATUS_NO_MEMORY ?
                         VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN);
      *handle = exported;
      return VK_SUCCESS;
   }

   HANDLE process = GetCurrentProcess();
   const bool same_access = fence->shared_handle_imported && !fence->export_access_explicit;
   BOOL ok = DuplicateHandle(process, (HANDLE)fence->shared_handle,
                             process, (HANDLE *)handle,
                             same_access ? 0 : fence->export_access,
                             fence->shared_handle_inheritable,
                             same_access ? DUPLICATE_SAME_ACCESS : 0);
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
   .set_win32_export_params = vk_wddm2_monitored_fence_set_win32_export_params,
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

/* BD-200: VK_SYNC_WAIT_PENDING.
 *
 * A wait for pending is a wait for some thread of this process to hand a
 * signal of at least the wait value to the kernel. Every such hand-over goes
 * through vk_wddm2_monitored_fence_note_submitted: the CPU signal above, the
 * GPU signal of vk_wddm2_monitored_fence_gpu_signal_many below and the GPU
 * signal of the winsys submit. It raises the fence's pending value and wakes
 * every pending wait of the process, which sleep on one condition variable.
 * A wake lets each wait read its own fences again, so a wake for another
 * fence costs a read, not a lost wake.
 */
static once_flag vk_wddm2_pending_once = ONCE_FLAG_INIT;
static mtx_t vk_wddm2_pending_mtx;
static cnd_t vk_wddm2_pending_cnd;

/* In a shared library, atexit handlers run when the library is unloaded. The
 * pending lock is a CRITICAL_SECTION on Windows: if it is not deleted before
 * the ICD is unloaded, Application Verifier stops with LOCK_IN_UNLOADED_DLL
 * (0x201). No device of this module exists at that time, so no wait uses it.
 */
static void
vk_wddm2_pending_fini(void)
{
   cnd_destroy(&vk_wddm2_pending_cnd);
   mtx_destroy(&vk_wddm2_pending_mtx);
}

static void
vk_wddm2_pending_init(void)
{
   mtx_init(&vk_wddm2_pending_mtx, mtx_plain);
   cnd_init(&vk_wddm2_pending_cnd);
   atexit(vk_wddm2_pending_fini);
}

void
vk_wddm2_monitored_fence_note_submitted(struct vk_wddm2_monitored_fence *fence, uint64_t value)
{
   uint64_t old = p_atomic_read(&fence->pending_value);
   while (old < value) {
      const uint64_t seen = p_atomic_cmpxchg(&fence->pending_value, old, value);
      if (seen == old)
         break;
      old = seen;
   }

   /* The raise is done before the lock, and a wait reads the value under the
    * lock before it sleeps, so a wait either reads the new value or sleeps
    * before this broadcast. */
   call_once(&vk_wddm2_pending_once, vk_wddm2_pending_init);
   mtx_lock(&vk_wddm2_pending_mtx);
   cnd_broadcast(&vk_wddm2_pending_cnd);
   mtx_unlock(&vk_wddm2_pending_mtx);
}

/* Whether this process sees every signal of these payloads: none can be
 * exported or was imported, so no other process can signal it. */
static bool
vk_wddm2_monitored_fence_pending_known(uint32_t wait_count,
                                       const struct vk_sync_wait *waits)
{
   for (uint32_t i = 0; i < wait_count; i++) {
      if (p_atomic_read(&waits[i].sync->flags) &
          (VK_SYNC_IS_SHAREABLE | VK_SYNC_IS_SHARED))
         return false;
#ifdef _WIN32
      if (to_wddm2_monitored_fence(waits[i].sync)->shared_handle_imported)
         return false;
#endif
   }
   return true;
}

/* VK_SUCCESS when the waits are pending (or complete), VK_NOT_READY when not,
 * VK_ERROR_DEVICE_LOST when a fence reads the lost-device value. Marks
 * nothing lost itself: it runs under the pending lock. */
static VkResult
vk_wddm2_monitored_fence_poll_pending(struct vk_device *device,
                                      uint32_t wait_count,
                                      const struct vk_sync_wait *waits,
                                      enum vk_sync_wait_flags wait_flags)
{
   uint32_t ready = 0;
   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_wddm2_monitored_fence *fence =
         to_wddm2_monitored_fence(waits[i].sync);

      const uint64_t value = p_atomic_read(fence->value_map);
      if (!bc250_host_fence_valid(&device->bc250_host, value))
         return VK_ERROR_DEVICE_LOST;
      const uint64_t pending = p_atomic_read(&fence->pending_value);
      if (MAX2(value, pending) >= waits[i].wait_value)
         ready++;
   }
   if (ready == wait_count || ((wait_flags & VK_SYNC_WAIT_ANY) && ready > 0))
      return VK_SUCCESS;
   return VK_NOT_READY;
}

static VkResult
vk_wddm2_monitored_fence_wait_pending(struct vk_device *device,
                                      uint32_t wait_count,
                                      const struct vk_sync_wait *waits,
                                      enum vk_sync_wait_flags wait_flags,
                                      uint64_t abs_timeout_ns)
{
   VkResult result = vk_wddm2_monitored_fence_poll_pending(device, wait_count,
                                                           waits, wait_flags);
   if (result == VK_ERROR_DEVICE_LOST)
      return vk_device_set_lost(device, "Hosted fence lost while polling");
   if (result != VK_NOT_READY)
      return result;
   if (abs_timeout_ns == 0)
      return VK_TIMEOUT;

   call_once(&vk_wddm2_pending_once, vk_wddm2_pending_init);
   mtx_lock(&vk_wddm2_pending_mtx);
   bool lost = false;
   /* BD-179: once per slice ask whether the device is lost: on a stopped
    * adapter nothing submits. The check runs at the end of each slice also
    * while submissions to other fences wake the wait. */
   uint64_t check_ns = os_time_get_nano() + vk_wddm2_monitored_fence_wait_slice_ns;
   for (;;) {
      result = vk_wddm2_monitored_fence_poll_pending(device, wait_count,
                                                     waits, wait_flags);
      if (result != VK_NOT_READY)
         break;

      const uint64_t now_ns = os_time_get_nano();
      if (now_ns >= abs_timeout_ns) {
         result = VK_TIMEOUT;
         break;
      }
      if (now_ns >= check_ns) {
         mtx_unlock(&vk_wddm2_pending_mtx);
         lost = vk_wddm2_wait_slice_device_lost(device);
         mtx_lock(&vk_wddm2_pending_mtx);
         if (lost)
            break;
         check_ns = now_ns + vk_wddm2_monitored_fence_wait_slice_ns;
         continue;
      }

      struct timespec deadline;
      timespec_get(&deadline, TIME_UTC);
      timespec_add_nsec(&deadline, &deadline,
                        MIN2(abs_timeout_ns, check_ns) - now_ns);
      if (cnd_timedwait(&vk_wddm2_pending_cnd, &vk_wddm2_pending_mtx,
                        &deadline) == thrd_error) {
         result = VK_ERROR_UNKNOWN;
         break;
      }
   }
   mtx_unlock(&vk_wddm2_pending_mtx);

   if (lost)
      return vk_device_set_lost(device,
         "Device lost during a pending wait (stopped adapter, BD-179)");
   if (result == VK_ERROR_DEVICE_LOST)
      return vk_device_set_lost(device, "Hosted fence lost while polling");
   if (result == VK_ERROR_UNKNOWN)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "cnd_timedwait failed");
   return result;
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

   for (uint32_t i = 0; i < signal_count; i++) {
      vk_wddm2_monitored_fence_note_submitted(to_wddm2_monitored_fence(signals[i].sync),
                                              signals[i].signal_value);
   }

   return VK_SUCCESS;
}
