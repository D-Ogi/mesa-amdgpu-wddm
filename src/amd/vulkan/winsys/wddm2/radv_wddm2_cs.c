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
#include "util/amdgpu_wddm_stdio.h"
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
#include <stdarg.h>

/* One IB of a bc250 submission, in submission order. calls_ib2 is the flag of the
 * command stream it came from: such an IB runs at IB1 level, copied into the gather slot. */
struct bc250_submit_ib {
   struct radv_winsys_ib ib;
   bool calls_ib2;
};

/* amdgpu-wddm fence-lifetime checks. The CPU reuses a gather slot, and the embedder
 * retires memory, on the strength of bc250_progress: a read of it never goes back and
 * never passes the last value this queue signalled on it (wait_value, stored after the
 * signal call of the same thread; submits on one queue are externally serialized). A
 * violation is printed, and with AMDGPU_WDDM_DDI_TRACE=2 (the embedder's failures-only
 * debugger mode) also sent to the debugger, at most 256 lines; nothing else changes.
 */
static bool
radv_wddm2_debugger_lines(void)
{
   static int mode = -1; /* benign race: every thread computes the same value */
   if (mode < 0) {
      const char *value = getenv("AMDGPU_WDDM_DDI_TRACE");
      mode = value && !strcmp(value, "2");
   }
   return mode == 1;
}

static int32_t radv_wddm2_invariant_budget = 256;

static void
radv_wddm2_invariant(const char *format, ...)
{
   char text[512];
   va_list args;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   amdgpu_wddm_log("bc250: invariant: %s\n", text);
#ifdef _WIN32
   if (radv_wddm2_debugger_lines() && p_atomic_dec_return(&radv_wddm2_invariant_budget) >= 0) {
      char line[600];
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      snprintf(line, sizeof(line), "amdgpu_wddm_radv invariant: %s qpc=%lld thread=%lu\n", text,
               (long long)now.QuadPart, (unsigned long)GetCurrentThreadId());
      OutputDebugStringA(line);
   }
#endif
}

/* A line that has to be seen, such as a fallback of the IB submission: stderr with
 * AMDGPU_WDDM_LOG=stderr (or its file), and with AMDGPU_WDDM_DDI_TRACE=2 also the debugger (a
 * game's stderr is not captured), at most 256 debugger lines per process. */
static int32_t radv_wddm2_notice_budget = 256;

static void
radv_wddm2_notice(const char *format, ...)
{
   char text[512];
   va_list args;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   amdgpu_wddm_log("bc250: %s\n", text);
#ifdef _WIN32
   if (radv_wddm2_debugger_lines() && p_atomic_dec_return(&radv_wddm2_notice_budget) >= 0) {
      char line[600];
      snprintf(line, sizeof(line), "amdgpu_wddm_radv: %s thread=%lu\n", text, (unsigned long)GetCurrentThreadId());
      OutputDebugStringA(line);
   }
#endif
}

static void
radv_wddm2_check_progress(struct radv_wddm2_queue *queue, uint64_t observed, const char *where)
{
   if (observed == UINT64_MAX) /* the lost-device value, reported by bc250_host_fence_valid */
      return;
   if (observed < queue->bc250_last_observed)
      radv_wddm2_invariant("progress fence of context 0x%x went back from %" PRIu64 " to %" PRIu64 " (%s)",
                           queue->context_h, queue->bc250_last_observed, observed, where);
   else
      queue->bc250_last_observed = observed;
   if (observed > queue->bc250_progress.wait_value)
      radv_wddm2_invariant("progress fence of context 0x%x reads %" PRIu64 " beyond its last signal %" PRIu64
                           " (%s)", queue->context_h, observed, queue->bc250_progress.wait_value, where);
}

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

/* Whether the queue's work retired: a bounded CPU wait for its last
 * submission and its last mapping update. A lost device, a failed or
 * timed-out wait and a submission the queue could not track leave the
 * answer unknown, and unknown is not retired.
 */
static bool
radv_wddm2_queue_retired(struct radv_wddm2_queue *queue)
{
   if (queue->bc250_submit_failed)
      return false;
   if (queue->bc250_progress.handle && queue->bc250_progress.wait_value &&
       !vk_wddm2_fence_wait(queue->bc250_ws, &queue->bc250_progress))
      return false;
   if (queue->vm_fence.handle && queue->vm_fence.value_map && queue->vm_fence.wait_value &&
       !vk_wddm2_fence_wait(queue->bc250_ws, &queue->vm_fence))
      return false;
   return true;
}

/* Whether the queue still owns a kernel object. */
static bool
radv_wddm2_queue_holds(const struct radv_wddm2_queue *queue)
{
   bool holds = queue->bc250_progress.handle || queue->vm_fence.handle || queue->context_h || queue->handle;
   for (unsigned i = 0; i < BC250_GATHER_SLOTS_MAX; i++)
      holds |= queue->bc250_gather[i].bo != NULL;
   return holds;
}

/* A fence stays with the queue, handle and all, if the host fails to
 * destroy it. */
static void
radv_wddm2_release_sync(struct radv_wddm2_winsys *ws, uint32_t *handle)
{
   if (!*handle)
      return;
   D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
      .hSyncObject = *handle,
   };
   if (NT_SUCCESS(BC250_WDDM_CALL(&ws->host, DestroySynchronizationObject, &destroy)))
      *handle = 0;
}

/* Releases the queue's kernel objects, once its work retired, in this
 * order: the progress fence, the gather BOs, the companion fence, the
 * context, the hardware queue. A context from the embedder's queue goes
 * back through BC250_HOST_DESTROY_QUEUE_CONTEXT with its cookie, and only
 * while the bind or unbind of that queue runs (in_scope); out of scope it
 * is abandoned to the embedder's queue. Work that did not retire keeps
 * everything, since the GPU may still use any of it. An object the host
 * fails to destroy stays too, and its handle with it. Only CPU memory goes
 * either way. Returns false when the queue still owns anything; *retired,
 * if given, tells whether its work retired.
 */
static bool
radv_wddm2_queue_release(struct radv_wddm2_queue *queue, bool in_scope, bool *retired)
{
   struct radv_wddm2_winsys *ws = queue->bc250_ws;
   radv_wddm2_sparse_groups_clear(queue);
   util_dynarray_fini(&queue->sparse_ops);
   free(queue->bc250_ibs);
   queue->bc250_ibs = NULL;
   queue->bc250_ib_capacity = 0;

   const bool work_retired = radv_wddm2_queue_retired(queue);
   if (retired)
      *retired = work_retired;
   if (!work_retired)
      return false;

   /* Every value of the progress fence retired: held BOs stop waiting on it before it goes. */
   radv_wddm2_tracker_detach(ws, queue->bc250_tracker, true);
   queue->bc250_tracker = NULL;
   radv_wddm2_release_sync(ws, &queue->bc250_progress.handle);
   for (unsigned i = 0; i < BC250_GATHER_SLOTS_MAX; i++) {
      /* buffer_destroy reports no status. */
      if (queue->bc250_gather[i].bo)
         ws->base.buffer_destroy(&ws->base, queue->bc250_gather[i].bo);
      queue->bc250_gather[i].bo = NULL;
      queue->bc250_gather[i].map = NULL;
   }
   radv_wddm2_release_sync(ws, &queue->vm_fence.handle);
   if (queue->context_h && (in_scope || !queue->bc250_queue_context)) {
      D3DKMT_DESTROYCONTEXT context_destroy = {
         .hContext = queue->context_h,
      };
      NTSTATUS status;
      if (queue->bc250_queue_context) {
         struct bc250_host_queue_context args = {
            .queue = queue->bc250_queue_cookie,
            .arguments = &context_destroy,
         };
         status = ws->host.dispatch(ws->host.userdata, BC250_HOST_DESTROY_QUEUE_CONTEXT, &args);
      } else {
         status = BC250_WDDM_CALL(&ws->host, DestroyContext, &context_destroy);
      }
      if (NT_SUCCESS(status)) {
         queue->context_h = 0;
         queue->bc250_queue_context = false;
         queue->bc250_queue_cookie = NULL;
      }
   }
   if (queue->handle) {
      D3DKMT_DESTROYHWQUEUE queue_destroy = {
         .hHwQueue = queue->handle,
      };
      if (NT_SUCCESS(BC250_WDDM_CALL(&ws->host, DestroyHwQueue, &queue_destroy)))
         queue->handle = 0;
   }
   return !radv_wddm2_queue_holds(queue);
}

/* BC250_PROGRESS_FENCE=gpu: the address the queue's IB1s write their progress value to, or 0 when
 * the kernel signals the fence instead. The write is a 64-bit RELEASE_MEM (8-byte aligned, a 48-bit
 * GFX10 address; bc250_gfx_emit_fence refuses the same). BC250_IB_DWORDS may cut the IB1 short of
 * its end, and with it the write, so it keeps the kernel signal. The choice holds for the queue's
 * life: a fence written by both the GPU and the kernel could see a kernel signal of an older value
 * land after a newer GPU write. The fence is the queue's own, created without Shared or
 * NtSecuritySharing, so every waiter is in this process, where dxgkrnl's scan at each DMA buffer
 * completion wakes it (context-monitoring.md); the application's fences, imported and shared ones
 * among them, stay with the kernel signal. */
static uint64_t
radv_wddm2_progress_gpu_va(struct radv_wddm2_winsys *ws, uint32_t context, uint64_t va)
{
   const char *why;
   if (!ws->bc250_progress_gpu)
      return 0;
   if (ws->bc250_ib_dwords_cap)
      why = "BC250_IB_DWORDS may cut the write off";
   else if (!va)
      why = "no FenceValueGPUVirtualAddress";
   else if (va & 7)
      why = "the address is not 8-byte aligned";
   else if (va >> 48)
      why = "the address is beyond 48 bits";
   else
      return va;
   /* The first 16 such queues, then every power of two: the periodic summary counts them all. */
   const uint64_t count = p_atomic_inc_return(&ws->submit_stats.kernel_queues);
   if (count <= 16 || util_is_power_of_two_or_zero64(count))
      radv_wddm2_notice("progress fence of context 0x%x is signalled by the kernel: %s (0x%" PRIx64 "); %" PRIu64
                        " such queues so far", context, why, va, count);
   return 0;
}

/* With queue_context set, the context comes from the embedder's queue
 * named by cookie; everything else is created the same way.
 */
static VkResult
radv_wddm2_queue_init(struct radv_wddm2_winsys *ws, enum amd_ip_type hw_ip,
                      enum radeon_ctx_priority priority, struct radv_wddm2_queue *parent,
                      struct radv_wddm2_queue *queue, bool queue_context, void *cookie)
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
      /* GFX1013 has no compute queue (fact M50). Node 1 is not a UMD queue. */
      if (hw_ip != AMD_IP_GFX)
         return VK_SUCCESS;
      node = 0;
      bc250_ctx.magic = BC250_CONTEXT_MAGIC;
      bc250_ctx.version = 2;
      bc250_ctx.size = sizeof(bc250_ctx);
      _Static_assert(sizeof(bc250_ctx) == 80, "BC2C version 2 is 80 bytes");
      bc250_ctx.ip_type = BC250_IP_GFX;
      bc250_ctx.node_ordinal = 0;
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

   if (queue_context) {
      struct bc250_host_queue_context args = {
         .queue = cookie,
         .arguments = &create_context,
      };
      status = ws->host.dispatch(ws->host.userdata, BC250_HOST_CREATE_QUEUE_CONTEXT, &args);
   } else {
      status = BC250_WDDM_CALL(&ws->host, CreateContextVirtual, &create_context);
   }
   if (!NT_SUCCESS(status) || !create_context.hContext) {
      amdgpu_wddm_log("Create context failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   queue->context_h = create_context.hContext;
   queue->bc250_queue_context = queue_context;
   queue->bc250_queue_cookie = cookie;
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
      queue->bc250_progress_va = radv_wddm2_progress_gpu_va(
         ws, queue->context_h, create_progress.Info.MonitoredFence.FenceValueGPUVirtualAddress);
      /* Deferred destruction waits on this fence from now on (radv_wddm2_bo.c). */
      queue->bc250_tracker = radv_wddm2_tracker_attach(ws, queue->bc250_progress.handle,
                                                       queue->bc250_progress.value_map, queue->context_h);
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
         amdgpu_wddm_log("Create queue failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
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
         amdgpu_wddm_log("CreateSynchronizationObject2 failed with NTSTATUS 0x%x\n", status);
         goto failed;
      }
      queue->vm_fence.handle = create_sync.hSyncObject;
   }

   return VK_SUCCESS;

failed:
   /* Nothing was submitted: release what was created, within the call. */
   radv_wddm2_queue_release(queue, queue_context, NULL);
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

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      result = radv_wddm2_queue_init(ws, ip, priority, NULL, &ctx->per_ip[ip].queue, false, NULL);
      if (result != VK_SUCCESS)
         goto fail_contexts;
   }

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;

fail_contexts:
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_release(&ctx->per_ip[ip].queue, false, NULL);

   FREE(ctx);
   return result;
}

/* An unbound queue: no kernel object and no state of an earlier bind, so
 * that the next bind starts its fences and gather slots from zero.
 */
static void
radv_wddm2_queue_reset_unbound(struct radv_wddm2_winsys *ws, enum amd_ip_type hw_ip,
                               struct radv_wddm2_queue *queue)
{
   /* Only a released queue gets here, and its release detached the tracker; never lose one. */
   radv_wddm2_tracker_detach(ws, queue->bc250_tracker, false);
   memset(queue, 0, sizeof(*queue));
   queue->hw_ip = hw_ip;
   queue->bc250_ws = ws;
   util_dynarray_init(&queue->sparse_ops, NULL);
}

/* The context of one runtime-bound VkQueue. It starts with no kernel
 * object, so creating and destroying it unbound never reaches the host.
 */
static VkResult
radv_wddm2_ctx_create_bindable(struct radeon_winsys *_ws, enum radeon_ctx_priority priority,
                               struct radeon_winsys_ctx **rctx)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (!ws->bc250 || !ws->host.dispatch)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (priority < RADEON_CTX_PRIORITY_LOW || priority > RADEON_CTX_PRIORITY_HIGH)
      return VK_ERROR_NOT_PERMITTED;

   struct radv_wddm2_ctx *ctx = CALLOC_STRUCT(radv_wddm2_ctx);
   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   ctx->ws = ws;
   ctx->bindable = true;
   ctx->priority = priority;

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_reset_unbound(ws, ip, &ctx->per_ip[ip].queue);
   radv_wddm2_queue_reset_unbound(ws, AMD_IP_COMPUTE, &ctx->ace_queue);

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;
}

/* Called inside the embedder's bind of the queue that cookie names (NULL
 * for the engine's internal queue). The context comes from that queue,
 * the progress and companion fences from the device. On failure whatever
 * was created is released within this call, the context through the same
 * cookie, and the context can be bound again, unless the host failed to
 * destroy something: then the context keeps it and is never bound again.
 */
static VkResult
radv_wddm2_ctx_bind(struct radeon_winsys_ctx *rwctx, void *cookie)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   struct radv_wddm2_winsys *ws = ctx->ws;
   struct radv_wddm2_queue *queue = &ctx->per_ip[AMD_IP_GFX].queue;

   if (!ctx->bindable || ctx->bound || ctx->kept)
      return VK_ERROR_INITIALIZATION_FAILED;
   /* A lost device gets no new context. */
   if (bc250_host_check_status(&ws->host) < 0)
      return VK_ERROR_DEVICE_LOST;

   radv_wddm2_queue_reset_unbound(ws, AMD_IP_GFX, queue);
   memset(&ctx->per_ip[AMD_IP_GFX].last_submission, 0, sizeof(ctx->per_ip[AMD_IP_GFX].last_submission));

   VkResult result = radv_wddm2_queue_init(ws, AMD_IP_GFX, ctx->priority, NULL, queue, true, cookie);
   if (result != VK_SUCCESS) {
      if (radv_wddm2_queue_holds(queue))
         ctx->kept = true;
      else
         radv_wddm2_queue_reset_unbound(ws, AMD_IP_GFX, queue);
      return result;
   }

   ctx->bound = true;
   return VK_SUCCESS;
}

/* Called inside the embedder's unbind, once the queue is idle for the
 * engine. Once the queue's work retired its objects are released within
 * this call, and no host call names the context or the cookie afterwards.
 * Otherwise the context keeps what it could not release and is never bound
 * again: VK_ERROR_DEVICE_LOST when the work did not retire (everything
 * kept), VK_ERROR_UNKNOWN when the host failed to destroy an object.
 */
static VkResult
radv_wddm2_ctx_unbind(struct radeon_winsys_ctx *rwctx)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   struct radv_wddm2_winsys *ws = ctx->ws;
   struct radv_wddm2_queue *queue = &ctx->per_ip[AMD_IP_GFX].queue;

   if (!ctx->bindable || !ctx->bound)
      return VK_ERROR_INITIALIZATION_FAILED;

   bool retired;
   const bool released = radv_wddm2_queue_release(queue, true, &retired);
   ctx->bound = false;
   memset(&ctx->per_ip[AMD_IP_GFX].last_submission, 0, sizeof(ctx->per_ip[AMD_IP_GFX].last_submission));

   if (released) {
      radv_wddm2_queue_reset_unbound(ws, AMD_IP_GFX, queue);
      return VK_SUCCESS;
   }
   ctx->kept = true;
   ctx->unretired = !retired;
   return retired ? VK_ERROR_UNKNOWN : VK_ERROR_DEVICE_LOST;
}

static void
radv_wddm2_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   bool abandoned = false;

   /* What an unbind or a failed bind kept is left to the device: that
    * call has returned, and the context makes no host call for it. Still
    * bound, the embedder's queue may be gone already, so its context is
    * left to it rather than destroyed through a cookie that may be stale;
    * the device's fences and gather BOs go if the queue's work retired and
    * are kept otherwise.
    */
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      abandoned |= ctx->kept || !radv_wddm2_queue_release(&ctx->per_ip[ip].queue, false, NULL);
   abandoned |= ctx->kept || !radv_wddm2_queue_release(&ctx->ace_queue, false, NULL);

   /* A queue whose work did not retire keeps its progress fence with the device: held BOs keep
    * waiting on it, through the tracker, after the queue's memory is gone. */
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_tracker_detach(ctx->ws, ctx->per_ip[ip].queue.bc250_tracker, false);
   radv_wddm2_tracker_detach(ctx->ws, ctx->ace_queue.bc250_tracker, false);

   if (abandoned)
      amdgpu_wddm_log("radv/wddm2: queue destroyed %s, its kernel objects are left to the device\n",
              ctx->bound ? "while bound" : "after a failed release");

   FREE(ctx);
}

/* The bounds of a CPU wait for one of the queue's own fences. The host tests
 * shorten them; nothing else writes them. */
uint64_t radv_wddm2_fence_wait_slice_ns = 1000000000ull;   /* 1 s between liveness checks */
uint64_t radv_wddm2_fence_wait_total_ns = 120000000000ull; /* 120 s: well past a TDR and its reset */

/* Whether the kernel device can still complete work. A hosted device has no
 * kernel device of its own: the host status and the fences tell its state,
 * and the host does not answer GetDeviceState. An unanswered query is no
 * evidence of a loss. */
static bool
radv_wddm2_device_executing(struct radv_wddm2_winsys *ws)
{
   if (ws->host.dispatch)
      return true;
   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   NTSTATUS status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
   return !NT_SUCCESS(status) || get_state.ExecutionState == D3DKMT_DEVICEEXECUTION_ACTIVE;
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
      amdgpu_wddm_log("fence wait failed: 0x%X\n", status);
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
    * or the kernel device is not in the ACTIVE execution state.
    */
   uint64_t waited_ns = 0;
   const uint64_t slice_ns = MAX2(radv_wddm2_fence_wait_slice_ns, 1000000ull);
   const uint64_t total_ns = MAX2(radv_wddm2_fence_wait_total_ns, slice_ns);
   bool lost = false;
   for (;;) {
      result = vk_async_event_wait(async_event, MIN2(slice_ns, total_ns - waited_ns));
      if (result != VK_TIMEOUT)
         break;
      waited_ns += MIN2(slice_ns, total_ns - waited_ns);
      observed = p_atomic_read(fence->value_map);
      if (!bc250_host_fence_valid(&ws->host, observed) || observed == UINT64_MAX ||
          !radv_wddm2_device_executing(ws)) {
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
         amdgpu_wddm_log("radv/wddm2: fence %u value %" PRIu64 " pending after %" PRIu64
                         " ms, device active: waiting on\n",
                 fence->handle, fence->wait_value, waited_ns / 1000000);
   }
   vk_async_event_close(async_event);
   if (result != VK_SUCCESS)
      amdgpu_wddm_log("async wait event: 0x%x after %" PRIu64 " ms%s\n", result, waited_ns / 1000000,
              lost ? ", device lost" : "");
   else if (waited_ns >= slice_ns)
      amdgpu_wddm_log("radv/wddm2: fence %u value %" PRIu64 " completed after %" PRIu64 " ms of wait\n",
              fence->handle, fence->wait_value, waited_ns / 1000000);

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };

   if (get_state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
      get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
      status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
      D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

      amdgpu_wddm_log("faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i), flags: %i, stage: %i\n",
             fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode,
             fault.FaultErrorCode.DeviceSpecificCode, fault.PageFaultFlags, fault.FaultedPipelineStage);
      return false;
   }

   return result == VK_SUCCESS && bc250_host_fence_valid(&ws->host,p_atomic_read(fence->value_map));
}

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

   /* Nothing of an unbound context is in flight, unless its unbind could
    * not retire its work. */
   if (radv_wddm2_ctx_unbound(ctx))
      return !ctx->unretired;

   if (ctx->per_ip[ip_type].last_submission.handle)
      ret = vk_wddm2_fence_wait(ctx->ws, &ctx->per_ip[ip_type].last_submission);

   /* A submission without application signals leaves last_submission behind; the queue's progress
    * fence follows every accepted IB. */
   struct radv_wddm2_queue *queue = &ctx->per_ip[ip_type].queue;
   if (ret && queue->bc250_progress.handle && queue->bc250_progress.wait_value)
      ret = vk_wddm2_fence_wait(ctx->ws, &queue->bc250_progress);

   /* An idle point: BOs held for this queue's work can go now. */
   if (ret)
      radv_wddm2_deferred_drain(ctx->ws);
   return ret;
}

/* Every bind of a vertex, index or copy buffer names its BO again (radv_cs_add_buffer): a game binds the same few
 * heap BOs thousands of times per stream, and a set insert per bind was 0.23 ms per frame of Witcher 3's main thread
 * (trial 291). recent is a direct-mapped cache in front of the set, as the amdgpu winsys keeps buffer_hash_table in
 * front of its handle list: a BO in recent is in buffers, so a hit skips the insert. Only add_buffer fills it, reset
 * clears it with the set. */
#define RADV_WDDM2_CS_RECENT_BOS 256

struct radv_wddm2_cs {
   struct radv_winsys_cs base;
   struct set *buffers; /* the prefix the queue tests' fake_cs mirrors */
   struct radv_wddm2_bo *recent[RADV_WDDM2_CS_RECENT_BOS];
};

static inline struct radv_wddm2_cs *
radv_wddm2_cs(struct ac_cmdbuf *base)
{
   return (struct radv_wddm2_cs *)base;
}

static inline unsigned
radv_wddm2_cs_recent_slot(const struct radv_wddm2_bo *bo)
{
   const uintptr_t p = (uintptr_t)bo >> 4;
   return (unsigned)(p ^ (p >> 8)) & (RADV_WDDM2_CS_RECENT_BOS - 1);
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
   memset(cs->recent, 0, sizeof(cs->recent));
}

static void
radv_wddm2_cs_add_buffer(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   struct radv_wddm2_bo **slot = &cs->recent[radv_wddm2_cs_recent_slot(bo)];

   if (*slot == bo)
      return;
   _mesa_set_add(cs->buffers, bo);
   *slot = bo;
}

static void
radv_wddm2_cs_execute_secondary(struct ac_cmdbuf *_parent, struct ac_cmdbuf *_child,
                                bool allow_ib2)
{
   struct radv_wddm2_cs *parent = radv_wddm2_cs(_parent), *child = radv_wddm2_cs(_child);
   radv_winsys_cs_execute_secondary(&parent->base, &child->base, allow_ib2);

   /* For the deferred-destruction witness the primary's submission names the secondary's BOs, its
    * command BOs too (an IB2 call reads them), as the amdgpu winsys's BO list does. */
   if (radv_wddm2_winsys(parent->base.ws)->deferred.witness && parent->buffers && child->buffers) {
      set_foreach (child->buffers, entry)
         _mesa_set_add(parent->buffers, entry->key);
      for (unsigned i = 0; i < child->base.num_ib_buffers; i++) {
         if (child->base.ib_buffers[i].bo)
            _mesa_set_add(parent->buffers, radv_wddm2_bo(child->base.ib_buffers[i].bo));
      }
   }
}

/* The deferred-destruction witness (radv_wddm2_bo.c): every BO a submission names takes the serial
 * of the queue's tracker and the progress value the submission signals. A command stream names its
 * BO set and its command BOs. */
void
radv_wddm2_witness_cs(struct ac_cmdbuf *base, uint32_t serial, uint64_t value, uint32_t *stale,
                      struct radv_wddm2_bo **first)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(base);
   if (cs->buffers) {
      set_foreach (cs->buffers, entry)
         radv_wddm2_witness_stamp((struct radv_wddm2_bo *)entry->key, serial, value, stale, first);
   }
   for (unsigned i = 0; i < cs->base.num_ib_buffers; i++) {
      if (cs->base.ib_buffers[i].bo)
         radv_wddm2_witness_stamp(radv_wddm2_bo(cs->base.ib_buffers[i].bo), serial, value, stale, first);
   }
   if (cs->base.ib_buffer)
      radv_wddm2_witness_stamp(radv_wddm2_bo(cs->base.ib_buffer), serial, value, stale, first);
}

static void
radv_wddm2_witness_array(struct ac_cmdbuf **array, unsigned count, uint32_t serial, uint64_t value,
                         uint32_t *stale, struct radv_wddm2_bo **first)
{
   for (unsigned i = 0; i < count; i++)
      radv_wddm2_witness_cs(array[i], serial, value, stale, first);
}

static void
radv_wddm2_witness_submit(struct radv_wddm2_winsys *ws, struct radv_wddm2_queue *queue,
                          const struct radv_winsys_submit_info *submit, uint64_t value)
{
   const uint32_t serial = queue->bc250_tracker->serial;
   uint32_t stale = 0;
   struct radv_wddm2_bo *first = NULL;
   radv_wddm2_witness_array(submit->cs_array, submit->cs_count, serial, value, &stale, &first);
   radv_wddm2_witness_array(submit->initial_preamble_cs, submit->initial_preamble_count, serial, value, &stale,
                            &first);
   radv_wddm2_witness_array(submit->continue_preamble_cs, submit->continue_preamble_count, serial, value, &stale,
                            &first);
   radv_wddm2_witness_array(submit->postamble_cs, submit->postamble_count, serial, value, &stale, &first);
   if (unlikely(stale))
      radv_wddm2_witness_stale(ws, first, stale, queue->context_h);
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
            amdgpu_wddm_log("Adding IB with VA 0x%" PRIx64 " and length %u bytes\n", ib.va, ib.cdw * 4);
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
      amdgpu_wddm_log("bc250: gather IB limit used=%u add=%u cap=%u\n", *n, count, cap);
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
 * and IB_SIZE. The caller checked that the IB lies inside its BO.
 */
static bool
bc250_ib2_callable(const struct bc250_submit_ib *s)
{
   return !s->calls_ib2 && s->ib.va >= s->ib.bo->va && (s->ib.va & 3) == 0 &&
          s->ib.cdw <= G_3F3_IB_SIZE(UINT32_MAX);
}

/* The CS BO bytes of one IB, NULL if the IB is not inside a mapped BO. */
static const uint8_t *
bc250_ib_bytes(struct radv_wddm2_winsys *ws, const struct radv_winsys_ib *ib)
{
   uint64_t off = 0;
   if (!ib->bo)
      return NULL;
   if (ib->va >= ib->bo->va)
      off = ib->va - ib->bo->va;
   if (off + (uint64_t)ib->cdw * 4 > ib->bo->size)
      return NULL;
   const uint8_t *src = ws->base.buffer_map(&ws->base, ib->bo, false, NULL);
   return src ? src + off : NULL;
}

/* The progress write that ends an IB1 with BC250_PROGRESS_FENCE=gpu: a RELEASE_MEM of the 64-bit value to
 * the progress fence's FenceValueGPUVirtualAddress, bit for bit the KMD's own ring fence
 * (driver/shim/bc250_gfx.c:1544-1561, gfx_v10_0_ring_emit_fence upstream): event
 * CACHE_FLUSH_AND_INV_TS at index 5 (end of pipe, after every earlier draw and dispatch of the queue),
 * GLM write-back and invalidate, GL2 write-back, SEQ 1, cache policy 3 (bypass), then DATA_SEL 2 (the
 * 64-bit value), INT_SEL 0 (no interrupt) and DST_SEL 0 (memory). The lab's positive control
 * (tools/win/monfence, run A0 on KMD 184) wrote this packet to that address: the CPU mapping read the
 * value 64 us after the write, WaitForSynchronizationObjectFromCpu woke, and a second context's GPU wait
 * on it released. dxgkrnl wakes the waiters of a GPU-written fence when a DMA buffer of the process
 * completes (context-monitoring.md: "Dxgkrnl goes through the list of fence objects with pending waits");
 * the KMD's ring fence follows this IB1, so that scan runs after the write. Eight dwords, a multiple of
 * the GFX IB padding. */
#define BC250_PROGRESS_WRITE_DW 8u
#define BC250_PROGRESS_RM_DW1                                                                               \
   (EVENT_TYPE(V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5) | S_491_GLM_WB(1) | S_491_GLM_INV(1) | \
    S_491_GL2_WB(1) | S_491_SEQ(1) | (3u << 25) /* CACHE_POLICY, no gfx10 macro */)
#define BC250_PROGRESS_RM_DW2                                                                               \
   (EOP_DST_SEL(EOP_DST_SEL_MEM) | EOP_INT_SEL(EOP_INT_SEL_NONE) | EOP_DATA_SEL(EOP_DATA_SEL_VALUE_64BIT))
_Static_assert(BC250_PROGRESS_RM_DW1 == 0x06603514u, "the KMD fence's RELEASE_MEM dword 1 (monfence_packets.h)");
_Static_assert(BC250_PROGRESS_RM_DW2 == 0x40000000u, "DATA_SEL 2, INT_SEL 0, DST_SEL 0");

static void
bc250_emit_progress_write(uint8_t *dst, uint64_t va, uint64_t value)
{
   const uint32_t packet[BC250_PROGRESS_WRITE_DW] = {
      PKT3(PKT3_RELEASE_MEM, 6, 0),
      BC250_PROGRESS_RM_DW1,
      BC250_PROGRESS_RM_DW2,
      (uint32_t)va,
      (uint32_t)(va >> 32),
      (uint32_t)value,
      (uint32_t)(value >> 32),
      0, /* INT_CTXID */
   };
   memcpy(dst, packet, sizeof(packet));
}

/* One IB1 on the gfx ring: the KMD runs the single IB of a BC2S blob (driver/kmd/umd_blob.c,
 * single_ib) and puts its own frame and fence around it on the ring. A submission of several
 * IBs is packed into a gather BO of the queue. By default the gather BO holds one IB2 call
 * (INDIRECT_BUFFER without CHAIN, the form RADV uses for secondaries) per IB, and the CP reads
 * the command streams where RADV recorded them; an IB whose stream calls an IB2 itself is
 * copied into the gather BO instead and runs at IB1 level. BC250_IB_NOCOPY=0 copies every IB,
 * the previous behaviour.
 *
 * The IB2 calls leave the command streams in use until this submission retires. That holds
 * as for any Vulkan submission: a command buffer stays pending until the application's
 * signal, which is queued after this IB1 on the same context, and RADV destroys a replaced
 * queue preamble only after ctx_wait_idle (radv_update_preamble_cs).
 *
 * progress_value (BC250_PROGRESS_FENCE=gpu, nonzero) ends the IB1 with the write of that value to
 * the queue's progress fence (bc250_emit_progress_write). Every submission then goes through the
 * gather slot, a single IB and an empty submission too: the stream is called as an IB2 (or copied
 * when it calls one itself), then the padding, then the write as the IB1's last packet. 0 keeps the
 * IB1 as before.
 */
static NTSTATUS
radv_wddm2_bc250_submit(struct radv_wddm2_ctx *ctx, struct radv_wddm2_queue *queue,
                        const struct radv_winsys_submit_info *submit, uint64_t progress_value)
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
         amdgpu_wddm_log("bc250: IB collection capacity=%u required=%u\n",
                 queue->bc250_ib_capacity, required);
      }
      ibs = queue->bc250_ibs;
   }
   if (!bc250_collect_submit(submit, ibs, &n, required))
      return STATUS_INVALID_PARAMETER;
   const bool gpu_progress = progress_value != 0;
   if (n == 0 && !gpu_progress)
      return STATUS_SUCCESS;

   const bool direct = n == 1 && !gpu_progress;
   if (direct) {
      va = ibs[0].ib.va;
      bytes = ibs[0].ib.cdw * 4;
   } else {
      const bool copy_all = ws->bc250_gather_copy;
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
      total = copied_dw + 4ull * calls + (gpu_progress ? BC250_PROGRESS_WRITE_DW : 0);
      /* The IB1 ends on the IB padding of the GFX queue, as every RADV IB does. Copied IBs are
       * already padded; four-dword calls may leave half a unit (the progress write is 8 dwords). */
      if (!copy_all || gpu_progress) {
         const uint32_t pad_mask = ws->gpu_info.ip[AMD_IP_GFX].ib_pad_dw_mask;
         pad = (unsigned)((pad_mask + 1u - (total & pad_mask)) & pad_mask);
         total += pad;
      }
      /* Match the GFX INDIRECT_BUFFER IB_SIZE field, not an arbitrary 1 MiB
       * staging limit. The caller retired the selected gather slot before any write or resize. */
      if (total == 0 || total > G_3F3_IB_SIZE(UINT32_MAX)) {
         amdgpu_wddm_log("bc250: gather exceeds IB_SIZE field: %" PRIu64 " dwords\n", total);
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
         amdgpu_wddm_log("bc250: gather capacity=%" PRIu64 " bytes required=%u ibs=%u calls=%u copies=%u\n",
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
      dst += pad * 4u;
      /* The last packet of the IB1, after every call, copy and the padding: the work of the whole
       * submission precedes the write, and once the CP has parsed it there is nothing left to fetch from
       * this slot or from the streams it called. A progress value seen on the CPU therefore also means
       * the CP is done with the slot's bytes, which the slot's reuse relies on, as it did on the kernel
       * signal queued after the IB1. */
      if (gpu_progress)
         bc250_emit_progress_write(dst, queue->bc250_progress_va, progress_value);
      va = queue->bc250_gather[queue->bc250_gather_index].bo->va;

      if (!copy_all && copies) {
         const uint64_t count = p_atomic_inc_return(&ws->bc250_inline_submits);
         if (count <= 16 || util_is_power_of_two_or_zero64(count))
            radv_wddm2_notice("IB2 fallback: %u of %u IBs copied into the IB1 (%u call an IB2 themselves, %u"
                              " outside IB_BASE/IB_SIZE), %" PRIu64 " dwords; %" PRIu64 " such submissions so far",
                              copies, n, nested, copies - nested, copied_dw, count);
      }
   }

   /* BC250_IB_DWORDS cuts the IB the CP executes. 5 is CONTEXT_CONTROL plus
    * CLEAR_STATE, a packet boundary. The ring fence still follows the IB, so a
    * timeout here means the CP did not finish that prefix. Unset, the whole IB runs.
    * Read once (radv_wddm2_cs_init_functions); it also selects the gather copy. */
   {
      const unsigned long cap = ws->bc250_ib_dwords_cap;
      if (!gpu_progress && cap > 0 && cap < 0x100000ul && cap * 4ul < bytes) {
         amdgpu_wddm_log("bc250: IB clamped to %lu dwords (%u were ready)\n", cap, bytes / 4u);
         bytes = (uint32_t)(cap * 4ul);
      }
   }

   memset(&blob, 0, sizeof(blob));
   _Static_assert(offsetof(struct bc250_submit_blob, ib) == 40, "BC2S prefix is 40 bytes");
   _Static_assert(sizeof(struct bc250_ib_blob) == 32, "BC2S ib is 32 bytes");
   blob.magic = BC250_SUBMIT_MAGIC;
   blob.version = 1;
   blob.num_ibs = 1;
   blob.ip_type = BC250_IP_GFX;
   blob.size = 40 + sizeof(struct bc250_ib_blob);
   blob.ib[0].va_start = va;
   blob.ib[0].ib_bytes = bytes;
   blob.ib[0].ip_type = BC250_IP_GFX;

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
      amdgpu_wddm_log("bc250: SubmitCommand 0x%X (%u ibs packed, %u bytes)\n", status, n, bytes);
   else if (ws->bc250_trace_submits) {
      const uint32_t *dw = NULL;
      if (direct && ibs[0].ib.bo)
         dw = (const uint32_t *)((uint8_t *)ws->base.buffer_map(&ws->base, ibs[0].ib.bo, false, NULL) +
                                 (ibs[0].ib.va - ibs[0].ib.bo->va));
      else
         dw = (const uint32_t *)queue->bc250_gather[queue->bc250_gather_index].map;
      amdgpu_wddm_log("bc250: SubmitCommand ib 0x%" PRIx64 " %u bytes (from %u) %08x %08x %08x %08x\n",
              va, bytes, n,
              dw ? dw[0] : 0, dw && bytes >= 8 ? dw[1] : 0,
              dw && bytes >= 12 ? dw[2] : 0, dw && bytes >= 16 ? dw[3] : 0);
      if (!direct)
         amdgpu_wddm_log("bc250: IB1 holds %u IB2 calls and %u copied IBs%s\n", calls, copies,
                 gpu_progress ? " and the progress write" : "");
      /* Slices of the unclamped preamble and the main CS, when the IB1 is their copy. 163 is
       * the first ACQUIRE_MEM, 176 the main IB, 216 the dispatch. */
      if (calls)
         dw = NULL;
      if (dw && bytes >= 176u * 4u) {
         amdgpu_wddm_log("bc250: IB +158 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[158], dw[159], dw[160], dw[161], dw[162], dw[163], dw[164], dw[165]);
         amdgpu_wddm_log("bc250: IB +166 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[166], dw[167], dw[168], dw[169], dw[170], dw[171], dw[172], dw[173]);
         amdgpu_wddm_log("bc250: IB +174 %08x %08x\n", dw[174], dw[175]);
      }
      if (dw && bytes >= 216u * 4u) {
         amdgpu_wddm_log("bc250: IB +176 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[176], dw[177], dw[178], dw[179], dw[180], dw[181], dw[182], dw[183]);
         amdgpu_wddm_log("bc250: IB +184 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[184], dw[185], dw[186], dw[187], dw[188], dw[189], dw[190], dw[191]);
         amdgpu_wddm_log("bc250: IB +192 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[192], dw[193], dw[194], dw[195], dw[196], dw[197], dw[198], dw[199]);
         amdgpu_wddm_log("bc250: IB +200 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[200], dw[201], dw[202], dw[203], dw[204], dw[205], dw[206], dw[207]);
         amdgpu_wddm_log("bc250: IB +208 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[208], dw[209], dw[210], dw[211], dw[212], dw[213], dw[214], dw[215]);
      }
      if (dw && bytes >= 240u * 4u) {
         amdgpu_wddm_log("bc250: IB +216 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[216], dw[217], dw[218], dw[219], dw[220], dw[221], dw[222], dw[223]);
         amdgpu_wddm_log("bc250: IB +224 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[224], dw[225], dw[226], dw[227], dw[228], dw[229], dw[230], dw[231]);
         amdgpu_wddm_log("bc250: IB +232 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[232], dw[233], dw[234], dw[235], dw[236], dw[237], dw[238], dw[239]);
      }
   }
   return status;
}

#ifndef D3DDDI_MAX_OBJECT_SIGNALED
#define D3DDDI_MAX_OBJECT_SIGNALED 32
#endif

/* The progress value next_value was signalled after the IB that used slot, in a call of its own or with
 * the application's signals, or that IB1 ends with its GPU write: the slot retires with it, it is the
 * queue's last signal, the embedder learns it, and the next submission takes the next slot. A failed
 * publication leaves the queue unable to track its work: it never reuses a slot again. */
static VkResult
radv_wddm2_progress_signalled(struct radv_wddm2_winsys *ws, struct radv_wddm2_queue *queue,
                              struct bc250_gather_slot *slot, uint64_t next_value)
{
   slot->retire_value = next_value;
   queue->bc250_progress.wait_value = next_value;
   if (ws->host.dispatch) {
      struct bc250_host_progress progress = {queue->context_h, queue->bc250_progress.handle, next_value,
                                             queue->bc250_progress.value_map};
      if (ws->host.dispatch(ws->host.userdata, BC250_HOST_PUBLISH_PROGRESS, &progress) < 0) {
         queue->bc250_submit_failed = true;
         return VK_ERROR_DEVICE_LOST;
      }
   }
   queue->bc250_gather_index = (queue->bc250_gather_index + 1u) % ws->bc250_gather_slots;
   return VK_SUCCESS;
}

/* A CPU wait for a gather slot to retire, for the periodic summary. */
static void
radv_wddm2_count_gather_wait(struct radv_wddm2_winsys *ws, uint64_t ns)
{
   p_atomic_inc(&ws->submit_stats.gather_waits);
   p_atomic_add(&ws->submit_stats.gather_wait_ns, ns);
   uint64_t max = p_atomic_read(&ws->submit_stats.gather_wait_max_ns);
   while (ns > max) {
      const uint64_t seen = p_atomic_cmpxchg(&ws->submit_stats.gather_wait_max_ns, max, ns);
      if (seen == max)
         break;
      max = seen;
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

   if (radv_wddm2_ctx_unbound(ctx))
      return VK_ERROR_VALIDATION_FAILED;

   /* Destroy the held BOs whose waits retired (no host call when none did). */
   radv_wddm2_deferred_drain(ws);
   /* The periodic summary: one clock read, the lines only once the period has passed. */
   radv_wddm2_summary_tick(ws);

   assert(queue->context_h != 0 && "Unsupported IP type");

   if (submit->is_gang && ace_queue->handle == 0) {
      assert(submit->ip_type == AMD_IP_GFX);
      radv_wddm2_queue_init(ctx->ws, AMD_IP_COMPUTE, 0, queue, ace_queue, false, NULL);
      if (ace_queue->handle == 0)
         return VK_ERROR_DEVICE_LOST;
   }

   if (wait_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
      STACK_ARRAY(uint64_t, values, wait_count);

      /* BC250_SUBMIT_COALESCE: a wait whose value the fence's CPU mapping already shows needs no GPU
       * wait. The value of a monitored fence of this winsys only grows (a timeline: no reset, and a CPU
       * signal asserts a larger value), so the GPU would pass that wait at once. A wait the CPU does
       * not see complete is queued as before, whichever queue signals it, and so is a fence that
       * reads the lost-device value: the kernel reports that. */
      const bool drop = ws->bc250 && ws->bc250_drop_waits && !queue->handle;
      uint32_t count = 0;
      for (uint32_t i = 0; i < wait_count; i++) {
         struct vk_wddm2_monitored_fence *fence = vk_sync_as_wddm2_monitored_fence(waits[i].sync);
         if (drop && fence->value_map) {
            const uint64_t seen = p_atomic_read(fence->value_map);
            if (seen != UINT64_MAX && seen >= waits[i].wait_value)
               continue;
         }
         handles[count] = fence->handle;
         values[count] = waits[i].wait_value;
         count++;
      }
      if (ws->bc250) {
         p_atomic_add(&ws->submit_stats.wait_objects, (uint64_t)wait_count);
         if (count < wait_count)
            p_atomic_add(&ws->submit_stats.wait_dropped, (uint64_t)(wait_count - count));
         p_atomic_inc(count ? &ws->submit_stats.wait_calls : &ws->submit_stats.wait_skipped);
      }

      status = STATUS_SUCCESS;
      if (count && queue->handle) {
         D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {
            .hHwQueue = queue->handle,
            .ObjectCount = count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, SubmitWaitForSyncObjectsToHwQueue, &wait);
      } else if (count) {
         D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
            .hContext = queue->context_h,
            .ObjectCount = count,
            .ObjectHandleArray = handles,
            .MonitoredFenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromGpu, &wait);
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;
   }

   bool merge_progress = false;
   uint64_t next_value = 0;
   struct bc250_gather_slot *slot = NULL;
   if (submit->cs_count > 0 && ctx->ws->bc250) {
      /* Independent of application signals: retire only the slot being reused, not the immediately
       * preceding job; CP consumption alone never permits CPU reuse. The ring bounds how far this
       * thread runs ahead of the GPU (BC250_GATHER_SLOTS). The KMD bounds its own ring apart from it
       * (seven completion records, a polling wait in its SubmitCommand DDI, which dxgkrnl calls). */
      slot = &queue->bc250_gather[queue->bc250_gather_index];
      uint64_t pending_value = queue->bc250_progress.wait_value;
      uint64_t observed = p_atomic_read(queue->bc250_progress.value_map);
      if (!bc250_host_fence_valid(&ws->host,observed)) return VK_ERROR_DEVICE_LOST;
      radv_wddm2_check_progress(queue, observed, "before submit");
      if (queue->bc250_submit_failed)
         return VK_ERROR_DEVICE_LOST;
      if (ctx->ws->bc250_trace_submits || pending_value == 0)
         amdgpu_wddm_log("bc250: progress before submit previous=%" PRIu64 " observed=%" PRIu64
                         " slot=%u retire=%" PRIu64 " waits=%u signals=%u\n",
                         pending_value, observed, queue->bc250_gather_index,
                         slot->retire_value, wait_count, signal_count);
      struct vk_wddm2_fence reuse = queue->bc250_progress;
      reuse.wait_value = slot->retire_value;
      if (reuse.wait_value) {
         /* observed, read above, tells a real wait apart: counted and timed for the summary. */
         const bool waiting = observed < reuse.wait_value;
         const uint64_t start = waiting ? os_time_get_nano() : 0;
         const bool retired_ok = vk_wddm2_fence_wait(ctx->ws, &reuse);
         if (waiting)
            radv_wddm2_count_gather_wait(ws, os_time_get_nano() - start);
         if (!retired_ok)
            return VK_ERROR_DEVICE_LOST;
         /* The CPU overwrites this slot's IB next: its job must have retired. */
         const uint64_t retired = p_atomic_read(queue->bc250_progress.value_map);
         if (retired != UINT64_MAX && retired < slot->retire_value)
            radv_wddm2_invariant("gather slot %u of context 0x%x reused at progress %" PRIu64
                                 " before its retirement value %" PRIu64,
                                 queue->bc250_gather_index, queue->context_h, retired, slot->retire_value);
         radv_wddm2_check_progress(queue, retired, "after slot wait");
      }
      /* The progress value of this submission: one more than the last, never the lost-device value
       * UINT64_MAX (checked before anything names it). At one submission per microsecond the 64-bit
       * count lasts 584,000 years: it never wraps, and a value the queue read never goes back
       * (radv_wddm2_check_progress). */
      if (queue->bc250_progress.wait_value >= UINT64_MAX - 1)
         return VK_ERROR_DEVICE_LOST;
      next_value = queue->bc250_progress.wait_value + 1;
      /* Published before the IB reaches the kernel: a BO destroyed from now on, on any thread,
       * also waits for this submission. */
      radv_wddm2_tracker_publish(queue->bc250_tracker, next_value);
      if (ws->deferred.witness && queue->bc250_tracker)
         radv_wddm2_witness_submit(ws, queue, submit, next_value);
      /* BC250_PROGRESS_FENCE=gpu: the IB1 ends with the GPU's write of next_value (radv_wddm2_bc250_submit);
       * the fence takes no kernel signal on this queue, ever (bc250_progress_va is fixed per binding). */
      const bool gpu_progress = queue->bc250_progress_va != 0;
      status = radv_wddm2_bc250_submit(ctx, queue, submit, gpu_progress ? next_value : 0);
      if (!NT_SUCCESS(status)) {
         amdgpu_wddm_log("bc250: native submit failed NTSTATUS=0x%X cs_count=%u\n", status, submit->cs_count);
         return VK_ERROR_DEVICE_LOST;
      }
      p_atomic_inc(&ws->submit_stats.submits);
      /* Ordering, BC250_PROGRESS_FENCE=gpu. The write is the IB1's last packet, an end-of-pipe
       * RELEASE_MEM; the KMD's ring fence, the same packet to its own fence page, follows the IB1 on the
       * ring, and end-of-pipe writes land in the order the CP issues them (Linux amdgpu_ib_schedule relies
       * on the same order: a job's user fence, then its ring fence). dxgkrnl signals the application's fences
       * of the call below only on that DMA buffer's completion, which the KMD reports from its ring
       * fence. Hence the invariant: an application fence of submission k reads its value only once
       * the progress fence reads at least k, so nothing that waited for the application's fence finds
       * a slot, a held BO or the embedder's record of k unretired. The reverse order is not promised
       * and not needed: progress may read k shortly before the application's fences of k, when all the
       * work of k is done and written back (CACHE_FLUSH_AND_INV_TS, GL2 write-back) and the CP has
       * fetched the last dword of the IB1. The application's fences keep their kernel call, imported
       * and shared ones included; only the progress fence, which the queue created unshared, moves. */
      if (gpu_progress) {
         p_atomic_inc(&ws->submit_stats.progress_gpu);
         const VkResult result = radv_wddm2_progress_signalled(ws, queue, slot, next_value);
         if (result != VK_SUCCESS)
            return result;
      }
      /* BC250_SUBMIT_COALESCE (kernel mode): the application's signals of this submission follow the IB
       * on the same context, and their call carries the progress value too. Every object of one signal
       * call is signalled at the same point of the context's queue, after the IB, so the slot, the
       * queue's last value, the embedder's record and every BO held behind next_value (published above)
       * see it retire when the IB has, as with a call of its own. Without application signals, or with
       * coalescing off, the progress value gets its own call, as before. */
      merge_progress = !gpu_progress && ws->bc250_merge_signals && signal_count > 0 &&
                       signal_count < D3DDDI_MAX_OBJECT_SIGNALED && !queue->handle;
      if (!gpu_progress && !merge_progress) {
         D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 progress_signal = {
            .ObjectCount = 1,
            .ObjectHandleArray = &queue->bc250_progress.handle,
            .BroadcastContextCount = 1,
            .BroadcastContextArray = &queue->context_h,
            .MonitoredFenceValueArray = &next_value,
         };
         status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &progress_signal);
         if (!NT_SUCCESS(status)) {
            queue->bc250_submit_failed = true; // accepted IB has no retirement value: never reuse its slot
            return VK_ERROR_DEVICE_LOST;
         }
         p_atomic_inc(&ws->submit_stats.progress_separate);
         const VkResult result = radv_wddm2_progress_signalled(ws, queue, slot, next_value);
         if (result != VK_SUCCESS)
            return result;
      }
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

      if (amdgpu_wddm_log_stream())
         print_hex_data(amdgpu_wddm_log_stream(), pdd.buffer, pdd.offset);

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
         amdgpu_wddm_log("SubmitCommand: VK_ERROR_DEVICE_LOST\n");
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (signal_count > 0) {
      const uint32_t count = signal_count + (merge_progress ? 1u : 0u);
      STACK_ARRAY(D3DKMT_HANDLE, handles, count);
      STACK_ARRAY(uint64_t, values, count);

      for (uint32_t i = 0; i < signal_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(signals[i].sync)->handle;
         values[i] = signals[i].signal_value;
      }
      if (merge_progress) {
         /* Last, so the application's fences keep their places. */
         handles[signal_count] = queue->bc250_progress.handle;
         values[signal_count] = next_value;
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
         D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
            .ObjectCount = count,
            .ObjectHandleArray = handles,
            .BroadcastContextCount = 1,
            .BroadcastContextArray = &queue->context_h,
            .MonitoredFenceValueArray = values,
         };
         status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &signal);
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      if (ws->bc250) {
         p_atomic_inc(&ws->submit_stats.signal_calls);
         p_atomic_add(&ws->submit_stats.signal_objects, (uint64_t)signal_count);
      }
      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status)) {
         if (merge_progress)
            queue->bc250_submit_failed = true; // accepted IB has no retirement value: never reuse its slot
         return VK_ERROR_DEVICE_LOST;
      }
      if (merge_progress) {
         p_atomic_inc(&ws->submit_stats.progress_merged);
         const VkResult result = radv_wddm2_progress_signalled(ws, queue, slot, next_value);
         if (result != VK_SUCCESS)
            return result;
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
      const char *nocopy = getenv("BC250_IB_NOCOPY");
      const char *cap = getenv("BC250_IB_DWORDS");
      ws->bc250_ib_dwords_cap = cap && cap[0] ? strtoul(cap, NULL, 0) : 0;
      ws->bc250_gather_copy = (nocopy && !strcmp(nocopy, "0")) || ws->bc250_ib_dwords_cap;
      if (!ws->adapter_query) {
         if (!ws->bc250_gather_copy)
            radv_wddm2_notice("IB submission: IB2 calls from the gather slot (BC250_IB_NOCOPY=0 copies every IB)");
         else if (nocopy && !strcmp(nocopy, "0"))
            radv_wddm2_notice("IB submission fallback: every IB copied into the gather slot (BC250_IB_NOCOPY=0)");
         else
            radv_wddm2_notice("IB submission fallback: every IB copied into the gather slot (BC250_IB_DWORDS=%lu"
                              " clamps the copied stream)", ws->bc250_ib_dwords_cap);
      }
   }

   ws->base.ctx_create = radv_wddm2_ctx_create;
   ws->base.ctx_destroy = radv_wddm2_ctx_destroy;
   if (ws->bc250 && ws->host.dispatch) {
      ws->base.ctx_create_bindable = radv_wddm2_ctx_create_bindable;
      ws->base.ctx_bind = radv_wddm2_ctx_bind;
      ws->base.ctx_unbind = radv_wddm2_ctx_unbind;
   }
   ws->base.ctx_wait_idle = radv_wddm2_ctx_wait_idle;
   ws->base.cs_domain = radv_wddm2_cs_domain;
   ws->base.cs_create = radv_wddm2_cs_create;
   ws->base.cs_finalize = radv_wddm2_cs_finalize;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_chain = radv_winsys_cs_chain;
   ws->base.cs_unchain = radv_winsys_cs_unchain;
   ws->base.cs_destroy = radv_wddm2_cs_destroy;
   ws->base.cs_grow = radv_winsys_cs_grow;
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
