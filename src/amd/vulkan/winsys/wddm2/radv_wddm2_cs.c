/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "radv_wddm2_cs.h"
#include "sid.h"
#include "radv_wddm2_bc250.h"
#include "radv_wddm2_bo.h"
#include "radv_winsys_cs.h"
#include "vk_async_event.h"
#include "vk_util.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/macros.h"
#include "util/set.h"
#include "util/u_memory.h"
#include <stdlib.h>
#include "ac_debug.h"
#include "radv_cs.h"

#include "amd/common/sid.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */

#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#else
#include "wsl/winadapter.h"
#endif

#include <assert.h>
#include <d3dkmthk.h>

/* One IB of a bc250 submission, in submission order. calls_ib2 is the flag of the
 * command stream it came from: such an IB runs at IB1 level, copied into the gather slot. */
struct bc250_submit_ib {
   struct radv_winsys_ib ib;
   bool calls_ib2;
};

struct PACKED create_context_private_data {
   uint32_t header_size;
   uint32_t flags;
   uint32_t reserved[11];
   uint32_t version;
   uint32_t dw13;
   uint32_t dw14;
   uint32_t section_size;
   uint32_t reserved2[89];
};
static_assert(sizeof(struct create_context_private_data) == 0x40 + 0x168, "This struct has no holes");

struct PACKED create_queue_private_data {
   uint32_t header_size;
   uint32_t reserved[3];
   uint32_t flags;
   uint32_t queue_id;
   uint32_t parent_queue_id;
   uint32_t reserved2[9];
};
static_assert(sizeof(struct create_queue_private_data) == 0x40, "This struct has no holes");

struct submit_pdd_writer {
   uint32_t num_entries;
   uint8_t *buffer;
   unsigned buffer_size;
   unsigned offset;
   uint8_t inline_data[256];
};

struct submit_pdd_header {
   uint32_t version;
   uint32_t num_entries;
};

struct submit_pdd_entry {
   uint32_t type;
   uint32_t size;
};

struct submit_pdd_gfx_ib {
   struct submit_pdd_entry base;
   uint32_t len;
   uint32_t flags;
   uint32_t _dw_2;
   uint32_t ip_type;
   uint32_t addr_lo;
   uint32_t addr_hi;
   uint32_t _dw_5;
   uint32_t _dw_7;
   uint32_t _dw_8;
   uint32_t _dw_9;
   uint32_t _dw_a;
   uint32_t _dw_b;
   uint32_t _dw_c;
   uint32_t _dw_d;
};
static_assert(sizeof(struct submit_pdd_gfx_ib) == 64, "This struct has no holes");

struct submit_pdd_gang_queue {
   struct submit_pdd_entry base;
   uint32_t always_1;
   uint32_t queue_id;
   uint32_t num_ibs;
   uint32_t _dw3;
};

struct submit_pdd_gang_ib {
   uint32_t len;
   uint32_t flags;
   uint32_t addr_lo;
   uint32_t addr_hi;
};

enum submit_pdd_entry_type {
   SUBMIT_PDD_ENTRY_GFX_IB = 0,
   SUBMIT_PDD_ENTRY_GANG_QUEUE = 6,
};

static void
submit_pdd_writer_init(struct submit_pdd_writer *writer)
{
   writer->num_entries = 0;
   writer->buffer = writer->inline_data;
   writer->buffer_size = sizeof(writer->inline_data);
   writer->offset = sizeof(struct submit_pdd_header);
}

static void
submit_pdd_writer_finalize(struct submit_pdd_writer *writer)
{
   struct submit_pdd_header *header = (struct submit_pdd_header *)writer->buffer;
   header->version = 0x9;
   header->num_entries = writer->num_entries;
}

static void
submit_pdd_writer_destroy(struct submit_pdd_writer *writer)
{
   if (writer->buffer != writer->inline_data)
      free(writer->buffer);
}

static void *
submit_pdd_writer_reserve(struct submit_pdd_writer *writer, unsigned size)
{
   if (writer->offset + size > writer->buffer_size) {
      uint8_t *old_buffer = writer->buffer;
      writer->buffer_size = MAX2(writer->buffer_size * 2, writer->offset + size);
      writer->buffer = malloc(writer->buffer_size);
      if (!writer->buffer)
         return NULL;
      memcpy(writer->buffer, old_buffer, writer->offset);
      if (old_buffer != writer->inline_data)
         free(old_buffer);
   }
   void *ptr = writer->buffer + writer->offset;
   memset(ptr, 0, size);
   writer->offset += size;
   return ptr;
}

static bool vk_wddm2_fence_wait(struct radv_wddm2_winsys *ws, struct vk_wddm2_fence *fence);

static void
radv_wddm2_queue_destroy(struct radv_wddm2_queue *queue)
{
   struct radv_wddm2_winsys *ws = queue->bc250_ws;
   radv_wddm2_sparse_groups_clear(queue);
   util_dynarray_fini(&queue->sparse_ops);
   if (queue->bc250_progress.handle) {
      if (queue->bc250_progress.wait_value)
         vk_wddm2_fence_wait(queue->bc250_ws, &queue->bc250_progress);
      /* BD-201: the progress fence goes next, so held BOs stop waiting on it first, and the drain
       * releases those whose work this wait retired. */
      radv_wddm2_tracker_detach(ws, queue->bc250_tracker, true);
      queue->bc250_tracker = NULL;
      radv_wddm2_deferred_drain(ws);
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_progress = {
         .hSyncObject = queue->bc250_progress.handle,
      };
      BC250_WDDM_CALL(&ws->host, DestroySynchronizationObject, &destroy_progress);
      queue->bc250_progress.handle = 0;
   }
   free(queue->bc250_ibs);
   queue->bc250_ibs = NULL;
   queue->bc250_ib_capacity = 0;
   for (unsigned i = 0; i < BC250_GATHER_SLOTS; i++) {
      if (queue->bc250_gather[i].bo)
         queue->bc250_ws->base.buffer_destroy(&queue->bc250_ws->base, queue->bc250_gather[i].bo);
      queue->bc250_gather[i].bo = NULL;
      queue->bc250_gather[i].map = NULL;
   }
   if (queue->vm_fence.handle) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_fence = {
         .hSyncObject = queue->vm_fence.handle,
      };
      BC250_WDDM_CALL(&ws->host, DestroySynchronizationObject, &destroy_fence);
      queue->vm_fence.handle = 0;
   }
   if (queue->context_h) {
      D3DKMT_DESTROYCONTEXT context_destroy = {
         .hContext = queue->context_h,
      };
      BC250_WDDM_CALL(&ws->host, DestroyContext, &context_destroy);
      queue->context_h = 0;
   }
   if (queue->handle) {
      D3DKMT_DESTROYHWQUEUE queue_destroy = {
         .hHwQueue = queue->handle,
      };
      BC250_WDDM_CALL(&ws->host, DestroyHwQueue, &queue_destroy);
      queue->handle = 0;
   }
}

static VkResult
radv_wddm2_queue_init(struct radv_wddm2_winsys *ws, enum amd_ip_type hw_ip,
                      enum radeon_ctx_priority priority, struct radv_wddm2_queue *parent,
                      struct radv_wddm2_queue *queue)
{
   NTSTATUS status;
   uint32_t node;

   queue->hw_ip = hw_ip;
   util_dynarray_init(&queue->sparse_ops, NULL);

   switch (hw_ip) {
   case AMD_IP_GFX:
      node = debug_get_num_option("RADV_DXGI_3D_NODE", 0);
      break;
   case AMD_IP_COMPUTE:
      node = debug_get_num_option("RADV_DXGI_COMPUTE_NODE", 2);
      break;
   default:
      /* Not supported */
      return VK_SUCCESS;
   }

   struct create_context_private_data create_context_data = {
      .header_size = 0x40,
      .flags = 0x100000,
      .dw13 = 0x200000,
      .section_size = 0x168,
   };
   struct bc250_context_blob bc250_ctx = {0};
   void *private_data = &create_context_data;
   UINT private_size = sizeof(create_context_data);

   if (ws->bc250) {
      /* GFX on node 0 and COMPUTE on node 2, the compute node, which the
       * caps offer (has_bc250_compute_node); ac_fill_hw_ip_info keeps the
       * COMPUTE IP only then. Node 1 is not a UMD queue. */
      if (hw_ip != AMD_IP_GFX && hw_ip != AMD_IP_COMPUTE)
         return VK_SUCCESS;
      /* A gang ACE queue (parent set) is a hardware queue under the GFX context, and
       * bc250kmd has no hardware-queue DDIs: no context for it. */
      if (hw_ip == AMD_IP_COMPUTE && (parent || !ws->gpu_info.ip[AMD_IP_COMPUTE].num_queues))
         return VK_SUCCESS;
      node = hw_ip == AMD_IP_COMPUTE ? BC250_COMPUTE_NODE : 0;
      bc250_ctx.magic = BC250_CONTEXT_MAGIC;
      bc250_ctx.version = 2;
      bc250_ctx.size = sizeof(bc250_ctx);
      _Static_assert(sizeof(bc250_ctx) == 80, "BC2C version 2 is 80 bytes");
      bc250_ctx.ip_type = hw_ip == AMD_IP_COMPUTE ? BC250_IP_COMPUTE : BC250_IP_GFX;
      bc250_ctx.node_ordinal = node;
      private_data = &bc250_ctx;
      private_size = sizeof(bc250_ctx);
   } else if (hw_ip == AMD_IP_GFX) {
      create_context_data.flags |= 0x40000;
      create_context_data.version = 9;
   }

   D3DKMT_CREATECONTEXTVIRTUAL create_context = {
      .hDevice = ws->device_h,
      .NodeOrdinal = node,
      .EngineAffinity = 1, // TODO
      .Flags = {
         .HwQueueSupported = !ws->bc250,
      },
      .pPrivateDriverData = private_data,
      .PrivateDriverDataSize = private_size,
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };

   status = BC250_WDDM_CALL(&ws->host, CreateContextVirtual, &create_context);
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Create context failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   queue->context_h = create_context.hContext;
   queue->bc250_ws = ws;

   /* bc250kmd has no hardware-queue DDIs. SubmitCommand is the packet path. */
   if (ws->bc250) {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_progress = {
         .hDevice = ws->device_h,
         .Info = {
            .Type = D3DDDI_MONITORED_FENCE,
            .MonitoredFence = { .InitialFenceValue = 0, .EngineAffinity = 1 },
         },
      };
      queue->bc250_ws = ws;
      status = BC250_WDDM_CALL(&ws->host, CreateSynchronizationObject2, &create_progress);
      if (!NT_SUCCESS(status))
         goto failed;
      queue->bc250_progress.handle = create_progress.hSyncObject;
      queue->bc250_progress.value_map = create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress;
      /* Packet submission also needs a distinct companion-paging fence.
       * The render progress fence protects CPU-repacked IBs and cannot share
       * ownership with UpdateGpuVirtualAddress increments. */
      create_progress.hSyncObject = 0;
      create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress = NULL;
      create_progress.Info.MonitoredFence.FenceValueGPUVirtualAddress = 0;
      status = BC250_WDDM_CALL(&ws->host, CreateSynchronizationObject2, &create_progress);
      if (!NT_SUCCESS(status))
         goto failed;
      queue->vm_fence.handle = create_progress.hSyncObject;
      queue->vm_fence.value_map = create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress;
      /* BD-201: the queue publishes, before each submission, the progress value that submission will
       * signal, and a destroyed BO waits for what every queue published. An untracked queue publishes
       * nothing, so no destroy would wait for its work and every BO it uses would be evicted and lose
       * its VA under running work. A queue that cannot be tracked is therefore not created: here the
       * failure is still reportable, at a destroy it is not. */
      queue->bc250_tracker = radv_wddm2_tracker_attach(ws, queue->bc250_progress.handle,
                                                       queue->bc250_progress.value_map, queue->context_h);
      if (!queue->bc250_tracker && ws->deferred.enabled && queue->bc250_progress.value_map) {
         radv_wddm2_queue_destroy(queue);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      return VK_SUCCESS;
   }

   if (hw_ip == AMD_IP_GFX || hw_ip == AMD_IP_COMPUTE) {
      struct create_queue_private_data create_queue_pdd = {
         .header_size = sizeof(struct create_queue_private_data),
         .flags = 0x21000,
      };

      if (parent) {
         create_queue_pdd.flags |= 0x80000;
         create_queue_pdd.parent_queue_id = parent->queue_id;
      }

      D3DKMT_CREATEHWQUEUE create_queue = {
         .hHwContext = queue->context_h,
         .pPrivateDriverData = &create_queue_pdd,
         .PrivateDriverDataSize = sizeof(create_queue_pdd),
      };
      status = BC250_WDDM_CALL(&ws->host, CreateHwQueue, &create_queue);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "Create queue failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
         goto failed;
      }
      queue->handle = create_queue.hHwQueue;
      queue->queue_id = create_queue_pdd.queue_id;

      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_sync = {
         .hDevice = ws->device_h,
         .Info = {
            .Type = D3DDDI_MONITORED_FENCE,
            .MonitoredFence = {
               .EngineAffinity = 1,
            },
         }
      };
      status = BC250_WDDM_CALL(&ws->host, CreateSynchronizationObject2, &create_sync);
      if (unlikely(!NT_SUCCESS(status))) {
         fprintf(stderr, "CreateSynchronizationObject2 failed with NTSTATUS 0x%x\n", status);
         goto failed;
      }
      queue->vm_fence.handle = create_sync.hSyncObject;
   }

   return VK_SUCCESS;

failed:
   radv_wddm2_queue_destroy(queue);
   return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult
radv_wddm2_ctx_create(struct radeon_winsys *_ws, enum radeon_ctx_priority priority,
                      struct radeon_winsys_ctx **rctx)
{
   VkResult result;
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (priority < RADEON_CTX_PRIORITY_LOW || priority > RADEON_CTX_PRIORITY_HIGH)
      return VK_ERROR_NOT_PERMITTED;

   struct radv_wddm2_ctx *ctx = CALLOC_STRUCT(radv_wddm2_ctx);
   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   ctx->ws = ws;
   ctx->priority = priority;

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      /* bc250: the node 2 context comes with the first COMPUTE submission
       * (radv_wddm2_cs_submit). A device that never uses the compute family
       * holds no node 2 context, and a closed or faulted node 2 cannot fail
       * the creation of a device that renders on node 0 only. */
      if (ws->bc250 && ip == AMD_IP_COMPUTE) {
         ctx->per_ip[ip].queue.hw_ip = ip;
         ctx->per_ip[ip].queue.bc250_ws = ws;
         util_dynarray_init(&ctx->per_ip[ip].queue.sparse_ops, NULL);
         continue;
      }
      result = radv_wddm2_queue_init(ws, ip, priority, NULL, &ctx->per_ip[ip].queue);
      if (result != VK_SUCCESS)
         goto fail_contexts;
   }

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;

fail_contexts:
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);

   FREE(ctx);
   return result;
}

static void
radv_wddm2_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);
   radv_wddm2_queue_destroy(&ctx->ace_queue);

   FREE(ctx);
}

/* The bounds of a CPU wait for one of the queue's own fences. The host tests
 * shorten them; nothing else writes them. */
uint64_t radv_wddm2_fence_wait_slice_ns = 1000000000ull;   /* 1 s between liveness checks */
uint64_t radv_wddm2_fence_wait_total_ns = 120000000000ull; /* 120 s: well past a TDR and its reset */

/* How many device-state queries may fail in a row before the wait ends. One
 * unanswered query is no evidence of a loss, and a second may still be a
 * transient failure; a device that keeps not answering cannot be waited out
 * to the total bound, because its wait will not be signalled either. */
#define RADV_WDDM2_UNANSWERED_STATE_MAX 3u

/* Whether the device can no longer complete this work. A hosted device has no
 * kernel device of its own: the host status and the fences tell its state,
 * and the host does not answer GetDeviceState.
 *
 * On a kernel device the query itself can fail. D3DKMTGetDeviceState
 * documents STATUS_DEVICE_REMOVED ("the graphics adapter was stopped or the
 * display device was reset") and STATUS_INVALID_PARAMETER, and may return
 * other NTSTATUS values. A removed device, or one whose handle the kernel no
 * longer knows, is gone: its async wait will never be signalled, so that is a
 * loss at once and the caller must not be held for the total bound. Any other
 * failure may be transient and is tolerated, up to
 * RADV_WDDM2_UNANSWERED_STATE_MAX in a row; after that the wait ends too,
 * which is what the query after the wait does with the same event. unanswered
 * counts the consecutive failures within one wait.
 */
static bool
radv_wddm2_device_lost(struct radv_wddm2_winsys *ws, unsigned *unanswered)
{
   if (ws->host.dispatch)
      return false;
   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   NTSTATUS status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
   if (unlikely(!NT_SUCCESS(status))) {
      if (status == STATUS_DEVICE_REMOVED || status == STATUS_INVALID_HANDLE) {
         fprintf(stderr, "radv/wddm2: GetDeviceState(EXECUTION): 0x%X, the device is gone\n", status);
         return true;
      }
      if (++*unanswered < RADV_WDDM2_UNANSWERED_STATE_MAX) {
         fprintf(stderr, "radv/wddm2: GetDeviceState(EXECUTION): 0x%X, unanswered %u of %u\n", status,
                 *unanswered, RADV_WDDM2_UNANSWERED_STATE_MAX);
         return false;
      }
      fprintf(stderr, "radv/wddm2: GetDeviceState(EXECUTION): 0x%X, %u unanswered in a row: the device is lost\n",
              status, *unanswered);
      return true;
   }
   *unanswered = 0;
   return get_state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE;
}

static bool
vk_wddm2_fence_wait(struct radv_wddm2_winsys *ws, struct vk_wddm2_fence *fence)
{
   VkResult result;
   NTSTATUS status;
   HANDLE async_event = 0;

   /* Quick poll all the fences ourselves.  We may not have to call into the
    * kernel at all.
    */
   uint64_t observed=p_atomic_read(fence->value_map);
   if (!bc250_host_fence_valid(&ws->host,observed)) return false;
   if (observed >= fence->wait_value) return true;

   result = vk_async_event_create(&async_event);
   if (unlikely(result != VK_SUCCESS))
      return false;

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &fence->handle,
      .FenceValueArray = &fence->wait_value,
      .hAsyncEvent = async_event,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);

   if (unlikely(!NT_SUCCESS(status))) {
      vk_async_event_close(async_event);
      fprintf(stderr, "fence wait failed: 0x%X\n", status);
      return false;
   }

   /* A wait that runs past one slice is not a device loss. When another
    * device hangs the engine, the work of this device waits behind it until
    * the scheduler resets the engine and resubmits the render packets of the
    * other devices with new fence ids. Only the device of the hung packet
    * goes into the error state ("TDR changes in Windows 8", engine reset).
    * That takes TdrDelay and the reset: 14.6 s in lab trial D1 of KMD
    * 0.7.216.17 with TdrDelay 10, which a single 10 s wait turned into a
    * lost DWM device (K225). So wait in slices and stop early only when the
    * device is lost: the fence reads UINT64_MAX, the host reports the loss,
    * or the kernel device is not in the ACTIVE execution state (and a kernel
    * device that is gone, or that stops answering at all, is lost as well).
    */
   uint64_t waited_ns = 0;
   const uint64_t slice_ns = MAX2(radv_wddm2_fence_wait_slice_ns, 1000000ull);
   const uint64_t total_ns = MAX2(radv_wddm2_fence_wait_total_ns, slice_ns);
   unsigned unanswered = 0;
   bool lost = false;
   for (;;) {
      result = vk_async_event_wait(async_event, MIN2(slice_ns, total_ns - waited_ns));
      if (result != VK_TIMEOUT)
         break;
      waited_ns += MIN2(slice_ns, total_ns - waited_ns);
      observed = p_atomic_read(fence->value_map);
      if (!bc250_host_fence_valid(&ws->host, observed) || observed == UINT64_MAX ||
          radv_wddm2_device_lost(ws, &unanswered)) {
         lost = true;
         break;
      }
      if (observed >= fence->wait_value) {
         result = VK_SUCCESS;
         break;
      }
      if (waited_ns >= total_ns)
         break;
      if (waited_ns == slice_ns)
         fprintf(stderr,
                 "radv/wddm2: fence %u value %" PRIu64 " pending after %" PRIu64
                 " ms, device active: waiting on\n",
                 fence->handle, fence->wait_value, waited_ns / 1000000);
   }
   vk_async_event_close(async_event);
   if (result != VK_SUCCESS)
      fprintf(stderr, "async wait event: 0x%x after %" PRIu64 " ms%s\n", result, waited_ns / 1000000,
              lost ? ", device lost" : "");
   else if (waited_ns >= slice_ns)
      fprintf(stderr, "radv/wddm2: fence %u value %" PRIu64 " completed after %" PRIu64 " ms of wait\n",
              fence->handle, fence->wait_value, waited_ns / 1000000);

   /* Read the fence back before asking the kernel anything. A wait that returned with the fence valid and at
    * or past its wait value is the ordinary case: the work is done, the device is executing, and the
    * execution state can say nothing the caller does not already know. GetDeviceState is a kernel call on
    * the wait path, so that case must not pay for it. Query the device only for the two shapes in which the
    * state is worth reading: a wait that failed or timed out, and a wait that returned while the fence did
    * not advance to its wait value (or reads the host's invalid value).
    */
   observed = p_atomic_read(fence->value_map);
   if (result == VK_SUCCESS && bc250_host_fence_valid(&ws->host, observed) && observed >= fence->wait_value)
      return true;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
   if (unlikely(!NT_SUCCESS(status))) {
      fprintf(stderr, "GetDeviceState(EXECUTION): 0x%X\n", status);
   } else if (get_state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE) {
      fprintf(stderr, "device execution state: %i\n", get_state.ExecutionState);

      if (get_state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
         get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
         status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
         if (unlikely(!NT_SUCCESS(status))) {
            fprintf(stderr, "GetDeviceState(PAGE_FAULT): 0x%X\n", status);
            return false;
         }
         D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

         fprintf(stderr, "faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i), flags: %i, stage: %i\n",
                fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode,
                fault.FaultErrorCode.DeviceSpecificCode, fault.PageFaultFlags, fault.FaultedPipelineStage);
      }
      return false;
   }

   return result == VK_SUCCESS && bc250_host_fence_valid(&ws->host,p_atomic_read(fence->value_map));
}

/* One CPU wait on a progress fence, for deferred destruction (radv_wddm2_bo.c, BD-201). */
bool
radv_wddm2_fence_wait_value(struct radv_wddm2_winsys *ws, uint32_t fence, const uint64_t *value_map, uint64_t value)
{
   struct vk_wddm2_fence wait = {
      .handle = fence,
      .wait_value = value,
      .value_map = (uint64_t *)value_map,
   };
   return vk_wddm2_fence_wait(ws, &wait);
}

static bool
radv_wddm2_ctx_wait_idle(struct radeon_winsys_ctx *rwctx, enum amd_ip_type ip_type, int ring_index)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   bool ret = true;

   if (ctx->per_ip[ip_type].last_submission.handle)
      ret = vk_wddm2_fence_wait(ctx->ws, &ctx->per_ip[ip_type].last_submission);

   /* An idle queue has retired what it published: the held BOs of its work may go (BD-201). */
   radv_wddm2_deferred_drain(ctx->ws);

   return ret;
}

struct radv_wddm2_cs {
   struct radv_winsys_cs base;
   struct set *buffers;
};

static inline struct radv_wddm2_cs *
radv_wddm2_cs(struct ac_cmdbuf *base)
{
   return (struct radv_wddm2_cs *)base;
}

static enum radeon_bo_domain
radv_wddm2_cs_domain(const struct radeon_winsys *_ws)
{
   return RADEON_DOMAIN_GTT;
}

static struct ac_cmdbuf *
radv_wddm2_cs_create(struct radeon_winsys *rws, enum amd_ip_type ip_type, bool is_secondary)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(rws);
   struct radv_wddm2_cs *cs;

   cs = calloc(1, sizeof(struct radv_wddm2_cs));
   if (!cs)
      return NULL;

   cs->buffers = _mesa_pointer_set_create(NULL);

   VkResult result = radv_winsys_cs_init(&cs->base, rws, ip_type, is_secondary, ws->chain_ib);
   if (result != VK_SUCCESS) {
      _mesa_set_destroy(cs->buffers, NULL);
      free(cs);
      return NULL;
   }

   _mesa_set_add(cs->buffers, radv_wddm2_bo(cs->base.ib_buffer));

   return &cs->base.base;
}

static unsigned
radv_wddm2_cs_num_ibs(UNUSED const struct radv_wddm2_cs *cs)
{
   return 1;
}

static uint32_t
radv_wddm2_cs_translate_ip_type(enum amd_ip_type ip_type)
{
   return (ip_type == AMD_IP_SDMA) ? 0 : 1;
}

static void
radv_wddm2_cs_destroy(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_destroy(&cs->base);
   _mesa_set_destroy(cs->buffers, NULL);

   FREE(cs);
}

static void
radv_wddm2_cs_reset(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_reset(&cs->base);
   _mesa_set_clear(cs->buffers, NULL);
}

static void
radv_wddm2_cs_add_buffer(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   _mesa_set_add(cs->buffers, radv_wddm2_bo(_bo));
}

static void
radv_wddm2_cs_execute_secondary(struct ac_cmdbuf *_parent, struct ac_cmdbuf *_child,
                                bool allow_ib2)
{
   radv_winsys_cs_execute_secondary(radv_winsys_cs(_parent), radv_winsys_cs(_child), allow_ib2);
}

static void
radv_wddm2_get_cpu_addr(void *_cs, uint64_t addr, struct ac_addr_info *info)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(cs->base.ws);

   memset(info, 0, sizeof(struct ac_addr_info));

   if (ws->debug_log_bos) {
      bool destroyed = false;

      if (radv_winsys_bo_log_find(&ws->bo_log, addr, &destroyed))
         info->use_after_free = destroyed;
   }

   if (info->use_after_free)
      return;

   info->valid = !ws->debug_all_bos;

   if (radv_winsys_cs_get_cpu_addr(&cs->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }

   if (radv_winsys_bo_list_get_cpu_addr(&ws->global_bo_list, &ws->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }
}

#define WRITE_STRUCT(pdd, type, item) \
   for (struct submit_pdd_##type *item = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##type)); \
        item != NULL; \
        item = NULL)

#define WRITE_ENTRY(pdd, TYPE, typ, entry) \
   for (struct submit_pdd_##typ *entry = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##typ)); \
        entry != NULL && \
        (((struct submit_pdd_entry *) entry)->size = sizeof(struct submit_pdd_##typ)); \
        ((struct submit_pdd_entry *) entry)->type = SUBMIT_PDD_ENTRY_##TYPE, \
        (pdd)->num_entries++, \
        entry = NULL)

static void
radv_wddm2_submit_add_cs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                         struct radv_winsys_cs *cs, enum radv_cs_dump_type type, bool is_gang,
                         struct radv_winsys_ib *first)
{
   const unsigned num_ib_buffers = cs->chain_ib ? 1 : cs->num_ib_buffers;
   uint32_t flags = 0;

   if (type == RADV_CS_DUMP_TYPE_PREAMBLE_IBS)
      flags = 0xc;
   else if (type == RADV_CS_DUMP_TYPE_POSTAMBLE_IBS)
      flags = 0x104;

   if (ctx->ws->dump_ibs)
      ctx->ws->base.cs_dump(&cs->base, stderr, NULL, 0, type);

   for  (unsigned i = 0; i < num_ib_buffers; i++) {
      struct radv_winsys_ib ib = cs->ib_buffers[i];

      if (first->va == 0)
         *first = ib;

      if (is_gang) {
         WRITE_STRUCT(pdd, gang_ib, gang_ib) {
            gang_ib->len = ib.cdw * 4;
            gang_ib->flags = flags;
            gang_ib->addr_lo = ib.va;
            gang_ib->addr_hi = ib.va >> 32;
         }
      } else {
         WRITE_ENTRY(pdd, GFX_IB, gfx_ib, entry) {
            fprintf(stderr, "Adding IB with VA 0x%" PRIx64 " and length %u bytes\n", ib.va, ib.cdw * 4);
            entry->len = ib.cdw * 4;
            entry->flags = flags;
            entry->ip_type = radv_wddm2_cs_translate_ip_type(cs->hw_ip);
            entry->addr_lo = ib.va;
            entry->addr_hi = ib.va >> 32;
         }
      }
   }
}

static unsigned
radv_wddm2_count_submitted_ibs(const struct radv_winsys_submit_info *submit, unsigned hw_ip)
{
   unsigned num_ibs = 0;

   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   return num_ibs;
}

static void
radv_wddm2_cs_submit_add_ibs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, unsigned hw_ip,
                             struct radv_winsys_ib *first_ib)
{
   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_PREAMBLE_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_MAIN_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_POSTAMBLE_IBS,
                               submit->is_gang, first_ib);
   }
}

static void
radv_wddm2_submit_add_queue(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, struct radv_wddm2_queue *queue,
                             struct radv_winsys_ib *first_ib)
{
   WRITE_ENTRY(pdd, GANG_QUEUE, gang_queue, entry) {
      entry->num_ibs = radv_wddm2_count_submitted_ibs(submit, queue->hw_ip);
      entry->base.size += entry->num_ibs * sizeof(struct submit_pdd_gang_ib);
      entry->always_1 = 1;
      entry->queue_id = queue->queue_id;
   }

   radv_wddm2_cs_submit_add_ibs(ctx, pdd, submit, queue->hw_ip, first_ib);
}

/* A chained CS is one IB: the CP follows the rest. An unchained CS is every piece, in order.
 * A NULL output counts the complete submission before allocating its collection.
 */
static bool
bc250_collect_cs(struct radv_winsys_cs *cs, struct bc250_submit_ib *out, unsigned *n, unsigned cap)
{
   unsigned count = cs->chain_ib ? 1 : cs->num_ib_buffers;
   unsigned i;
   if (*n > cap || count > cap - *n) {
      fprintf(stderr, "bc250: gather IB limit used=%u add=%u cap=%u\n", *n, count, cap);
      return false;
   }
   if (out) {
      for (i = 0; i < count; i++) {
         out[*n + i].ib = cs->ib_buffers[i];
         out[*n + i].calls_ib2 = cs->calls_ib2;
      }
   }
   *n += count;
   return true;
}

static bool
bc250_collect_array(struct ac_cmdbuf **arr, unsigned count, struct bc250_submit_ib *out, unsigned *n, unsigned cap)
{
   unsigned i;
   for (i = 0; i < count; i++) {
      if (!bc250_collect_cs(radv_winsys_cs(arr[i]), out, n, cap))
         return false;
   }
   return true;
}

static bool
bc250_collect_submit(const struct radv_winsys_submit_info *submit,
                      struct bc250_submit_ib *ibs, unsigned *n, unsigned cap)
{
   /* Same order as the amdgpu winsys: initial preamble on the first CS, the continue
    * preamble on every later CS in this submit, postamble once at the end.
    */
   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct ac_cmdbuf **preambles = i ? submit->continue_preamble_cs : submit->initial_preamble_cs;
      unsigned preamble_count = i ? submit->continue_preamble_count : submit->initial_preamble_count;
      if (!bc250_collect_array(preambles, preamble_count, ibs, n, cap) ||
          !bc250_collect_cs(radv_winsys_cs(submit->cs_array[i]), ibs, n, cap))
         return false;
   }
   if (!bc250_collect_array(submit->postamble_cs, submit->postamble_count, ibs, n, cap))
      return false;
   return true;
}

/* Whether the CP may run this IB as an IB2 called from the gather slot: it calls no IB2
 * itself (there is no third level), and its address and size fit IB_BASE (dword aligned)
 * and IB_SIZE. The caller checked that the IB lies inside its BO (bc250_ib_in_bo).
 */
static bool
bc250_ib2_callable(const struct bc250_submit_ib *s)
{
   return !s->calls_ib2 && (s->ib.va & 3) == 0 && s->ib.cdw <= G_3F3_IB_SIZE(UINT32_MAX);
}

/* Whether all dwords of the IB lie inside its BO. The checks subtract, so that no sum can wrap.
 * An IB that starts below its BO is refused too: the copy read the start of the BO for it before.
 */
static bool
bc250_ib_in_bo(const struct radv_winsys_ib *ib)
{
   if (!ib->bo || ib->va < ib->bo->va)
      return false;
   const uint64_t off = ib->va - ib->bo->va;
   return off <= ib->bo->size && (uint64_t)ib->cdw * 4 <= ib->bo->size - off;
}

/* The CS BO bytes of one IB, NULL if the IB is not inside a mapped BO. */
static const uint8_t *
bc250_ib_bytes(struct radv_wddm2_winsys *ws, const struct radv_winsys_ib *ib)
{
   if (!bc250_ib_in_bo(ib))
      return NULL;
   const uint8_t *src = ws->base.buffer_map(&ws->base, ib->bo, false, NULL);
   return src ? src + (ib->va - ib->bo->va) : NULL;
}

/* One IB1 on the gfx ring: the KMD runs the single IB of a BC2S blob (driver/kmd/umd_blob.c,
 * single_ib) and puts its own frame and fence around it on the ring. A submission of several
 * IBs is packed into a gather BO of the queue. By default the gather BO holds one IB2 call
 * (INDIRECT_BUFFER without CHAIN, the form RADV uses for secondaries) per IB, and the CP reads
 * the command streams where RADV recorded them; an IB whose stream calls an IB2 itself is
 * copied into the gather BO instead and runs at IB1 level. The lab flavour's BC250_IB_NOCOPY=0
 * copies every IB, the previous behaviour.
 *
 * Every IB of a submission was copied before (BD-207): the CPU read write-combined command
 * buffers on each submission, and a submission of more than 1048575 dwords, the IB_SIZE field,
 * was refused, which the application saw as VK_ERROR_DEVICE_LOST. vkd3d-proton 3.0.1 sent
 * 1101808 dwords in one submission of Cyberpunk 2077. The calls make the IB1 four dwords per IB.
 *
 * The IB2 calls leave the command streams in use until this submission retires. Vulkan keeps
 * an application command buffer pending until the application's signal, which is queued after
 * this IB1 on the same context. RADV can destroy other streams earlier: radv_update_preamble_cs
 * destroys a replaced queue preamble with no wait. A destroyed command BO goes through
 * DestroyAllocation2, which the OS defers until the work queued before it finishes, and a VA
 * that the winsys owns stays reserved until that work retires (radv_wddm2_bo.c, BD-201). So
 * the CP never reads a released stream.
 */
static NTSTATUS
radv_wddm2_bc250_submit(struct radv_wddm2_ctx *ctx, struct radv_wddm2_queue *queue,
                        const struct radv_winsys_submit_info *submit)
{
   struct radv_wddm2_winsys *ws = ctx->ws;
   struct bc250_submit_ib local_ibs[16];
   struct bc250_submit_ib *ibs = local_ibs;
   struct bc250_submit_blob blob;
   unsigned n = 0, i;
   unsigned calls = 0, copies = 0, nested = 0;
   uint64_t total = 0;
   uint64_t va;
   uint32_t bytes;
   NTSTATUS status;
   D3DKMT_SUBMITCOMMAND cmd;

   unsigned required = 0;
   if (!bc250_collect_submit(submit, NULL, &required, UINT_MAX))
      return STATUS_INVALID_PARAMETER;
   if (required > ARRAY_SIZE(local_ibs)) {
      if (required > queue->bc250_ib_capacity) {
         size_t capacity = MAX2((size_t)required, (size_t)queue->bc250_ib_capacity * 2);
         if (capacity > UINT_MAX || capacity > SIZE_MAX / sizeof(*ibs))
            return STATUS_NO_MEMORY;
         void *replacement = realloc(queue->bc250_ibs, capacity * sizeof(*ibs));
         if (!replacement)
            return STATUS_NO_MEMORY;
         queue->bc250_ibs = replacement;
         queue->bc250_ib_capacity = (unsigned)capacity;
         fprintf(stderr, "bc250: IB collection capacity=%u required=%u\n",
                 queue->bc250_ib_capacity, required);
      }
      ibs = queue->bc250_ibs;
   }
   if (!bc250_collect_submit(submit, ibs, &n, required))
      return STATUS_INVALID_PARAMETER;
   if (n == 0)
      return STATUS_SUCCESS;

   const bool direct = n == 1;
   if (direct) {
      /* The KMD runs this IB as the IB1, so it gets the same range check as a gathered IB. */
      if (!bc250_ib_in_bo(&ibs[0].ib))
         return STATUS_INVALID_PARAMETER;
      va = ibs[0].ib.va;
      bytes = ibs[0].ib.cdw * 4;
   } else {
      /* The compute ring runs no IB2 (upstream RADV calls IB2 on the gfx IP only,
       * radv_winsys_cs_execute_secondary): every IB of a COMPUTE submission is
       * copied into the IB1. */
      const bool copy_all = ws->bc250_gather_copy || queue->hw_ip == AMD_IP_COMPUTE;
      uint64_t copied_dw = 0;
      unsigned pad = 0;
      uint8_t *dst;
      for (i = 0; i < n; i++) {
         const struct radv_winsys_ib *ib = &ibs[i].ib;
         if (!bc250_ib_bytes(ws, ib))
            return STATUS_INVALID_PARAMETER;
         if (!copy_all && !ib->cdw)
            continue; /* nothing to call */
         if (copy_all || !bc250_ib2_callable(&ibs[i])) {
            copied_dw += ib->cdw;
            copies++;
            nested += ibs[i].calls_ib2;
         } else {
            calls++;
         }
      }
      total = copied_dw + 4ull * calls;
      /* The IB1 ends on the IB padding of the GFX queue, as every RADV IB does. Copied IBs are
       * already padded; four-dword calls may leave half a unit. */
      if (!copy_all) {
         const uint32_t pad_mask = ws->gpu_info.ip[queue->hw_ip].ib_pad_dw_mask;
         pad = (unsigned)((pad_mask + 1u - (total & pad_mask)) & pad_mask);
         total += pad;
      }
      /* Match the GFX INDIRECT_BUFFER IB_SIZE field, not an arbitrary 1 MiB
       * staging limit. The caller retired the selected gather slot before any write or resize. */
      if (total == 0 || total > G_3F3_IB_SIZE(UINT32_MAX)) {
         fprintf(stderr, "bc250: gather exceeds IB_SIZE field: %" PRIu64 " dwords (%u calls, %u copies)\n",
                 total, calls, copies);
         return STATUS_INVALID_PARAMETER;
      }
      bytes = (uint32_t)(total * 4);
      if (!queue->bc250_gather[queue->bc250_gather_index].bo || queue->bc250_gather[queue->bc250_gather_index].bo->size < bytes) {
         struct radeon_winsys_bo *replacement = NULL;
         uint64_t capacity = align64(bytes, 4096);
         VkResult result = ws->base.buffer_create(&ws->base, capacity, 4096, RADEON_DOMAIN_GTT,
                                                  RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING |
                                                     RADEON_FLAG_INTERNAL,
                                                  0, 0, NULL, &replacement);
         if (result != VK_SUCCESS)
            return STATUS_NO_MEMORY;
         uint8_t *mapping = ws->base.buffer_map(&ws->base, replacement, false, NULL);
         if (!mapping) {
            ws->base.buffer_destroy(&ws->base, replacement);
            return STATUS_NO_MEMORY;
         }
         if (queue->bc250_gather[queue->bc250_gather_index].bo)
            ws->base.buffer_destroy(&ws->base, queue->bc250_gather[queue->bc250_gather_index].bo);
         queue->bc250_gather[queue->bc250_gather_index].bo = replacement;
         queue->bc250_gather[queue->bc250_gather_index].map = mapping;
         fprintf(stderr, "bc250: gather capacity=%" PRIu64 " bytes required=%u ibs=%u calls=%u copies=%u\n",
                 capacity, bytes, n, calls, copies);
      }
      dst = queue->bc250_gather[queue->bc250_gather_index].map;
      for (i = 0; i < n; i++) {
         const struct radv_winsys_ib *ib = &ibs[i].ib;
         if (!copy_all && !ib->cdw)
            continue;
         if (copy_all || !bc250_ib2_callable(&ibs[i])) {
            /* Checked above; the mapping is the BO's persistent one. */
            memcpy(dst, bc250_ib_bytes(ws, ib), ib->cdw * 4);
            dst += ib->cdw * 4;
         } else {
            /* IB2: header, IB_BASE_LO, IB_BASE_HI, IB_SIZE. No CHAIN and no VMID, as RADV's
             * IB2 for secondaries (radv_winsys_cs_emit_secondary_ib2); the CP runs it at the
             * IB1's VMID, which the KMD put on the ring. */
            const uint32_t packet[4] = {
               PKT3(PKT3_INDIRECT_BUFFER, 2, 0),
               (uint32_t)ib->va,
               (uint32_t)(ib->va >> 32),
               S_3F3_IB_SIZE(ib->cdw),
            };
            memcpy(dst, packet, sizeof(packet));
            dst += sizeof(packet);
         }
      }
      if (pad == 1) {
         const uint32_t nop = PKT3_NOP_PAD;
         memcpy(dst, &nop, 4);
      } else if (pad > 1) {
         const uint32_t nop = PKT3(PKT3_NOP, pad - 2, 0);
         memcpy(dst, &nop, 4);
         memset(dst + 4, 0, (pad - 1) * 4);
      }
      va = queue->bc250_gather[queue->bc250_gather_index].bo->va;

      if (!copy_all && copies) {
         const uint64_t count = p_atomic_inc_return(&ws->bc250_inline_submits);
         if (count <= 16 || util_is_power_of_two_or_zero64(count))
            fprintf(stderr,
                    "bc250: IB2 fallback: %u of %u IBs copied into the IB1 (%u call an IB2 themselves, %u outside "
                    "IB_BASE/IB_SIZE), %" PRIu64 " dwords; %" PRIu64 " such submissions so far\n",
                    copies, n, nested, copies - nested, copied_dw, count);
      }
   }

   /* BC250_IB_DWORDS cuts the IB the CP executes. 5 is CONTEXT_CONTROL plus
    * CLEAR_STATE, a packet boundary. The ring fence still follows the IB, so a
    * timeout here means the CP did not finish that prefix. Unset, the whole IB runs.
    * Read once by a lab build (radv_wddm2_cs_init_functions), where it also selects the
    * gather copy; the release flavour reads no variable (BD-167) and the whole IB runs. */
   {
      const unsigned long cap = ws->bc250_ib_dwords_cap;
      if (cap > 0 && cap < 0x100000ul && cap * 4ul < bytes) {
         fprintf(stderr, "bc250: IB clamped to %lu dwords (%u were ready)\n", cap, bytes / 4u);
         bytes = (uint32_t)(cap * 4ul);
      }
   }

   memset(&blob, 0, sizeof(blob));
   _Static_assert(offsetof(struct bc250_submit_blob, ib) == 40, "BC2S prefix is 40 bytes");
   _Static_assert(sizeof(struct bc250_ib_blob) == 32, "BC2S ib is 32 bytes");
   blob.magic = BC250_SUBMIT_MAGIC;
   blob.version = 1;
   blob.num_ibs = 1;
   /* bc250kmd runs a BC2S blob only on the node of its IP: GFX on node 0, COMPUTE on node 2. */
   blob.ip_type = queue->hw_ip == AMD_IP_COMPUTE ? BC250_IP_COMPUTE : BC250_IP_GFX;
   blob.size = 40 + sizeof(struct bc250_ib_blob);
   blob.ib[0].va_start = va;
   blob.ib[0].ib_bytes = bytes;
   blob.ib[0].ip_type = blob.ip_type;

   memset(&cmd, 0, sizeof(cmd));
   cmd.Commands = va;
   cmd.CommandLength = bytes;
   cmd.pPrivateDriverData = &blob;
   cmd.PrivateDriverDataSize = blob.size;
   cmd.BroadcastContextCount = 1;
   cmd.BroadcastContext[0] = queue->context_h;
   /* Write-combined stores sit in the WC buffers until a fence. The syscall drains them
    * too; this one makes the order obvious.
    */
   _mm_sfence();
   status = BC250_WDDM_CALL(&ws->host, SubmitCommand, &cmd);
   if (!NT_SUCCESS(status))
      fprintf(stderr, "bc250: SubmitCommand 0x%X (%u ibs packed, %u bytes)\n", status, n, bytes);
   else if (ws->bc250_trace_submits) {
      const uint32_t *dw = NULL;
      if (direct && ibs[0].ib.bo)
         dw = (const uint32_t *)((uint8_t *)ws->base.buffer_map(&ws->base, ibs[0].ib.bo, false, NULL) +
                                 (ibs[0].ib.va - ibs[0].ib.bo->va));
      else
         dw = (const uint32_t *)queue->bc250_gather[queue->bc250_gather_index].map;
      fprintf(stderr, "bc250: SubmitCommand ib 0x%" PRIx64 " %u bytes (from %u) %08x %08x %08x %08x\n",
              va, bytes, n,
              dw ? dw[0] : 0, dw && bytes >= 8 ? dw[1] : 0,
              dw && bytes >= 12 ? dw[2] : 0, dw && bytes >= 16 ? dw[3] : 0);
      if (!direct)
         fprintf(stderr, "bc250: IB1 holds %u IB2 calls and %u copied IBs\n", calls, copies);
      /* Slices of the unclamped preamble and the main CS, when the IB1 is their copy. 163 is
       * the first ACQUIRE_MEM, 176 the main IB, 216 the dispatch. */
      if (calls)
         dw = NULL;
      if (dw && bytes >= 176u * 4u) {
         fprintf(stderr, "bc250: IB +158 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[158], dw[159], dw[160], dw[161], dw[162], dw[163], dw[164], dw[165]);
         fprintf(stderr, "bc250: IB +166 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[166], dw[167], dw[168], dw[169], dw[170], dw[171], dw[172], dw[173]);
         fprintf(stderr, "bc250: IB +174 %08x %08x\n", dw[174], dw[175]);
      }
      if (dw && bytes >= 216u * 4u) {
         fprintf(stderr, "bc250: IB +176 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[176], dw[177], dw[178], dw[179], dw[180], dw[181], dw[182], dw[183]);
         fprintf(stderr, "bc250: IB +184 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[184], dw[185], dw[186], dw[187], dw[188], dw[189], dw[190], dw[191]);
         fprintf(stderr, "bc250: IB +192 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[192], dw[193], dw[194], dw[195], dw[196], dw[197], dw[198], dw[199]);
         fprintf(stderr, "bc250: IB +200 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[200], dw[201], dw[202], dw[203], dw[204], dw[205], dw[206], dw[207]);
         fprintf(stderr, "bc250: IB +208 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[208], dw[209], dw[210], dw[211], dw[212], dw[213], dw[214], dw[215]);
      }
      if (dw && bytes >= 240u * 4u) {
         fprintf(stderr, "bc250: IB +216 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[216], dw[217], dw[218], dw[219], dw[220], dw[221], dw[222], dw[223]);
         fprintf(stderr, "bc250: IB +224 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[224], dw[225], dw[226], dw[227], dw[228], dw[229], dw[230], dw[231]);
         fprintf(stderr, "bc250: IB +232 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[232], dw[233], dw[234], dw[235], dw[236], dw[237], dw[238], dw[239]);
      }
   }
   return status;
}

/* amdgpu-wddm: the private driver data of a submission, as hex, on stderr, only with AMDGPU_WDDM_LOG_PDD=1, the
 * variable of the triplet line's dump. Only the submission path of a device without the BC-250 caps blob
 * (ws->bc250 false) builds that data. There, without the gate, every submission formatted a line per 32 bytes of it,
 * for a stderr that a game does not read. */
static bool
radv_wddm2_pdd_lines(void)
{
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
   static int mode = -1; /* benign race: every thread computes the same value */
   if (mode < 0) {
      const char *value = getenv("AMDGPU_WDDM_LOG_PDD");
      mode = value && !strcmp(value, "1");
   }
   return mode == 1;
#else
   /* The release flavour reads no diagnostic variable (BD-167): no dump, as with it unset. */
   return false;
#endif
}

/* R6 (BD-201, DESIGN.md 6.4): the value a submission was to signal stays published when the submission
 * or its signal call fails, because a destroy running next to the submit may already have captured it
 * and a published value is never withdrawn. A value nothing will ever signal would hold those ranges for
 * the life of the queue, so a failure is followed by a repair signal of the same value on the same
 * queue: the GPU, or the kernel on its behalf, writes it exactly once more. The repair is never an
 * artificial completion by the CPU: it asks the same engine for the same value, and a repair that cannot
 * be sent is counted and said so, which leaves that value unproven until the queue is released. */
static void
radv_wddm2_progress_repair(struct radv_wddm2_winsys *ws, struct radv_wddm2_queue *queue, uint64_t value)
{
   if (!value || !queue->bc250_progress.handle)
      return;
   NTSTATUS status;
   if (queue->handle) {
      const D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE repair = {
         .BroadcastHwQueueCount = 1,
         .BroadcastHwQueueArray = &queue->handle,
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->bc250_progress.handle,
         .FenceValueArray = &value,
      };
      status = BC250_WDDM_CALL(&ws->host, SubmitSignalSyncObjectsToHwQueue, &repair);
   } else if (queue->context_h) {
      const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 repair = {
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->bc250_progress.handle,
         .BroadcastContextCount = 1,
         .BroadcastContextArray = &queue->context_h,
         .MonitoredFenceValueArray = &value,
      };
      status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &repair);
   } else {
      status = STATUS_INVALID_HANDLE;
   }
   if (NT_SUCCESS(status)) {
      p_atomic_inc(&ws->deferred.progress_repairs);
      fprintf(stderr,
              "bc250: repair signal of progress %" PRIu64 " queued on context 0x%x after a failed submission\n",
              value, queue->context_h);
   } else {
      p_atomic_inc(&ws->deferred.progress_repairs_failed);
      fprintf(stderr,
              "bc250: the repair signal of progress %" PRIu64 " on context 0x%x failed 0x%X: that value stays "
              "unproven\n",
              value, queue->context_h, (unsigned)status);
   }
}

static VkResult
radv_wddm2_cs_submit(struct radeon_winsys_ctx *_ctx,
                     const struct radv_winsys_submit_info *submit,
                     uint32_t wait_count, const struct vk_sync_wait *waits,
                     uint32_t signal_count, const struct vk_sync_signal *signals)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(_ctx);
   struct radv_wddm2_winsys *ws = ctx->ws;
   struct radv_wddm2_queue *queue = &ctx->per_ip[submit->ip_type].queue;
   struct radv_wddm2_queue *ace_queue = &ctx->ace_queue;
   NTSTATUS status;

   /* bc250: the node 2 context of the compute family, made at its first
    * submission (radv_wddm2_ctx_create). Only the one VkQueue of that family
    * submits COMPUTE work on this context, under the queue's external
    * synchronization, so no other thread makes it at the same time. A failed
    * init has destroyed what it made (radv_wddm2_queue_init). */
   if (ws->bc250 && submit->ip_type == AMD_IP_COMPUTE && !queue->context_h) {
      if (radv_wddm2_queue_init(ws, AMD_IP_COMPUTE, ctx->priority, NULL, queue) != VK_SUCCESS ||
          !queue->context_h) {
         fprintf(stderr, "bc250: no context on the compute node: the compute queue is lost\n");
         return VK_ERROR_DEVICE_LOST;
      }
      fprintf(stderr, "bc250: compute queue context 0x%x on node %u\n", queue->context_h, BC250_COMPUTE_NODE);
   }

   assert(queue->context_h != 0 && "Unsupported IP type");

   if (submit->is_gang && ace_queue->handle == 0) {
      assert(submit->ip_type == AMD_IP_GFX);
      radv_wddm2_queue_init(ctx->ws, AMD_IP_COMPUTE, 0, queue, ace_queue);
      if (ace_queue->handle == 0)
         return VK_ERROR_DEVICE_LOST;
   }

   if (wait_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
      STACK_ARRAY(uint64_t, values, wait_count);

      for (uint32_t i = 0; i < wait_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(waits[i].sync)->handle;
         values[i] = waits[i].wait_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {
            .hHwQueue = queue->handle,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, SubmitWaitForSyncObjectsToHwQueue, &wait);
      } else {
         D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
            .hContext = queue->context_h,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .MonitoredFenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromGpu, &wait);
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "bc250: GPU wait refused: NTSTATUS=0x%X, %u fences, ip %u\n", (unsigned)status,
                 wait_count, (unsigned)submit->ip_type);
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (submit->cs_count > 0 && ctx->ws->bc250) {
      /* Independent of application signals: retire only the slot being reused,
       * not the immediately preceding job. Seven BOs match the bounded KMD
       * completion capacity; CP consumption alone never permits CPU reuse. */
      struct bc250_gather_slot *slot = &queue->bc250_gather[queue->bc250_gather_index];
      uint64_t pending_value = queue->bc250_progress.wait_value;
      uint64_t observed = p_atomic_read(queue->bc250_progress.value_map);
      if (!bc250_host_fence_valid(&ws->host,observed)) return VK_ERROR_DEVICE_LOST;
      if (queue->bc250_submit_failed)
         return VK_ERROR_DEVICE_LOST;
      if (ctx->ws->bc250_trace_submits || pending_value == 0)
         fprintf(stderr, "bc250: progress before submit previous=%" PRIu64 " observed=%" PRIu64
                         " slot=%u retire=%" PRIu64 " waits=%u signals=%u\n",
                         pending_value, observed, queue->bc250_gather_index,
                         slot->retire_value, wait_count, signal_count);
      struct vk_wddm2_fence reuse = queue->bc250_progress;
      reuse.wait_value = slot->retire_value;
      if (reuse.wait_value && !vk_wddm2_fence_wait(ctx->ws, &reuse))
         return VK_ERROR_DEVICE_LOST;
      if (queue->bc250_progress.wait_value >= UINT64_MAX-1) return VK_ERROR_DEVICE_LOST;
      uint64_t next_value = queue->bc250_progress.wait_value + 1;
      /* BD-201: the value this submission will signal is published before its IB reaches the kernel, so
       * that a destroy running next to this submit waits for this work too. A submission that then
       * fails leaves the value published, which only holds BOs until the queue is released. */
      radv_wddm2_tracker_publish(queue->bc250_tracker, next_value);
      /* Held BOs whose work retired go back now, while this queue's fences have just been read. */
      radv_wddm2_deferred_drain(ws);
      status = radv_wddm2_bc250_submit(ctx, queue, submit);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "bc250: native submit failed NTSTATUS=0x%X cs_count=%u\n", status, submit->cs_count);
         /* R6: the IB never reached the kernel, so nothing will signal the value published above. */
         radv_wddm2_progress_repair(ws, queue, next_value);
         return VK_ERROR_DEVICE_LOST;
      }
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 progress_signal = {
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->bc250_progress.handle,
         .BroadcastContextCount = 1,
         .BroadcastContextArray = &queue->context_h,
         .MonitoredFenceValueArray = &next_value,
      };
      status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &progress_signal);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "bc250: progress signal refused: NTSTATUS=0x%X, value %" PRIu64 "\n", (unsigned)status,
                 next_value);
         queue->bc250_submit_failed = true; // accepted IB has no retirement value: never reuse its slot
         /* R6: the IB runs, but no signal of this value was queued behind it: ask again, on the same
          * queue and for the same value, so the held ranges of destroys that captured it are proved. */
         radv_wddm2_progress_repair(ws, queue, next_value);
         return VK_ERROR_DEVICE_LOST;
      }
      slot->retire_value = next_value;
      queue->bc250_progress.wait_value = next_value;
      /* R6: this only tells the host about a value the GPU will signal anyway, so a failure here needs
       * no repair: the signal call above was accepted and the value is on its way. */
      if (ws->host.dispatch) {
         struct bc250_host_progress progress = {queue->context_h, queue->bc250_progress.handle, next_value, queue->bc250_progress.value_map};
         if (ws->host.dispatch(ws->host.userdata, BC250_HOST_PUBLISH_PROGRESS, &progress) < 0) {
            queue->bc250_submit_failed = true;
            return VK_ERROR_DEVICE_LOST;
         }
      }
      queue->bc250_gather_index = (queue->bc250_gather_index + 1u) % BC250_GATHER_SLOTS;
   } else if (submit->cs_count > 0) {
      struct radv_winsys_ib first_ib = {};
      struct submit_pdd_writer pdd;

      submit_pdd_writer_init(&pdd);
      if (submit->is_gang) {
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, queue, &first_ib);
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, ace_queue, &first_ib);
      } else {
         radv_wddm2_cs_submit_add_ibs(ctx, &pdd, submit, submit->ip_type, &first_ib);
      }
      submit_pdd_writer_finalize(&pdd);

      if (radv_wddm2_pdd_lines())
         print_hex_data(stderr, pdd.buffer, pdd.offset);

      if (queue->handle) {
         static uint32_t submit_count = 0;
         submit_count++;
         D3DKMT_SUBMITCOMMANDTOHWQUEUE wddm2_submit = {
            .hHwQueue = queue->handle,
            .HwQueueProgressFenceId = submit_count,
            .CommandBuffer = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
         };
         status = BC250_WDDM_CALL(&ws->host, SubmitCommandToHwQueue, &wddm2_submit);
      } else {
         D3DKMT_SUBMITCOMMAND wddm2_submit = {
            .Commands = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
            .BroadcastContextCount = 1,
            .BroadcastContext[0] = queue->context_h,
         };
         status = BC250_WDDM_CALL(&ws->host, SubmitCommand, &wddm2_submit);          
      }
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "SubmitCommand: VK_ERROR_DEVICE_LOST\n");
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (signal_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, signal_count);
      STACK_ARRAY(uint64_t, values, signal_count);

      for (uint32_t i = 0; i < signal_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(signals[i].sync)->handle;
         values[i] = signals[i].signal_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {
            .Flags = {
               .AllowFenceRewind = 1,
            },
            .BroadcastHwQueueCount = 1,
            .BroadcastHwQueueArray = &queue->handle,
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, SubmitSignalSyncObjectsToHwQueue, &signal);
      } else {
         /* The GPU signal of a value can come after a signal of a larger value to the same fence:
          * a queue that waits for a value and then signals the next one is submitted first
          * (wait-before-signal across two queues, legal on a Vulkan timeline). The flag lets the
          * kernel queue a value behind one already queued (d3dukmdt.h, D3DDDICB_SIGNALFLAGS). The
          * wait keeps the order on the GPU, so the value of the fence still only grows. The
          * hardware-queue path above and the CPU signal (vk_wddm2_monitored_fence.c) set it too. */
         D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
            .Flags = {
               .AllowFenceRewind = 1,
            },
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .BroadcastContextCount = 1,
            .BroadcastContextArray = &queue->context_h,
            .MonitoredFenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &signal);
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "bc250: GPU signal refused: NTSTATUS=0x%X, %u fences, ip %u\n", (unsigned)status,
                 signal_count, (unsigned)submit->ip_type);
         return VK_ERROR_DEVICE_LOST;
      }

      struct vk_wddm2_monitored_fence *fence = vk_sync_as_wddm2_monitored_fence(signals[0].sync);
      ctx->per_ip[submit->ip_type].last_submission.handle = fence->handle;
      ctx->per_ip[submit->ip_type].last_submission.wait_value = signals[0].signal_value;
      ctx->per_ip[submit->ip_type].last_submission.value_map = fence->value_map;
   }   

   return VK_SUCCESS;
}

/* The CS was written through a write-combined mapping on this thread. With IB2 calls the
 * CP reads it where it is, and another thread may submit it: drain this core's write-
 * combining buffers before the stream leaves the recording thread. The submit's own sfence
 * drains the submitting core only. */
static VkResult
radv_wddm2_cs_finalize(struct ac_cmdbuf *_cs)
{
   VkResult result = radv_winsys_cs_finalize(_cs);
   _mm_sfence();
   return result;
}

void
radv_wddm2_cs_init_functions(struct radv_wddm2_winsys *ws)
{
   if (ws->bc250) {
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
      const char *nocopy = getenv("BC250_IB_NOCOPY");
      const char *cap = getenv("BC250_IB_DWORDS");
      ws->bc250_ib_dwords_cap = cap && cap[0] ? strtoul(cap, NULL, 0) : 0;
      ws->bc250_gather_copy = (nocopy && !strcmp(nocopy, "0")) || ws->bc250_ib_dwords_cap;
      if (!ws->bc250_gather_copy)
         fprintf(stderr, "bc250: IB submission: IB2 calls from the gather slot (BC250_IB_NOCOPY=0 copies every IB)\n");
      else if (nocopy && !strcmp(nocopy, "0"))
         fprintf(stderr, "bc250: IB submission fallback: every IB copied into the gather slot (BC250_IB_NOCOPY=0)\n");
      else
         fprintf(stderr, "bc250: IB submission fallback: every IB copied into the gather slot (BC250_IB_DWORDS=%lu"
                         " clamps the copied stream)\n", ws->bc250_ib_dwords_cap);
#else
      /* The release flavour reads no diagnostic variable (BD-167): IB2 calls and no cap, as with both unset. */
      ws->bc250_ib_dwords_cap = 0;
      ws->bc250_gather_copy = false;
#endif
   }

   ws->base.ctx_create = radv_wddm2_ctx_create;
   ws->base.ctx_destroy = radv_wddm2_ctx_destroy;
   ws->base.ctx_wait_idle = radv_wddm2_ctx_wait_idle;
   ws->base.cs_domain = radv_wddm2_cs_domain;
   ws->base.cs_create = radv_wddm2_cs_create;
   ws->base.cs_finalize = radv_wddm2_cs_finalize;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_chain = radv_winsys_cs_chain;
   ws->base.cs_unchain = radv_winsys_cs_unchain;
   ws->base.cs_destroy = radv_wddm2_cs_destroy;
   ws->base.cs_grow = radv_winsys_cs_grow;
   ws->base.cs_set_last_cp_dma_header = radv_winsys_cs_set_last_cp_dma_header;
   ws->base.cs_get_last_cp_dma_header = radv_winsys_cs_get_last_cp_dma_header;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_add_buffer = radv_wddm2_cs_add_buffer;
   ws->base.cs_submit = radv_wddm2_cs_submit;
   ws->base.cs_execute_secondary = radv_wddm2_cs_execute_secondary;
   ws->base.cs_execute_ib = radv_winsys_cs_execute_ib;
   ws->base.cs_chain_dgc_ib = radv_winsys_cs_chain_dgc_ib;
   ws->base.cs_dump = radv_winsys_cs_dump;
   ws->base.cs_get_cpu_addr = radv_wddm2_get_cpu_addr;
   ws->base.cs_annotate = radv_winsys_cs_annotate;
   ws->base.cs_pad = radv_winsys_cs_pad;
}
