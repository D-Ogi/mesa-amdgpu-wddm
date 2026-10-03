/* SPDX-License-Identifier: MIT
 *
 * Tests of runtime-bound queue contexts in the hosted BC250 winsys
 * (radv_wddm2_cs.c and radv_wddm2_bo.c, linked as compiled for
 * vulkan_radeon.dll). A scripted host dispatch stands in for the embedder:
 * it hands out contexts, fences and allocations, records every call with its
 * thread and bind scope, and fails a chosen call on request.
 *
 * Every test also checks the contract as a whole: queue-scoped operations
 * only inside the embedder's bind or unbind of that queue and with its
 * cookie, every call on the test's thread, no call naming a destroyed
 * context or fence, no device-level context.
 *
 * Scope: the tests call the winsys hooks (ctx_create_bindable, ctx_bind,
 * ctx_unbind, cs_submit, the sparse hooks, ctx_destroy, and for host
 * imports buffer_from_hosted, buffer_map, buffer_destroy, buffer_from_handle) directly, and the
 * allocate-chain parser of radv_host_import.h. They do
 * not cover the instance chain parser (radv_instance.c), the public bind
 * and unbind entries with their queue lock (radv_queue.c) or the queue
 * count admission of vkCreateDevice (radv_device.c).
 *
 * Built outside meson: compiled with the compile command of radv_wddm2_cs.c
 * and linked against the radv_wddm2_cs.c and radv_wddm2_bo.c objects and
 * the vulkan_util, amd_common, mesa_util and mesa_util_c11 libraries.
 *
 * Usage: radv_wddm2_queue_binding_test [test], no argument runs all.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bc250.h"
#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "winsys/common/radv_winsys_cs.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/bc250_host_bootstrap.h"
#include "radv_host_import.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/set.h"

#ifdef Status
#undef Status
#endif
#include <windows.h>
#include "d3dkmthk.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The winsys objects reference these; the tests provide them. BC250_WDDM_CALL
 * never takes the native path while host.dispatch is set. */
struct vk_wddm2_dispatch_table;
struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   fprintf(stderr, "native dispatch table requested: test setup error\n");
   abort();
}

/* Only the submission path of other WDDM drivers dumps its private data. */
void
print_hex_data(FILE *fp, const void *data, uint32_t size)
{
   fprintf(stderr, "non-BC250 submission path reached: test setup error\n");
   abort();
}

/* The application fences of the tests carry this type. */
const struct vk_sync_type vk_wddm2_monitored_fence_type = {0};
const struct vk_sync_type vk_wddm2_monitored_fence_hosted_type = {0};

#define HOST_FAIL        ((int32_t)0xC0000001) /* STATUS_UNSUCCESSFUL */
#define HOST_UNEXPECTED  ((int32_t)0xC0000002) /* STATUS_NOT_IMPLEMENTED */
#define DEVICE_H         0x20u
#define PAGING_QUEUE_H   0x30u
#define PAGING_FENCE_H   0x34u
#define MAX_OBJ          256
#define MAX_EV           4096
#define LOCK_BYTES       (256 * 1024)

struct obj {
   uint32_t handle;
   bool live;
   bool app;          /* syncs: an application fence of the test;
                       * allocations: the host's own, imported by the test */
   void *cookie;      /* contexts: the cookie they were created with */
   uint64_t value;    /* syncs: what the fence's CPU mapping shows */
   uint64_t gpu_va;   /* syncs: the FenceValueGPUVirtualAddress the host gave (h.gpu_va_mode) */
   void *mem;         /* allocations: Lock2 memory */
   bool locked;       /* allocations: a Lock2 the host has not seen unlocked */
};

struct event {
   uint32_t op;
   uint32_t context;  /* the context the call names, 0 if none */
   uint32_t sync;     /* the first fence it names, 0 if none */
   uint64_t value;
   void *cookie;
   bool in_scope;
   /* GPU signals and waits: every fence the call names, the first EV_OBJS of them with their values. */
   uint32_t n_objs;
   uint32_t objs[4];
   uint64_t vals[4];
   /* SubmitCommand of a watched queue: the value its IB1's RELEASE_MEM writes, 0 if none. */
   uint64_t progress_write;
};
#define EV_OBJS 4u

/* The fake GPU of BC250_PROGRESS_FENCE=gpu: the 64-bit RELEASE_MEM writes of the IB1s of the queues a test
 * watches, read from the gather slot at SubmitCommand and applied, in submission order, when the test runs
 * the GPU (gpu_run) or the CPU waits for such a fence. */
#define MAX_WRITES 4096
struct gpu_write {
   uint64_t va, value;
};

static struct {
   struct obj contexts[MAX_OBJ], syncs[MAX_OBJ], allocs[MAX_OBJ];
   unsigned n_contexts, n_syncs, n_allocs;
   struct event ev[MAX_EV];
   unsigned n_ev;
   uint64_t next_va;

   /* The fail_nth call of fail_op fails, and the fail_more calls after it. */
   uint32_t fail_op;
   unsigned fail_nth, fail_seen, fail_more;

   /* The event log starts over when full (the cost loops) instead of aborting. */
   bool recycle;

   int32_t status;       /* CHECK_STATUS; negative: the device is lost */

   /* The embedder's bind or unbind that is running, set by the tests. */
   bool in_scope;
   void *scope_cookie;
   DWORD thread;

   unsigned queue_op_out_of_scope, cookie_mismatch, wrong_thread;
   unsigned dead_context, dead_sync, device_contexts, unexpected;
   /* A DestroyAllocation2 of a locked allocation: refused, as the D3D12 shell's HostedDispatch refuses it. */
   unsigned locked_destroy;

   /* The last SubmitCommand: its IB and its BC2S blob. */
   uint64_t submit_va;
   uint32_t submit_bytes;
   struct bc250_submit_blob submit_blob;

   /* The FenceValueGPUVirtualAddress of the fences the host creates: 0 none (the default: the tests of
    * version 2 run the kernel signal, a host without GPU addresses), 1 a valid one, 2 one off its 8-byte
    * alignment, 3 one beyond 48 bits. */
   unsigned gpu_va_mode;
   const struct radv_wddm2_queue *watch[4];
   unsigned n_watch;
   struct gpu_write writes[MAX_WRITES];
   unsigned n_writes, n_done;
   unsigned rm_packets;  /* RELEASE_MEM packets in the watched IB1s */
   unsigned rm_not_last; /* of them, not the IB1's last packet */
   unsigned rm_bad;      /* not the KMD fence's packet, a malformed IB1, or a write to no fence */
} h;

static unsigned checks, failures;
static const char *current;

static void
check(bool ok, const char *fmt, ...)
{
   va_list ap;
   checks++;
   if (!ok)
      failures++;
   printf("%s %-16s ", ok ? "PASS" : "FAIL", current);
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

static struct obj *
find(struct obj *arr, unsigned n, uint32_t handle)
{
   for (unsigned i = 0; i < n; i++) {
      if (arr[i].handle == handle)
         return &arr[i];
   }
   return NULL;
}

static struct obj *
new_obj(struct obj *arr, unsigned *n, uint32_t base)
{
   if (*n >= MAX_OBJ)
      abort();
   struct obj *o = &arr[(*n)++];
   memset(o, 0, sizeof(*o));
   o->handle = base + *n;
   o->live = true;
   return o;
}

static unsigned
live(struct obj *arr, unsigned n, bool app)
{
   unsigned count = 0;
   for (unsigned i = 0; i < n; i++)
      count += arr[i].live && arr[i].app == app;
   return count;
}

static void
record(uint32_t op, uint32_t context, uint32_t sync, uint64_t value, void *cookie)
{
   if (h.n_ev >= MAX_EV) {
      if (!h.recycle)
         abort();
      h.n_ev = 0;
   }
   h.ev[h.n_ev++] = (struct event){op, context, sync, value, cookie, h.in_scope};
}

/* The fences of the GPU signal or wait just recorded. */
static void
record_objs(uint32_t count, const D3DKMT_HANDLE *handles, const uint64_t *values)
{
   struct event *e = &h.ev[h.n_ev - 1];
   e->n_objs = count;
   for (uint32_t i = 0; i < count && i < EV_OBJS; i++) {
      e->objs[i] = handles[i];
      e->vals[i] = values[i];
   }
}

static void
use_context(uint32_t handle)
{
   struct obj *o = find(h.contexts, h.n_contexts, handle);
   if (!o || !o->live)
      h.dead_context++;
}

static void
use_sync(uint32_t handle)
{
   if (handle == PAGING_FENCE_H)
      return;
   struct obj *o = find(h.syncs, h.n_syncs, handle);
   if (!o || !o->live)
      h.dead_sync++;
}

static void
queue_scope(const struct bc250_host_queue_context *q)
{
   if (!h.in_scope || q->queue != h.scope_cookie)
      h.queue_op_out_of_scope++;
}

/* The RELEASE_MEM writes of an IB1 in the gather slot of a watched queue; the value of the last, 0 if none. */
static uint64_t
gpu_take_writes(uint64_t va, uint32_t bytes)
{
   for (unsigned w = 0; w < h.n_watch; w++) {
      const struct radv_wddm2_queue *q = h.watch[w];
      for (unsigned s = 0; s < BC250_GATHER_SLOTS_MAX; s++) {
         if (!q->bc250_gather[s].bo || q->bc250_gather[s].bo->va != va)
            continue;
         const uint32_t *dw = (const uint32_t *)q->bc250_gather[s].map;
         const unsigned n = bytes / 4;
         uint64_t last = 0;
         for (unsigned i = 0; i < n;) {
            const uint32_t hdr = dw[i];
            unsigned len;
            if (hdr == 0xFFFF1000u /* PKT3_NOP_PAD */ || (hdr >> 30) == 2)
               len = 1;
            else if ((hdr >> 30) == 3)
               len = ((hdr >> 16) & 0x3FFF) + 2;
            else {
               h.rm_bad++;
               return last;
            }
            if ((hdr >> 30) == 3 && hdr != 0xFFFF1000u && ((hdr >> 8) & 0xFF) == 0x49) {
               h.rm_packets++;
               if (i + len != n)
                  h.rm_not_last++;
               if (hdr != 0xC0064900u || i + 8 > n || dw[i + 1] != 0x06603514u || dw[i + 2] != 0x40000000u ||
                   dw[i + 7]) {
                  h.rm_bad++;
               } else {
                  if (h.n_writes >= MAX_WRITES) {
                     if (!h.recycle)
                        abort();
                     h.n_writes = h.n_done = 0;
                  }
                  last = dw[i + 5] | (uint64_t)dw[i + 6] << 32;
                  h.writes[h.n_writes++] = (struct gpu_write){dw[i + 3] | (uint64_t)dw[i + 4] << 32, last};
               }
            }
            i += len;
         }
         return last;
      }
   }
   return 0;
}

/* The GPU runs the next count writes, in submission order; returns how many it ran. */
static unsigned
gpu_run(unsigned count)
{
   unsigned done = 0;
   while (h.n_done < h.n_writes && done < count) {
      const struct gpu_write *w = &h.writes[h.n_done++];
      struct obj *o = NULL;
      for (unsigned i = 0; i < h.n_syncs && !o; i++) {
         if (h.syncs[i].live && h.syncs[i].gpu_va && h.syncs[i].gpu_va == w->va)
            o = &h.syncs[i];
      }
      if (o)
         o->value = w->value;
      else
         h.rm_bad++;
      done++;
   }
   return done;
}

static int32_t
fake_dispatch(void *userdata, uint32_t op, void *arg)
{
   (void)userdata;
   if (GetCurrentThreadId() != h.thread)
      h.wrong_thread++;
   if (h.fail_op == op && ++h.fail_seen >= h.fail_nth && h.fail_seen <= h.fail_nth + h.fail_more) {
      record(op, 0, 0, 0, NULL);
      return HOST_FAIL;
   }

   switch (op) {
   case BC250_HOST_CHECK_STATUS:
      record(op, 0, 0, 0, NULL);
      return h.status;
   case BC250_HOST_REPORT_LOST:
      record(op, 0, 0, 0, NULL);
      return 0;
   case BC250_HOST_PUBLISH_PROGRESS: {
      const struct bc250_host_progress *p = arg;
      use_context(p->context);
      use_sync(p->sync);
      record(op, p->context, p->sync, p->value, NULL);
      return 0;
   }
   case BC250_HOST_CREATE_QUEUE_CONTEXT: {
      const struct bc250_host_queue_context *q = arg;
      D3DKMT_CREATECONTEXTVIRTUAL *c = q->arguments;
      queue_scope(q);
      if (c->hDevice != DEVICE_H || c->PrivateDriverDataSize != 80)
         h.unexpected++;
      struct obj *o = new_obj(h.contexts, &h.n_contexts, 0x1000);
      o->cookie = q->queue;
      c->hContext = o->handle;
      record(op, o->handle, 0, 0, q->queue);
      return 0;
   }
   case BC250_HOST_DESTROY_QUEUE_CONTEXT: {
      const struct bc250_host_queue_context *q = arg;
      const D3DKMT_DESTROYCONTEXT *d = q->arguments;
      queue_scope(q);
      struct obj *o = find(h.contexts, h.n_contexts, d->hContext);
      if (!o || !o->live)
         h.dead_context++;
      else if (o->cookie != q->queue)
         h.cookie_mismatch++;
      if (o)
         o->live = false;
      record(op, d->hContext, 0, 0, q->queue);
      return 0;
   }
   case BC250_HOST_CreateContextVirtual: {
      D3DKMT_CREATECONTEXTVIRTUAL *c = arg;
      h.device_contexts++;
      c->hContext = new_obj(h.contexts, &h.n_contexts, 0x1000)->handle;
      record(op, c->hContext, 0, 0, NULL);
      return 0;
   }
   case BC250_HOST_DestroyContext: {
      const D3DKMT_DESTROYCONTEXT *d = arg;
      h.device_contexts++;
      record(op, d->hContext, 0, 0, NULL);
      return 0;
   }
   case BC250_HOST_CreateSynchronizationObject2: {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *c = arg;
      struct obj *o = new_obj(h.syncs, &h.n_syncs, 0x2000);
      if (c->hDevice != DEVICE_H || c->Info.Type != D3DDDI_MONITORED_FENCE)
         h.unexpected++;
      if (c->Info.Flags.Shared || c->Info.Flags.NtSecuritySharing)
         h.unexpected++; /* the queue's fences are its own */
      c->hSyncObject = o->handle;
      c->Info.MonitoredFence.FenceValueCPUVirtualAddress = &o->value;
      static const uint64_t va_base[4] = {0, 0x7F0000000000ull, 0x7F0000000004ull, 0x1000000000000ull};
      o->gpu_va = h.gpu_va_mode < 4 && h.gpu_va_mode ? va_base[h.gpu_va_mode] + (uint64_t)o->handle * 0x100 : 0;
      c->Info.MonitoredFence.FenceValueGPUVirtualAddress = o->gpu_va;
      record(op, 0, o->handle, 0, NULL);
      return 0;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *d = arg;
      use_sync(d->hSyncObject);
      struct obj *o = find(h.syncs, h.n_syncs, d->hSyncObject);
      if (o)
         o->live = false;
      record(op, 0, d->hSyncObject, 0, NULL);
      return 0;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      /* The GPU completes what the CPU waits for, and nothing earlier: a fence the GPU writes through
       * the IB1s the fake GPU took gets its writes run in order up to the value. */
      for (unsigned i = 0; i < w->ObjectCount; i++) {
         use_sync(w->ObjectHandleArray[i]);
         struct obj *o = find(h.syncs, h.n_syncs, w->ObjectHandleArray[i]);
         while (o && o->gpu_va && o->value < w->FenceValueArray[i] && h.n_done < h.n_writes)
            gpu_run(1);
         if (o && o->value < w->FenceValueArray[i])
            o->value = w->FenceValueArray[i];
      }
      record(op, 0, w->ObjectCount ? w->ObjectHandleArray[0] : 0,
             w->ObjectCount ? w->FenceValueArray[0] : 0, NULL);
      if (w->hAsyncEvent)
         SetEvent(w->hAsyncEvent);
      return 0;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromGpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *w = arg;
      use_context(w->hContext);
      for (unsigned i = 0; i < w->ObjectCount; i++)
         use_sync(w->ObjectHandleArray[i]);
      if (!w->ObjectCount)
         h.unexpected++;
      record(op, w->hContext, w->ObjectHandleArray[0], w->MonitoredFenceValueArray[0], NULL);
      record_objs(w->ObjectCount, w->ObjectHandleArray, w->MonitoredFenceValueArray);
      return 0;
   }
   case BC250_HOST_SignalSynchronizationObjectFromGpu2: {
      const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *s = arg;
      for (unsigned i = 0; i < s->BroadcastContextCount; i++)
         use_context(s->BroadcastContextArray[i]);
      for (unsigned i = 0; i < s->ObjectCount; i++)
         use_sync(s->ObjectHandleArray[i]);
      if (!s->ObjectCount)
         h.unexpected++;
      record(op, s->BroadcastContextArray[0], s->ObjectHandleArray[0], s->MonitoredFenceValueArray[0], NULL);
      record_objs(s->ObjectCount, s->ObjectHandleArray, s->MonitoredFenceValueArray);
      return 0;
   }
   case BC250_HOST_SubmitCommand: {
      const D3DKMT_SUBMITCOMMAND *c = arg;
      for (unsigned i = 0; i < c->BroadcastContextCount; i++)
         use_context(c->BroadcastContext[i]);
      h.submit_va = c->Commands;
      h.submit_bytes = c->CommandLength;
      memset(&h.submit_blob, 0, sizeof(h.submit_blob));
      if (c->pPrivateDriverData)
         memcpy(&h.submit_blob, c->pPrivateDriverData, MIN2(c->PrivateDriverDataSize, sizeof(h.submit_blob)));
      const uint64_t write = gpu_take_writes(c->Commands, c->CommandLength);
      record(op, c->BroadcastContext[0], 0, c->Commands, NULL);
      h.ev[h.n_ev - 1].progress_write = write;
      return 0;
   }
   case BC250_HOST_UpdateGpuVirtualAddress: {
      const D3DKMT_UPDATEGPUVIRTUALADDRESS *u = arg;
      use_context(u->hContext);
      use_sync(u->hFenceObject);
      record(op, u->hContext, u->hFenceObject, u->FenceValue, NULL);
      return 0;
   }
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *c = arg;
      struct obj *o = new_obj(h.allocs, &h.n_allocs, 0x3000);
      c->pAllocationInfo2[0].hAllocation = o->handle;
      record(op, 0, 0, o->handle, NULL);
      return 0;
   }
   case BC250_HOST_DestroyAllocation2: {
      const D3DKMT_DESTROYALLOCATION2 *d = arg;
      for (unsigned i = 0; i < d->AllocationCount; i++) {
         struct obj *o = find(h.allocs, h.n_allocs, d->phAllocationList[i]);
         if (!o || !o->live) {
            h.unexpected++;
            continue;
         }
         if (o->locked) {
            h.locked_destroy++;
            record(op, 0, 0, o->handle, NULL);
            return HOST_FAIL;
         }
         o->live = false;
         free(o->mem);
         o->mem = NULL;
         record(op, 0, 0, o->handle, NULL);
      }
      return 0;
   }
   case BC250_HOST_ReserveGpuVirtualAddress: {
      D3DDDI_RESERVEGPUVIRTUALADDRESS *r = arg;
      if (r->BaseAddress) {
         r->VirtualAddress = r->BaseAddress;
      } else {
         h.next_va = MAX2(align64(h.next_va, 65536), align64(r->MinimumAddress, 65536));
         r->VirtualAddress = h.next_va;
         h.next_va += align64(r->Size, 65536);
      }
      record(op, 0, 0, r->VirtualAddress, NULL);
      return 0;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      if (m->BaseAddress) {
         m->VirtualAddress = m->BaseAddress;
      } else {
         h.next_va = MAX2(align64(h.next_va, 65536), align64(m->MinimumAddress, 65536));
         m->VirtualAddress = h.next_va;
         h.next_va += align64(m->SizeInPages * 4096ull, 65536);
      }
      m->PagingFenceValue = 0;
      record(op, 0, 0, m->VirtualAddress, NULL);
      return 0;
   }
   case BC250_HOST_MakeResident: {
      D3DDDI_MAKERESIDENT *r = arg;
      r->PagingFenceValue = 0;
      record(op, 0, 0, 0, NULL);
      return 0;
   }
   case BC250_HOST_Lock2: {
      D3DKMT_LOCK2 *l = arg;
      struct obj *o = find(h.allocs, h.n_allocs, l->hAllocation);
      if (!o || !o->live) {
         h.unexpected++;
         return HOST_FAIL;
      }
      if (o->locked)
         h.unexpected++; /* the ICD keeps one lock per allocation */
      o->locked = true;
      if (!o->mem)
         o->mem = calloc(1, LOCK_BYTES);
      l->pData = o->mem;
      record(op, 0, 0, o->handle, NULL);
      return 0;
   }
   case BC250_HOST_Unlock2: {
      const D3DKMT_UNLOCK2 *u = arg;
      struct obj *o = find(h.allocs, h.n_allocs, u->hAllocation);
      if (!o || !o->live || !o->locked)
         h.unexpected++; /* an Unlock2 without its Lock2 */
      if (o)
         o->locked = false;
      record(op, 0, 0, u->hAllocation, NULL);
      return 0;
   }
   case BC250_HOST_FreeGpuVirtualAddress:
   case BC250_HOST_Evict:
   case BC250_HOST_GetDeviceState:
      record(op, 0, 0, 0, NULL);
      return 0;
   default:
      h.unexpected++;
      record(op, 0, 0, 0, NULL);
      return HOST_UNEXPECTED;
   }
}

/* The host's policy for the next make_ws, as radv_wddm2_winsys_create would have copied it in;
 * NULL is a host that chained none. */
static const struct bc250_host_policy_values *ws_policy;

static struct radv_wddm2_winsys *
make_ws(void)
{
   memset(&h, 0, sizeof(h));
   h.thread = GetCurrentThreadId();

   struct radv_wddm2_winsys *ws = calloc(1, sizeof(*ws));
   ws->bc250 = true;
   if (ws_policy)
      ws->bc250_policy = *ws_policy;
   ws->host.sType = BC250_HOST_STYPE;
   ws->host.version = BC250_HOST_VERSION;
   ws->host.size = sizeof(ws->host);
   ws->host.identity = (void *)(uintptr_t)0x1;
   ws->host.userdata = &h;
   ws->host.dispatch = fake_dispatch;
   ws->adapter_h = 0x10;
   ws->device_h = DEVICE_H;
   ws->paging_queue_h = PAGING_QUEUE_H;
   ws->paging_fence_h = PAGING_FENCE_H;
   ws->gpu_info.pte_fragment_size = 0x200000;
   ws->gpu_info.gart_page_size = 4096;
   radv_wddm2_bo_init_functions(ws);
   radv_wddm2_cs_init_functions(ws);
   return ws;
}

/* The embedder's bind and unbind of the queue that cookie names. */
static VkResult
bind(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx, void *cookie)
{
   h.in_scope = true;
   h.scope_cookie = cookie;
   VkResult result = ws->base.ctx_bind(ctx, cookie);
   h.in_scope = false;
   h.scope_cookie = NULL;
   return result;
}

static VkResult
unbind(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx, void *cookie)
{
   h.in_scope = true;
   h.scope_cookie = cookie;
   VkResult result = ws->base.ctx_unbind(ctx);
   h.in_scope = false;
   h.scope_cookie = NULL;
   return result;
}

static struct radeon_winsys_ctx *
new_ctx(struct radv_wddm2_winsys *ws)
{
   struct radeon_winsys_ctx *ctx = NULL;
   if (!ws->base.ctx_create_bindable ||
       ws->base.ctx_create_bindable(&ws->base, RADEON_CTX_PRIORITY_MEDIUM, &ctx) != VK_SUCCESS) {
      fprintf(stderr, "ctx_create_bindable failed\n");
      exit(2);
   }
   return ctx;
}

static struct radv_wddm2_queue *
gfx(struct radeon_winsys_ctx *ctx)
{
   return &radv_wddm2_ctx(ctx)->per_ip[AMD_IP_GFX].queue;
}

static void
app_fence(struct vk_wddm2_monitored_fence *fence)
{
   memset(fence, 0, sizeof(*fence));
   fence->base.type = &vk_wddm2_monitored_fence_type;
   struct obj *o = new_obj(h.syncs, &h.n_syncs, 0x2000);
   o->app = true;
   fence->handle = o->handle;
   fence->value_map = &o->value;
}

/* An allocation the host owns and hands over through bc250_host_import. */
static struct obj *
host_alloc(void)
{
   struct obj *o = new_obj(h.allocs, &h.n_allocs, 0x3000);
   o->app = true;
   return o;
}

/* Laid out as radv_wddm2_cs: the stream, then its BO set (none unless a test makes one). */
struct fake_cs {
   struct radv_winsys_cs cs;
   struct set *buffers;
   struct radv_winsys_ib ib;
};

static struct ac_cmdbuf *
fake_cs(struct fake_cs *f, struct radeon_winsys_bo *bo, uint64_t va, unsigned cdw)
{
   memset(f, 0, sizeof(*f));
   f->ib.bo = bo;
   f->ib.va = va;
   f->ib.cdw = cdw;
   f->cs.ib_buffers = &f->ib;
   f->cs.num_ib_buffers = 1;
   f->cs.hw_ip = AMD_IP_GFX;
   return &f->cs.base;
}

static VkResult
submit(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx, unsigned cs_count, struct ac_cmdbuf **cs,
       uint32_t wait_count, const struct vk_sync_wait *waits, uint32_t signal_count,
       const struct vk_sync_signal *signals)
{
   const struct radv_winsys_submit_info info = {
      .ip_type = AMD_IP_GFX,
      .cs_count = cs_count,
      .cs_array = cs,
   };
   return ws->base.cs_submit(ctx, &info, wait_count, waits, signal_count, signals);
}

static VkResult
submit_one(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx)
{
   struct fake_cs f;
   struct ac_cmdbuf *cs = fake_cs(&f, NULL, 0x100000, 8);
   return submit(ws, ctx, 1, &cs, 0, NULL, 0, NULL);
}

static unsigned
count_op(unsigned from, uint32_t op)
{
   unsigned n = 0;
   for (unsigned i = from; i < h.n_ev; i++)
      n += h.ev[i].op == op;
   return n;
}

static int
find_op(unsigned from, uint32_t op, uint32_t sync)
{
   for (unsigned i = from; i < h.n_ev; i++) {
      if (h.ev[i].op == op && (!sync || h.ev[i].sync == sync))
         return (int)i;
   }
   return -1;
}

/* Every call since from that names a context names this one. */
static bool
names_only(unsigned from, uint32_t context)
{
   for (unsigned i = from; i < h.n_ev; i++) {
      if (h.ev[i].context && h.ev[i].context != context)
         return false;
   }
   return true;
}

static bool
names_sync(unsigned from, uint32_t sync)
{
   for (unsigned i = from; i < h.n_ev; i++) {
      if (h.ev[i].sync == sync)
         return true;
   }
   return false;
}

static bool
event_is(int index, uint32_t op, uint32_t context, uint32_t sync, uint64_t value)
{
   if (index < 0)
      return false;
   const struct event *e = &h.ev[index];
   return e->op == op && e->context == context && e->sync == sync && e->value == value;
}

/* The GPU signal or wait at index names fence with value. */
static bool
event_names(int index, uint32_t fence, uint64_t value)
{
   if (index < 0)
      return false;
   const struct event *e = &h.ev[index];
   for (uint32_t i = 0; i < e->n_objs && i < EV_OBJS; i++) {
      if (e->objs[i] == fence && e->vals[i] == value)
         return true;
   }
   return false;
}

/* The first GPU signal or wait since from that names fence at any position. */
static int
find_naming(unsigned from, uint32_t op, uint32_t fence)
{
   for (unsigned i = from; i < h.n_ev; i++) {
      if (h.ev[i].op != op)
         continue;
      for (uint32_t k = 0; k < h.ev[i].n_objs && k < EV_OBJS; k++) {
         if (h.ev[i].objs[k] == fence)
            return (int)i;
      }
   }
   return -1;
}

static void
contract(void)
{
   check(!h.queue_op_out_of_scope, "queue-scoped calls only inside the bind or unbind of their cookie (%u outside)",
         h.queue_op_out_of_scope);
   check(!h.cookie_mismatch, "every context goes back through the cookie it came from (%u mismatches)",
         h.cookie_mismatch);
   check(!h.wrong_thread, "every call on the calling thread (%u on others)", h.wrong_thread);
   check(!h.dead_context && !h.dead_sync, "no call names a destroyed context or fence (%u, %u)", h.dead_context,
         h.dead_sync);
   check(!h.device_contexts, "no device-level context (%u calls)", h.device_contexts);
   check(!h.unexpected, "no unexpected host call (%u)", h.unexpected);
   check(!h.locked_destroy, "no allocation destroyed while locked (%u)", h.locked_destroy);
}

static void
test_bind_failure(void)
{
   static const struct {
      uint32_t op;
      unsigned nth;
      const char *name;
      unsigned destroyed_contexts;
   } steps[] = {
      {BC250_HOST_CREATE_QUEUE_CONTEXT, 1, "context", 0},
      {BC250_HOST_CreateSynchronizationObject2, 1, "progress fence", 1},
      {BC250_HOST_CreateSynchronizationObject2, 2, "companion fence", 1},
   };
   void *const cookie = (void *)(uintptr_t)0xA0;

   for (unsigned i = 0; i < ARRAY_SIZE(steps); i++) {
      struct radv_wddm2_winsys *ws = make_ws();
      struct radeon_winsys_ctx *ctx = new_ctx(ws);
      h.fail_op = steps[i].op;
      h.fail_nth = steps[i].nth;

      VkResult result = bind(ws, ctx, cookie);
      check(result != VK_SUCCESS, "%s failure fails the bind (%d)", steps[i].name, result);
      check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false),
            "%s failure leaves no context or fence (%u, %u live)", steps[i].name,
            live(h.contexts, h.n_contexts, false), live(h.syncs, h.n_syncs, false));
      check(count_op(0, BC250_HOST_DESTROY_QUEUE_CONTEXT) == steps[i].destroyed_contexts,
            "%s failure destroys the context it created, within the bind (%u)", steps[i].name,
            count_op(0, BC250_HOST_DESTROY_QUEUE_CONTEXT));

      unsigned mark = h.n_ev;
      check(submit_one(ws, ctx) == VK_ERROR_VALIDATION_FAILED && h.n_ev == mark,
            "after a failed bind the queue refuses to submit, with no host call");

      h.fail_op = 0;
      result = bind(ws, ctx, cookie);
      check(result == VK_SUCCESS && live(h.contexts, h.n_contexts, false) == 1 &&
               live(h.syncs, h.n_syncs, false) == 2,
            "a retry binds: one context, two fences (%d)", result);
      check(unbind(ws, ctx, cookie) == VK_SUCCESS && !live(h.contexts, h.n_contexts, false) &&
               !live(h.syncs, h.n_syncs, false),
            "unbind after the retry releases everything");
      mark = h.n_ev;
      ws->base.ctx_destroy(ctx);
      check(h.n_ev == mark, "destroying the unbound context makes no host call");
      contract();
   }

   /* A lost device and a second bind are refused before creating anything. */
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *ctx = new_ctx(ws);
   h.status = -1;
   VkResult result = bind(ws, ctx, cookie);
   check(result == VK_ERROR_DEVICE_LOST && !count_op(0, BC250_HOST_CREATE_QUEUE_CONTEXT) &&
            !count_op(0, BC250_HOST_CreateSynchronizationObject2),
         "a lost device is refused before any creation (%d)", result);
   h.status = 0;
   check(bind(ws, ctx, cookie) == VK_SUCCESS, "the same queue binds once the device is back");
   unsigned mark = h.n_ev;
   check(bind(ws, ctx, cookie) != VK_SUCCESS && h.n_ev == mark, "a bound queue refuses a second bind, no host call");
   check(unbind(ws, ctx, cookie) == VK_SUCCESS, "unbind");
   mark = h.n_ev;
   check(unbind(ws, ctx, cookie) != VK_SUCCESS && h.n_ev == mark, "an unbound queue refuses unbind, no host call");
   ws->base.ctx_destroy(ctx);
   contract();
}

static void
test_second_queue(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);

   check(bind(ws, a, cookie_a) == VK_SUCCESS && bind(ws, b, cookie_b) == VK_SUCCESS, "both queues bind");
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   check(qa->context_h && qb->context_h && qa->context_h != qb->context_h, "each queue has its own context (%x, %x)",
         qa->context_h, qb->context_h);
   const uint32_t fences[4] = {qa->bc250_progress.handle, qa->vm_fence.handle, qb->bc250_progress.handle,
                               qb->vm_fence.handle};
   bool distinct = true;
   for (unsigned i = 0; i < 4; i++) {
      distinct &= fences[i] != 0;
      for (unsigned j = 0; j < i; j++)
         distinct &= fences[i] != fences[j];
   }
   check(distinct, "each queue has its own progress and companion fences (%x %x %x %x)", fences[0], fences[1],
         fences[2], fences[3]);
   check(find(h.contexts, h.n_contexts, qa->context_h)->cookie == cookie_a &&
            find(h.contexts, h.n_contexts, qb->context_h)->cookie == cookie_b,
         "each context was created through its own queue's cookie");
   check(live(h.contexts, h.n_contexts, false) == 2 && live(h.syncs, h.n_syncs, false) == 4,
         "two queues hold two contexts and four fences, no more");

   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false), "nothing left");
   contract();
}

/* BC250_SUBMIT_COALESCE=0: the sequence before coalescing, a progress signal of its own after every IB
 * (test_submit_coalesce has the default). */
static void
test_signal_wait(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   _putenv_s("BC250_SUBMIT_COALESCE", "0");
   struct radv_wddm2_winsys *ws = make_ws();
   _putenv_s("BC250_SUBMIT_COALESCE", "");
   check(!ws->bc250_merge_signals && !ws->bc250_drop_waits, "BC250_SUBMIT_COALESCE=0: no merge, no dropped wait");
   const unsigned slots = ws->bc250_gather_slots;
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   struct vk_wddm2_monitored_fence f1, f2;
   app_fence(&f1);
   app_fence(&f2);
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, NULL, 0x100000, 8);

   /* A signals f1. */
   unsigned mark = h.n_ev;
   const struct vk_sync_signal signal1 = {.sync = &f1.base, .signal_value = 1};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &signal1) == VK_SUCCESS, "A submits and signals f1");
   int sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
   int prog = find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, qa->bc250_progress.handle);
   int pub = find_op(mark, BC250_HOST_PUBLISH_PROGRESS, 0);
   int sig = find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, f1.handle);
   check(event_is(sub, BC250_HOST_SubmitCommand, qa->context_h, 0, 0x100000), "A's IB goes to A's context");
   check(event_is(prog, BC250_HOST_SignalSynchronizationObjectFromGpu2, qa->context_h, qa->bc250_progress.handle, 1),
         "A's progress fence 1 is signalled on A's context");
   check(event_is(pub, BC250_HOST_PUBLISH_PROGRESS, qa->context_h, qa->bc250_progress.handle, 1),
         "A's progress is published with A's context and fence");
   check(event_is(sig, BC250_HOST_SignalSynchronizationObjectFromGpu2, qa->context_h, f1.handle, 1),
         "f1 is signalled on A's context");
   check(sub < prog && prog < sig, "order: IB, progress, application signal");
   check(names_only(mark, qa->context_h) && !names_sync(mark, qb->bc250_progress.handle) &&
            !names_sync(mark, qb->vm_fence.handle),
         "A's submission names nothing of B");

   /* B waits for f1 and signals f2. */
   mark = h.n_ev;
   const struct vk_sync_wait wait1 = {.sync = &f1.base, .wait_value = 1};
   const struct vk_sync_signal signal2 = {.sync = &f2.base, .signal_value = 1};
   check(submit(ws, b, 1, &cs, 1, &wait1, 1, &signal2) == VK_SUCCESS, "B waits for f1, submits, signals f2");
   int wait = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromGpu, 0);
   sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
   prog = find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, qb->bc250_progress.handle);
   sig = find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, f2.handle);
   check(event_is(wait, BC250_HOST_WaitForSynchronizationObjectFromGpu, qb->context_h, f1.handle, 1),
         "B's context waits for f1 on the GPU");
   check(event_is(sub, BC250_HOST_SubmitCommand, qb->context_h, 0, 0x100000), "B's IB goes to B's context");
   check(event_is(prog, BC250_HOST_SignalSynchronizationObjectFromGpu2, qb->context_h, qb->bc250_progress.handle, 1),
         "B's progress starts at 1 on its own fence");
   check(event_is(sig, BC250_HOST_SignalSynchronizationObjectFromGpu2, qb->context_h, f2.handle, 1),
         "f2 is signalled on B's context");
   check(wait < sub && sub < prog && prog < sig, "order: wait, IB, progress, application signal");
   check(names_only(mark, qb->context_h) && !names_sync(mark, qa->bc250_progress.handle) &&
            !names_sync(mark, qa->vm_fence.handle),
         "B's submission names nothing of A");
   check(!count_op(0, BC250_HOST_WaitForSynchronizationObjectFromCpu), "no CPU wait so far");

   /* Off, a wait the CPU already sees complete still reaches the GPU. */
   *f1.value_map = 1;
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 1, &wait1, 0, NULL) == VK_SUCCESS &&
            event_is(find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromGpu, 0),
                     BC250_HOST_WaitForSynchronizationObjectFromGpu, qb->context_h, f1.handle, 1),
         "off, B's wait for a complete f1 still goes to the GPU");
   check(ws->submit_stats.progress_separate == 3 && !ws->submit_stats.progress_merged &&
            ws->submit_stats.wait_calls == 2 && !ws->submit_stats.wait_dropped,
         "counted: 3 progress signals of their own, none merged, 2 wait calls, none dropped");

   /* Gather slots retire on the queue's own progress fence. The fake GPU
    * completes nothing on its own, so reusing A's first slot needs a CPU
    * wait for A's value 1. */
   for (unsigned i = 1; i < slots; i++)
      submit_one(ws, a);
   check(!count_op(0, BC250_HOST_WaitForSynchronizationObjectFromCpu), "A's first %u submissions need no CPU wait",
         slots);
   mark = h.n_ev;
   check(submit_one(ws, a) == VK_SUCCESS, "A's submission %u", slots + 1);
   int cpu = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0);
   check(event_is(cpu, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0, qa->bc250_progress.handle, 1) &&
            count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 1,
         "reusing A's first slot waits for A's progress value 1, once");
   check(names_only(mark, qa->context_h) && !names_sync(mark, qb->bc250_progress.handle),
         "A's slot reuse never waits on B");
   mark = h.n_ev;
   for (unsigned i = 2; i < slots; i++)
      submit_one(ws, b);
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu),
         "B's slots are its own: %u submissions, no CPU wait", slots - 2);
   check(qa->bc250_progress.wait_value == slots + 1 && qb->bc250_progress.wait_value == slots,
         "progress values count per queue (A %llu, B %llu)", (unsigned long long)qa->bc250_progress.wait_value,
         (unsigned long long)qb->bc250_progress.wait_value);

   unbind(ws, a, cookie_a);
   unbind(ws, b, cookie_b);
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
}

static void
test_gather_teardown(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   const uint32_t a_context = qa->context_h, a_progress = qa->bc250_progress.handle, a_vm = qa->vm_fence.handle;

   /* Two command streams are packed into a gather BO of A's. */
   struct radeon_winsys_bo *src[2] = {NULL, NULL};
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT,
                             RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &src[i]);
   check(src[0] && src[1], "two command BOs");
   const unsigned allocs_before = live(h.allocs, h.n_allocs, false);
   struct fake_cs f0, f1;
   struct ac_cmdbuf *cs[2] = {fake_cs(&f0, src[0], src[0]->va, 16), fake_cs(&f1, src[1], src[1]->va, 16)};
   check(submit(ws, a, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "A submits two command streams");
   check(live(h.allocs, h.n_allocs, false) == allocs_before + 1 && qa->bc250_gather[0].bo,
         "A owns one gather BO");
   const uint32_t gather = qa->bc250_gather[0].bo->handle;

   /* Unbind: wait for A's last submission, then progress fence, gather
    * BOs, companion fence, context, all inside the unbind. */
   unsigned mark = h.n_ev;
   check(unbind(ws, a, cookie_a) == VK_SUCCESS, "A unbinds");
   int wait = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0);
   int progress = find_op(mark, BC250_HOST_DestroySynchronizationObject, a_progress);
   int bo = -1;
   for (unsigned i = mark; i < h.n_ev; i++) {
      if (h.ev[i].op == BC250_HOST_DestroyAllocation2 && h.ev[i].value == gather)
         bo = (int)i;
   }
   int vm = find_op(mark, BC250_HOST_DestroySynchronizationObject, a_vm);
   int context = find_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT, 0);
   check(event_is(wait, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0, a_progress, 1),
         "unbind first waits for A's last submission (progress 1)");
   check(wait < progress && progress < bo && bo < vm && vm < context,
         "then releases progress fence, gather BO, companion fence, context (%d %d %d %d %d)", wait, progress, bo,
         vm, context);
   check(context >= 0 && h.ev[context].context == a_context && h.ev[context].cookie == cookie_a &&
            h.ev[context].in_scope,
         "A's context goes back through A's cookie, inside the unbind");
   check(live(h.allocs, h.n_allocs, false) == allocs_before && live(h.syncs, h.n_syncs, false) == 2 &&
            live(h.contexts, h.n_contexts, false) == 1,
         "only B's context and fences remain");

   /* Nothing names A again. */
   mark = h.n_ev;
   check(submit_one(ws, a) == VK_ERROR_VALIDATION_FAILED, "A refuses to submit after unbind");
   check(ws->base.buffer_virtual_bind_begin(&ws->base, a, AMD_IP_GFX) == VK_ERROR_VALIDATION_FAILED,
         "A refuses a sparse transaction after unbind");
   check(ws->base.ctx_wait_idle(a, AMD_IP_GFX, 0), "A is idle after unbind");
   ws->base.ctx_destroy(a);
   check(h.n_ev == mark, "after unbind A makes no host call at all");

   /* B is untouched. */
   mark = h.n_ev;
   check(submit_one(ws, b) == VK_SUCCESS && names_only(mark, qb->context_h), "B still submits on its own context");

   unbind(ws, b, cookie_b);
   ws->base.ctx_destroy(b);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, src[i]);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false) &&
            !live(h.allocs, h.n_allocs, false),
         "nothing left");
   contract();
}

static void
test_device_loss(void)
{
   void *const cookie = (void *)(uintptr_t)0xC0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *c = new_ctx(ws);
   bind(ws, c, cookie);
   const uint32_t context = gfx(c)->context_h;
   const uint32_t progress = gfx(c)->bc250_progress.handle, vm = gfx(c)->vm_fence.handle;
   check(submit_one(ws, c) == VK_SUCCESS, "C submits once; its progress 1 is pending");

   h.status = -1;
   unsigned mark = h.n_ev;
   VkResult result = unbind(ws, c, cookie);
   check(result == VK_ERROR_DEVICE_LOST, "unbind on a lost device reports the unretired submission (%d)", result);
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu), "no CPU wait on a lost device");
   check(!count_op(mark, BC250_HOST_DestroySynchronizationObject) && !count_op(mark, BC250_HOST_DestroyAllocation2) &&
            !count_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT),
         "a lost device is not retirement: nothing is destroyed");
   check(gfx(c)->context_h == context && gfx(c)->bc250_progress.handle == progress && gfx(c)->vm_fence.handle == vm,
         "the queue keeps its context and both fences");
   check(find(h.contexts, h.n_contexts, context)->live && live(h.syncs, h.n_syncs, false) == 2, "and they stay live");
   check(!ws->base.ctx_wait_idle(c, AMD_IP_GFX, 0), "the unbound queue does not report idle");

   h.status = 0;
   mark = h.n_ev;
   check(bind(ws, c, cookie) != VK_SUCCESS && h.n_ev == mark,
         "the queue is not bound again, even once the device is back, with no host call");
   check(submit_one(ws, c) == VK_ERROR_VALIDATION_FAILED && h.n_ev == mark, "and refuses to submit");
   ws->base.ctx_destroy(c);
   check(h.n_ev == mark, "destroying it makes no host call: what it kept is left to the device");
   contract();
}

static void
test_abandon(void)
{
   void *const cookie = (void *)(uintptr_t)0xD0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *d = new_ctx(ws);
   bind(ws, d, cookie);
   const uint32_t context = gfx(d)->context_h;
   submit_one(ws, d);

   /* vkDestroyDevice with the queue still bound: the context stays with the
    * embedder's queue, the device's fences go. */
   unsigned mark = h.n_ev;
   ws->base.ctx_destroy(d);
   bool cookie_used = false;
   for (unsigned i = mark; i < h.n_ev; i++)
      cookie_used |= h.ev[i].cookie != NULL;
   check(!count_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT) && !cookie_used,
         "destroy while bound never calls through the cookie");
   check(count_op(mark, BC250_HOST_DestroySynchronizationObject) == 2 && !live(h.syncs, h.n_syncs, false),
         "its fences are released");
   check(find(h.contexts, h.n_contexts, context)->live, "its context is left to the embedder's queue");
   contract();
}

static void
test_outside_scope(void)
{
   void *const cookie = (void *)(uintptr_t)0xE0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *e = new_ctx(ws);
   struct vk_wddm2_monitored_fence f;
   app_fence(&f);
   const struct vk_sync_wait wait = {.sync = &f.base, .wait_value = 1};
   const struct vk_sync_signal signal = {.sync = &f.base, .signal_value = 2};
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, NULL, 0x100000, 8);

   unsigned mark = h.n_ev;
   check(submit(ws, e, 1, &cs, 1, &wait, 1, &signal) == VK_ERROR_VALIDATION_FAILED,
         "a queue never bound refuses a submission");
   check(submit(ws, e, 0, NULL, 0, NULL, 1, &signal) == VK_ERROR_VALIDATION_FAILED,
         "and a signal-only submission");
   check(ws->base.buffer_virtual_bind_begin(&ws->base, e, AMD_IP_GFX) == VK_ERROR_VALIDATION_FAILED,
         "and a sparse transaction");
   check(ws->base.ctx_wait_idle(e, AMD_IP_GFX, 0), "and is idle");
   check(h.n_ev == mark, "none of that reaches the host");

   check(bind(ws, e, cookie) == VK_SUCCESS && submit(ws, e, 1, &cs, 0, NULL, 1, &signal) == VK_SUCCESS,
         "bound, it submits");
   check(unbind(ws, e, cookie) == VK_SUCCESS, "unbind");
   mark = h.n_ev;
   check(submit(ws, e, 1, &cs, 1, &wait, 1, &signal) == VK_ERROR_VALIDATION_FAILED && h.n_ev == mark,
         "after unbind it refuses again, with no host call");

   /* Nothing arrives after the calls returned: the winsys submits nothing
    * asynchronously. */
   mark = h.n_ev;
   Sleep(100);
   check(h.n_ev == mark, "no host call after the last call returned (100 ms)");
   ws->base.ctx_destroy(e);
   contract();
}

static void
test_internal_queue(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *internal = new_ctx(ws), *a = new_ctx(ws);

   check(bind(ws, internal, NULL) == VK_SUCCESS, "the internal queue binds with a NULL cookie");
   check(bind(ws, a, cookie_a) == VK_SUCCESS, "an application queue binds");
   const uint32_t internal_context = gfx(internal)->context_h, a_context = gfx(a)->context_h;
   check(find(h.contexts, h.n_contexts, internal_context)->cookie == NULL &&
            find(h.contexts, h.n_contexts, a_context)->cookie == cookie_a,
         "the internal context comes from the NULL cookie, the other from its own");

   unsigned mark = h.n_ev;
   check(unbind(ws, a, cookie_a) == VK_SUCCESS, "the application queue unbinds");
   int destroy = find_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT, 0);
   check(event_is(destroy, BC250_HOST_DESTROY_QUEUE_CONTEXT, a_context, 0, 0) && h.ev[destroy].cookie == cookie_a &&
            count_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT) == 1,
         "only its context goes, through its cookie");
   check(find(h.contexts, h.n_contexts, internal_context)->live, "the internal context lives on");
   mark = h.n_ev;
   check(submit_one(ws, internal) == VK_SUCCESS && names_only(mark, internal_context),
         "the internal queue still submits on its own context");

   mark = h.n_ev;
   check(unbind(ws, internal, NULL) == VK_SUCCESS, "the internal queue unbinds last");
   destroy = find_op(mark, BC250_HOST_DESTROY_QUEUE_CONTEXT, 0);
   check(event_is(destroy, BC250_HOST_DESTROY_QUEUE_CONTEXT, internal_context, 0, 0) &&
            h.ev[destroy].cookie == NULL,
         "its context goes back through the NULL cookie");
   ws->base.ctx_destroy(internal);
   ws->base.ctx_destroy(a);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false), "nothing left");
   contract();
}

static void
test_sparse(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);

   struct radeon_winsys_bo *parent = NULL, *memory = NULL;
   ws->base.buffer_create(&ws->base, 65536, 65536, RADEON_DOMAIN_VRAM, RADEON_FLAG_VIRTUAL, 0, 0, NULL, &parent);
   ws->base.buffer_create(&ws->base, 65536, 65536, RADEON_DOMAIN_VRAM, RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0,
                          NULL, &memory);
   check(parent && memory, "a sparse buffer and its memory");

   unsigned mark = h.n_ev;
   check(ws->base.buffer_virtual_bind_begin(&ws->base, b, AMD_IP_GFX) == VK_SUCCESS &&
            ws->base.buffer_virtual_bind(&ws->base, b, AMD_IP_GFX, parent, 0, 65536, memory, 0) == VK_SUCCESS &&
            ws->base.buffer_virtual_bind_end(&ws->base, b, AMD_IP_GFX, 0, 0, NULL, true) == VK_SUCCESS,
         "B binds sparse memory");
   int boundary = find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, 0);
   int update = find_op(mark, BC250_HOST_UpdateGpuVirtualAddress, 0);
   int wait = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromGpu, 0);
   check(event_is(boundary, BC250_HOST_SignalSynchronizationObjectFromGpu2, qb->context_h, qb->vm_fence.handle, 1),
         "the boundary is signalled on B's context with B's companion fence");
   check(event_is(update, BC250_HOST_UpdateGpuVirtualAddress, qb->context_h, qb->vm_fence.handle, 1),
         "the mapping update runs on B's context and B's companion fence");
   check(event_is(wait, BC250_HOST_WaitForSynchronizationObjectFromGpu, qb->context_h, qb->vm_fence.handle, 2),
         "B's context waits for the update before later work");
   check(names_only(mark, qb->context_h) && !names_sync(mark, qa->vm_fence.handle) &&
            !names_sync(mark, qa->bc250_progress.handle),
         "nothing of A takes part");
   check(qa->vm_fence.wait_value == 0 && qb->vm_fence.wait_value == 2, "only B's companion fence advanced");

   unbind(ws, a, cookie_a);
   unbind(ws, b, cookie_b);
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   ws->base.buffer_destroy(&ws->base, parent);
   ws->base.buffer_destroy(&ws->base, memory);
   contract();
}

static void
test_rebind(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *q = new_ctx(ws);

   /* The first user wraps the gather slots, so the queue ends with a
    * gather BO, retirement values and a progress value of its own. */
   check(bind(ws, q, cookie_a) == VK_SUCCESS, "the queue binds for its first user");
   const uint32_t old_context = gfx(q)->context_h;
   const uint32_t old_progress = gfx(q)->bc250_progress.handle, old_vm = gfx(q)->vm_fence.handle;
   struct radeon_winsys_bo *src[2] = {NULL, NULL};
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT,
                             RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &src[i]);
   struct fake_cs f0, f1;
   struct ac_cmdbuf *cs[2] = {fake_cs(&f0, src[0], src[0]->va, 16), fake_cs(&f1, src[1], src[1]->va, 16)};
   check(submit(ws, q, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "the first user packs a gather BO");
   for (unsigned i = 1; i <= ws->bc250_gather_slots; i++)
      submit_one(ws, q);
   unsigned reuse_waits = 0;
   for (unsigned i = 0; i < h.n_ev; i++)
      reuse_waits += h.ev[i].op == BC250_HOST_WaitForSynchronizationObjectFromCpu && h.ev[i].sync == old_progress;
   check(reuse_waits == 1 && gfx(q)->bc250_progress.wait_value == ws->bc250_gather_slots + 1,
         "and wraps the gather slots (%llu submissions, one reuse wait)",
         (unsigned long long)gfx(q)->bc250_progress.wait_value);
   check(unbind(ws, q, cookie_a) == VK_SUCCESS && live(h.allocs, h.n_allocs, false) == 2,
         "unbind releases the gather BO with the rest");

   /* V7: the engine may hand the same VkQueue to another D3D12 queue. */
   check(bind(ws, q, cookie_b) == VK_SUCCESS, "the same queue binds for another user");
   struct radv_wddm2_queue *g = gfx(q);
   check(g->context_h && g->context_h != old_context &&
            find(h.contexts, h.n_contexts, g->context_h)->cookie == cookie_b,
         "with a new context, created through the new cookie");
   check(g->bc250_progress.handle != old_progress && g->vm_fence.handle != old_vm &&
            !g->bc250_progress.wait_value && !g->vm_fence.wait_value && !g->bc250_gather_index,
         "new fences, and no fence value or gather slot of the first user");
   unsigned mark = h.n_ev;
   check(submit_one(ws, q) == VK_SUCCESS, "the new user submits");
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu),
         "its first submission waits for nothing of the first user");
   check(event_is(find_op(mark, BC250_HOST_SignalSynchronizationObjectFromGpu2, g->bc250_progress.handle),
                  BC250_HOST_SignalSynchronizationObjectFromGpu2, g->context_h, g->bc250_progress.handle, 1),
         "its progress starts at 1 on its own fence");
   check(names_only(mark, g->context_h) && !names_sync(mark, old_progress) && !names_sync(mark, old_vm),
         "nothing names the first user's context or fences");

   check(unbind(ws, q, cookie_b) == VK_SUCCESS, "unbind");
   ws->base.ctx_destroy(q);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, src[i]);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false) &&
            !live(h.allocs, h.n_allocs, false),
         "nothing left");
   contract();
}

/* The two reproducers of review 606, with their checks unchanged. Each
 * also checks that the queue keeping objects is not bound again and that
 * destroying it makes no host call. */
static void
test_review_wait_failure(void)
{
   void *cookie = (void *)(uintptr_t)0xAA;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *ctx = new_ctx(ws);
   check(bind(ws, ctx, cookie) == VK_SUCCESS, "bind positive control");
   check(submit_one(ws, ctx) == VK_SUCCESS, "pending work positive control");
   h.fail_op = BC250_HOST_WaitForSynchronizationObjectFromCpu;
   h.fail_nth = 1;
   unsigned mark = h.n_ev;
   check(unbind(ws, ctx, cookie) == VK_ERROR_DEVICE_LOST, "failed wait reports loss");
   check(h.fail_seen == 1, "wait failure was injected");
   check(!count_op(mark, BC250_HOST_DestroySynchronizationObject),
         "unretired work retains fences until terminal ownership handoff");
   check(gfx(ctx)->bc250_progress.handle != 0,
         "unretired progress ownership is retained");

   h.fail_op = 0;
   mark = h.n_ev;
   check(bind(ws, ctx, cookie) != VK_SUCCESS && h.n_ev == mark, "the queue is not bound again, no host call");
   ws->base.ctx_destroy(ctx);
   check(h.n_ev == mark, "destroying it makes no host call");
   contract();
}

static void
test_review_destroy_failure(void)
{
   void *cookie = (void *)(uintptr_t)0xBB;
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *ctx = new_ctx(ws);
   check(bind(ws, ctx, cookie) == VK_SUCCESS, "bind positive control");
   uint32_t context = gfx(ctx)->context_h;
   h.fail_op = BC250_HOST_DESTROY_QUEUE_CONTEXT;
   h.fail_nth = 1;
   VkResult result = unbind(ws, ctx, cookie);
   check(h.fail_seen == 1 && find(h.contexts, h.n_contexts, context)->live,
         "destroy failure leaves host context live");
   check(result != VK_SUCCESS, "failed context destruction must not report success (result=%d)", result);
   check(gfx(ctx)->context_h == context,
         "failed context destruction retains ownership pending explicit handoff");

   h.fail_op = 0;
   unsigned mark = h.n_ev;
   check(bind(ws, ctx, cookie) != VK_SUCCESS && h.n_ev == mark, "the queue is not bound again, no host call");
   ws->base.ctx_destroy(ctx);
   check(h.n_ev == mark, "destroying it makes no host call, through the cookie or otherwise");
   contract();
}

/* Allocations imported from the host: mapped only with
 * BC250_HOST_IMPORT_CPU_MAP, through the host's Lock2 and Unlock2, and
 * never released by the ICD. */
static void
test_hosted_map(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   void *const identity = ws->host.identity;
   const uint64_t va = 0x800000000ull, size = 65536;
   const uint64_t vram_vis = ws->allocated_vram_vis;

   /* Without the flag the ICD never maps it. */
   struct obj *plain = host_alloc();
   struct radeon_winsys_bo *bo = NULL;
   unsigned mark = h.n_ev;
   check(ws->base.buffer_from_hosted(&ws->base, identity, plain->handle, 0, va, size, &bo) == VK_SUCCESS && bo &&
            h.n_ev == mark,
         "an import without flags succeeds, no host call");
   check(ws->base.buffer_map(&ws->base, bo, false, NULL) == NULL && h.n_ev == mark,
         "and is not mapped, with no host call");
   ws->base.buffer_destroy(&ws->base, bo);
   check(h.n_ev == mark && plain->live, "destroying it makes no host call; the allocation stays the host's");

   /* With it, the first map locks the host's allocation, once. */
   struct obj *mappable = host_alloc();
   bo = NULL;
   mark = h.n_ev;
   check(ws->base.buffer_from_hosted(&ws->base, identity, mappable->handle, BC250_HOST_IMPORT_CPU_MAP, va, size,
                                     &bo) == VK_SUCCESS && bo && h.n_ev == mark,
         "an import with BC250_HOST_IMPORT_CPU_MAP succeeds, no host call");
   void *ptr = ws->base.buffer_map(&ws->base, bo, false, NULL);
   check(ptr && ptr == mappable->mem, "the map returns the host's pData");
   check(h.n_ev == mark + 1 && event_is((int)mark, BC250_HOST_Lock2, 0, 0, mappable->handle),
         "through exactly one Lock2 of the imported allocation (calls: %u)", h.n_ev - mark);
   mark = h.n_ev;
   check(ws->base.buffer_map(&ws->base, bo, false, NULL) == ptr && h.n_ev == mark,
         "a second map returns the same pointer, no host call");

   /* Destroy unlocks it and gives nothing else back: the host owns it. */
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(h.n_ev == mark + 1 && event_is((int)mark, BC250_HOST_Unlock2, 0, 0, mappable->handle),
         "destroying it makes exactly one Unlock2 of the allocation (calls: %u)", h.n_ev - mark);
   check(!count_op(mark, BC250_HOST_Evict) && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress) &&
            !count_op(mark, BC250_HOST_DestroyAllocation2) && mappable->live,
         "no evict, VA free or allocation destroy; the allocation stays the host's");

   /* An unknown flag bit is refused before anything is made. */
   bo = NULL;
   mark = h.n_ev;
   check(ws->base.buffer_from_hosted(&ws->base, identity, plain->handle, 0x80000000u, va, size, &bo) ==
               VK_ERROR_INVALID_EXTERNAL_HANDLE &&
            !bo && h.n_ev == mark,
         "an unknown flag bit is refused, no BO, no host call");
   check(ws->allocated_vram_vis == vram_vis, "the imports leave no byte charge behind");
   contract();
}

/* Deferred destruction (radv_wddm2_bo.c): a BO destroyed while a submission is in flight keeps its
 * allocation and its VA until the progress value every queue had published at the destroy retires.
 * The fake GPU completes nothing on its own, so the tests retire a value by writing the fence's CPU
 * mapping, or by a CPU wait. */
static struct radeon_winsys_bo *
gtt_bo(struct radv_wddm2_winsys *ws)
{
   struct radeon_winsys_bo *bo = NULL;
   ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT,
                          RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &bo);
   if (!bo) {
      fprintf(stderr, "buffer_create failed\n");
      exit(2);
   }
   return bo;
}

static bool
alloc_live(uint32_t handle)
{
   struct obj *o = find(h.allocs, h.n_allocs, handle);
   return o && o->live;
}

static int
find_destroy(unsigned from, uint32_t handle)
{
   for (unsigned i = from; i < h.n_ev; i++) {
      if (h.ev[i].op == BC250_HOST_DestroyAllocation2 && h.ev[i].value == handle)
         return (int)i;
   }
   return -1;
}

static void
retire(uint32_t progress, uint64_t value)
{
   find(h.syncs, h.n_syncs, progress)->value = value;
}

static void
test_deferred_destroy(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   struct radv_wddm2_winsys *ws = make_ws();
   check(ws->deferred.enabled, "deferred destruction is on by default");
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   struct radv_wddm2_queue *qa = gfx(a);
   const uint32_t a_progress = qa->bc250_progress.handle;
   check(qa->bc250_tracker != NULL, "a bound queue's progress is tracked");

   /* Nothing submitted: a destroy is immediate. */
   struct radeon_winsys_bo *early = gtt_bo(ws);
   const uint32_t early_h = early->handle;
   unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, early);
   check(!alloc_live(early_h) && find_destroy(mark, early_h) >= 0 && !ws->deferred.count &&
            ws->deferred.immediate == 1,
         "with nothing in flight a destroy is immediate");

   /* A's progress 1 is in flight: the destroy holds the BO. */
   struct radeon_winsys_bo *x = gtt_bo(ws);
   const uint32_t x_h = x->handle;
   check(submit_one(ws, a) == VK_SUCCESS, "A submits; its progress 1 is in flight");
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, x);
   check(h.n_ev == mark, "destroying a BO with a submission in flight makes no host call");
   check(alloc_live(x_h) && ws->deferred.count == 1 && ws->deferred.bytes == 4096,
         "its allocation stays live, held (%u BOs, %llu bytes)", ws->deferred.count,
         (unsigned long long)ws->deferred.bytes);

   mark = h.n_ev;
   check(submit_one(ws, a) == VK_SUCCESS && alloc_live(x_h) && find_destroy(mark, x_h) < 0 &&
            !count_op(mark, BC250_HOST_Evict) && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress),
         "a later submission leaves it held while progress 1 has not retired");

   retire(a_progress, 1);
   mark = h.n_ev;
   check(submit_one(ws, a) == VK_SUCCESS, "A submits once progress 1 retired");
   const int evict = find_op(mark, BC250_HOST_Evict, 0);
   const int free_va = find_op(mark, BC250_HOST_FreeGpuVirtualAddress, 0);
   const int destroy = find_destroy(mark, x_h);
   const int sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
   check(!alloc_live(x_h) && destroy >= 0 && !ws->deferred.count, "then it is destroyed, at that submission");
   check(evict >= 0 && evict < free_va && free_va < destroy && destroy < sub,
         "evict, VA free, allocation destroy, all before the IB goes out (%d %d %d %d)", evict, free_va, destroy,
         sub);

   /* Two queues busy: a BO waits for both. */
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qb = gfx(b);
   const uint32_t b_progress = qb->bc250_progress.handle, b_context = qb->context_h;
   check(submit_one(ws, b) == VK_SUCCESS, "B submits; its progress 1 is in flight, A's 2 and 3 too");
   struct radeon_winsys_bo *y = gtt_bo(ws);
   const uint32_t y_h = y->handle;
   ws->base.buffer_destroy(&ws->base, y);
   check(alloc_live(y_h) && ws->deferred.count == 1, "a BO destroyed with both queues busy is held");
   retire(a_progress, 3);
   check(ws->base.ctx_wait_idle(a, AMD_IP_GFX, 0) && alloc_live(y_h) && ws->deferred.count == 1,
         "A idle is not enough: it also waits for B");
   mark = h.n_ev;
   check(ws->base.ctx_wait_idle(b, AMD_IP_GFX, 0), "B waits idle (the CPU wait completes its progress 1)");
   check(!alloc_live(y_h) && find_destroy(mark, y_h) >= 0 && !ws->deferred.count,
         "then it is destroyed, at B's idle point");

   /* A host import is the host's: never held. */
   check(submit_one(ws, a) == VK_SUCCESS, "A submits; its progress 4 is in flight");
   struct obj *imported = host_alloc();
   struct radeon_winsys_bo *import = NULL;
   ws->base.buffer_from_hosted(&ws->base, ws->host.identity, imported->handle, 0, 0x800000000ull, 65536, &import);
   const uint64_t total = ws->deferred.total;
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, import);
   check(import && h.n_ev == mark && imported->live && ws->deferred.total == total && !ws->deferred.count,
         "a host import is destroyed at once, no host call, never held");

   /* A lost device reads UINT64_MAX: the GPU runs nothing any more. */
   struct radeon_winsys_bo *z = gtt_bo(ws);
   const uint32_t z_h = z->handle;
   ws->base.buffer_destroy(&ws->base, z);
   check(alloc_live(z_h) && ws->deferred.count == 1, "held behind A's progress 4");
   retire(a_progress, UINT64_MAX);
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(z_h) && !ws->deferred.count, "a progress fence reading UINT64_MAX (lost device) frees it");
   retire(a_progress, 4);

   /* A queue destroyed with its work in flight leaves its fence to the device; held BOs keep waiting on it. */
   check(submit_one(ws, b) == VK_SUCCESS, "B submits; its progress 2 is in flight");
   struct radeon_winsys_bo *u = gtt_bo(ws);
   const uint32_t u_h = u->handle;
   ws->base.buffer_destroy(&ws->base, u);
   check(alloc_live(u_h) && ws->deferred.count == 1, "held behind B's progress 2 (A is idle)");
   h.status = -1;
   check(unbind(ws, b, cookie_b) == VK_ERROR_DEVICE_LOST, "B's unbind on a lost device keeps everything");
   mark = h.n_ev;
   ws->base.ctx_destroy(b);
   h.status = 0;
   check(h.n_ev == mark && find(h.syncs, h.n_syncs, b_progress)->live,
         "destroying B makes no host call; its progress fence stays with the device");
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(u_h) && ws->deferred.count == 1, "the BO still waits on B's fence after B is gone");
   retire(b_progress, 2);
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(u_h) && find_destroy(mark, u_h) >= 0 && !ws->deferred.count && !names_sync(mark, b_progress),
         "once it retires the BO goes, and nothing names B's fence");

   /* Teardown: a CPU wait for the held BO's work, then its release. */
   check(submit_one(ws, a) == VK_SUCCESS, "A submits; its progress 5 is in flight");
   struct radeon_winsys_bo *t = gtt_bo(ws);
   const uint32_t t_h = t->handle;
   ws->base.buffer_destroy(&ws->base, t);
   mark = h.n_ev;
   radv_wddm2_deferred_finish(ws);
   const int tw = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0);
   check(event_is(tw, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0, a_progress, 5) &&
            tw < find_destroy(mark, t_h) && !alloc_live(t_h) && !ws->deferred.count && !ws->deferred.forced,
         "teardown CPU-waits for A's progress 5, then destroys the BO (%llu forced)",
         (unsigned long long)ws->deferred.forced);
   check(ws->deferred.total == 5 && ws->deferred.peak_count == 1 && ws->deferred.peak_bytes == 4096,
         "counters: %llu held, peak %u BOs, %llu bytes", (unsigned long long)ws->deferred.total,
         ws->deferred.peak_count, (unsigned long long)ws->deferred.peak_bytes);

   retire(a_progress, 5);
   check(unbind(ws, a, cookie_a) == VK_SUCCESS, "A unbinds");
   ws->base.ctx_destroy(a);
   check(live(h.contexts, h.n_contexts, false) == 1 && find(h.contexts, h.n_contexts, b_context)->live &&
            !live(h.allocs, h.n_allocs, false) && !ws->deferred.count,
         "nothing left but what B's abandoned unbind kept (%u contexts, %u allocations live)",
         live(h.contexts, h.n_contexts, false), live(h.allocs, h.n_allocs, false));
   contract();

   /* BC250_DEFERRED_DESTROY=0: destroyed at once, as before. */
   _putenv_s("BC250_DEFERRED_DESTROY", "0");
   ws = make_ws();
   _putenv_s("BC250_DEFERRED_DESTROY", "");
   check(!ws->deferred.enabled, "BC250_DEFERRED_DESTROY=0 turns it off");
   struct radeon_winsys_ctx *c = new_ctx(ws);
   bind(ws, c, cookie_a);
   check(gfx(c)->bc250_tracker != NULL, "the witness still tracks the queue");
   check(submit_one(ws, c) == VK_SUCCESS, "C submits; its progress 1 is in flight");
   struct radeon_winsys_bo *v = gtt_bo(ws);
   const uint32_t v_h = v->handle;
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, v);
   check(!alloc_live(v_h) && find_destroy(mark, v_h) >= 0 && !ws->deferred.total,
         "off, a destroy with work in flight is immediate");
   unbind(ws, c, cookie_a);
   ws->base.ctx_destroy(c);
   contract();
}

/* A bc250 submission of several IBs (radv_wddm2_bc250_submit): the KMD runs one IB1, which by
 * default calls each command stream as an IB2 from the gather BO, in submission order; a stream
 * that calls an IB2 itself (or lies outside IB_BASE) is copied there instead, an empty one is
 * skipped, and the IB1 ends on the GFX padding. BC250_IB_NOCOPY=0 copies every stream. */

#define IB2_HEADER 0xC0023F00u /* PKT3(PKT3_INDIRECT_BUFFER, 2, 0) */
#define NOP_4DW    0xC0021000u /* PKT3(PKT3_NOP, 2, 0): a header and three dwords */
#define NOP_1DW    0xFFFF1000u /* PKT3_NOP_PAD */

static struct radeon_info *
test_query_info(struct radeon_winsys *rws)
{
   return &radv_wddm2_winsys(rws)->gpu_info;
}

static void
clear_ib_env(void)
{
   _putenv_s("BC250_IB_NOCOPY", "");
   _putenv_s("BC250_IB_DWORDS", "");
}

/* make_ws with the GFX IB rules of the lab's caps (ac_fill_hw_ip_info): IBs padded to 8 dwords,
 * IB BOs aligned to 256 bytes. The environment is read here, as at winsys creation. */
static struct radv_wddm2_winsys *
make_ib_ws(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   ws->base.query_info = test_query_info;
   ws->gpu_info.ip[AMD_IP_GFX].ib_pad_dw_mask = 7;
   ws->gpu_info.ip[AMD_IP_GFX].ib_alignment = 256;
   return ws;
}

/* The IB1 of the last submission, in the queue's gather BO it was packed into. */
static const uint32_t *
last_ib1(const struct radv_wddm2_queue *q)
{
   for (unsigned i = 0; i < BC250_GATHER_SLOTS_MAX; i++) {
      if (q->bc250_gather[i].bo && q->bc250_gather[i].bo->va == h.submit_va)
         return (const uint32_t *)q->bc250_gather[i].map;
   }
   return NULL;
}

static bool
is_ib2_call(const uint32_t *dw, uint64_t va, uint32_t cdw)
{
   return dw && dw[0] == IB2_HEADER && dw[1] == (uint32_t)va && dw[2] == (uint32_t)(va >> 32) && dw[3] == cdw;
}

/* The IB address of the last blob. Read by offset: MSVC's stdarg.h defines va_start as an
 * object-like macro, which renames the member where it is spelled after that include. */
static uint64_t
blob_ib_va(void)
{
   uint64_t va;
   _Static_assert(offsetof(struct bc250_ib_blob, ib_bytes) == 8, "va_start is the first 8 bytes");
   memcpy(&va, &h.submit_blob.ib[0], sizeof(va));
   return va;
}

/* The last submission ran one IB1 of dwords dwords from a gather BO, and its blob says so. */
static bool
one_ib1(const struct radv_wddm2_queue *q, unsigned dwords)
{
   return last_ib1(q) && h.submit_bytes == dwords * 4 && h.submit_blob.magic == BC250_SUBMIT_MAGIC &&
          h.submit_blob.num_ibs == 1 && blob_ib_va() == h.submit_va && h.submit_blob.ib[0].ib_bytes == dwords * 4;
}

/* Command BOs with known contents: dword j of BO i is 0xA0000000 | i << 16 | j. */
static bool
pattern_bos(struct radv_wddm2_winsys *ws, unsigned count, struct radeon_winsys_bo **bo, uint32_t **map)
{
   for (unsigned i = 0; i < count; i++) {
      bo[i] = NULL;
      map[i] = NULL;
      ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT,
                             RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &bo[i]);
      if (!bo[i] || !(map[i] = ws->base.buffer_map(&ws->base, bo[i], false, NULL)))
         return false;
      for (unsigned j = 0; j < 1024; j++)
         map[i][j] = 0xA0000000u | (i << 16) | j;
   }
   return true;
}

static void
test_ib2_calls(void)
{
   void *const cookie = (void *)(uintptr_t)0xD0;
   clear_ib_env();
   struct radv_wddm2_winsys *ws = make_ib_ws();
   check(!ws->bc250_gather_copy && !ws->bc250_ib_dwords_cap, "without BC250_IB_NOCOPY the IB2 calls are on");
   struct radeon_winsys_ctx *c = new_ctx(ws);
   check(bind(ws, c, cookie) == VK_SUCCESS, "the queue binds");
   struct radv_wddm2_queue *q = gfx(c);

   struct radeon_winsys_bo *bo[3];
   uint32_t *map[3];
   if (!pattern_bos(ws, 3, bo, map)) {
      check(false, "three mapped command BOs");
      return;
   }
   /* b starts 256 bytes into its BO, as a later IB of a command buffer may. */
   const uint64_t va_a = bo[0]->va, va_b = bo[1]->va + 256, va_n = bo[2]->va;
   struct fake_cs fa, fb, fn, fe;
   struct ac_cmdbuf *cs[4];

   /* Two plain streams: two IB2 calls, nothing copied. */
   cs[0] = fake_cs(&fa, bo[0], va_a, 16);
   cs[1] = fake_cs(&fb, bo[1], va_b, 24);
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "two plain streams submit");
   const uint32_t *ib1 = last_ib1(q);
   check(one_ib1(q, 8), "one IB1 of 8 dwords from a gather BO, its blob names it (%u bytes)", h.submit_bytes);
   check(is_ib2_call(ib1, va_a, 16) && is_ib2_call(ib1 + 4, va_b, 24),
         "an IB2 call of each stream in submission order, at its own address and size");
   check(ws->bc250_inline_submits == 0, "no stream copied");

   /* A stream that calls an IB2 itself runs at IB1 level: copied between the two calls. */
   cs[0] = fake_cs(&fa, bo[0], va_a, 16);
   cs[1] = fake_cs(&fn, bo[2], va_n, 8);
   fn.cs.calls_ib2 = true;
   cs[2] = fake_cs(&fb, bo[1], va_b, 24);
   check(submit(ws, c, 3, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "a stream calling an IB2 between two plain ones");
   ib1 = last_ib1(q);
   check(one_ib1(q, 16) && is_ib2_call(ib1, va_a, 16) && !memcmp(ib1 + 4, map[2], 8 * 4) &&
            is_ib2_call(ib1 + 12, va_b, 24),
         "the IB1 calls the first, holds the 8 dwords of the second and calls the third");
   check(ws->bc250_inline_submits == 1, "the copy is counted (and reported on stderr)");

   /* Three calls are 12 dwords: a 4-dword NOP pads the IB1 to 16. */
   cs[0] = fake_cs(&fa, bo[0], va_a, 16);
   cs[1] = fake_cs(&fb, bo[1], va_b, 24);
   cs[2] = fake_cs(&fn, bo[2], va_n, 8);
   check(submit(ws, c, 3, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "three plain streams submit");
   ib1 = last_ib1(q);
   check(one_ib1(q, 16) && is_ib2_call(ib1 + 8, va_n, 8) && ib1[12] == NOP_4DW && !ib1[13] && !ib1[14] && !ib1[15],
         "three calls, then PKT3 NOP 0x%08x and three zero dwords to the 8-dword padding", ib1 ? ib1[12] : 0);

   /* A 3-dword copy and a call are 7 dwords: a 1-dword NOP pads them. */
   cs[0] = fake_cs(&fn, bo[2], va_n, 3);
   fn.cs.calls_ib2 = true;
   cs[1] = fake_cs(&fa, bo[0], va_a, 16);
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "a 3-dword copy and a call submit");
   ib1 = last_ib1(q);
   check(one_ib1(q, 8) && !memcmp(ib1, map[2], 3 * 4) && is_ib2_call(ib1 + 3, va_a, 16) && ib1[7] == NOP_1DW,
         "the copy, the call and PKT3_NOP_PAD");

   /* An empty stream is skipped; a stream at an address that is not dword aligned is copied. */
   cs[0] = fake_cs(&fa, bo[0], va_a, 16);
   cs[1] = fake_cs(&fe, bo[2], va_n, 0);
   cs[2] = fake_cs(&fb, bo[1], va_b, 24);
   check(submit(ws, c, 3, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "a stream, an empty one, a stream");
   ib1 = last_ib1(q);
   check(one_ib1(q, 8) && is_ib2_call(ib1, va_a, 16) && is_ib2_call(ib1 + 4, va_b, 24), "the empty one is not called");
   cs[0] = fake_cs(&fn, bo[2], va_n + 2, 4);
   cs[1] = fake_cs(&fa, bo[0], va_a, 16);
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "a stream at an unaligned address and a stream");
   ib1 = last_ib1(q);
   check(one_ib1(q, 8) && !memcmp(ib1, (const uint8_t *)map[2] + 2, 4 * 4) && is_ib2_call(ib1 + 4, va_a, 16),
         "the unaligned one is copied, the other called");
   check(ws->bc250_inline_submits == 3, "three submissions with a copy so far (%" PRIu64 ")", ws->bc250_inline_submits);

   /* One stream alone is the IB1 itself, as before: no gather BO, nothing written. */
   cs[0] = fake_cs(&fa, bo[0], va_a, 16);
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS && h.submit_va == va_a && h.submit_bytes == 64 &&
            blob_ib_va() == va_a,
         "a single stream is launched where it is");

   /* Real streams: the winsys marks the one that calls an IB2, and a reset clears the mark. */
   struct ac_cmdbuf *plain = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *caller = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *child = ws->base.cs_create(&ws->base, AMD_IP_GFX, true);
   struct ac_cmdbuf *parent_ib2 = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *parent_copy = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   if (!plain || !caller || !child || !parent_ib2 || !parent_copy) {
      check(false, "five real GFX streams");
      return;
   }
   /* What radv_cs_check_space does before RADV writes packets. */
   plain->reserved_dw = child->reserved_dw = caller->reserved_dw = 16;
   for (unsigned i = 0; i < 3; i++) {
      plain->buf[plain->cdw++] = NOP_1DW;
      child->buf[child->cdw++] = NOP_1DW;
   }
   ws->base.cs_execute_ib(caller, NULL, va_a, 16, false);
   check(radv_winsys_cs(caller)->calls_ib2 && !radv_winsys_cs(plain)->calls_ib2,
         "cs_execute_ib marks its stream, a plain one stays unmarked");
   ws->base.cs_finalize(plain);
   ws->base.cs_finalize(caller);
   ws->base.cs_finalize(child);
   ws->base.cs_execute_secondary(parent_ib2, child, true);
   ws->base.cs_execute_secondary(parent_copy, child, false);
   check(radv_winsys_cs(parent_ib2)->calls_ib2 && !radv_winsys_cs(parent_copy)->calls_ib2 &&
            parent_copy->cdw == 8,
         "a secondary run as an IB2 marks its primary; copied (8 dwords), an unmarked secondary does not");
   ws->base.cs_reset(parent_copy);
   ws->base.cs_execute_secondary(parent_copy, caller, false);
   check(radv_winsys_cs(parent_copy)->calls_ib2 && parent_copy->cdw == 8 && parent_copy->buf[0] == IB2_HEADER,
         "a copied secondary passes its IB2 call on to its primary");

   const struct radv_winsys_ib pib = radv_winsys_cs(plain)->ib_buffers[0];
   cs[0] = plain;
   cs[1] = caller;
   check(radv_winsys_cs(plain)->num_ib_buffers == 1 && radv_winsys_cs(caller)->num_ib_buffers == 1 &&
            pib.cdw == 8 && radv_winsys_cs(caller)->ib_buffers[0].cdw == 8,
         "both finalized to one IB of 8 dwords");
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "the two real streams submit");
   ib1 = last_ib1(q);
   check(one_ib1(q, 16) && is_ib2_call(ib1, pib.va, 8) && is_ib2_call(ib1 + 4, va_a, 16) && ib1[12] == NOP_4DW,
         "the plain one is called; the caller is copied, its IB2 call of a now at IB1 level");
   ws->base.cs_reset(caller);
   check(!radv_winsys_cs(caller)->calls_ib2, "a reset clears the mark");

   ws->base.cs_destroy(plain);
   ws->base.cs_destroy(caller);
   ws->base.cs_destroy(child);
   ws->base.cs_destroy(parent_ib2);
   ws->base.cs_destroy(parent_copy);
   check(unbind(ws, c, cookie) == VK_SUCCESS, "the queue unbinds");
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 3; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false) &&
            !live(h.allocs, h.n_allocs, false),
         "nothing left");
   contract();
}

static void
test_ib2_fallback(void)
{
   void *const cookie = (void *)(uintptr_t)0xE0;
   struct radv_wddm2_winsys *ws;

   clear_ib_env();
   _putenv_s("BC250_IB_DWORDS", "4096");
   ws = make_ib_ws();
   check(ws->bc250_gather_copy && ws->bc250_ib_dwords_cap == 4096, "BC250_IB_DWORDS selects the gather copy");
   clear_ib_env();
   _putenv_s("BC250_IB_NOCOPY", "1");
   ws = make_ib_ws();
   check(!ws->bc250_gather_copy, "BC250_IB_NOCOPY=1 keeps the IB2 calls");

   clear_ib_env();
   _putenv_s("BC250_IB_NOCOPY", "0");
   ws = make_ib_ws();
   check(ws->bc250_gather_copy, "BC250_IB_NOCOPY=0 selects the gather copy");
   struct radeon_winsys_ctx *c = new_ctx(ws);
   check(bind(ws, c, cookie) == VK_SUCCESS, "the queue binds");
   struct radv_wddm2_queue *q = gfx(c);
   struct radeon_winsys_bo *bo[2];
   uint32_t *map[2];
   if (!pattern_bos(ws, 2, bo, map)) {
      check(false, "two mapped command BOs");
      clear_ib_env();
      return;
   }
   const uint64_t va_a = bo[0]->va, va_b = bo[1]->va + 256;
   struct fake_cs fa, fb;
   struct ac_cmdbuf *cs[2] = {fake_cs(&fa, bo[0], va_a, 16), fake_cs(&fb, bo[1], va_b, 24)};
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "two plain streams submit");
   const uint32_t *ib1 = last_ib1(q);
   check(one_ib1(q, 40) && !memcmp(ib1, map[0], 16 * 4) && !memcmp(ib1 + 16, (const uint8_t *)map[1] + 256, 24 * 4),
         "the IB1 is the copy of both streams, 40 dwords, as before (%u bytes)", h.submit_bytes);
   check(ws->bc250_inline_submits == 0, "the full copy is not counted as an IB2 fallback");

   check(unbind(ws, c, cookie) == VK_SUCCESS, "the queue unbinds");
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   clear_ib_env();
   contract();
}

/* Retire-deferred destroy beyond test_deferred_destroy: the cap of held bytes, an allocation that fails
 * for want of device memory, teardown, the witness, the configuration file and the costs. The fake
 * GPU completes a value when the CPU waits for it (WaitForSynchronizationObjectFromCpu). The log of the
 * test process is BC250_DEFERRED_LOG (queue_tests_deferred.py sets it). */

#define PLAIN_FLAGS  (RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING)
#define UPLOAD_FLAGS (RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT | RADEON_FLAG_GTT_WC)
#define CMD_FLAGS                                                                                                   \
   (RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_READ_ONLY | RADEON_FLAG_GL2_BYPASS |  \
    RADEON_FLAG_GTT_WC)

/* The witness's classes, as radv_wddm2_bo.c numbers them. */
enum { C_VIRTUAL, C_COMMAND_STREAM, C_SHADER_RING, C_CPU_UPLOAD, C_DESCRIPTOR_POOL, C_QUERY_POOL, C_APPLICATION, C_OTHER };

static void
clear_deferred_env(void)
{
   _putenv_s("BC250_DEFERRED_DESTROY", "");
   _putenv_s("BC250_DEFERRED_WITNESS", "");
   _putenv_s("BC250_DEFERRED_CAP_MB", "");
   _putenv_s("BC250_DEFERRED_SUMMARY_S", "");
   _putenv_s("BC250_SUBMIT_COALESCE", "");
   _putenv_s("BC250_GATHER_SLOTS", "");
   _putenv_s("BC250_PROGRESS_FENCE", "");
}

static struct radeon_winsys_bo *
sized_bo(struct radv_wddm2_winsys *ws, uint64_t size, enum radeon_bo_flag flags, unsigned priority)
{
   struct radeon_winsys_bo *bo = NULL;
   ws->base.buffer_create(&ws->base, size, 4096, RADEON_DOMAIN_GTT, flags, priority, 0, NULL, &bo);
   if (!bo) {
      fprintf(stderr, "buffer_create failed\n");
      exit(2);
   }
   return bo;
}

static void
fail_calls(uint32_t op, unsigned count)
{
   h.fail_op = op;
   h.fail_seen = 0;
   h.fail_nth = 1;
   h.fail_more = count - 1;
}

static void
no_failures(void)
{
   h.fail_op = 0;
   h.fail_more = 0;
}

static bool
cpu_wait_is(int index, uint32_t fence, uint64_t value)
{
   return event_is(index, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0, fence, value);
}

static const char *
log_file(void)
{
   static char expanded[512];
   const char *path = getenv("BC250_DEFERRED_LOG");
   if (!path || !*path)
      return "deferred-test.log";
   /* The ICD expands %p in the name to the process id; a test that reads the log back does the same. */
   const char *mark = strstr(path, "%p");
   if (!mark)
      return path;
   if (!expanded[0])
      snprintf(expanded, sizeof(expanded), "%.*s%lu%s", (int)(mark - path), path,
               (unsigned long)GetCurrentProcessId(), mark + 2);
   return expanded;
}

static char log_text[1 << 20];

static long
log_mark(void)
{
   FILE *f = fopen(log_file(), "rb");
   if (!f)
      return 0;
   fseek(f, 0, SEEK_END);
   const long n = ftell(f);
   fclose(f);
   return n;
}

/* The lines since mark that hold needle: how many, and the first one in out. */
static unsigned
log_lines(long mark, const char *needle, char *out, size_t size)
{
   unsigned n = 0;
   if (out)
      out[0] = 0;
   log_text[0] = 0;
   FILE *f = fopen(log_file(), "rb");
   if (f) {
      fseek(f, mark, SEEK_SET);
      log_text[fread(log_text, 1, sizeof(log_text) - 1, f)] = 0;
      fclose(f);
   }
   for (const char *line = log_text; *line;) {
      const char *end = strchr(line, '\n');
      const size_t len = end ? (size_t)(end - line) : strlen(line);
      char copy[4096];
      snprintf(copy, sizeof(copy), "%.*s", (int)MIN2(len, sizeof(copy) - 1), line);
      if (strstr(copy, needle)) {
         if (!n && out)
            snprintf(out, size, "%s", copy);
         n++;
      }
      line += len + (end ? 1 : 0);
   }
   return n;
}

static bool
has(const char *line, const char *needle)
{
   return strstr(line, needle) != NULL;
}

static void
test_deferred_cap(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   const uint64_t half = 512 * 1024, three_quarters = 768 * 1024;
   clear_deferred_env();
   _putenv_s("BC250_DEFERRED_CAP_MB", "1");
   struct radv_wddm2_winsys *ws = make_ws();
   clear_deferred_env();
   check(ws->deferred.enabled && ws->deferred.cap_bytes == 1ull << 20, "BC250_DEFERRED_CAP_MB=1: a 1 MiB cap");
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   const uint32_t a_progress = gfx(a)->bc250_progress.handle;

   /* A creation releases held BOs whose work retired before it allocates. */
   struct radeon_winsys_bo *r = sized_bo(ws, 4096, PLAIN_FLAGS, 0);
   const uint32_t r_h = r->handle;
   submit_one(ws, a);
   ws->base.buffer_destroy(&ws->base, r);
   retire(a_progress, 1);
   unsigned mark = h.n_ev;
   struct radeon_winsys_bo *r2 = sized_bo(ws, 4096, PLAIN_FLAGS, 0);
   check(!alloc_live(r_h) && find_destroy(mark, r_h) >= 0 &&
            find_destroy(mark, r_h) < find_op(mark, BC250_HOST_CreateAllocation2, 0) && !ws->deferred.count,
         "a creation first releases the held BO whose work retired");
   ws->base.buffer_destroy(&ws->base, r2);

   struct radeon_winsys_bo *bo[3];
   uint32_t hd[3];
   for (unsigned i = 0; i < 3; i++) {
      bo[i] = sized_bo(ws, half, PLAIN_FLAGS, 0);
      hd[i] = bo[i]->handle;
   }
   const uint64_t gtt = ws->allocated_gtt;
   mark = h.n_ev;
   for (unsigned i = 0; i < 2; i++) {
      submit_one(ws, a); /* A's progress 2, 3 */
      ws->base.buffer_destroy(&ws->base, bo[i]);
   }
   check(ws->deferred.count == 2 && ws->deferred.bytes == 1ull << 20 && alloc_live(hd[0]) && alloc_live(hd[1]) &&
            !count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu),
         "two 512 KiB BOs held: 1 MiB, at the cap, no CPU wait");
   check(ws->allocated_gtt == gtt, "held BOs stay in the byte accounting (%llu bytes)",
         (unsigned long long)ws->allocated_gtt);

   submit_one(ws, a); /* A's progress 4 */
   long lm = log_mark();
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo[2]);
   const int w = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0);
   check(cpu_wait_is(w, a_progress, 2) && count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 1,
         "over the cap: one CPU wait, for A's progress 2, the oldest held BO's");
   check(!alloc_live(hd[0]) && find_destroy(mark, hd[0]) > w && alloc_live(hd[1]) && alloc_live(hd[2]) &&
            ws->deferred.count == 2 && ws->deferred.bytes == 1ull << 20,
         "the oldest is released after the wait; 1 MiB held again");
   check(ws->allocated_gtt == gtt - half, "its bytes leave the accounting at the release");
   char line[4096];
   check(ws->deferred.cap_waits == 1 && !ws->deferred.cap_failed &&
            log_lines(lm, "deferred destroy: cap: 1 MiB held over the 1 MiB cap", line, sizeof(line)) == 1 &&
            has(line, "released it"),
         "one cap wait, counted and logged: %.120s", line);
   retire(a_progress, 4);
   radv_wddm2_deferred_drain(ws);
   check(!ws->deferred.count && !ws->deferred.bytes, "A's progress 4: nothing held");

   /* Two queues busy: the oldest held BO waits for both, so the cap waits for both. */
   bind(ws, b, cookie_b);
   const uint32_t b_progress = gfx(b)->bc250_progress.handle;
   struct radeon_winsys_bo *c1 = sized_bo(ws, half, PLAIN_FLAGS, 0), *c2 = sized_bo(ws, three_quarters, PLAIN_FLAGS, 0);
   const uint32_t c1_h = c1->handle, c2_h = c2->handle;
   submit_one(ws, a); /* A's 5 */
   submit_one(ws, b); /* B's 1 */
   ws->base.buffer_destroy(&ws->base, c1);
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, c2);
   const int wa = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, a_progress);
   const int wb = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, b_progress);
   check(cpu_wait_is(wa, a_progress, 5) && cpu_wait_is(wb, b_progress, 1) &&
            count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 2,
         "1.25 MiB over the cap: CPU waits for A's 5 and B's 1");
   check(!alloc_live(c1_h) && !alloc_live(c2_h) && !ws->deferred.count && ws->deferred.cap_waits == 2,
         "both released: the second waited for the same values");

   /* A failed CPU wait ends the cap's waiting; the BOs stay held. */
   struct radeon_winsys_bo *d1 = sized_bo(ws, half, PLAIN_FLAGS, 0), *d2 = sized_bo(ws, three_quarters, PLAIN_FLAGS, 0);
   const uint32_t d1_h = d1->handle, d2_h = d2->handle;
   submit_one(ws, a); /* A's 6 */
   ws->base.buffer_destroy(&ws->base, d1);
   lm = log_mark();
   fail_calls(BC250_HOST_WaitForSynchronizationObjectFromCpu, 1);
   ws->base.buffer_destroy(&ws->base, d2);
   no_failures();
   check(alloc_live(d1_h) && alloc_live(d2_h) && ws->deferred.count == 2 && ws->deferred.cap_failed == 1 &&
            log_lines(lm, "failed or released nothing", NULL, 0) == 1,
         "a failed cap wait: both stay held, one failure counted and logged");
   retire(a_progress, 6);
   retire(b_progress, 1);
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(d1_h) && !alloc_live(d2_h) && !ws->deferred.count, "released once A's 6 retired");

   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false), "no context or fence left");
   contract();
}

static void
test_deferred_oom(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   bind(ws, a, cookie);
   const uint32_t a_progress = gfx(a)->bc250_progress.handle;
   struct radeon_winsys_bo *b0 = sized_bo(ws, 65536, PLAIN_FLAGS, 0), *b1 = sized_bo(ws, 65536, PLAIN_FLAGS, 0);
   const uint32_t b0_h = b0->handle, b1_h = b1->handle;
   submit_one(ws, a); /* A's 1 */
   ws->base.buffer_destroy(&ws->base, b0);
   submit_one(ws, a); /* A's 2 */
   ws->base.buffer_destroy(&ws->base, b1);
   check(ws->deferred.count == 2, "two BOs held, behind A's 1 and A's 2");

   /* The next two allocations fail: each time the CPU waits for the oldest held BO and tries again. */
   fail_calls(BC250_HOST_CreateAllocation2, 2);
   const long lm = log_mark();
   unsigned mark = h.n_ev;
   struct radeon_winsys_bo *nb = NULL;
   VkResult result = ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &nb);
   no_failures();
   const int w1 = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0);
   const int w2 = w1 >= 0 ? find_op(w1 + 1, BC250_HOST_WaitForSynchronizationObjectFromCpu, 0) : -1;
   const int d0 = find_destroy(mark, b0_h), d1 = find_destroy(mark, b1_h);
   check(result == VK_SUCCESS && nb && count_op(mark, BC250_HOST_CreateAllocation2) == 3,
         "the allocation succeeds at its third try (%d)", result);
   check(cpu_wait_is(w1, a_progress, 1) && w1 < d0 && cpu_wait_is(w2, a_progress, 2) && d0 < w2 && w2 < d1,
         "wait for A's 1, release the oldest, fail again, wait for A's 2, release the other (%d %d %d %d)", w1, d0, w2,
         d1);
   char line[4096];
   check(ws->deferred.retries == 1 && ws->deferred.oom_waits == 2 && !ws->deferred.count &&
            log_lines(lm, "allocation failed with 2 BOs", line, sizeof(line)) == 1 && has(line, "2 CPU waits") &&
            has(line, "the retry succeeded"),
         "counted and logged: %.160s", line);

   /* Nothing held: the failure is returned at once, with no wait. */
   fail_calls(BC250_HOST_CreateAllocation2, 1);
   mark = h.n_ev;
   struct radeon_winsys_bo *fb = NULL;
   result = ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &fb);
   no_failures();
   check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !fb &&
            !count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) && ws->deferred.retries == 1,
         "with nothing held a failed allocation fails at once");

   ws->base.buffer_destroy(&ws->base, nb);
   check(unbind(ws, a, cookie) == VK_SUCCESS, "the queue unbinds");
   ws->base.ctx_destroy(a);
   check(!live(h.allocs, h.n_allocs, false), "no allocation left");
   contract();
}

static void
test_deferred_teardown(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   bind(ws, b, cookie_b);
   const uint32_t a_progress = gfx(a)->bc250_progress.handle, b_progress = gfx(b)->bc250_progress.handle;
   struct radeon_winsys_bo *t1 = gtt_bo(ws), *t2 = gtt_bo(ws), *t3 = gtt_bo(ws);
   const uint32_t t1_h = t1->handle, t2_h = t2->handle, t3_h = t3->handle;
   submit_one(ws, a); /* A's 1 */
   submit_one(ws, b); /* B's 1 */
   ws->base.buffer_destroy(&ws->base, t1);
   submit_one(ws, a); /* A's 2 */
   ws->base.buffer_destroy(&ws->base, t2);
   check(ws->deferred.count == 2, "two BOs held: behind A's 1 and B's 1, and behind A's 2 and B's 1");

   long lm = log_mark();
   unsigned mark = h.n_ev;
   radv_wddm2_deferred_finish(ws);
   const int wa1 = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, a_progress);
   const int wb1 = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu, b_progress);
   const int wa2 = wa1 >= 0 ? find_op(wa1 + 1, BC250_HOST_WaitForSynchronizationObjectFromCpu, a_progress) : -1;
   check(cpu_wait_is(wa1, a_progress, 1) && cpu_wait_is(wb1, b_progress, 1) && cpu_wait_is(wa2, a_progress, 2) &&
            count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 3,
         "teardown CPU-waits for A's 1 and B's 1 (the oldest), then A's 2");
   check(!alloc_live(t1_h) && !alloc_live(t2_h) && find_destroy(mark, t1_h) < wa2 && wa2 < find_destroy(mark, t2_h) &&
            !ws->deferred.count && !ws->deferred.forced,
         "each released after its wait, none destroyed unretired");
   char line[4096];
   check(log_lines(lm, "summary: 2 BOs held", line, sizeof(line)) == 1 && has(line, "teardown: 2 CPU waits"),
         "the summary line: %.200s", line);
   check(log_lines(lm, "witness finish destroys=2 ", line, sizeof(line)) == 1, "the witness's finish line: %.200s",
         line);

   /* A failed wait: what is left is destroyed anyway, counted. */
   submit_one(ws, a); /* A's 3 */
   ws->base.buffer_destroy(&ws->base, t3);
   lm = log_mark();
   fail_calls(BC250_HOST_WaitForSynchronizationObjectFromCpu, 1);
   radv_wddm2_deferred_finish(ws);
   no_failures();
   check(!alloc_live(t3_h) && !ws->deferred.count && ws->deferred.forced == 1 &&
            log_lines(lm, "still held after 0 CPU waits", line, sizeof(line)) == 1 && has(line, "a wait failed"),
         "a failed teardown wait: destroyed anyway, one forced: %.160s", line);
   retire(a_progress, 3);
   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   check(!live(h.allocs, h.n_allocs, false) && !live(h.syncs, h.n_syncs, false), "nothing left");
   contract();
}

static void
test_deferred_witness(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ib_ws();
   check(ws->deferred.enabled && ws->deferred.witness, "deferred destruction and its witness are on by default");
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   struct radv_wddm2_queue *qa = gfx(a);
   const uint32_t a_progress = qa->bc250_progress.handle, serial_a = qa->bc250_tracker->serial;

   struct radeon_winsys_bo *up = sized_bo(ws, 65536, UPLOAD_FLAGS, RADV_BO_PRIORITY_UPLOAD_BUFFER);
   struct radeon_winsys_bo *app = sized_bo(ws, 4096, PLAIN_FLAGS, 5);
   struct radeon_winsys_bo *query = sized_bo(ws, 4096, PLAIN_FLAGS, RADV_BO_PRIORITY_QUERY_POOL);
   struct radeon_winsys_bo *unnamed = sized_bo(ws, 4096, PLAIN_FLAGS, 7);
   struct radeon_winsys_bo *cmd = sized_bo(ws, 4096, CMD_FLAGS, RADV_BO_PRIORITY_CS);
   const uint32_t up_h = up->handle, app_h = app->handle;
   struct fake_cs f;
   struct ac_cmdbuf *cs = fake_cs(&f, cmd, cmd->va, 8);
   f.buffers = _mesa_pointer_set_create(NULL);
   _mesa_set_add(f.buffers, up);
   _mesa_set_add(f.buffers, app);
   _mesa_set_add(f.buffers, query);
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS, "A submits a stream that names three BOs; progress 1");
   check(radv_wddm2_bo(up)->last_use_serial == serial_a && radv_wddm2_bo(up)->last_use_value == 1 &&
            radv_wddm2_bo(cmd)->last_use_serial == serial_a && !radv_wddm2_bo(unnamed)->last_use_serial,
         "the submission stamps the BOs of the set and the command BO with A and progress 1, no other");

   long lm = log_mark();
   ws->base.buffer_destroy(&ws->base, up);
   char line[4096];
   check(ws->deferred.in_flight[C_CPU_UPLOAD] == 1 && ws->deferred.in_flight_32bit == 1 &&
            ws->deferred.held_by_class[C_CPU_UPLOAD] == 1 && alloc_live(up_h),
         "the upload BO is destroyed in flight: counted as cpu-upload and 32-bit, and held");
   check(log_lines(lm, "witness in flight #1 ", line, sizeof(line)) == 1 && has(line, "class=cpu-upload size=65536 ") &&
            has(line, "flags=0x143(GTT_WC|CPU_ACCESS|NO_INTERPROCESS_SHARING|32BIT)") && has(line, "prio=30 ") &&
            has(line, "32bit=yes ") && has(line, "last_use=1 completed=0 published=1 held=yes ") &&
            /* The module name is the harness's: build-radv-queue-tests.ps1 links queue-test.exe. */
            has(line, ".exe+0x"),
         "its line names the class, size, flags, priority, the values and the destroy stack: %.200s", line);

   ws->base.buffer_destroy(&ws->base, unnamed);
   check(ws->deferred.in_flight_total == 1 && ws->deferred.held_by_class[C_APPLICATION] == 1 &&
            ws->deferred.destroys[C_APPLICATION] == 1 && !ws->deferred.in_flight[C_APPLICATION],
         "a BO no submission named: held (A is busy), not in flight");
   ws->base.buffer_destroy(&ws->base, cmd);
   check(ws->deferred.in_flight[C_COMMAND_STREAM] == 1, "the command BO the IB1 runs: in flight, command-stream");

   retire(a_progress, 1);
   unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, app);
   check(!alloc_live(app_h) && find_destroy(mark, app_h) >= 0 && ws->deferred.in_flight_total == 2 &&
            ws->deferred.destroys[C_APPLICATION] == 2,
         "once A's 1 retired a named BO is not in flight, and nothing holds it");

   /* The last submission names the queue: query, named on A's 1 (retired), then on B's 1. */
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qb = gfx(b);
   struct fake_cs fq;
   struct ac_cmdbuf *qcs = fake_cs(&fq, NULL, 0x100000, 8);
   fq.buffers = _mesa_pointer_set_create(NULL);
   _mesa_set_add(fq.buffers, query);
   check(submit(ws, b, 1, &qcs, 0, NULL, 0, NULL) == VK_SUCCESS, "B submits a stream that names the query BO");
   lm = log_mark();
   ws->base.buffer_destroy(&ws->base, query);
   char context[32];
   snprintf(context, sizeof(context), "context=0x%x ", qb->context_h);
   check(ws->deferred.in_flight[C_QUERY_POOL] == 1 && log_lines(lm, "witness in flight #3 ", line, sizeof(line)) == 1 &&
            has(line, "class=query-pool ") && has(line, context) && has(line, "held=yes "),
         "in flight on B, its last queue: %.160s", line);

   /* A stale name: a submission that names a destroyed BO (released: its struct waits in the pool). */
   struct fake_cs fs;
   struct ac_cmdbuf *scs = fake_cs(&fs, NULL, 0x100000, 8);
   fs.buffers = _mesa_pointer_set_create(NULL);
   _mesa_set_add(fs.buffers, up);
   lm = log_mark();
   check(submit(ws, a, 1, &scs, 0, NULL, 0, NULL) == VK_SUCCESS, "A submits a stream that still names the upload BO");
   check(ws->deferred.stale_names == 1 && log_lines(lm, "witness stale name: ", line, sizeof(line)) == 1 &&
            has(line, "names 1 destroyed BOs (1 so far)") && has(line, "class=cpu-upload size=65536 "),
         "counted and logged: %.160s", line);

   /* A secondary's BOs, its command BO too, reach the primary's set. */
   struct radeon_winsys_bo *sec = sized_bo(ws, 4096, PLAIN_FLAGS, 3);
   struct ac_cmdbuf *parent = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *child = ws->base.cs_create(&ws->base, AMD_IP_GFX, true);
   child->reserved_dw = 16;
   for (unsigned i = 0; i < 3; i++)
      child->buf[child->cdw++] = NOP_1DW;
   ws->base.cs_add_buffer(child, sec);
   ws->base.cs_finalize(child);
   ws->base.cs_execute_secondary(parent, child, true);
   ws->base.cs_finalize(parent);
   struct radeon_winsys_bo *child_ib = radv_winsys_cs(child)->ib_buffers[0].bo;
   check(submit(ws, a, 1, &parent, 0, NULL, 0, NULL) == VK_SUCCESS, "A submits the primary only");
   const uint64_t a_value = qa->bc250_progress.wait_value;
   check(radv_wddm2_bo(sec)->last_use_serial == serial_a && radv_wddm2_bo(sec)->last_use_value == a_value &&
            child_ib && radv_wddm2_bo(child_ib)->last_use_value == a_value,
         "the secondary's BO and its command BO carry A's stamp");
   ws->base.buffer_destroy(&ws->base, sec);
   check(ws->deferred.in_flight[C_APPLICATION] == 1, "destroying the secondary's BO counts in flight");
   retire(a_progress, a_value);
   retire(qb->bc250_progress.handle, 1);
   ws->base.cs_destroy(parent);
   ws->base.cs_destroy(child);

   /* Destroyed: up, unnamed, cmd, app, query, sec, and the two streams' command BOs (all retired). */
   lm = log_mark();
   radv_wddm2_deferred_finish(ws);
   check(log_lines(lm, "witness finish destroys=8 ", line, sizeof(line)) == 1 && has(line, " in_flight=4 ") &&
            has(line, " in_flight_32bit=1 ") && has(line, " stale_names=1 ") && has(line, "cpu-upload:1/1/1 ") &&
            has(line, "command-stream:3/1/1 ") && has(line, "query-pool:1/1/1 ") && has(line, "application:3/2/1 "),
         "the finish line: %.400s", line);
   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   _mesa_set_destroy(f.buffers, NULL);
   _mesa_set_destroy(fq.buffers, NULL);
   _mesa_set_destroy(fs.buffers, NULL);
   contract();

   /* Deferred destruction off, the witness on: an in-flight destroy is counted and destroyed at once. */
   _putenv_s("BC250_DEFERRED_DESTROY", "0");
   ws = make_ws();
   clear_deferred_env();
   struct radeon_winsys_ctx *c = new_ctx(ws);
   bind(ws, c, cookie_a);
   check(!ws->deferred.enabled && ws->deferred.witness && gfx(c)->bc250_tracker, "off, the witness still tracks the queue");
   struct radeon_winsys_bo *x = gtt_bo(ws);
   const uint32_t x_h = x->handle;
   struct fake_cs fx;
   struct ac_cmdbuf *xcs = fake_cs(&fx, NULL, 0x100000, 8);
   fx.buffers = _mesa_pointer_set_create(NULL);
   _mesa_set_add(fx.buffers, x);
   submit(ws, c, 1, &xcs, 0, NULL, 0, NULL);
   lm = log_mark();
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, x);
   check(!alloc_live(x_h) && find_destroy(mark, x_h) >= 0 && ws->deferred.in_flight_total == 1 &&
            !ws->deferred.held_by_class[C_APPLICATION] && log_lines(lm, "held=no ", NULL, 0) == 1,
         "counted in flight, destroyed at once, logged held=no");
   unbind(ws, c, cookie_a);
   ws->base.ctx_destroy(c);
   _mesa_set_destroy(fx.buffers, NULL);
   contract();

   /* Both off: no queue is tracked. */
   _putenv_s("BC250_DEFERRED_DESTROY", "0");
   _putenv_s("BC250_DEFERRED_WITNESS", "0");
   ws = make_ws();
   clear_deferred_env();
   c = new_ctx(ws);
   bind(ws, c, cookie_a);
   check(!ws->deferred.enabled && !ws->deferred.witness && !gfx(c)->bc250_tracker, "both off: no tracker");
   unbind(ws, c, cookie_a);
   ws->base.ctx_destroy(c);
   contract();
}

/* The knobs with no file: the environment alone, and the host's policy over it. */
static void
test_deferred_policy(void)
{
   char line[4096];
   clear_deferred_env();

   long lm = log_mark();
   struct radv_wddm2_winsys *ws = make_ws();
   check(ws->deferred.enabled && ws->deferred.witness && ws->deferred.cap_bytes == 512ull << 20 &&
            log_lines(lm, "deferred destroy: header version=4 ", line, sizeof(line)) == 1 &&
            has(line, "destroy=on(default) witness=on(default) cap_mb=512(default) ") &&
            has(line, " policy=absent/00 ") &&
            has(line, " summary_s=30(default) coalesce=on(default) gather_slots=16(default) progress_fence=gpu(default)"),
         "no file and no policy: the compiled defaults, and the header says so: %.300s", line);
   check(ws->bc250_gather_slots == 16 && ws->bc250_merge_signals && ws->bc250_drop_waits && ws->bc250_progress_gpu &&
            ws->summary.interval_ns == 30000000000ull && ws->summary.next_ns == ws->summary.start_ns + 30000000000ull,
         "defaults: 16 slots, both coalescings, a summary every 30 s from creation");

   /* The environment, key by key, for a host that named nothing. */
   _putenv_s("BC250_DEFERRED_CAP_MB", "7");
   _putenv_s("BC250_DEFERRED_WITNESS", "0");
   lm = log_mark();
   ws = make_ws();
   check(ws->deferred.cap_bytes == 7ull << 20 && !ws->deferred.witness &&
            log_lines(lm, "cap_mb=7(env)", NULL, 0) == 1 && log_lines(lm, "witness=off(env)", NULL, 0) == 1,
         "the environment sets a knob the host did not name");
   _putenv_s("BC250_DEFERRED_CAP_MB", "lots");
   lm = log_mark();
   ws = make_ws();
   check(ws->deferred.cap_bytes == 512ull << 20 && log_lines(lm, "cap_mb=512(invalid, default)", NULL, 0) == 1,
         "an invalid cap: the default, 512 MiB");
   _putenv_s("BC250_DEFERRED_CAP_MB", "0");
   ws = make_ws();
   check(!ws->deferred.cap_bytes, "BC250_DEFERRED_CAP_MB=0: no cap");
   clear_deferred_env();

   _putenv_s("BC250_GATHER_SLOTS", "4");
   _putenv_s("BC250_SUBMIT_COALESCE", "waits");
   _putenv_s("BC250_DEFERRED_SUMMARY_S", "0");
   lm = log_mark();
   ws = make_ws();
   check(ws->bc250_gather_slots == 4 && !ws->bc250_merge_signals && ws->bc250_drop_waits && !ws->summary.next_ns &&
            log_lines(lm, " summary_s=0(env) coalesce=waits(env) gather_slots=4(env)", NULL, 0) == 1,
         "the environment: 4 slots, waits only, no summary");
   static const char *const bad_slots[] = {"3", "33", "16x", "-4"};
   bool all_default = true;
   for (unsigned i = 0; i < ARRAY_SIZE(bad_slots); i++) {
      _putenv_s("BC250_GATHER_SLOTS", bad_slots[i]);
      lm = log_mark();
      ws = make_ws();
      all_default &= ws->bc250_gather_slots == 16 && log_lines(lm, "gather_slots=16(invalid, default)", NULL, 0) == 1;
   }
   check(all_default, "BC250_GATHER_SLOTS 3, 33, 16x and -4: invalid, 16");
   _putenv_s("BC250_SUBMIT_COALESCE", "yes");
   _putenv_s("BC250_DEFERRED_SUMMARY_S", "soon");
   _putenv_s("BC250_PROGRESS_FENCE", "fast");
   lm = log_mark();
   ws = make_ws();
   check(ws->bc250_merge_signals && ws->bc250_drop_waits && ws->summary.interval_ns == 30000000000ull &&
            ws->bc250_progress_gpu &&
            log_lines(lm, " summary_s=30(invalid, default) coalesce=on(invalid, default)", NULL, 0) == 1 &&
            log_lines(lm, " progress_fence=gpu(invalid, default)", NULL, 0) == 1,
         "an invalid coalescing, period or progress fence: the defaults");
   clear_deferred_env();

   /* A host that names a knob owns it: the environment is not asked, and the header says host. */
   static const struct bc250_host_policy_values named = {
      .present = 1,
      .specified = BC250_HOST_POLICY_HAS_COALESCE | BC250_HOST_POLICY_HAS_GATHER_SLOTS |
                   BC250_HOST_POLICY_HAS_PROGRESS_GPU | BC250_HOST_POLICY_HAS_DEFERRED_DESTROY,
      .coalesce = BC250_HOST_POLICY_COALESCE_SIGNALS,
      .gather_slots = 24,
      .progress_gpu = 0,
      .deferred_destroy = 0,
   };
   _putenv_s("BC250_SUBMIT_COALESCE", "0");
   _putenv_s("BC250_GATHER_SLOTS", "8");
   _putenv_s("BC250_PROGRESS_FENCE", "gpu");
   _putenv_s("BC250_DEFERRED_DESTROY", "1");
   ws_policy = &named;
   lm = log_mark();
   ws = make_ws();
   check(ws->bc250_merge_signals && !ws->bc250_drop_waits && ws->bc250_gather_slots == 24 &&
            !ws->bc250_progress_gpu && !ws->deferred.enabled,
         "the host's four values stand although the environment says otherwise for every one of them");
   check(log_lines(lm, "deferred destroy: header version=4 ", line, sizeof(line)) == 1 &&
            has(line, "destroy=off(host) ") && has(line, " policy=host/0f ") &&
            has(line, " coalesce=signals(host) gather_slots=24(host) progress_fence=kernel(host)"),
         "the header names the host for each: %.300s", line);
   check(ws->deferred.witness && log_lines(lm, "witness=on(default)", NULL, 0) == 1,
         "a knob the host did not name is untouched by the policy");

   /* A host that names nothing (a version 1 policy, or a version 2 with specified 0) leaves the
    * environment in charge, which is what every trial before this change measured. */
   static const struct bc250_host_policy_values silent = {.present = 1};
   ws_policy = &silent;
   lm = log_mark();
   ws = make_ws();
   check(ws->bc250_gather_slots == 8 && !ws->bc250_merge_signals && !ws->bc250_drop_waits &&
            ws->bc250_progress_gpu && ws->deferred.enabled &&
            log_lines(lm, " policy=host/00 ", NULL, 0) == 1 &&
            log_lines(lm, "gather_slots=8(env)", NULL, 0) == 1,
         "a policy that names no knob: the environment, as before, and policy=host/00 in the header");
   ws_policy = NULL;
   clear_deferred_env();
   contract();
}

/* The costs, on this host's CPU (not the lab's), printed as INFO lines: the witness's stamp per BO a
 * submission names, a whole submission with the witness on and off, and a destroy in each of its
 * paths. */
static void
test_deferred_cost(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   LARGE_INTEGER frequency, t0, t1;
   QueryPerformanceFrequency(&frequency);
   const double ns_per_tick = 1e9 / (double)frequency.QuadPart;
   clear_ib_env();
   clear_deferred_env();

   /* The stamp walk of one stream: its set (n BOs and its own command BO) and its command BOs. */
   struct radv_wddm2_winsys *ws = make_ib_ws();
   const unsigned sizes[3] = {16, 256, 4096};
   struct radv_wddm2_bo *bos = calloc(4096, sizeof(*bos));
   for (unsigned s = 0; s < 3; s++) {
      struct ac_cmdbuf *cs = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
      for (unsigned i = 0; i < sizes[s]; i++)
         ws->base.cs_add_buffer(cs, &bos[i].base);
      const unsigned named = sizes[s] + 1;
      const unsigned passes = 4000000 / named;
      uint32_t stale = 0;
      struct radv_wddm2_bo *first = NULL;
      for (unsigned i = 0; i < 1000; i++)
         radv_wddm2_witness_cs(cs, 1, i, &stale, &first);
      QueryPerformanceCounter(&t0);
      for (unsigned i = 0; i < passes; i++)
         radv_wddm2_witness_cs(cs, 1, i, &stale, &first);
      QueryPerformanceCounter(&t1);
      const double ns = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick;
      printf("INFO stamp cost: %u BOs named, %u passes, %.2f ns per BO, %.1f ns per stream\n", named, passes,
             ns / ((double)passes * named), ns / passes);
      check(!stale && bos[0].last_use_serial == 1, "%u BOs: stamped, none stale", named);
      ws->base.cs_destroy(cs);
   }
   free(bos);

   /* A whole submission (fake host calls included) of a stream naming 256 BOs, witness on and off. */
   double submit_ns[2];
   for (unsigned pass = 0; pass < 2; pass++) {
      if (pass)
         _putenv_s("BC250_DEFERRED_WITNESS", "0");
      ws = make_ib_ws();
      clear_deferred_env();
      h.recycle = true;
      struct radeon_winsys_ctx *c = new_ctx(ws);
      bind(ws, c, cookie);
      struct radv_wddm2_bo *many = calloc(256, sizeof(*many));
      struct fake_cs f;
      struct ac_cmdbuf *cs = fake_cs(&f, NULL, 0x100000, 8);
      f.buffers = _mesa_pointer_set_create(NULL);
      for (unsigned i = 0; i < 256; i++)
         _mesa_set_add(f.buffers, &many[i]);
      const uint32_t progress = gfx(c)->bc250_progress.handle;
      enum { N = 20000 };
      for (unsigned i = 0; i < 1000; i++)
         submit(ws, c, 1, &cs, 0, NULL, 0, NULL);
      QueryPerformanceCounter(&t0);
      for (unsigned i = 0; i < N; i++)
         submit(ws, c, 1, &cs, 0, NULL, 0, NULL);
      QueryPerformanceCounter(&t1);
      submit_ns[pass] = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / N;
      check(pass ? !many[0].last_use_serial : many[0].last_use_value == gfx(c)->bc250_progress.wait_value,
            "witness %s: %s", pass ? "off" : "on", pass ? "nothing stamped" : "every submission stamps");
      retire(progress, gfx(c)->bc250_progress.wait_value);
      h.recycle = false;
      h.n_ev = 0;
      unbind(ws, c, cookie);
      ws->base.ctx_destroy(c);
      _mesa_set_destroy(f.buffers, NULL);
      free(many);
   }
   printf("INFO submission cost (fake host included), a stream naming 256 BOs: %.0f ns with the witness, %.0f ns "
          "without, %.1f ns per BO\n",
          submit_ns[0], submit_ns[1], (submit_ns[0] - submit_ns[1]) / 256.0);

   /* The destroy: held (a queue busy, the BO not named), held and in flight (named; destroys 65..200
    * without the 64 logged ones and the powers of two, which write a line), immediate (nothing in
    * flight), and with both off. Each winsys after the other: make_ws resets the fake host. */
   enum { NQ = 200 };
   static const char *const names[4] = {"held", "held, in flight", "immediate", "both off"};
   double cost[4] = {0}, drain_held_ns = 0, drain_empty_ns = 0;
   unsigned n_cost[4] = {0};
   bool counted[4] = {false};
   struct radeon_winsys_bo *qb[NQ];
   for (unsigned pass = 0; pass < 4; pass++) {
      if (pass == 3) {
         _putenv_s("BC250_DEFERRED_DESTROY", "0");
         _putenv_s("BC250_DEFERRED_WITNESS", "0");
      }
      struct radv_wddm2_winsys *w = make_ib_ws();
      clear_deferred_env();
      struct radeon_winsys_ctx *c = new_ctx(w);
      bind(w, c, cookie);
      for (unsigned i = 0; i < NQ; i++)
         qb[i] = sized_bo(w, 4096, PLAIN_FLAGS, 0);
      struct fake_cs f;
      struct ac_cmdbuf *cs = fake_cs(&f, NULL, 0x100000, 8);
      f.buffers = _mesa_pointer_set_create(NULL);
      if (pass == 1) {
         for (unsigned i = 0; i < NQ; i++)
            _mesa_set_add(f.buffers, qb[i]);
      }
      if (pass != 2)
         submit(w, c, 1, &cs, 0, NULL, 0, NULL);
      for (unsigned i = 0; i < NQ; i++) {
         QueryPerformanceCounter(&t0);
         w->base.buffer_destroy(&w->base, qb[i]);
         QueryPerformanceCounter(&t1);
         if (i + 1 > 64 && !util_is_power_of_two_nonzero64(i + 1)) {
            cost[pass] += (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick;
            n_cost[pass]++;
         }
      }
      if (pass == 0) {
         /* The drain of every submission, creation and destroy: NQ BOs held, none retired. */
         QueryPerformanceCounter(&t0);
         for (unsigned i = 0; i < 100000; i++)
            radv_wddm2_deferred_drain(w);
         QueryPerformanceCounter(&t1);
         drain_held_ns = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / 100000;
      }
      counted[pass] = pass == 0   ? w->deferred.count == NQ && !w->deferred.in_flight_total
                      : pass == 1 ? w->deferred.count == NQ && w->deferred.in_flight_total == NQ
                      : pass == 2 ? !w->deferred.count && w->deferred.immediate == NQ
                                  : !w->deferred.count && !w->deferred.destroys_total;
      retire(gfx(c)->bc250_progress.handle, gfx(c)->bc250_progress.wait_value);
      radv_wddm2_deferred_drain(w);
      if (pass == 0) {
         QueryPerformanceCounter(&t0);
         for (unsigned i = 0; i < 100000; i++)
            radv_wddm2_deferred_drain(w);
         QueryPerformanceCounter(&t1);
         drain_empty_ns = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / 100000;
      }
      unbind(w, c, cookie);
      w->base.ctx_destroy(c);
      _mesa_set_destroy(f.buffers, NULL);
   }
   for (unsigned pass = 0; pass < 4; pass++)
      printf("INFO destroy cost, %s: %.2f us (%u destroys, the fake host's calls included)\n", names[pass],
             cost[pass] / 1000.0 / n_cost[pass], n_cost[pass]);
   printf("INFO drain cost: %.1f ns with %u BOs held and none retired, %.1f ns with none held\n", drain_held_ns, NQ,
          drain_empty_ns);
   check(counted[0] && counted[1] && counted[2] && counted[3], "each pass took its path for all %u destroys", NQ);
   contract();
}

/* BC250_SUBMIT_COALESCE, on by default: the progress value rides on the application's signal call of the
 * same submission, and a wait the CPU already sees complete is left out (test_signal_wait has it off). */
static void
test_submit_coalesce(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   const uint32_t GPU_WAIT = BC250_HOST_WaitForSynchronizationObjectFromGpu;
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   check(ws->bc250_merge_signals && ws->bc250_drop_waits, "coalescing is on by default");
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   bind(ws, a, cookie_a);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   const uint32_t a_progress = qa->bc250_progress.handle, b_progress = qb->bc250_progress.handle;
   struct vk_wddm2_monitored_fence f1, f2, f3;
   app_fence(&f1);
   app_fence(&f2);
   app_fence(&f3);
   f3.shared_handle = (HANDLE)(uintptr_t)0x77; /* imported: opened from a shared handle */
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, NULL, 0x100000, 8);

   /* A signals f1: one call carries f1 = 1 and A's progress 1. */
   unsigned mark = h.n_ev;
   const struct vk_sync_signal signal1 = {.sync = &f1.base, .signal_value = 1};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &signal1) == VK_SUCCESS, "A submits and signals f1");
   int sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
   int sig = find_op(mark, SIGNAL, 0);
   int pub = find_op(mark, BC250_HOST_PUBLISH_PROGRESS, 0);
   check(count_op(mark, SIGNAL) == 1, "one signal call (%u)", count_op(mark, SIGNAL));
   check(sig >= 0 && event_is(sig, SIGNAL, qa->context_h, f1.handle, 1) && h.ev[sig].n_objs == 2 &&
            h.ev[sig].objs[1] == a_progress && h.ev[sig].vals[1] == 1,
         "on A's context: f1 = 1 first, A's progress 1 last");
   check(sub < sig && sig < pub && event_is(pub, BC250_HOST_PUBLISH_PROGRESS, qa->context_h, a_progress, 1),
         "order: IB, the signal call, then the publication of progress 1");
   check(qa->bc250_progress.wait_value == 1 && qa->bc250_gather[0].retire_value == 1 && qa->bc250_gather_index == 1,
         "A's last value is 1, slot 0 retires with it, slot 1 is next");
   check(radv_wddm2_ctx(a)->per_ip[AMD_IP_GFX].last_submission.handle == f1.handle &&
            radv_wddm2_ctx(a)->per_ip[AMD_IP_GFX].last_submission.wait_value == 1,
         "the context's last submission is still the application's f1 = 1");

   /* B waits for f1, not complete: queued. It signals f2 and f3: B's progress after both. */
   mark = h.n_ev;
   const struct vk_sync_wait wait1 = {.sync = &f1.base, .wait_value = 1};
   const struct vk_sync_signal signals23[2] = {{.sync = &f2.base, .signal_value = 1}, {.sync = &f3.base, .signal_value = 5}};
   check(submit(ws, b, 1, &cs, 1, &wait1, 2, signals23) == VK_SUCCESS, "B waits for f1, submits, signals f2 and f3");
   int wait = find_op(mark, GPU_WAIT, 0);
   sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
   sig = find_op(mark, SIGNAL, 0);
   check(wait >= 0 && event_is(wait, GPU_WAIT, qb->context_h, f1.handle, 1) && h.ev[wait].n_objs == 1,
         "f1 is not complete: B's context waits for it on the GPU");
   check(sig >= 0 && count_op(mark, SIGNAL) == 1 && h.ev[sig].n_objs == 3 && h.ev[sig].objs[0] == f2.handle &&
            h.ev[sig].vals[0] == 1 && h.ev[sig].objs[1] == f3.handle && h.ev[sig].vals[1] == 5 &&
            h.ev[sig].objs[2] == b_progress && h.ev[sig].vals[2] == 1,
         "one call: f2 = 1, the imported f3 = 5, in their order and unchanged, then B's progress 1");
   check(wait < sub && sub < sig, "order: wait, IB, signal call");
   check(names_only(mark, qb->context_h) && !names_sync(mark, a_progress) && find_naming(mark, SIGNAL, a_progress) < 0,
         "B's submission names nothing of A");

   /* f1 is complete on the CPU's read: the next wait for it is left out, and with it the call. */
   *f1.value_map = 1;
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 1, &wait1, 0, NULL) == VK_SUCCESS && !count_op(mark, GPU_WAIT),
         "a wait for complete f1: no GPU wait call");
   check(event_is(find_op(mark, SIGNAL, b_progress), SIGNAL, qb->context_h, b_progress, 2) && count_op(mark, SIGNAL) == 1,
         "without application signals B's progress 2 gets its own call, as before");

   /* Two waits, one complete: the call names only the other. */
   const struct vk_sync_wait waits12[2] = {{.sync = &f1.base, .wait_value = 1}, {.sync = &f2.base, .wait_value = 1}};
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 2, waits12, 0, NULL) == VK_SUCCESS, "A waits for f1 (complete) and f2 (not)");
   wait = find_op(mark, GPU_WAIT, 0);
   check(wait >= 0 && event_is(wait, GPU_WAIT, qa->context_h, f2.handle, 1) && h.ev[wait].n_objs == 1 &&
            count_op(mark, GPU_WAIT) == 1,
         "one wait call, for f2 alone");

   /* A later value than the CPU sees is waited for; so is a fence reading the lost-device value. */
   const struct vk_sync_wait wait1_2 = {.sync = &f1.base, .wait_value = 2};
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 1, &wait1_2, 0, NULL) == VK_SUCCESS &&
            event_is(find_op(mark, GPU_WAIT, 0), GPU_WAIT, qb->context_h, f1.handle, 2),
         "f1 reads 1: a wait for 2 goes to the GPU");
   *f3.value_map = UINT64_MAX;
   const struct vk_sync_wait wait3 = {.sync = &f3.base, .wait_value = 5};
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 1, &wait3, 0, NULL) == VK_SUCCESS &&
            event_is(find_op(mark, GPU_WAIT, 0), GPU_WAIT, qb->context_h, f3.handle, 5),
         "a fence reading UINT64_MAX: the wait goes to the kernel, which reports the loss");
   *f3.value_map = 0;

   /* A submission without an IB signals the application's fence alone. */
   const struct vk_sync_signal signal2_2 = {.sync = &f2.base, .signal_value = 2};
   mark = h.n_ev;
   check(submit(ws, a, 0, NULL, 0, NULL, 1, &signal2_2) == VK_SUCCESS, "A signals f2 = 2 without an IB");
   sig = find_op(mark, SIGNAL, 0);
   check(sig >= 0 && count_op(mark, SIGNAL) == 1 && event_is(sig, SIGNAL, qa->context_h, f2.handle, 2) &&
            h.ev[sig].n_objs == 1 && qa->bc250_progress.wait_value == 2,
         "one call naming f2 alone; A's progress stays at 2");

   check(ws->submit_stats.submits == 6 && ws->submit_stats.progress_merged == 2 &&
            ws->submit_stats.progress_separate == 4 && ws->submit_stats.signal_calls == 3 &&
            ws->submit_stats.signal_objects == 4,
         "counted: 6 submissions, 2 progress values merged, 4 on their own, 3 signal calls of 4 fences");
   check(ws->submit_stats.wait_objects == 6 && ws->submit_stats.wait_dropped == 2 &&
            ws->submit_stats.wait_calls == 4 && ws->submit_stats.wait_skipped == 1,
         "counted: 6 waits, 2 left out, 4 calls, 1 call left out");

   /* One call names at most D3DDDI_MAX_OBJECT_SIGNALED (32) fences: 31 application fences take the
    * progress value along, 32 leave it a call of its own. */
   struct vk_wddm2_monitored_fence many[32];
   struct vk_sync_signal many_signals[32];
   for (unsigned i = 0; i < 32; i++) {
      app_fence(&many[i]);
      many_signals[i] = (struct vk_sync_signal){.sync = &many[i].base, .signal_value = 1};
   }
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 31, many_signals) == VK_SUCCESS, "A signals 31 fences");
   sig = find_op(mark, SIGNAL, 0);
   check(sig >= 0 && count_op(mark, SIGNAL) == 1 && h.ev[sig].n_objs == 32 && h.ev[sig].objs[0] == many[0].handle,
         "one call of 32 fences, the progress value last");
   for (unsigned i = 0; i < 32; i++)
      many_signals[i].signal_value = 2;
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 32, many_signals) == VK_SUCCESS, "A signals 32 fences");
   const int own = find_op(mark, SIGNAL, a_progress), apps = find_op(mark, SIGNAL, many[0].handle);
   check(count_op(mark, SIGNAL) == 2 && event_is(own, SIGNAL, qa->context_h, a_progress, 4) && own < apps &&
            h.ev[apps].n_objs == 32,
         "two calls: A's progress 4 on its own, then the 32 application fences");

   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();

   /* "signals" merges the progress value and queues every wait; "waits" the other way round. */
   static const char *const modes[2] = {"signals", "waits"};
   for (unsigned m = 0; m < 2; m++) {
      _putenv_s("BC250_SUBMIT_COALESCE", modes[m]);
      ws = make_ws();
      clear_deferred_env();
      struct radeon_winsys_ctx *c = new_ctx(ws);
      bind(ws, c, cookie_a);
      struct vk_wddm2_monitored_fence done, out;
      app_fence(&done);
      app_fence(&out);
      *done.value_map = 3;
      const struct vk_sync_wait wd = {.sync = &done.base, .wait_value = 3};
      const struct vk_sync_signal so = {.sync = &out.base, .signal_value = 1};
      mark = h.n_ev;
      submit(ws, c, 1, &cs, 1, &wd, 1, &so);
      const bool merged = count_op(mark, SIGNAL) == 1 && event_names(find_op(mark, SIGNAL, 0), gfx(c)->bc250_progress.handle, 1);
      const bool waited = count_op(mark, GPU_WAIT) == 1;
      check(m == 0 ? merged && waited : !merged && count_op(mark, SIGNAL) == 2 && !waited,
            "BC250_SUBMIT_COALESCE=%s: %s", modes[m],
            m == 0 ? "one signal call with the progress value, the complete wait queued"
                   : "the progress value in a call of its own, the complete wait left out");
      unbind(ws, c, cookie_a);
      ws->base.ctx_destroy(c);
      contract();
   }
}

/* Deferred destruction behind a merged progress value: a held BO waits for the value the application's
 * signal call carries, and the hold, the cap's CPU wait, teardown's CPU wait and the gather slots all see
 * that value retire. */
static void
test_coalesce_hold(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   const uint32_t CPU_WAIT = BC250_HOST_WaitForSynchronizationObjectFromCpu;
   clear_ib_env();
   clear_deferred_env();
   _putenv_s("BC250_DEFERRED_CAP_MB", "1");
   struct radv_wddm2_winsys *ws = make_ws();
   clear_deferred_env();
   check(ws->bc250_merge_signals && ws->deferred.enabled && ws->deferred.cap_bytes == 1ull << 20,
         "coalescing on, deferred destruction on, a 1 MiB cap");
   struct radeon_winsys_ctx *a = new_ctx(ws);
   bind(ws, a, cookie_a);
   struct radv_wddm2_queue *qa = gfx(a);
   const uint32_t a_progress = qa->bc250_progress.handle;
   struct vk_wddm2_monitored_fence f;
   app_fence(&f);
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, NULL, 0x100000, 8);

   /* A signals f = 1; its progress 1 rides on that call only. A BO destroyed now is held behind it. */
   struct radeon_winsys_bo *x = gtt_bo(ws);
   const uint32_t x_h = x->handle;
   unsigned mark = h.n_ev;
   const struct vk_sync_signal s1 = {.sync = &f.base, .signal_value = 1};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s1) == VK_SUCCESS, "A submits and signals f = 1");
   int sig = find_naming(mark, SIGNAL, a_progress);
   check(sig >= 0 && event_names(sig, a_progress, 1) && event_names(sig, f.handle, 1) && count_op(mark, SIGNAL) == 1,
         "A's progress 1 is signalled by the application's signal call, and by nothing else");
   ws->base.buffer_destroy(&ws->base, x);
   check(alloc_live(x_h) && ws->deferred.count == 1 && qa->bc250_tracker->published == 1,
         "a BO destroyed now is held (A published 1)");
   *f.value_map = 1;
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(x_h) && ws->deferred.count == 1, "f reads 1 but A's progress does not: still held");
   retire(a_progress, 1);
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(x_h) && find_destroy(mark, x_h) >= 0 && !ws->deferred.count,
         "A's progress reads the merged value 1: released");

   /* The cap: 1.25 MiB held, the CPU waits for the merged value 2. */
   struct radeon_winsys_bo *c1 = sized_bo(ws, 512 * 1024, PLAIN_FLAGS, 0), *c2 = sized_bo(ws, 768 * 1024, PLAIN_FLAGS, 0);
   const uint32_t c1_h = c1->handle, c2_h = c2->handle;
   const struct vk_sync_signal s2 = {.sync = &f.base, .signal_value = 2};
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s2) == VK_SUCCESS, "A signals f = 2");
   sig = find_naming(mark, SIGNAL, a_progress);
   check(sig >= 0 && event_names(sig, a_progress, 2) && count_op(mark, SIGNAL) == 1, "with A's progress 2 in the same call");
   ws->base.buffer_destroy(&ws->base, c1);
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, c2);
   const int w = find_op(mark, CPU_WAIT, 0);
   check(cpu_wait_is(w, a_progress, 2) && count_op(mark, CPU_WAIT) == 1,
         "over the cap: one CPU wait, for A's progress 2, the merged value");
   check(!alloc_live(c1_h) && !alloc_live(c2_h) && find_destroy(mark, c1_h) > w && !ws->deferred.count &&
            ws->deferred.cap_waits == 1 && !ws->deferred.cap_failed,
         "both released after it, the cap wait counted");

   /* Teardown: the CPU waits for the merged value 3. */
   struct radeon_winsys_bo *t = gtt_bo(ws);
   const uint32_t t_h = t->handle;
   const struct vk_sync_signal s3 = {.sync = &f.base, .signal_value = 3};
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s3) == VK_SUCCESS &&
            event_names(find_naming(mark, SIGNAL, a_progress), a_progress, 3) && count_op(mark, SIGNAL) == 1,
         "A signals f = 3 and its progress 3 in one call");
   ws->base.buffer_destroy(&ws->base, t);
   mark = h.n_ev;
   radv_wddm2_deferred_finish(ws);
   const int tw = find_op(mark, CPU_WAIT, 0);
   check(cpu_wait_is(tw, a_progress, 3) && tw < find_destroy(mark, t_h) && !alloc_live(t_h) && !ws->deferred.forced,
         "teardown CPU-waits for the merged value 3, then releases the BO, none forced");

   /* The gather slots retire on merged values: B's slot 0 is reused after B's progress 1, merged. */
   struct radeon_winsys_ctx *b = new_ctx(ws);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qb = gfx(b);
   const uint32_t b_progress = qb->bc250_progress.handle;
   struct vk_wddm2_monitored_fence g;
   app_fence(&g);
   bool all_merged = true;
   const unsigned fill = h.n_ev;
   for (unsigned i = 1; i <= ws->bc250_gather_slots; i++) {
      const struct vk_sync_signal si = {.sync = &g.base, .signal_value = i};
      mark = h.n_ev;
      submit(ws, b, 1, &cs, 0, NULL, 1, &si);
      all_merged &= count_op(mark, SIGNAL) == 1 && event_names(find_naming(mark, SIGNAL, b_progress), b_progress, i);
   }
   check(all_merged && !count_op(fill, CPU_WAIT) && qb->bc250_gather[0].retire_value == 1 &&
            qb->bc250_gather_index == 0,
         "B fills its %u slots, each submission one signal call with its progress value", ws->bc250_gather_slots);
   const struct vk_sync_signal sn = {.sync = &g.base, .signal_value = ws->bc250_gather_slots + 1};
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 0, NULL, 1, &sn) == VK_SUCCESS, "B's next submission");
   const int rw = find_op(mark, CPU_WAIT, 0);
   check(cpu_wait_is(rw, b_progress, 1) && count_op(mark, CPU_WAIT) == 1 && ws->submit_stats.gather_waits >= 1,
         "reusing slot 0 CPU-waits for B's progress 1, the value of B's first merged call");

   retire(b_progress, qb->bc250_progress.wait_value);
   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   check(!live(h.allocs, h.n_allocs, false) && !ws->deferred.count, "nothing held, no allocation left");
   contract();

#ifdef NDEBUG
   /* A failed merged call: the accepted IB has no retirement value, so the queue stops. */
   ws = make_ws();
   struct radeon_winsys_ctx *c = new_ctx(ws);
   bind(ws, c, cookie_a);
   struct radv_wddm2_queue *qc = gfx(c);
   struct vk_wddm2_monitored_fence k;
   app_fence(&k);
   const struct vk_sync_signal sk = {.sync = &k.base, .signal_value = 1};
   fail_calls(SIGNAL, 1);
   const VkResult failed = submit(ws, c, 1, &cs, 0, NULL, 1, &sk);
   no_failures();
   check(failed == VK_ERROR_DEVICE_LOST && qc->bc250_submit_failed && !qc->bc250_progress.wait_value &&
            !qc->bc250_gather[0].retire_value && !qc->bc250_gather_index,
         "the call fails: device lost, no retirement value recorded, the slot not advanced");
   mark = h.n_ev;
   check(submit_one(ws, c) == VK_ERROR_DEVICE_LOST && !count_op(mark, BC250_HOST_SubmitCommand),
         "the queue refuses further work, no IB goes out");
   check(unbind(ws, c, cookie_a) == VK_ERROR_DEVICE_LOST, "its unbind keeps everything (its work cannot retire)");
   ws->base.ctx_destroy(c);
#endif
}

/* BC250_GATHER_SLOTS: a queue keeps that many submissions in flight before the next one CPU-waits for the
 * oldest, and packs a submission of several IBs into any of its slots. */
static void
test_gather_slots(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   const uint32_t CPU_WAIT = BC250_HOST_WaitForSynchronizationObjectFromCpu;
   static const char *const values[3] = {"4", "", "32"};
   static const unsigned counts[3] = {4, 16, 32};
   for (unsigned k = 0; k < 3; k++) {
      clear_ib_env();
      clear_deferred_env();
      _putenv_s("BC250_GATHER_SLOTS", values[k]);
      struct radv_wddm2_winsys *ws = make_ib_ws();
      clear_deferred_env();
      const unsigned n = counts[k];
      check(ws->bc250_gather_slots == n, "BC250_GATHER_SLOTS=%s: %u slots", values[k][0] ? values[k] : "(unset)", n);
      struct radeon_winsys_ctx *c = new_ctx(ws);
      bind(ws, c, cookie);
      struct radv_wddm2_queue *q = gfx(c);
      const uint32_t progress = q->bc250_progress.handle;
      struct radeon_winsys_bo *bo[2];
      uint32_t *map[2];
      if (!pattern_bos(ws, 2, bo, map)) {
         check(false, "two mapped command BOs");
         return;
      }
      for (unsigned i = 0; i + 1 < n; i++)
         submit_one(ws, c);
      /* The last slot before the wrap packs two IBs: a gather BO there, beyond the 7 of before. */
      struct fake_cs fa, fb;
      struct ac_cmdbuf *two[2] = {fake_cs(&fa, bo[0], bo[0]->va, 16), fake_cs(&fb, bo[1], bo[1]->va, 16)};
      check(submit(ws, c, 2, two, 0, NULL, 0, NULL) == VK_SUCCESS && q->bc250_gather[n - 1].bo &&
               last_ib1(q) == (const uint32_t *)q->bc250_gather[n - 1].map && is_ib2_call(last_ib1(q), bo[0]->va, 16),
            "%u slots: submission %u packs its two IBs into slot %u", n, n, n - 1);
      /* The allocations' paging-fence waits aside, nothing waits on the CPU. */
      check(find_op(0, CPU_WAIT, progress) < 0 && !ws->submit_stats.gather_waits && !q->bc250_gather_index &&
               q->bc250_progress.wait_value == n,
            "%u submissions in flight, no CPU wait for progress, the ring wrapped", n);
      unsigned mark = h.n_ev;
      check(submit_one(ws, c) == VK_SUCCESS, "submission %u", n + 1);
      check(cpu_wait_is(find_op(mark, CPU_WAIT, 0), progress, 1) && count_op(mark, CPU_WAIT) == 1 &&
               ws->submit_stats.gather_waits == 1 && q->bc250_gather_index == 1,
            "reusing slot 0 CPU-waits for progress 1, once, counted");
      retire(progress, q->bc250_progress.wait_value);
      check(unbind(ws, c, cookie) == VK_SUCCESS, "the queue unbinds");
      ws->base.ctx_destroy(c);
      for (unsigned i = 0; i < 2; i++)
         ws->base.buffer_destroy(&ws->base, bo[i]);
      check(!live(h.allocs, h.n_allocs, false) && !live(h.syncs, h.n_syncs, false),
            "the unbind released the gather BO of slot %u with the rest", n - 1);
      contract();
   }
}

/* The periodic summary: two lines when the period has passed, each only when its counters changed, both
 * again at teardown. The tests force the deadline (summary.next_ns = 1) instead of sleeping, but once. */
static void
test_summary(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   check(ws->summary.interval_ns == 30000000000ull && ws->summary.next_ns == ws->summary.start_ns + 30000000000ull,
         "a summary every 30 s, the first due 30 s after creation");
   struct radeon_winsys_ctx *a = new_ctx(ws);
   bind(ws, a, cookie);
   const uint32_t a_progress = gfx(a)->bc250_progress.handle;
   long lm = log_mark();
   for (unsigned i = 0; i < 4; i++)
      submit_one(ws, a);
   check(!log_lines(lm, "periodic ", NULL, 0) && !ws->summary.lines, "not due: no line");

   /* A held BO and a host import. */
   struct radeon_winsys_bo *x = gtt_bo(ws);
   ws->base.buffer_destroy(&ws->base, x);
   struct obj *imported = host_alloc();
   struct radeon_winsys_bo *import = NULL;
   ws->base.buffer_from_hosted(&ws->base, ws->host.identity, imported->handle, 0, 0x800000000ull, 65536, &import);
   ws->base.buffer_destroy(&ws->base, import);
   check(ws->deferred.count == 1 && ws->deferred.borrowed == 1, "one BO held, one host import destroyed");

   /* The deadline passes: the next submission writes both lines first. */
   ws->summary.next_ns = 1;
   lm = log_mark();
   submit_one(ws, a);
   char line[4096];
   check(log_lines(lm, "deferred destroy: periodic #1 t=", NULL, 0) == 2 && ws->summary.lines == 1,
         "the next submission writes two lines, #1");
   check(log_lines(lm, "periodic #1 t=0s deferred: ", line, sizeof(line)) == 1 &&
            has(line, " held=1 held_kib=4 peak=1 peak_kib=4 held_total=1 ") && has(line, " destroys=1 borrowed=1 ") &&
            has(line, " in_flight=0 in_flight_32bit=0 ") && has(line, " stale_names=0 cap_waits=0 ") &&
            has(line, " retries=0 ") && has(line, "classes(destroyed/held/in_flight)=virtual:0/0/0 "),
         "the deferred line: %.400s", line);
   check(log_lines(lm, "periodic #1 t=0s submit: submits=4 progress_separate=4 progress_merged=0 ", line,
                   sizeof(line)) == 1 &&
            has(line, " gather_slots=16 gather_waits=0 ") &&
            has(line, " coalesce=on progress_fence=gpu progress_gpu=0 kernel_queues=1"),
         "the submit line, before this submission counts (the fake host gives no GPU address: one queue on the "
         "kernel signal): %.400s",
         line);
   const uint64_t now = os_time_get_nano();
   check(ws->summary.next_ns > now && ws->summary.next_ns <= now + 30000000000ull,
         "the next one is due a period after this one");

   /* Only the submission counters changed: their line alone. */
   ws->summary.next_ns = 1;
   lm = log_mark();
   submit_one(ws, a);
   check(log_lines(lm, "periodic #2 ", NULL, 0) == 1 && log_lines(lm, "periodic #2 t=0s submit: submits=5 ", NULL, 0) == 1,
         "only the submit counters changed: one line, #2");
   ws->summary.next_ns = 1;
   radv_wddm2_summary_tick(ws);
   ws->summary.next_ns = 1;
   lm = log_mark();
   radv_wddm2_summary_tick(ws);
   check(!log_lines(lm, "periodic", NULL, 0) && ws->summary.lines == 3, "a tick with nothing changed writes nothing");

   /* A tick that lost the race for its deadline does nothing. */
   const uint64_t next = ws->summary.next_ns;
   submit_one(ws, a);
   lm = log_mark();
   radv_wddm2_summary_write(ws, os_time_get_nano(), next - 1, false);
   check(!log_lines(lm, "periodic", NULL, 0) && ws->summary.next_ns == next, "another thread's deadline: no line");

   /* Teardown: both lines, final. */
   retire(a_progress, gfx(a)->bc250_progress.wait_value);
   check(unbind(ws, a, cookie) == VK_SUCCESS, "the queue unbinds");
   ws->base.ctx_destroy(a);
   lm = log_mark();
   radv_wddm2_deferred_finish(ws);
   check(log_lines(lm, "periodic final t=", NULL, 0) == 2 &&
            log_lines(lm, "periodic final t=0s deferred: held=0 held_kib=0 peak=1 ", line, sizeof(line)) == 1 &&
            has(line, " released=1 ") && has(line, " borrowed=1 "),
         "teardown writes both lines, final: %.300s", line);
   check(log_lines(lm, "summary: 1 BOs held", line, sizeof(line)) == 1 && has(line, "; 1 host imports destroyed, never held"),
         "and the teardown summary counts the host imports: %.300s", line);
   contract();

   /* BC250_DEFERRED_SUMMARY_S=0: never. */
   _putenv_s("BC250_DEFERRED_SUMMARY_S", "0");
   ws = make_ws();
   clear_deferred_env();
   a = new_ctx(ws);
   bind(ws, a, cookie);
   lm = log_mark();
   submit_one(ws, a);
   radv_wddm2_summary_tick(ws);
   retire(gfx(a)->bc250_progress.handle, gfx(a)->bc250_progress.wait_value);
   unbind(ws, a, cookie);
   ws->base.ctx_destroy(a);
   radv_wddm2_deferred_finish(ws);
   check(!ws->summary.next_ns && !log_lines(lm, "periodic", NULL, 0), "BC250_DEFERRED_SUMMARY_S=0: no line, not even at teardown");
   contract();

   /* A period of 1 s on the real clock. */
   _putenv_s("BC250_DEFERRED_SUMMARY_S", "1");
   ws = make_ws();
   clear_deferred_env();
   a = new_ctx(ws);
   bind(ws, a, cookie);
   submit_one(ws, a);
   Sleep(1100);
   lm = log_mark();
   submit_one(ws, a);
   check(log_lines(lm, "periodic #1 t=1s submit: submits=1 ", NULL, 0) == 1,
         "BC250_DEFERRED_SUMMARY_S=1: the first submission after 1.1 s writes #1");
   retire(gfx(a)->bc250_progress.handle, gfx(a)->bc250_progress.wait_value);
   unbind(ws, a, cookie);
   ws->base.ctx_destroy(a);
   contract();
}

/* The costs of this change on this host's CPU, printed as INFO lines: the summary tick when not due, a
 * submission with one application signal merged and not, one with one complete wait left out and not.
 * The fake host's calls cost a function call; on the lab each call left out is a D3DKMT call. */
static void
test_submit_cost(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   LARGE_INTEGER frequency, t0, t1;
   QueryPerformanceFrequency(&frequency);
   const double ns_per_tick = 1e9 / (double)frequency.QuadPart;
   clear_ib_env();
   clear_deferred_env();

   struct radv_wddm2_winsys *ws = make_ws();
   enum { TICKS = 10000000 };
   for (unsigned i = 0; i < 100000; i++)
      radv_wddm2_summary_tick(ws);
   QueryPerformanceCounter(&t0);
   for (unsigned i = 0; i < TICKS; i++)
      radv_wddm2_summary_tick(ws);
   QueryPerformanceCounter(&t1);
   const double tick_ns = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / TICKS;
   printf("INFO summary tick, not due: %.1f ns (one clock read)\n", tick_ns);
   check(!ws->summary.lines, "%u ticks before the deadline write nothing", TICKS);

   /* Each pass: N submissions of one IB; the CPU retires every value at once, so no slot waits. */
   enum { N = 20000 };
   static const char *const modes[2] = {"1", "0"};
   double signal_ns[2], wait_ns[2];
   uint64_t calls[2][2];
   for (unsigned pass = 0; pass < 2; pass++) {
      _putenv_s("BC250_SUBMIT_COALESCE", modes[pass]);
      ws = make_ws();
      clear_deferred_env();
      h.recycle = true;
      struct radeon_winsys_ctx *c = new_ctx(ws);
      bind(ws, c, cookie);
      struct radv_wddm2_queue *q = gfx(c);
      uint64_t *const progress = &find(h.syncs, h.n_syncs, q->bc250_progress.handle)->value;
      struct vk_wddm2_monitored_fence app, done;
      app_fence(&app);
      app_fence(&done);
      *done.value_map = 1;
      struct fake_cs fc;
      struct ac_cmdbuf *cs = fake_cs(&fc, NULL, 0x100000, 8);
      uint64_t value = 0;
      struct vk_sync_signal s = {.sync = &app.base};
      for (unsigned i = 0; i < 1000; i++) {
         s.signal_value = ++value;
         submit(ws, c, 1, &cs, 0, NULL, 1, &s);
         *progress = q->bc250_progress.wait_value;
      }
      const uint64_t merged0 = ws->submit_stats.progress_merged, separate0 = ws->submit_stats.progress_separate;
      QueryPerformanceCounter(&t0);
      for (unsigned i = 0; i < N; i++) {
         s.signal_value = ++value;
         submit(ws, c, 1, &cs, 0, NULL, 1, &s);
         *progress = q->bc250_progress.wait_value;
      }
      QueryPerformanceCounter(&t1);
      signal_ns[pass] = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / N;
      calls[pass][0] = ws->submit_stats.progress_merged - merged0;
      calls[pass][1] = ws->submit_stats.progress_separate - separate0;

      const struct vk_sync_wait w = {.sync = &done.base, .wait_value = 1};
      const uint64_t dropped0 = ws->submit_stats.wait_dropped, calls0 = ws->submit_stats.wait_calls;
      QueryPerformanceCounter(&t0);
      for (unsigned i = 0; i < N; i++) {
         submit(ws, c, 1, &cs, 1, &w, 0, NULL);
         *progress = q->bc250_progress.wait_value;
      }
      QueryPerformanceCounter(&t1);
      wait_ns[pass] = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / N;
      check(pass ? ws->submit_stats.wait_calls - calls0 == N && ws->submit_stats.wait_dropped == dropped0
                 : ws->submit_stats.wait_dropped - dropped0 == N && ws->submit_stats.wait_calls == calls0,
            "coalescing %s: %s", pass ? "off" : "on", pass ? "every complete wait queued" : "every complete wait left out");
      h.recycle = false;
      h.n_ev = 0;
      unbind(ws, c, cookie);
      ws->base.ctx_destroy(c);
   }
   clear_deferred_env();
   check(calls[0][0] == N && !calls[0][1] && !calls[1][0] && calls[1][1] == N,
         "on: %llu merged, off: %llu progress signals of their own", (unsigned long long)calls[0][0],
         (unsigned long long)calls[1][1]);
   printf("INFO submission with one application signal (fake host included): %.0f ns merged (1 signal call), %.0f ns "
          "separate (2 calls)\n",
          signal_ns[0], signal_ns[1]);
   printf("INFO submission with one complete wait (fake host included): %.0f ns left out (no wait call), %.0f ns "
          "queued (1 call)\n",
          wait_ns[0], wait_ns[1]);
   contract();
}

/* BC250_PROGRESS_FENCE=gpu, the default of version 3: each IB1 of a queue whose progress fence has a usable
 * FenceValueGPUVirtualAddress ends with the GPU's write of its progress value, the kernel never signals that
 * fence, and the application's fences keep their kernel call. The fake host gives the address with
 * h.gpu_va_mode = 1; the fake GPU runs the writes it took from the watched queues' IB1s (gpu_run), and a CPU
 * wait for such a fence runs them up to the value waited for. */

#define RM_HEADER 0xC0064900u /* PKT3(PKT3_RELEASE_MEM, 6, 0) */

/* The 8 dwords at dw are the write of value to va, bit for bit the KMD's ring fence packet. */
static bool
is_progress_write(const uint32_t *dw, uint64_t va, uint64_t value)
{
   return dw && dw[0] == RM_HEADER && dw[1] == 0x06603514u && dw[2] == 0x40000000u && dw[3] == (uint32_t)va &&
          dw[4] == (uint32_t)(va >> 32) && dw[5] == (uint32_t)value && dw[6] == (uint32_t)(value >> 32) && !dw[7];
}

/* make_ib_ws whose host gives each fence a GPU address of the given h.gpu_va_mode. */
static struct radv_wddm2_winsys *
make_gpu_ws(unsigned mode)
{
   struct radv_wddm2_winsys *ws = make_ib_ws();
   h.gpu_va_mode = mode;
   return ws;
}

static void
watch(const struct radv_wddm2_queue *q)
{
   h.watch[h.n_watch++] = q;
}

static uint64_t
fence_va(uint32_t handle)
{
   const struct obj *o = find(h.syncs, h.n_syncs, handle);
   return o ? o->gpu_va : 0;
}

static uint64_t
fence_value(uint32_t handle)
{
   const struct obj *o = find(h.syncs, h.n_syncs, handle);
   return o ? o->value : 0;
}

/* The invariant on the calls since from: every application signal call of q's context comes after the
 * SubmitCommand of an IB1 whose last packet writes the progress value published between the two, and no
 * kernel call names q's progress fence. calls: the application calls seen. */
static bool
signals_behind_writes(unsigned from, const struct radv_wddm2_queue *q, unsigned *calls)
{
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   uint64_t written = 0, published = 0;
   bool ok = true;
   *calls = 0;
   for (unsigned i = from; i < h.n_ev; i++) {
      const struct event *e = &h.ev[i];
      if (e->op == BC250_HOST_SubmitCommand && e->context == q->context_h) {
         written = e->progress_write;
         published = 0;
      } else if (e->op == BC250_HOST_PUBLISH_PROGRESS && e->context == q->context_h) {
         published = e->value;
      } else if (e->op == SIGNAL && e->context == q->context_h) {
         (*calls)++;
         ok &= written && published == written;
      }
   }
   return ok && find_naming(from, SIGNAL, q->bc250_progress.handle) < 0;
}

static void
test_progress_gpu(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   const uint32_t SUBMIT = BC250_HOST_SubmitCommand, PUBLISH = BC250_HOST_PUBLISH_PROGRESS;
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_gpu_ws(1);
   check(ws->bc250_progress_gpu && ws->bc250_merge_signals && !ws->bc250_gather_copy,
         "by default the GPU writes the progress fence (coalescing and IB2 calls on too)");
   struct radeon_winsys_ctx *c = new_ctx(ws);
   check(bind(ws, c, cookie) == VK_SUCCESS, "the queue binds");
   struct radv_wddm2_queue *q = gfx(c);
   const uint32_t progress = q->bc250_progress.handle;
   uint64_t va = fence_va(progress);
   check(va && q->bc250_progress_va == va && !ws->submit_stats.kernel_queues,
         "the queue keeps its fence's FenceValueGPUVirtualAddress 0x%" PRIx64 "; no queue on the kernel signal", va);
   watch(q);
   struct radeon_winsys_bo *bo[3];
   uint32_t *map[3];
   if (!pattern_bos(ws, 3, bo, map)) {
      check(false, "three mapped command BOs");
      return;
   }
   struct fake_cs fa, fb, fn;
   struct ac_cmdbuf *cs[2];

   /* One plain stream, no application signal: the IB1 calls it, pads, then writes progress 1. */
   unsigned mark = h.n_ev;
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "one stream, no application signal");
   const uint32_t *ib1 = last_ib1(q);
   check(one_ib1(q, 16) && is_ib2_call(ib1, bo[0]->va, 16) && ib1[4] == NOP_4DW && !ib1[5] && !ib1[6] && !ib1[7] &&
            is_progress_write(ib1 + 8, va, 1),
         "an IB1 of 16 dwords in the gather slot: the IB2 call, the 4-dword NOP, then RELEASE_MEM %08x %08x %08x of "
         "progress 1 to the fence's GPU address, last",
         ib1 ? ib1[8] : 0, ib1 ? ib1[9] : 0, ib1 ? ib1[10] : 0);
   check(!count_op(mark, SIGNAL), "no kernel signal call at all (%u)", count_op(mark, SIGNAL));
   int sub = find_op(mark, SUBMIT, 0), pub = find_op(mark, PUBLISH, 0);
   check(sub >= 0 && sub < pub && event_is(pub, PUBLISH, q->context_h, progress, 1) && h.ev[sub].progress_write == 1,
         "order: the IB1, then the publication of progress 1");
   check(q->bc250_progress.wait_value == 1 && q->bc250_gather[0].retire_value == 1 && q->bc250_gather_index == 1 &&
            q->bc250_tracker->published == 1,
         "1 is the queue's last value, slot 0's retirement value and the tracker's published value");
   check(fence_value(progress) == 0 && gpu_run(1) == 1 && fence_value(progress) == 1,
         "the fence reads 0 until the GPU runs the IB1, then 1");

   /* Two streams and two application signals, one imported: one kernel call names just those two. */
   struct vk_wddm2_monitored_fence f1, f2, f3;
   app_fence(&f1);
   app_fence(&f2);
   app_fence(&f3);
   f2.shared_handle = (HANDLE)(uintptr_t)0x77; /* imported: opened from a shared handle */
   const struct vk_sync_signal s12[2] = {{.sync = &f1.base, .signal_value = 1}, {.sync = &f2.base, .signal_value = 5}};
   mark = h.n_ev;
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   cs[1] = fake_cs(&fb, bo[1], bo[1]->va + 256, 24);
   check(submit(ws, c, 2, cs, 0, NULL, 2, s12) == VK_SUCCESS, "two streams, two application signals (f2 imported)");
   ib1 = last_ib1(q);
   check(one_ib1(q, 16) && is_ib2_call(ib1, bo[0]->va, 16) && is_ib2_call(ib1 + 4, bo[1]->va + 256, 24) &&
            is_progress_write(ib1 + 8, va, 2),
         "an IB1 of 16 dwords: two calls, then the write of progress 2 (no padding needed)");
   int sig = find_op(mark, SIGNAL, 0);
   check(count_op(mark, SIGNAL) == 1 && sig >= 0 && h.ev[sig].n_objs == 2 && h.ev[sig].objs[0] == f1.handle &&
            event_names(sig, f1.handle, 1) && event_names(sig, f2.handle, 5) && h.ev[sig].context == q->context_h,
         "one kernel call on the queue's context: f1 = 1, then the imported f2 = 5, nothing else");
   check(find_op(mark, SUBMIT, 0) < find_op(mark, PUBLISH, 0) && find_op(mark, PUBLISH, 0) < sig,
         "order: the IB1 that writes 2, the publication of 2, the application's call");
   check(radv_wddm2_ctx(c)->per_ip[AMD_IP_GFX].last_submission.handle == f1.handle &&
            radv_wddm2_ctx(c)->per_ip[AMD_IP_GFX].last_submission.wait_value == 1,
         "the context's last submission is the application's f1 = 1");

   /* 32 application signals: one call of 32, the most one call names (no room needed for progress). */
   struct vk_wddm2_monitored_fence many[32];
   struct vk_sync_signal many_signals[32];
   for (unsigned i = 0; i < 32; i++) {
      app_fence(&many[i]);
      many_signals[i] = (struct vk_sync_signal){.sync = &many[i].base, .signal_value = 1};
   }
   mark = h.n_ev;
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   check(submit(ws, c, 1, cs, 0, NULL, 32, many_signals) == VK_SUCCESS, "one stream, 32 application signals");
   sig = find_op(mark, SIGNAL, 0);
   check(count_op(mark, SIGNAL) == 1 && sig >= 0 && h.ev[sig].n_objs == 32 && is_progress_write(last_ib1(q) + 8, va, 3),
         "one call of the 32 application fences; progress 3 in the IB1");

   /* A stream that calls an IB2 itself is copied, the write after it. */
   cs[0] = fake_cs(&fn, bo[2], bo[2]->va, 8);
   fn.cs.calls_ib2 = true;
   const uint64_t inline0 = ws->bc250_inline_submits;
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "one stream that calls an IB2");
   ib1 = last_ib1(q);
   check(one_ib1(q, 16) && !memcmp(ib1, map[2], 8 * 4) && is_progress_write(ib1 + 8, va, 4) &&
            ws->bc250_inline_submits == inline0 + 1,
         "an IB1 of 16 dwords: its 8 dwords copied, then the write of progress 4; the copy counted");

   /* An empty stream, and a stream with no IB: the IB1 is the write alone. */
   cs[0] = fake_cs(&fn, bo[2], bo[2]->va, 0);
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS && one_ib1(q, 8) && is_progress_write(last_ib1(q), va, 5),
         "an empty stream: the IB1 is the write of progress 5 alone, 8 dwords");
   cs[0] = fake_cs(&fn, bo[2], bo[2]->va, 8);
   fn.cs.num_ib_buffers = 0;
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS && one_ib1(q, 8) && is_progress_write(last_ib1(q), va, 6),
         "a stream without an IB: the write of progress 6 alone");

   /* A submission without a stream signals the application's fence alone: no IB1, no write. */
   const struct vk_sync_signal s3 = {.sync = &f3.base, .signal_value = 1};
   mark = h.n_ev;
   check(submit(ws, c, 0, NULL, 0, NULL, 1, &s3) == VK_SUCCESS && !count_op(mark, SUBMIT) && count_op(mark, SIGNAL) == 1 &&
            event_is(find_op(mark, SIGNAL, 0), SIGNAL, q->context_h, f3.handle, 1) &&
            h.ev[find_op(mark, SIGNAL, 0)].n_objs == 1 && q->bc250_progress.wait_value == 6,
         "a submission without a stream: one call naming f3 alone, progress stays at 6");

   /* The trace of a packed IB1 names the write. */
   ws->bc250_trace_submits = true;
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS && is_progress_write(last_ib1(q) + 8, va, 7),
         "with BC250_TRACE_SUBMITS the submission is the same (progress 7)");
   ws->bc250_trace_submits = false;

   /* The fake GPU: the writes land in submission order, each the IB1's last packet, the KMD's packet. */
   bool all_to_progress = true;
   for (unsigned i = 0; i < h.n_writes; i++)
      all_to_progress &= h.writes[i].va == va && h.writes[i].value == i + 1;
   check(gpu_run(64) == 6 && fence_value(progress) == 7 && h.rm_packets == 7 && !h.rm_not_last && !h.rm_bad &&
            all_to_progress,
         "7 IB1s, 7 RELEASE_MEM packets of 1..7 to the progress fence, each last, none malformed; it reads 7");
   unsigned calls = 0;
   check(signals_behind_writes(0, q, &calls) && calls == 3,
         "each of the %u application calls comes after the IB1 that writes, and the publication of, the progress "
         "value its work retires with; no call names the progress fence",
         calls);
   check(ws->submit_stats.submits == 7 && ws->submit_stats.progress_gpu == 7 && !ws->submit_stats.progress_merged &&
            !ws->submit_stats.progress_separate && ws->submit_stats.signal_calls == 3 &&
            ws->submit_stats.signal_objects == 35,
         "counted: 7 submissions, 7 values written by the GPU, none signalled by the kernel, 3 calls of 35 fences");

   /* The summary's submit line. */
   ws->summary.next_ns = 1;
   const long lm = log_mark();
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   submit(ws, c, 1, cs, 0, NULL, 0, NULL);
   char line[4096];
   check(log_lines(lm, "submit: submits=7 progress_separate=0 progress_merged=0 signal_calls=3 signal_objects=35 ", line,
                   sizeof(line)) == 1 &&
            has(line, " coalesce=on progress_fence=gpu progress_gpu=7 kernel_queues=0"),
         "the summary's submit line: %.400s", line);

   gpu_run(64);
   check(unbind(ws, c, cookie) == VK_SUCCESS, "the queue unbinds, its work retired");
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 3; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   check(!live(h.contexts, h.n_contexts, false) && !live(h.syncs, h.n_syncs, false) && !live(h.allocs, h.n_allocs, false),
         "nothing left");
   contract();

   /* BC250_IB_NOCOPY=0: the IB1 copies the streams, the write after them. */
   _putenv_s("BC250_IB_NOCOPY", "0");
   ws = make_gpu_ws(1);
   clear_ib_env();
   check(ws->bc250_gather_copy && ws->bc250_progress_gpu, "BC250_IB_NOCOPY=0 with the GPU write");
   c = new_ctx(ws);
   bind(ws, c, cookie);
   q = gfx(c);
   va = fence_va(q->bc250_progress.handle);
   watch(q);
   if (!pattern_bos(ws, 2, bo, map)) {
      check(false, "two mapped command BOs");
      return;
   }
   cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
   cs[1] = fake_cs(&fb, bo[1], bo[1]->va + 256, 24);
   check(submit(ws, c, 2, cs, 0, NULL, 0, NULL) == VK_SUCCESS, "two streams");
   ib1 = last_ib1(q);
   check(one_ib1(q, 48) && !memcmp(ib1, map[0], 16 * 4) && !memcmp(ib1 + 16, (const uint8_t *)map[1] + 256, 24 * 4) &&
            is_progress_write(ib1 + 40, va, 1),
         "an IB1 of 48 dwords: both streams copied, then the write of progress 1");
   check(submit(ws, c, 1, cs, 0, NULL, 0, NULL) == VK_SUCCESS && one_ib1(q, 24) && !memcmp(last_ib1(q), map[0], 16 * 4) &&
            is_progress_write(last_ib1(q) + 16, va, 2),
         "one stream: copied too (24 dwords), then the write of progress 2");
   gpu_run(64);
   check(fence_value(q->bc250_progress.handle) == 2 && !h.rm_not_last && !h.rm_bad, "the GPU wrote 2, last in each IB1");
   unbind(ws, c, cookie);
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   contract();
}

/* The waits that use the progress value with the GPU writing it: a held BO, the cap, teardown, a gather slot's
 * reuse and the idle wait all wait for the GPU's write, and no kernel call ever names the fence. */
static void
test_progress_gpu_hold(void)
{
   void *const cookie_a = (void *)(uintptr_t)0xA0, *const cookie_b = (void *)(uintptr_t)0xB0;
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   const uint32_t CPU_WAIT = BC250_HOST_WaitForSynchronizationObjectFromCpu;
   clear_ib_env();
   clear_deferred_env();
   _putenv_s("BC250_DEFERRED_CAP_MB", "1");
   struct radv_wddm2_winsys *ws = make_gpu_ws(1);
   clear_deferred_env();
   check(ws->bc250_progress_gpu && ws->deferred.enabled && ws->deferred.cap_bytes == 1ull << 20,
         "the GPU write, deferred destruction, a 1 MiB cap");
   struct radeon_winsys_ctx *a = new_ctx(ws);
   bind(ws, a, cookie_a);
   struct radv_wddm2_queue *qa = gfx(a);
   const uint32_t a_progress = qa->bc250_progress.handle;
   watch(qa);
   struct radeon_winsys_bo *bo[1];
   uint32_t *map[1];
   if (!pattern_bos(ws, 1, bo, map)) {
      check(false, "a mapped command BO");
      return;
   }
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, bo[0], bo[0]->va, 16);
   struct vk_wddm2_monitored_fence f;
   app_fence(&f);

   /* A BO destroyed behind progress 1 is held until the progress fence reads 1. */
   struct radeon_winsys_bo *x = gtt_bo(ws);
   const uint32_t x_h = x->handle;
   const struct vk_sync_signal s1 = {.sync = &f.base, .signal_value = 1};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s1) == VK_SUCCESS, "A submits and signals f = 1");
   ws->base.buffer_destroy(&ws->base, x);
   check(alloc_live(x_h) && ws->deferred.count == 1 && qa->bc250_tracker->published == 1,
         "a BO destroyed now is held (A published 1)");
   *f.value_map = 1;
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(x_h) && ws->deferred.count == 1,
         "f reads 1, the progress fence 0: still held (the hold reads the progress fence alone)");
   gpu_run(1);
   unsigned mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(x_h) && find_destroy(mark, x_h) >= 0 && !ws->deferred.count,
         "the GPU's write of 1 releases it");

   /* The cap: 1.25 MiB held, the CPU waits for progress 2, which the GPU writes. */
   struct radeon_winsys_bo *c1 = sized_bo(ws, 512 * 1024, PLAIN_FLAGS, 0), *c2 = sized_bo(ws, 768 * 1024, PLAIN_FLAGS, 0);
   const uint32_t c1_h = c1->handle, c2_h = c2->handle;
   const struct vk_sync_signal s2 = {.sync = &f.base, .signal_value = 2};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s2) == VK_SUCCESS, "A signals f = 2");
   ws->base.buffer_destroy(&ws->base, c1);
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, c2);
   const int w = find_op(mark, CPU_WAIT, 0);
   check(cpu_wait_is(w, a_progress, 2) && count_op(mark, CPU_WAIT) == 1 && fence_value(a_progress) == 2,
         "over the cap: one CPU wait, for A's progress 2, which the GPU's write completes");
   check(!alloc_live(c1_h) && !alloc_live(c2_h) && find_destroy(mark, c1_h) > w && !ws->deferred.count &&
            ws->deferred.cap_waits == 1 && !ws->deferred.cap_failed,
         "both released after it, the cap wait counted");

   /* Teardown: the CPU waits for progress 3. */
   struct radeon_winsys_bo *t = gtt_bo(ws);
   const uint32_t t_h = t->handle;
   const struct vk_sync_signal s3 = {.sync = &f.base, .signal_value = 3};
   check(submit(ws, a, 1, &cs, 0, NULL, 1, &s3) == VK_SUCCESS, "A signals f = 3");
   ws->base.buffer_destroy(&ws->base, t);
   mark = h.n_ev;
   radv_wddm2_deferred_finish(ws);
   const int tw = find_op(mark, CPU_WAIT, 0);
   check(cpu_wait_is(tw, a_progress, 3) && tw < find_destroy(mark, t_h) && !alloc_live(t_h) && !ws->deferred.forced &&
            fence_value(a_progress) == 3,
         "teardown CPU-waits for the GPU's write of 3, then releases the BO, none forced");

   /* The gather slots: B fills its 16 with no CPU wait; slot 0's reuse waits for the write of 1. */
   struct radeon_winsys_ctx *b = new_ctx(ws);
   bind(ws, b, cookie_b);
   struct radv_wddm2_queue *qb = gfx(b);
   const uint32_t b_progress = qb->bc250_progress.handle;
   const uint64_t b_va = fence_va(b_progress);
   watch(qb);
   const unsigned fill = h.n_ev;
   for (unsigned i = 0; i < ws->bc250_gather_slots; i++)
      submit(ws, b, 1, &cs, 0, NULL, 0, NULL);
   const uint32_t *slot0 = (const uint32_t *)qb->bc250_gather[0].map;
   /* The gather BOs' allocations wait for the paging fence; nothing waits for progress. */
   check(find_op(fill, CPU_WAIT, b_progress) < 0 && !count_op(fill, SIGNAL) && !qb->bc250_gather_index &&
            qb->bc250_progress.wait_value == ws->bc250_gather_slots && !fence_value(b_progress) &&
            is_progress_write(slot0 + 8, b_va, 1),
         "B fills its %u slots: no CPU wait, no kernel signal, the GPU ran none, slot 0 still writes 1",
         ws->bc250_gather_slots);
   mark = h.n_ev;
   check(submit(ws, b, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS, "B's next submission");
   const int rw = find_op(mark, CPU_WAIT, 0), rs = find_op(mark, BC250_HOST_SubmitCommand, 0);
   check(cpu_wait_is(rw, b_progress, 1) && count_op(mark, CPU_WAIT) == 1 && rw < rs && ws->submit_stats.gather_waits == 1 &&
            fence_value(b_progress) == 1,
         "reusing slot 0 CPU-waits for B's progress 1 (the GPU ran the first IB1, and only it), before the slot "
         "is rewritten and submitted, counted");
   check(is_progress_write(slot0 + 8, b_va, ws->bc250_gather_slots + 1) &&
            qb->bc250_gather[0].retire_value == ws->bc250_gather_slots + 1,
         "then slot 0 holds the IB1 that writes %u", ws->bc250_gather_slots + 1);

   /* The idle wait: the application's last fence, then progress. */
   struct vk_wddm2_monitored_fence g;
   app_fence(&g);
   const struct vk_sync_signal sg = {.sync = &g.base, .signal_value = 1};
   check(submit(ws, b, 1, &cs, 0, NULL, 1, &sg) == VK_SUCCESS, "B signals g = 1");
   const uint64_t last = qb->bc250_progress.wait_value;
   mark = h.n_ev;
   check(ws->base.ctx_wait_idle(b, AMD_IP_GFX, 0), "B waits idle");
   const int wg = find_op(mark, CPU_WAIT, g.handle), wp = find_op(mark, CPU_WAIT, b_progress);
   check(cpu_wait_is(wg, g.handle, 1) && cpu_wait_is(wp, b_progress, last) && wg < wp && fence_value(b_progress) == last,
         "the idle wait: g = 1, then B's progress %" PRIu64 ", which the GPU wrote", last);

   unsigned calls = 0;
   check(signals_behind_writes(0, qa, &calls) && calls == 3 && signals_behind_writes(0, qb, &calls) && calls == 1,
         "every application call comes after its IB1's write and publication; no kernel call names either progress fence");
   check(!h.rm_not_last && !h.rm_bad && ws->submit_stats.progress_gpu == ws->submit_stats.submits &&
            !ws->submit_stats.progress_merged && !ws->submit_stats.progress_separate,
         "every value written by the GPU (%" PRIu64 "), none by the kernel", ws->submit_stats.progress_gpu);
   check(unbind(ws, a, cookie_a) == VK_SUCCESS && unbind(ws, b, cookie_b) == VK_SUCCESS, "both queues unbind");
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   ws->base.buffer_destroy(&ws->base, bo[0]);
   check(!live(h.allocs, h.n_allocs, false) && !ws->deferred.count, "nothing held, no allocation left");
   contract();
}

/* Five submissions of every shape on c's queue (one stream, one with an application signal, two streams,
 * one with a signal again, two with a signal). With gpu each IB1 ends with the write of its value and no
 * kernel call names the progress fence; without, no IB1 holds a RELEASE_MEM, each value is in a kernel
 * call, and a single stream is launched where it is. */
static bool
mixed_submissions(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *c, struct radeon_winsys_bo *const *bo, bool gpu)
{
   const uint32_t SIGNAL = BC250_HOST_SignalSynchronizationObjectFromGpu2;
   struct radv_wddm2_queue *q = gfx(c);
   const uint32_t progress = q->bc250_progress.handle;
   struct vk_wddm2_monitored_fence f;
   app_fence(&f);
   struct fake_cs fa, fb;
   struct ac_cmdbuf *cs[2];
   bool ok = true;
   for (unsigned k = 0; k < 5; k++) {
      const unsigned rm0 = h.rm_packets, mark = h.n_ev;
      const unsigned n = k == 2 || k == 4 ? 2 : 1;
      const bool signals = k == 1 || k == 3 || k == 4;
      cs[0] = fake_cs(&fa, bo[0], bo[0]->va, 16);
      cs[1] = fake_cs(&fb, bo[1], bo[1]->va, 16);
      const struct vk_sync_signal s = {.sync = &f.base, .signal_value = k + 1};
      if (submit(ws, c, n, cs, 0, NULL, signals ? 1 : 0, &s) != VK_SUCCESS)
         return false;
      const uint64_t value = q->bc250_progress.wait_value;
      const int sub = find_op(mark, BC250_HOST_SubmitCommand, 0);
      const int own = find_naming(mark, SIGNAL, progress);
      if (gpu)
         ok &= sub >= 0 && h.ev[sub].progress_write == value && own < 0 && h.rm_packets == rm0 + 1 &&
               count_op(mark, SIGNAL) == (signals ? 1u : 0u);
      else
         ok &= sub >= 0 && h.rm_packets == rm0 && own >= 0 && event_names(own, progress, value) &&
               (n == 2 || h.ev[sub].value == bo[0]->va);
   }
   return ok && !h.rm_bad;
}

static void
test_progress_gpu_fallback(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   struct radeon_winsys_bo *bo[2];
   uint32_t *map[2];
   clear_ib_env();
   clear_deferred_env();

   /* BC250_PROGRESS_FENCE=kernel: the kernel signals the fence, as version 2 did, though the host gives an
    * address; a choice of the knob is not a fallback. */
   _putenv_s("BC250_PROGRESS_FENCE", "kernel");
   long lm = log_mark();
   struct radv_wddm2_winsys *ws = make_gpu_ws(1);
   clear_deferred_env();
   check(!ws->bc250_progress_gpu && log_lines(lm, " progress_fence=kernel(env)", NULL, 0) == 1,
         "BC250_PROGRESS_FENCE=kernel, and the header says so");
   struct radeon_winsys_ctx *c = new_ctx(ws);
   bind(ws, c, cookie);
   watch(gfx(c));
   check(!gfx(c)->bc250_progress_va && fence_va(gfx(c)->bc250_progress.handle) && !ws->submit_stats.kernel_queues,
         "the queue leaves the address unused; no fallback counted");
   if (!pattern_bos(ws, 2, bo, map)) {
      check(false, "two mapped command BOs");
      return;
   }
   check(mixed_submissions(ws, c, bo, false), "five submissions of every shape: the kernel signals each value, no write");
   check(ws->submit_stats.progress_merged == 3 && ws->submit_stats.progress_separate == 2 && !ws->submit_stats.progress_gpu,
         "3 values merged into the application's call, 2 in calls of their own, as version 2");
   retire(gfx(c)->bc250_progress.handle, gfx(c)->bc250_progress.wait_value);
   unbind(ws, c, cookie);
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   contract();

   /* No usable address, or BC250_IB_DWORDS (it may cut the write off): that queue keeps the kernel signal for
    * its life, counted as a fallback. */
   static const struct {
      unsigned mode;
      const char *ib_dwords;
      const char *what;
   } cases[] = {
      {0, NULL, "no FenceValueGPUVirtualAddress"},
      {2, NULL, "an address off its 8-byte alignment"},
      {3, NULL, "an address beyond 48 bits"},
      {1, "4096", "BC250_IB_DWORDS=4096"},
   };
   for (unsigned k = 0; k < ARRAY_SIZE(cases); k++) {
      clear_ib_env();
      if (cases[k].ib_dwords)
         _putenv_s("BC250_IB_DWORDS", cases[k].ib_dwords);
      ws = make_gpu_ws(cases[k].mode);
      clear_ib_env();
      c = new_ctx(ws);
      bind(ws, c, cookie);
      struct radv_wddm2_queue *q = gfx(c);
      watch(q);
      if (!pattern_bos(ws, 2, bo, map)) {
         check(false, "two mapped command BOs");
         return;
      }
      check(ws->bc250_progress_gpu && !q->bc250_progress_va && ws->submit_stats.kernel_queues == 1,
            "%s: the GPU write asked for, the queue on the kernel signal, one fallback counted", cases[k].what);
      check(mixed_submissions(ws, c, bo, false), "%s: every value signalled by the kernel, no write in any IB1",
            cases[k].what);
      retire(q->bc250_progress.handle, q->bc250_progress.wait_value);
      unbind(ws, c, cookie);
      ws->base.ctx_destroy(c);
      for (unsigned i = 0; i < 2; i++)
         ws->base.buffer_destroy(&ws->base, bo[i]);
      contract();
   }

   /* The mode is fixed per binding: a queue on the GPU write keeps it through submissions of every shape,
    * and a rebind takes the new fence's address and starts its values over at 1. */
   ws = make_gpu_ws(1);
   c = new_ctx(ws);
   bind(ws, c, cookie);
   struct radv_wddm2_queue *q = gfx(c);
   watch(q);
   if (!pattern_bos(ws, 2, bo, map)) {
      check(false, "two mapped command BOs");
      return;
   }
   const uint64_t old_va = q->bc250_progress_va;
   check(old_va && mixed_submissions(ws, c, bo, true) && mixed_submissions(ws, c, bo, true),
         "ten submissions of every shape: each IB1 ends with the write, the kernel never names the fence");
   gpu_run(64);
   check(unbind(ws, c, cookie) == VK_SUCCESS && !q->bc250_progress_va, "the unbind clears the queue, address included");
   check(bind(ws, c, cookie) == VK_SUCCESS && q->bc250_progress_va && q->bc250_progress_va != old_va &&
            q->bc250_progress_va == fence_va(q->bc250_progress.handle),
         "the rebind takes the new fence's address");
   struct fake_cs fa;
   struct ac_cmdbuf *cs = fake_cs(&fa, bo[0], bo[0]->va, 16);
   check(submit(ws, c, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS && is_progress_write(last_ib1(q) + 8, q->bc250_progress_va, 1),
         "its first IB1 writes 1 to the new address");
   gpu_run(64);
   check(fence_value(q->bc250_progress.handle) == 1 && !h.rm_bad, "the new fence reads 1");
   unbind(ws, c, cookie);
   ws->base.ctx_destroy(c);
   for (unsigned i = 0; i < 2; i++)
      ws->base.buffer_destroy(&ws->base, bo[i]);
   contract();
}

/* What the GPU write changes when things go wrong: an IB1 that never completes, the lost device's UINT64_MAX,
 * a refused SubmitCommand, a failed application call or publication, and the end of the 64-bit count. */
static void
test_progress_gpu_loss(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   const uint32_t SUBMIT = BC250_HOST_SubmitCommand, PUBLISH = BC250_HOST_PUBLISH_PROGRESS;
   const uint32_t CPU_WAIT = BC250_HOST_WaitForSynchronizationObjectFromCpu;
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_gpu_ws(1);
   struct radeon_winsys_ctx *a = new_ctx(ws);
   bind(ws, a, cookie);
   struct radv_wddm2_queue *qa = gfx(a);
   const uint32_t a_progress = qa->bc250_progress.handle;
   const uint64_t va = qa->bc250_progress_va;
   watch(qa);
   struct radeon_winsys_bo *bo[1];
   uint32_t *map[1];
   if (!pattern_bos(ws, 1, bo, map)) {
      check(false, "a mapped command BO");
      return;
   }
   struct fake_cs fc;
   struct ac_cmdbuf *cs = fake_cs(&fc, bo[0], bo[0]->va, 16);

   /* The IB1s never complete (a hung GPU): no value is ever written. The CPU wait for slot 0 fails (on the
    * lab: vk_wddm2_fence_wait's 10 s bound) before anything touches the slot. */
   struct radeon_winsys_bo *x = gtt_bo(ws);
   const uint32_t x_h = x->handle;
   for (unsigned i = 0; i < ws->bc250_gather_slots; i++)
      submit(ws, a, 1, &cs, 0, NULL, 0, NULL);
   ws->base.buffer_destroy(&ws->base, x);
   const uint32_t *slot0 = (const uint32_t *)qa->bc250_gather[0].map;
   unsigned mark = h.n_ev;
   fail_calls(CPU_WAIT, 1);
   VkResult r = submit(ws, a, 1, &cs, 0, NULL, 0, NULL);
   no_failures();
   check(r == VK_ERROR_DEVICE_LOST && count_op(mark, CPU_WAIT) == 1 && !count_op(mark, SUBMIT) && !count_op(mark, PUBLISH) &&
            qa->bc250_progress.wait_value == ws->bc250_gather_slots && !qa->bc250_gather_index &&
            qa->bc250_gather[0].retire_value == 1 && is_progress_write(slot0 + 8, va, 1),
         "the failed wait for progress 1 loses the device: no IB1, no publication, slot 0 untouched");
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(x_h) && ws->deferred.count == 1, "a BO destroyed meanwhile stays held: nothing was written");
   mark = h.n_ev;
   fail_calls(CPU_WAIT, 1);
   radv_wddm2_deferred_finish(ws);
   no_failures();
   check(!alloc_live(x_h) && ws->deferred.forced == 1, "teardown's failed wait destroys it anyway, counted as forced");

   /* The KMD's refusal of an IB1 (its 500 ms ring bound, WddmFailSubmission) or a TDR ends in the device's
    * removal, and dxgkrnl's UINT64_MAX in every fence: the GPU's write never comes, nor would the kernel's. */
   struct radeon_winsys_bo *y = gtt_bo(ws);
   const uint32_t y_h = y->handle;
   ws->base.buffer_destroy(&ws->base, y);
   check(alloc_live(y_h) && ws->deferred.count == 1, "another BO held behind the unwritten values");
   retire(a_progress, UINT64_MAX);
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_ERROR_DEVICE_LOST && !count_op(mark, SUBMIT) &&
            count_op(mark, BC250_HOST_REPORT_LOST) >= 1,
         "a progress fence reading UINT64_MAX: the next submission is refused, the loss reported, nothing goes out");
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(y_h) && !ws->deferred.count, "every held BO is released: UINT64_MAX retires every value");
   check(unbind(ws, a, cookie) == VK_ERROR_DEVICE_LOST, "the unbind of the lost queue keeps its objects");
   ws->base.ctx_destroy(a);

   /* A refused SubmitCommand: the value is not consumed; the next submission writes it. */
   ws = make_gpu_ws(1);
   a = new_ctx(ws);
   bind(ws, a, cookie);
   qa = gfx(a);
   watch(qa);
   if (!pattern_bos(ws, 1, bo, map)) {
      check(false, "a mapped command BO");
      return;
   }
   cs = fake_cs(&fc, bo[0], bo[0]->va, 16);
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS, "progress 1");
   mark = h.n_ev;
   fail_calls(SUBMIT, 1);
   r = submit(ws, a, 1, &cs, 0, NULL, 0, NULL);
   no_failures();
   check(r == VK_ERROR_DEVICE_LOST && !count_op(mark, PUBLISH) && qa->bc250_progress.wait_value == 1 &&
            qa->bc250_gather_index == 1 && ws->submit_stats.progress_gpu == 1 && qa->bc250_tracker->published == 2,
         "a refused IB1 loses the device: 2 not published to the embedder, slot 1 not advanced, the tracker holds 2");
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS && h.ev[find_op(mark, SUBMIT, 0)].progress_write == 2 &&
            qa->bc250_progress.wait_value == 2 && qa->bc250_gather[1].retire_value == 2,
         "the next submission writes 2 from slot 1");

#ifdef NDEBUG
   /* The application's call fails after an accepted IB1: device lost, but the GPU writes the value anyway,
    * so the queue keeps track of its work (version 2's merged call had to stop the queue). */
   struct vk_wddm2_monitored_fence k;
   app_fence(&k);
   const struct vk_sync_signal sk = {.sync = &k.base, .signal_value = 1};
   mark = h.n_ev;
   fail_calls(BC250_HOST_SignalSynchronizationObjectFromGpu2, 1);
   r = submit(ws, a, 1, &cs, 0, NULL, 1, &sk);
   no_failures();
   check(r == VK_ERROR_DEVICE_LOST && !qa->bc250_submit_failed && qa->bc250_progress.wait_value == 3 &&
            qa->bc250_gather[2].retire_value == 3 && qa->bc250_gather_index == 3 &&
            event_is(find_op(mark, PUBLISH, 0), PUBLISH, qa->context_h, qa->bc250_progress.handle, 3),
         "the application's call fails: device lost, progress 3 published and slot 2 retiring with it");
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS && qa->bc250_progress.wait_value == 4,
         "the queue still submits (progress 4)");

   /* The embedder's publication fails: the queue stops, as in version 2. */
   mark = h.n_ev;
   fail_calls(PUBLISH, 1);
   r = submit(ws, a, 1, &cs, 0, NULL, 0, NULL);
   no_failures();
   check(r == VK_ERROR_DEVICE_LOST && qa->bc250_submit_failed, "a failed publication of 5: device lost, the queue stops");
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_ERROR_DEVICE_LOST && !count_op(mark, SUBMIT),
         "it refuses further work, no IB1 goes out");
   check(unbind(ws, a, cookie) == VK_ERROR_DEVICE_LOST, "its unbind keeps everything (its work cannot be tracked)");
   ws->base.ctx_destroy(a);
#endif

   /* The end of the count: UINT64_MAX is the lost device's value and is never written. */
   ws = make_gpu_ws(1);
   a = new_ctx(ws);
   bind(ws, a, cookie);
   qa = gfx(a);
   watch(qa);
   if (!pattern_bos(ws, 1, bo, map)) {
      check(false, "a mapped command BO");
      return;
   }
   cs = fake_cs(&fc, bo[0], bo[0]->va, 16);
   qa->bc250_progress.wait_value = UINT64_MAX - 2;
   retire(qa->bc250_progress.handle, UINT64_MAX - 2);
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_SUCCESS && h.ev[find_op(mark, SUBMIT, 0)].progress_write == UINT64_MAX - 1 &&
            is_progress_write(last_ib1(qa) + 8, qa->bc250_progress_va, UINT64_MAX - 1),
         "the last value, UINT64_MAX - 1, is written (data 0xFFFFFFFE 0xFFFFFFFF)");
   gpu_run(64);
   const uint64_t published = qa->bc250_tracker->published;
   mark = h.n_ev;
   check(submit(ws, a, 1, &cs, 0, NULL, 0, NULL) == VK_ERROR_DEVICE_LOST && !count_op(mark, SUBMIT) &&
            !count_op(mark, PUBLISH) && qa->bc250_tracker->published == published && qa->bc250_progress.wait_value == UINT64_MAX - 1,
         "the next would be UINT64_MAX: refused before the tracker, the slot or the kernel hear of it");
   check(unbind(ws, a, cookie) == VK_SUCCESS, "the queue unbinds, its last value retired");
   ws->base.ctx_destroy(a);
   ws->base.buffer_destroy(&ws->base, bo[0]);
   contract();
}

/* The costs on this host's CPU, as INFO lines: a submission of one stream and of two, with no application
 * signal and with two (the LOW profile of session 225: about two application fences per submission), the
 * progress fence written by the GPU and signalled by the kernel; with the host calls each one makes. */
static void
test_progress_gpu_cost(void)
{
   void *const cookie = (void *)(uintptr_t)0xA0;
   LARGE_INTEGER frequency, t0, t1;
   QueryPerformanceFrequency(&frequency);
   const double ns_per_tick = 1e9 / (double)frequency.QuadPart;
   clear_ib_env();
   clear_deferred_env();
   enum { N = 20000 };
   static const char *const shapes[4] = {"1 stream, no signal", "1 stream, 2 signals", "2 streams, no signal",
                                         "2 streams, 2 signals"};
   double ns[2][4];
   double calls[2][4];
   for (unsigned mode = 0; mode < 2; mode++) {
      _putenv_s("BC250_PROGRESS_FENCE", mode ? "kernel" : "gpu");
      struct radv_wddm2_winsys *ws = make_gpu_ws(1);
      clear_deferred_env();
      h.recycle = true;
      struct radeon_winsys_ctx *c = new_ctx(ws);
      bind(ws, c, cookie);
      struct radv_wddm2_queue *q = gfx(c);
      uint64_t *const progress = &find(h.syncs, h.n_syncs, q->bc250_progress.handle)->value;
      struct radeon_winsys_bo *bo[2];
      uint32_t *map[2];
      if (!pattern_bos(ws, 2, bo, map)) {
         check(false, "two mapped command BOs");
         return;
      }
      struct fake_cs fa, fb;
      struct ac_cmdbuf *cs[2] = {fake_cs(&fa, bo[0], bo[0]->va, 16), fake_cs(&fb, bo[1], bo[1]->va, 16)};
      struct vk_wddm2_monitored_fence f1, f2;
      app_fence(&f1);
      app_fence(&f2);
      uint64_t value = 0;
      for (unsigned shape = 0; shape < 4; shape++) {
         const unsigned n = shape >= 2 ? 2 : 1, signals = shape & 1 ? 2 : 0;
         struct vk_sync_signal s[2] = {{.sync = &f1.base}, {.sync = &f2.base}};
         for (unsigned i = 0; i < 1000; i++) {
            s[0].signal_value = s[1].signal_value = ++value;
            submit(ws, c, n, cs, 0, NULL, signals, s);
            *progress = q->bc250_progress.wait_value;
         }
         const uint64_t sig0 = ws->submit_stats.signal_calls, sep0 = ws->submit_stats.progress_separate;
         QueryPerformanceCounter(&t0);
         for (unsigned i = 0; i < N; i++) {
            s[0].signal_value = s[1].signal_value = ++value;
            submit(ws, c, n, cs, 0, NULL, signals, s);
            *progress = q->bc250_progress.wait_value;
         }
         QueryPerformanceCounter(&t1);
         ns[mode][shape] = (double)(t1.QuadPart - t0.QuadPart) * ns_per_tick / N;
         /* SubmitCommand, the application's call, a progress call of its own. */
         calls[mode][shape] = 1.0 + (double)(ws->submit_stats.signal_calls - sig0 + ws->submit_stats.progress_separate - sep0) / N;
      }
      h.recycle = false;
      h.n_ev = 0;
      unbind(ws, c, cookie);
      ws->base.ctx_destroy(c);
      for (unsigned i = 0; i < 2; i++)
         ws->base.buffer_destroy(&ws->base, bo[i]);
   }
   clear_deferred_env();
   for (unsigned shape = 0; shape < 4; shape++)
      printf("INFO submission, %s (fake host included): %.0f ns and %.0f kernel calls with the GPU write, %.0f ns and "
             "%.0f calls with the kernel signal\n",
             shapes[shape], ns[0][shape], calls[0][shape], ns[1][shape], calls[1][shape]);
   check(calls[0][0] == 1.0 && calls[0][1] == 2.0 && calls[0][2] == 1.0 && calls[0][3] == 2.0,
         "GPU write: SubmitCommand alone without application signals, plus one call with them");
   check(calls[1][0] == 2.0 && calls[1][1] == 2.0 && calls[1][2] == 2.0 && calls[1][3] == 2.0,
         "kernel signal: always SubmitCommand and one signal call (the progress value merged or on its own)");
   contract();
}

/* cs_add_buffer's recent-BO cache in front of the stream's BO set: whatever the order of the adds, repeats and
 * slot collisions, the witness stamps every BO added since the stream's last reset and no other; the cost of a
 * repeated add, the case of every vertex, index and copy buffer bind, is printed as an INFO line. */
static void
test_cs_add_buffer(void)
{
   clear_ib_env();
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ib_ws();
   enum { NB = 4096 };
   struct radv_wddm2_bo *bos = calloc(NB, sizeof(*bos));
   struct ac_cmdbuf *cs = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   uint32_t stale = 0;
   struct radv_wddm2_bo *first = NULL;

   /* Every BO twice, then every eighth BO again after BOs that share its cache slot (4096 BOs, 256 slots). */
   for (unsigned pass = 0; pass < 2; pass++)
      for (unsigned i = 0; i < NB; i++)
         ws->base.cs_add_buffer(cs, &bos[i].base);
   for (unsigned i = 0; i < NB; i += 8)
      ws->base.cs_add_buffer(cs, &bos[i].base);
   radv_wddm2_witness_cs(cs, 1, 5, &stale, &first);
   unsigned stamped = 0;
   for (unsigned i = 0; i < NB; i++)
      stamped += bos[i].last_use_serial == 1 && bos[i].last_use_value == 5;
   check(stamped == NB && !stale, "%u of %u BOs added, repeated and colliding are stamped", stamped, NB);

   /* A reset empties the set and the cache: only the BOs added after it are named. The first eight were in the
    * cache before the reset; a cache that survived it would skip their insert. */
   ws->base.cs_reset(cs);
   for (unsigned i = 0; i < 8; i++)
      ws->base.cs_add_buffer(cs, &bos[i].base);
   ws->base.cs_add_buffer(cs, &bos[NB - 1].base);
   radv_wddm2_witness_cs(cs, 1, 6, &stale, &first);
   unsigned renamed = 0, kept = 0;
   for (unsigned i = 0; i < NB; i++) {
      if (i < 8 || i == NB - 1)
         renamed += bos[i].last_use_value == 6;
      else
         kept += bos[i].last_use_value == 5;
   }
   check(renamed == 9 && kept == NB - 9, "after a reset: the %u BOs added again are stamped, %u others keep "
                                         "their value (expected 9 and %u)", renamed, kept, NB - 9);

   /* The cost of a repeated add: eight BOs bound in turn, as a draw loop rebinds its vertex and index buffers. */
   LARGE_INTEGER frequency, t0, t1;
   QueryPerformanceFrequency(&frequency);
   enum { N = 4000000 };
   QueryPerformanceCounter(&t0);
   for (unsigned i = 0; i < N; i++)
      ws->base.cs_add_buffer(cs, &bos[i & 7].base);
   QueryPerformanceCounter(&t1);
   const double hit_ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)frequency.QuadPart / N;
   /* What each of those adds was before the cache: the set insert of a BO the set holds, in a set of NB BOs. */
   struct set *reference = _mesa_pointer_set_create(NULL);
   for (unsigned i = 0; i < NB; i++)
      _mesa_set_add(reference, &bos[i]);
   QueryPerformanceCounter(&t0);
   for (unsigned i = 0; i < N; i++)
      _mesa_set_add(reference, &bos[i & 7]);
   QueryPerformanceCounter(&t1);
   const double set_ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)frequency.QuadPart / N;
   _mesa_set_destroy(reference, NULL);
   printf("INFO cs_add_buffer of a BO already in the stream: %.2f ns per add; the set insert alone: %.2f ns\n",
          hit_ns, set_ns);
   ws->base.cs_destroy(cs);
   free(bos);
   contract();
}

/* BC250_DRAW_STATS: off by default (no draw_stats_add, no line); with =1, every command buffer's counters add
 * up and the summary writes them as its draw line, by name in RADV_DRAW_STATS order, only when they changed,
 * and again at teardown. */
static void
test_draw_stats(void)
{
   clear_ib_env();
   clear_deferred_env();
   _putenv_s("BC250_DRAW_STATS", "");
   long lm = log_mark();
   struct radv_wddm2_winsys *ws = make_ws();
   check(!ws->base.draw_stats_add && !log_lines(lm, "draw stats on", NULL, 0), "unset: no counting, no line");
   _putenv_s("BC250_DRAW_STATS", "2");
   ws = make_ws();
   check(!ws->base.draw_stats_add, "only 1 turns it on");

   _putenv_s("BC250_DRAW_STATS", "1");
   lm = log_mark();
   ws = make_ws();
   _putenv_s("BC250_DRAW_STATS", "");
   check(ws->base.draw_stats_add && log_lines(lm, "draw stats on (env)", NULL, 0) == 1, "=1: counting, one line");

   /* Two command buffers: counts i + 1, then 1 each, except the last counter. */
   uint32_t counts[RADV_DRAW_STAT_COUNT];
   for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT; i++)
      counts[i] = i + 1;
   ws->base.draw_stats_add(&ws->base, counts);
   for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT; i++)
      counts[i] = i + 1 < RADV_DRAW_STAT_COUNT;
   ws->base.draw_stats_add(&ws->base, counts);

   ws->summary.next_ns = 1;
   lm = log_mark();
   radv_wddm2_summary_tick(ws);
   static const char *const names[] = {
#define TEST_DRAW_STAT_NAME(name) #name,
      RADV_DRAW_STATS(TEST_DRAW_STAT_NAME)
#undef TEST_DRAW_STAT_NAME
   };
   char line[4096], expected[2048];
   size_t len = 0;
   for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT; i++)
      len += snprintf(expected + len, sizeof(expected) - len, " %s=%u", names[i], i + 1 + (i + 1 < RADV_DRAW_STAT_COUNT));
   check(log_lines(lm, "periodic #1 t=0s draw:", line, sizeof(line)) == 1 && has(line, expected) &&
            has(line, "draw: cmdbufs=2 passes=3 "),
         "the draw line names every counter with its sum: %.300s", line);
   check(!log_lines(lm, "periodic #1 t=0s deferred:", NULL, 0) && !log_lines(lm, "periodic #1 t=0s submit:", NULL, 0),
         "nothing else changed: the draw line alone");

   ws->summary.next_ns = 1;
   lm = log_mark();
   radv_wddm2_summary_tick(ws);
   check(!log_lines(lm, "periodic", NULL, 0), "unchanged: no line");

   lm = log_mark();
   radv_wddm2_deferred_finish(ws);
   check(log_lines(lm, "periodic final t=0s draw: cmdbufs=2 passes=3 ", NULL, 0) == 1, "teardown writes it again, final");
   contract();
}

/* BD-045: a CPU lock belongs to whoever releases the allocation. The fake host keeps one lock per allocation
 * (a second Lock2 or an Unlock2 without one is unexpected) and refuses to destroy a locked allocation, as the
 * D3D12 shell's HostedDispatch does. */
static void
test_lock_ownership(void)
{
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();

   /* Map, unmap, map again: Lock2, Unlock2, Lock2; destroy unlocks before anything else. */
   struct radeon_winsys_bo *bo = gtt_bo(ws);
   const uint32_t bo_h = bo->handle;
   unsigned mark = h.n_ev;
   void *ptr = ws->base.buffer_map(&ws->base, bo, false, NULL);
   ws->base.buffer_unmap(&ws->base, bo, false);
   void *again = ws->base.buffer_map(&ws->base, bo, false, NULL);
   check(ptr && again && h.n_ev == mark + 3 && event_is((int)mark, BC250_HOST_Lock2, 0, 0, bo_h) &&
            event_is((int)mark + 1, BC250_HOST_Unlock2, 0, 0, bo_h) &&
            event_is((int)mark + 2, BC250_HOST_Lock2, 0, 0, bo_h),
         "map, unmap, map: Lock2, Unlock2, Lock2 (calls: %u)", h.n_ev - mark);
   mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   const int unlock = find_op(mark, BC250_HOST_Unlock2, 0);
   check(event_is(unlock, BC250_HOST_Unlock2, 0, 0, bo_h) && unlock < find_destroy(mark, bo_h) && !alloc_live(bo_h),
         "destroy unlocks first, then destroys the allocation");

   /* A failed Lock2 maps nothing, and nothing unlocks it later. */
   bo = gtt_bo(ws);
   const uint32_t nolock_h = bo->handle;
   h.fail_op = BC250_HOST_Lock2;
   h.fail_nth = 1;
   h.fail_seen = h.fail_more = 0;
   check(ws->base.buffer_map(&ws->base, bo, false, NULL) == NULL && h.fail_seen == 1, "a failed Lock2 maps nothing");
   h.fail_op = 0;
   mark = h.n_ev;
   ws->base.buffer_unmap(&ws->base, bo, false);
   ws->base.buffer_destroy(&ws->base, bo);
   check(!count_op(mark, BC250_HOST_Unlock2) && !alloc_live(nolock_h),
         "neither unmap nor destroy unlocks it (%u Unlock2), and the allocation goes", count_op(mark, BC250_HOST_Unlock2));

   /* A failed Unlock2 at unmap keeps the lock and the pointer: the next map makes no host call, and destroy
    * unlocks it. */
   bo = gtt_bo(ws);
   const uint32_t kept_h = bo->handle;
   ptr = ws->base.buffer_map(&ws->base, bo, false, NULL);
   h.fail_op = BC250_HOST_Unlock2;
   h.fail_nth = 1;
   h.fail_seen = h.fail_more = 0;
   ws->base.buffer_unmap(&ws->base, bo, false);
   h.fail_op = 0;
   check(h.fail_seen == 1 && find(h.allocs, h.n_allocs, kept_h)->locked, "the unmap's Unlock2 failed; the host holds the lock");
   mark = h.n_ev;
   check(ws->base.buffer_map(&ws->base, bo, false, NULL) == ptr && h.n_ev == mark,
         "the next map returns the same pointer, no host call");
   ws->base.buffer_destroy(&ws->base, bo);
   check(count_op(mark, BC250_HOST_Unlock2) == 1 && !alloc_live(kept_h), "destroy unlocks it once, then the allocation goes");
   contract();
}

/* BD-045: an owned BO whose Unlock2 fails at destroy keeps its lock, VA, allocation and byte charge, and the
 * drain points retry it, at most once per interval; teardown tries once more and leaves the rest to the host. */
static void
test_lock_kept(void)
{
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   const uint64_t gtt = ws->allocated_gtt;
   struct radeon_winsys_bo *bo = gtt_bo(ws);
   const uint32_t bo_h = bo->handle;
   const uint64_t charged = ws->allocated_gtt;
   check(charged > gtt && ws->base.buffer_map(&ws->base, bo, false, NULL) != NULL, "charged and mapped");

   long log = log_mark();
   h.fail_op = BC250_HOST_Unlock2;
   h.fail_nth = 1;
   h.fail_seen = 0;
   h.fail_more = 1000;
   unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(count_op(mark, BC250_HOST_Unlock2) == 1 && !count_op(mark, BC250_HOST_Evict) &&
            !count_op(mark, BC250_HOST_FreeGpuVirtualAddress) && !count_op(mark, BC250_HOST_DestroyAllocation2),
         "a failed Unlock2 at destroy: no evict, VA free or allocation destroy follows");
   check(alloc_live(bo_h) && find(h.allocs, h.n_allocs, bo_h)->locked && ws->allocated_gtt == charged &&
            ws->deferred.locked_count == 1,
         "the BO is kept: allocation live and locked, still charged, one kept (%u)", ws->deferred.locked_count);
   check(log_lines(log, "kept: its allocation stays CPU-locked", NULL, 0) == 1, "one log line says so");

   /* Within the interval a drain makes no host call. */
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(h.n_ev == mark, "a drain within the retry interval makes no host call");

   /* Past it, the drain retries; the fault holds, so it is kept again, with no second log line. */
   ws->deferred.locked_interval_ns = 0;
   ws->deferred.locked_next_ns = 0;
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(count_op(mark, BC250_HOST_Unlock2) == 1 && !count_op(mark, BC250_HOST_DestroyAllocation2) &&
            alloc_live(bo_h) && ws->deferred.locked_count == 1,
         "the drain retries the Unlock2 once; it fails, the BO stays kept");
   check(log_lines(log, "kept: its allocation stays CPU-locked", NULL, 0) == 1, "still one log line");

   /* Once the host unlocks, the next drain releases everything. */
   h.fail_op = 0;
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   const int unlock = find_op(mark, BC250_HOST_Unlock2, 0);
   check(unlock >= 0 && count_op(mark, BC250_HOST_FreeGpuVirtualAddress) == 1 && unlock < find_destroy(mark, bo_h) &&
            !alloc_live(bo_h) && ws->allocated_gtt == gtt && ws->deferred.locked_count == 0,
         "the next drain unlocks, frees the VA and destroys the allocation; the byte charge goes");

   /* At teardown the fault still holds: one more try, then the BO is left to the host. */
   bo = gtt_bo(ws);
   const uint32_t left_h = bo->handle;
   ws->base.buffer_map(&ws->base, bo, false, NULL);
   h.fail_op = BC250_HOST_Unlock2;
   h.fail_nth = 1;
   h.fail_seen = 0;
   ws->base.buffer_destroy(&ws->base, bo);
   ws->deferred.locked_interval_ns = 1000000000000ull;
   log = log_mark();
   mark = h.n_ev;
   radv_wddm2_deferred_finish(ws);
   check(count_op(mark, BC250_HOST_Unlock2) == 1 && !count_op(mark, BC250_HOST_DestroyAllocation2) && alloc_live(left_h) &&
            ws->deferred.locked_count == 0,
         "teardown tries the unlock once more, whatever the interval, and destroys nothing");
   check(log_lines(log, "still CPU-locked at teardown", NULL, 0) == 1, "and says what it left to the host");
   h.fail_op = 0;
   find(h.allocs, h.n_allocs, left_h)->locked = false; /* the host releases it with the device */
   contract();
}

/* BD-045: a host import whose Unlock2 fails at destroy: the lock passes to the host with its allocation. */
static void
test_lock_borrowed(void)
{
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   struct obj *mappable = host_alloc();
   struct radeon_winsys_bo *bo = NULL;
   check(ws->base.buffer_from_hosted(&ws->base, ws->host.identity, mappable->handle, BC250_HOST_IMPORT_CPU_MAP,
                                     0x800000000ull, 65536, &bo) == VK_SUCCESS && bo,
         "a mappable import");
   check(ws->base.buffer_map(&ws->base, bo, false, NULL) == mappable->mem, "mapped through the host's Lock2");
   h.fail_op = BC250_HOST_Unlock2;
   h.fail_nth = 1;
   h.fail_seen = 0;
   h.fail_more = 1000;
   const unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(h.n_ev == mark + 1 && h.ev[mark].op == BC250_HOST_Unlock2,
         "destroy makes exactly one Unlock2 (calls: %u)", h.n_ev - mark);
   check(mappable->live && mappable->locked && ws->deferred.locked_count == 0,
         "it fails: the allocation and its lock stay the host's, the ICD keeps nothing");
   h.fail_op = 0;
   mappable->locked = false; /* the host's to release */
   contract();
}

/* BD-039: what vkAllocateMemory accepts around a host import (radv_host_import.h, the parser radv_alloc_memory
 * calls). The chains of the three real callers pass; every other import, an export with a handle type, a
 * capture address other than the VA, a second import block or a short import is refused. BD-038: in hosted
 * mode a Win32 memory import is refused with or without a host import. */
static VkResult
parse_chain(const void *chain, uint64_t size, bool hosted, struct radv_host_import_request *req)
{
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = chain,
      .allocationSize = size,
      .memoryTypeIndex = 0,
   };
   req->import = (const void *)(uintptr_t)1; /* the parser must write every field */
   req->flags = 0xdead;
   req->refused = "unset";
   return radv_host_import_parse(&info, hosted, req);
}

static void
test_host_import_chain(void)
{
   struct radv_host_import_request req;
   const uint64_t va = 0x800000000ull, size = 65536;

   /* D3D12 shell, heap-import.cpp: VkMemoryAllocateFlagsInfo (DEVICE_ADDRESS) -> flags-sType import with CPU_MAP. */
   struct bc250_host_import d3d12 = {.sType = BC250_HOST_IMPORT_FLAGS_STYPE, .identity = (void *)0x10,
                                     .allocation = 7, .flags = BC250_HOST_IMPORT_CPU_MAP, .va = va, .size = size};
   VkMemoryAllocateFlagsInfo address = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &d3d12,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
   check(parse_chain(&address, size, true, &req) == VK_SUCCESS && req.import == &d3d12 &&
            req.flags == BC250_HOST_IMPORT_CPU_MAP && !req.refused,
         "D3D12 shell heap import (address flags, CPU_MAP) accepted with its flags");

   /* DXVK shell, runtime-image-memory.cpp: plain-sType import -> dedicated image; the flags bytes are not read. */
   VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                              .image = (VkImage)(uintptr_t)0x20};
   struct bc250_host_import dxvk = {.sType = BC250_HOST_IMPORT_STYPE, .pNext = &dedicated,
                                    .identity = (void *)0x10, .allocation = 8, .flags = 0xffffffffu,
                                    .va = va, .size = size};
   check(parse_chain(&dxvk, size, true, &req) == VK_SUCCESS && req.import == &dxvk && req.flags == 0,
         "DXVK shell image import (plain sType, dedicated) accepted, flags 0");

   /* zink hosted (E34 runtime-import): import -> export with no handle type -> dedicated -> priority. */
   VkMemoryPriorityAllocateInfoEXT priority = {.sType = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,
                                               .priority = 0.5f};
   VkMemoryDedicatedAllocateInfo zink_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                   .pNext = &priority, .image = (VkImage)(uintptr_t)0x30};
   VkExportMemoryAllocateInfo no_export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           .pNext = &zink_dedicated, .handleTypes = 0};
   struct bc250_host_import zink = {.sType = BC250_HOST_IMPORT_STYPE, .pNext = &no_export,
                                    .identity = (void *)0x10, .allocation = 9, .va = va, .size = size};
   check(parse_chain(&zink, size, true, &req) == VK_SUCCESS && req.import == &zink,
         "zink hosted import (export of no handle type, dedicated, priority) accepted");

   /* An allocation larger than the import, and the same import smaller than asked. */
   check(parse_chain(&address, size / 2, true, &req) == VK_SUCCESS && req.import == &d3d12,
         "an import larger than allocationSize accepted");
   check(parse_chain(&address, size * 2, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE && !req.import &&
            req.refused,
         "an import smaller than allocationSize refused");

   /* Without a host: refused (the instance carries none). */
   check(parse_chain(&address, size, false, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE && !req.import,
         "a host import on a device without a host refused");

   /* Two import blocks, of either sType. */
   struct bc250_host_import second = d3d12;
   second.sType = BC250_HOST_IMPORT_STYPE;
   second.pNext = NULL;
   struct bc250_host_import first = d3d12;
   first.pNext = &second;
   check(parse_chain(&first, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "both import sTypes refused");
   second.sType = BC250_HOST_IMPORT_FLAGS_STYPE;
   check(parse_chain(&first, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "two flags-sType imports refused");

   /* Any other import with a handle type. */
   struct bc250_host_import tail = d3d12;
   tail.pNext = NULL;
#ifdef VK_USE_PLATFORM_WIN32_KHR
   VkImportMemoryWin32HandleInfoKHR win32 = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                             .pNext = &tail,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
                                             .handle = (HANDLE)(uintptr_t)0x44};
   check(parse_chain(&win32, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with a Win32 import refused");
   win32.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
   check(parse_chain(&win32, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with a D3D12 resource import refused");
   win32.handleType = 0;
   check(parse_chain(&win32, size, true, &req) == VK_SUCCESS && req.import == &tail,
         "a Win32 import block of no handle type imports nothing: accepted");
#else
   check(false, "the test is built without VK_USE_PLATFORM_WIN32_KHR");
#endif
   VkImportMemoryFdInfoKHR fd = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &tail,
                                 .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = 3};
   check(parse_chain(&fd, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "host import with an fd import refused");
   static char page[4096];
   VkImportMemoryHostPointerInfoEXT pointer = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                               .pNext = &tail,
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               .pHostPointer = page};
   check(parse_chain(&pointer, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with a host pointer import refused");
   VkBaseInStructure ahb = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                            .pNext = (const void *)&tail};
   check(parse_chain(&ahb, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with an Android hardware buffer import refused");

   /* An export with a handle type. */
   VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &tail,
                                        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
   check(parse_chain(&export, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with an export refused");

   /* An opaque capture address: only the import's own VA. */
   VkMemoryOpaqueCaptureAddressAllocateInfo capture = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO, .pNext = &tail,
      .opaqueCaptureAddress = va + 65536};
   check(parse_chain(&capture, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "host import with a capture address other than its VA refused");
   capture.opaqueCaptureAddress = va;
   check(parse_chain(&capture, size, true, &req) == VK_SUCCESS && req.import == &tail,
         "host import with its own VA as capture address accepted");
   capture.opaqueCaptureAddress = 0;
   check(parse_chain(&capture, size, true, &req) == VK_SUCCESS && req.import == &tail,
         "host import with capture address 0 accepted");

   /* No host import at all. */
   check(parse_chain(NULL, size, true, &req) == VK_SUCCESS && !req.import && !req.refused,
         "a plain allocation: no import");
   check(parse_chain(&export, size, false, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
         "the export chain still names a host import: refused without a host");
#ifdef VK_USE_PLATFORM_WIN32_KHR
   VkImportMemoryWin32HandleInfoKHR plain_win32 = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
                                                   .handle = (HANDLE)(uintptr_t)0x44};
   check(parse_chain(&plain_win32, size, true, &req) == VK_ERROR_INVALID_EXTERNAL_HANDLE && req.refused,
         "hosted: a Win32 import without a host import refused (BD-038)");
   check(parse_chain(&plain_win32, size, false, &req) == VK_SUCCESS && !req.import,
         "not hosted: a Win32 import is left to radv_alloc_memory as before");
   VkExportMemoryAllocateInfo plain_export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                              .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
   check(parse_chain(&plain_export, size, true, &req) == VK_SUCCESS && !req.import,
         "hosted: an export without a host import is not the parser's to refuse");
#endif
}

/* BD-038: a hosted winsys refuses an NT-handle memory import before any host call: the host has no
 * QueryResourceInfoFromNtHandle or OpenResourceFromNtHandle. */
static void
test_hosted_nt_import(void)
{
   clear_deferred_env();
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_bo *bo = (struct radeon_winsys_bo *)(uintptr_t)1;
   uint64_t size = 77;
   const unsigned mark = h.n_ev;
   check(ws->base.buffer_from_handle(&ws->base, (void *)(uintptr_t)0x44, 0, &bo, &size) ==
               VK_ERROR_INVALID_EXTERNAL_HANDLE &&
            bo == NULL && h.n_ev == mark,
         "hosted NT-handle import refused, no BO, no host call (calls: %u)", h.n_ev - mark);
   contract();
}

static const struct {
   const char *name;
   void (*run)(void);
} tests[] = {
   {"bind_failure", test_bind_failure},
   {"second_queue", test_second_queue},
   {"signal_wait", test_signal_wait},
   {"gather_teardown", test_gather_teardown},
   {"device_loss", test_device_loss},
   {"abandon", test_abandon},
   {"outside_scope", test_outside_scope},
   {"internal_queue", test_internal_queue},
   {"sparse", test_sparse},
   {"rebind", test_rebind},
   {"review_wait_failure", test_review_wait_failure},
   {"review_destroy_failure", test_review_destroy_failure},
   {"hosted_map", test_hosted_map},
   {"lock_ownership", test_lock_ownership},
   {"lock_kept", test_lock_kept},
   {"lock_borrowed", test_lock_borrowed},
   {"host_import_chain", test_host_import_chain},
   {"hosted_nt_import", test_hosted_nt_import},
   {"deferred_destroy", test_deferred_destroy},
   {"ib2_calls", test_ib2_calls},
   {"ib2_fallback", test_ib2_fallback},
   {"deferred_cap", test_deferred_cap},
   {"deferred_oom", test_deferred_oom},
   {"deferred_teardown", test_deferred_teardown},
   {"deferred_witness", test_deferred_witness},
   {"deferred_policy", test_deferred_policy},
   {"deferred_cost", test_deferred_cost},
   {"submit_coalesce", test_submit_coalesce},
   {"coalesce_hold", test_coalesce_hold},
   {"gather_slots", test_gather_slots},
   {"summary", test_summary},
   {"submit_cost", test_submit_cost},
   {"progress_gpu", test_progress_gpu},
   {"progress_gpu_hold", test_progress_gpu_hold},
   {"progress_gpu_fallback", test_progress_gpu_fallback},
   {"progress_gpu_loss", test_progress_gpu_loss},
   {"progress_gpu_cost", test_progress_gpu_cost},
   {"cs_add_buffer", test_cs_add_buffer},
   {"draw_stats", test_draw_stats},
};

int
main(int argc, char **argv)
{
   unsigned run = 0;
   setvbuf(stdout, NULL, _IONBF, 0);
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      current = tests[i].name;
      tests[i].run();
      run++;
   }
   if (!run) {
      fprintf(stderr, "unknown test %s\n", argv[1]);
      return 2;
   }
   printf("tests=%u checks=%u failures=%u\n", run, checks, failures);
   return failures ? 1 : 0;
}
