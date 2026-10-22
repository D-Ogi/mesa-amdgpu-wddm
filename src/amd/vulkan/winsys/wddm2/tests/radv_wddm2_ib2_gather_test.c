/* SPDX-License-Identifier: MIT
 *
 * The IB1 that the system BC250 winsys makes of a submission (radv_wddm2_bc250_submit in
 * radv_wddm2_cs.c, BD-207), linked as compiled for vulkan_radeon.dll. A scripted host dispatch
 * stands in for the kernel: it hands out allocations with host memory behind them, keeps a copy of
 * the IB1 of every SubmitCommand, and plays the GPU by reaching at once every fence value it is
 * asked to signal or to wait for.
 *
 * What the tests are for: the KMD runs one IB per job, so a submission of several IBs is packed
 * into a gather BO of the queue. Before this change every IB was copied there. The CPU then read
 * write-combined command buffers on each submission, and a submission above the IB_SIZE field
 * (1048575 dwords) was refused with STATUS_INVALID_PARAMETER, which the application saw as
 * VK_ERROR_DEVICE_LOST (vkd3d-proton 3.0.1 in Cyberpunk 2077 sent 1101808 dwords in one
 * submission, 2026-10-22). Now each IB is called as an IB2 from the gather BO, and an IB whose
 * stream calls an IB2 itself is copied there, because the CP has no third IB level.
 *
 * The cases drive the real cs_create, cs_finalize, cs_execute_secondary, cs_execute_ib, cs_reset
 * and cs_submit of the winsys, on a context made by the real ctx_create. A walker reads each IB1
 * as the CP does: type-3 packets only, every IB2 target inside a live allocation, and the last
 * packet ending at the end of the IB1.
 *
 * Built outside meson by scratch/bd201-fix/tools/winsys-tests.py: compiled with the compile
 * command of radv_wddm2_bo.c and linked against the radv_wddm2_cs.c and radv_wddm2_bo.c objects
 * and the vulkan_util, amd_common, mesa_util and mesa_util_c11 libraries.
 *
 * The negative control is the source before the change (29d95fd7), with this file compiled with
 * IB2_GATHER_NEGATIVE_CONTROL defined: that source has no mark and no copy knob. There every IB is
 * copied, the copy accepts an IB below its BO, and the direct path checks no range. So two_calls,
 * caller_copied, nested_marks, above_ib_size, below_bo, direct_outside_bo, dgc_mark and
 * odd_call_padding must fail, and above_ib_size gets VK_ERROR_DEVICE_LOST. above_bo must pass there
 * too, because the copy refused an IB past the end of its BO before the change. In the lab flavour,
 * copy_fallback is the same control inside this build: BC250_IB_NOCOPY=0 copies every IB, and the
 * submission above IB_SIZE is refused again.
 *
 * Usage: radv_wddm2_ib2_gather_test [test], no argument runs all.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bc250.h"
#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "util/bc250_host_bootstrap.h"
#include "vk_wddm2_monitored_fence.h"
#include "sid.h"
/* The layout of the common command stream, for its IB list and its mark. The static helpers of
 * the header belong to the winsys and are not used here. */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "winsys/common/radv_winsys_cs.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

/* The source before the change marks no stream and copies every IB. */
#ifdef IB2_GATHER_NEGATIVE_CONTROL
#define CALLS_IB2(cs)   false
#define GATHER_COPY(ws) true
#define IB_CAP(ws)      0ul
#else
#define CALLS_IB2(cs)   (radv_winsys_cs(cs)->calls_ib2)
#define GATHER_COPY(ws) ((ws)->bc250_gather_copy)
#define IB_CAP(ws)      ((ws)->bc250_ib_dwords_cap)
#endif

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

/* Every test here is hosted: nothing may reach the kernel dispatch table. */
struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   fprintf(stderr, "kernel dispatch table requested: test setup error\n");
   abort();
}

/* Only the submission path of other WDDM drivers dumps its private data. */
void
print_hex_data(FILE *fp, const void *data, uint32_t size)
{
   (void)fp;
   (void)data;
   (void)size;
   fprintf(stderr, "non-BC250 submission path reached: test setup error\n");
   abort();
}

/* The winsys names the monitored-fence sync type; nothing here uses it. */
const struct vk_sync_type vk_wddm2_monitored_fence_type = {0};

#define DEVICE_H     0x20u
#define PAGING_H     0x30u
#define PAGING_FENCE 0x34u
#define CONTEXT_H    0x50u
#define SYNC_BASE    0x60u
#define ALLOC_BASE   0x100u
#define MAX_ALLOC    256u
#define MAX_SYNC     8u
#define IB1_COPY_DW  65536u

#define IB2_HEADER   0xC0023F00u /* PKT3(PKT3_INDIRECT_BUFFER, 2, 0) */
#define IB_SIZE_MAX  0xFFFFFu    /* G_3F3_IB_SIZE(UINT32_MAX): the IB_SIZE field, in dwords */

struct alloc {
   uint32_t handle;
   uint64_t size; /* bytes, from the BC2A blob */
   uint64_t va;   /* 0 until mapped */
   uint8_t *mem;
};

static struct {
   struct alloc allocs[MAX_ALLOC];
   unsigned n_allocs, next_alloc;
   uint64_t next_va;

   struct {
      uint32_t handle;
      bool live;
      uint64_t value; /* the CPU view of a monitored fence */
   } syncs[MAX_SYNC];
   unsigned n_syncs;

   unsigned contexts, submits, unknown, lost_reports;

   /* The last SubmitCommand, with a copy of its IB1 taken in the call. */
   uint64_t submit_va;
   uint32_t submit_bytes, submit_context;
   uint32_t blob_magic, blob_num_ibs, blob_bytes;
   uint64_t blob_va;
   uint32_t ib1[IB1_COPY_DW];
   uint32_t ib1_dw;
   bool ib1_in_alloc;
} h;

static unsigned checks, failures, skipped;
static const char *current = "";

static void
check(bool ok, const char *fmt, ...)
{
   va_list ap;
   checks++;
   if (!ok)
      failures++;
   printf("%s %-18s ", ok ? "PASS" : "FAIL", current);
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

/* A check that needs a lab-flavour behaviour (a BC250_* variable) is reported as skipped in the
 * release flavour, which reads none (BD-167). */
UNUSED static void
skip(const char *what)
{
   skipped++;
   printf("SKIP %-18s (release flavour reads no BC250_* variable) %s\n", current, what);
}

static struct alloc *
alloc_find(uint32_t handle)
{
   for (unsigned i = 0; i < h.n_allocs; i++)
      if (h.allocs[i].handle == handle)
         return &h.allocs[i];
   return NULL;
}

/* The live, mapped allocation that holds [va, va + bytes), or NULL. */
static struct alloc *
alloc_at(uint64_t va, uint64_t bytes)
{
   for (unsigned i = 0; i < h.n_allocs; i++) {
      struct alloc *a = &h.allocs[i];
      if (a->va && va >= a->va && bytes <= a->size && va - a->va <= a->size - bytes)
         return a;
   }
   return NULL;
}

static uint64_t *
sync_value(uint32_t handle)
{
   for (unsigned i = 0; i < h.n_syncs; i++)
      if (h.syncs[i].handle == handle && h.syncs[i].live)
         return &h.syncs[i].value;
   return NULL;
}

static int32_t
fake_dispatch(void *userdata, uint32_t op, void *arg)
{
   (void)userdata;
   switch (op) {
   case BC250_HOST_CHECK_STATUS:
      return 0;
   case BC250_HOST_REPORT_LOST:
      h.lost_reports++;
      return 0;
   case BC250_HOST_PUBLISH_PROGRESS:
      return 0;
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *a = arg;
      const struct bc250_alloc_blob *blob = a->pAllocationInfo2[0].pPrivateDriverData;
      if (h.n_allocs == MAX_ALLOC || a->pAllocationInfo2[0].PrivateDriverDataSize < sizeof(*blob) ||
          blob->magic != BC250_ALLOC_MAGIC || !blob->alloc_size)
         return (int32_t)0xC0000017; /* STATUS_NO_MEMORY */
      struct alloc *n = &h.allocs[h.n_allocs];
      n->mem = calloc(1, (size_t)blob->alloc_size);
      if (!n->mem)
         return (int32_t)0xC0000017;
      n->handle = ALLOC_BASE + ++h.next_alloc;
      n->size = blob->alloc_size;
      n->va = 0;
      h.n_allocs++;
      a->pAllocationInfo2[0].hAllocation = n->handle;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      struct alloc *a = alloc_find(m->hAllocation);
      if (!a)
         return (int32_t)0xC000000D; /* STATUS_INVALID_PARAMETER */
      m->VirtualAddress = m->BaseAddress ? m->BaseAddress : (h.next_va += 0x10000000ull);
      a->va = m->VirtualAddress;
      m->PagingFenceValue = 2;
      return STATUS_PENDING;
   }
   case BC250_HOST_MakeResident: {
      D3DDDI_MAKERESIDENT *r = arg;
      r->PagingFenceValue = 3;
      return STATUS_PENDING;
   }
   case BC250_HOST_Lock2: {
      D3DKMT_LOCK2 *l = arg;
      struct alloc *a = alloc_find(l->hAllocation);
      if (!a)
         return (int32_t)0xC000000D;
      l->pData = a->mem;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_Unlock2:
      return STATUS_SUCCESS;
   case BC250_HOST_Evict:
      return STATUS_SUCCESS;
   case BC250_HOST_FreeGpuVirtualAddress:
      return STATUS_SUCCESS;
   case BC250_HOST_DestroyAllocation2: {
      const D3DKMT_DESTROYALLOCATION2 *d = arg;
      for (unsigned k = 0; k < d->AllocationCount; k++) {
         struct alloc *a = alloc_find(d->phAllocationList[k]);
         if (!a)
            return (int32_t)0xC000000D;
         free(a->mem);
         *a = h.allocs[--h.n_allocs];
      }
      return STATUS_SUCCESS;
   }
   case BC250_HOST_CreateContextVirtual: {
      D3DKMT_CREATECONTEXTVIRTUAL *c = arg;
      c->hContext = CONTEXT_H + h.contexts++;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_DestroyContext:
      return STATUS_SUCCESS;
   case BC250_HOST_CreateSynchronizationObject2: {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *s = arg;
      if (h.n_syncs == MAX_SYNC)
         return (int32_t)0xC0000017;
      h.syncs[h.n_syncs].handle = SYNC_BASE + h.n_syncs;
      h.syncs[h.n_syncs].live = true;
      h.syncs[h.n_syncs].value = s->Info.MonitoredFence.InitialFenceValue;
      s->hSyncObject = h.syncs[h.n_syncs].handle;
      s->Info.MonitoredFence.FenceValueCPUVirtualAddress = &h.syncs[h.n_syncs].value;
      s->Info.MonitoredFence.FenceValueGPUVirtualAddress = 0x7f0000000ull + 8ull * h.n_syncs;
      h.n_syncs++;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *d = arg;
      for (unsigned i = 0; i < h.n_syncs; i++)
         if (h.syncs[i].handle == d->hSyncObject)
            h.syncs[i].live = false;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_SubmitCommand: {
      const D3DKMT_SUBMITCOMMAND *c = arg;
      const struct bc250_submit_blob *blob = c->pPrivateDriverData;
      h.submits++;
      h.submit_va = c->Commands;
      h.submit_bytes = c->CommandLength;
      h.submit_context = c->BroadcastContextCount ? c->BroadcastContext[0] : 0;
      h.blob_magic = blob && c->PrivateDriverDataSize >= 40 + sizeof(blob->ib[0]) ? blob->magic : 0;
      h.blob_num_ibs = h.blob_magic ? blob->num_ibs : 0;
      h.blob_va = h.blob_magic ? blob->ib[0].va_start : 0;
      h.blob_bytes = h.blob_magic ? blob->ib[0].ib_bytes : 0;
      const struct alloc *a = alloc_at(c->Commands, c->CommandLength);
      h.ib1_in_alloc = a != NULL;
      h.ib1_dw = 0;
      if (a && c->CommandLength / 4 <= IB1_COPY_DW) {
         h.ib1_dw = c->CommandLength / 4;
         memcpy(h.ib1, a->mem + (c->Commands - a->va), c->CommandLength);
      }
      return STATUS_SUCCESS;
   }
   case BC250_HOST_SignalSynchronizationObjectFromGpu2: {
      /* The GPU of the test: the work before the signal is done at once. */
      const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *s = arg;
      for (unsigned i = 0; i < s->ObjectCount; i++) {
         uint64_t *v = sync_value(s->ObjectHandleArray[i]);
         if (v && s->MonitoredFenceValueArray[i] > *v)
            *v = s->MonitoredFenceValueArray[i];
      }
      return STATUS_SUCCESS;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      for (unsigned i = 0; i < w->ObjectCount; i++) {
         uint64_t *v = sync_value(w->ObjectHandleArray[i]);
         if (v && w->FenceValueArray[i] > *v)
            *v = w->FenceValueArray[i];
      }
      if (w->hAsyncEvent)
         SetEvent(w->hAsyncEvent);
      return STATUS_SUCCESS;
   }
   case BC250_HOST_GetDeviceState: {
      D3DKMT_GETDEVICESTATE *g = arg;
      g->ExecutionState = D3DKMT_DEVICEEXECUTION_ACTIVE;
      return STATUS_SUCCESS;
   }
   default:
      h.unknown++;
      return (int32_t)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
   }
}

static struct radeon_info *
test_query_info(struct radeon_winsys *ws)
{
   return &radv_wddm2_winsys(ws)->gpu_info;
}

/* A winsys as the system ICD makes it on a BC-250 (caps blob, so ws->bc250), with a hosted
 * dispatch; nocopy is the BC250_IB_NOCOPY value of the case, NULL for unset. */
static struct radv_wddm2_winsys *
make_ws(const char *nocopy)
{
   for (unsigned i = 0; i < h.n_allocs; i++)
      free(h.allocs[i].mem);
   memset(&h, 0, sizeof(h));
   /* The case must not inherit a variable from the shell. Each case runs in its own process. */
   _putenv("BC250_IB_DWORDS=");
   if (nocopy) {
      char text[64];
      snprintf(text, sizeof(text), "BC250_IB_NOCOPY=%s", nocopy);
      _putenv(text);
   } else {
      _putenv("BC250_IB_NOCOPY=");
   }

   struct radv_wddm2_winsys *ws = calloc(1, sizeof(*ws));
   if (!ws)
      abort();
   ws->bc250 = true;
   ws->host.sType = BC250_HOST_STYPE;
   ws->host.version = BC250_HOST_VERSION;
   ws->host.size = sizeof(ws->host);
   ws->host.identity = &h;
   ws->host.userdata = &h;
   ws->host.dispatch = fake_dispatch;
   ws->device_h = DEVICE_H;
   ws->paging_queue_h = PAGING_H;
   ws->paging_fence_h = PAGING_FENCE;
   ws->gpu_info.gfx_level = GFX10;
   ws->gpu_info.pte_fragment_size = 0x200000;
   ws->gpu_info.gart_page_size = 4096;
   ws->gpu_info.ip[AMD_IP_GFX].ib_alignment = 256;
   ws->gpu_info.ip[AMD_IP_GFX].ib_pad_dw_mask = 0x7;
   ws->base.query_info = test_query_info;
   radv_wddm2_bo_init_functions(ws);
   radv_wddm2_cs_init_functions(ws);
   return ws;
}

static struct radeon_winsys_ctx *
make_ctx(struct radv_wddm2_winsys *ws)
{
   struct radeon_winsys_ctx *ctx = NULL;
   const VkResult r = ws->base.ctx_create(&ws->base, RADEON_CTX_PRIORITY_MEDIUM, &ctx);
   check(r == VK_SUCCESS && ctx, "ctx_create makes the gfx queue (%d)", r);
   return ctx;
}

static struct radv_wddm2_queue *
gfx(struct radeon_winsys_ctx *ctx)
{
   return &radv_wddm2_ctx(ctx)->per_ip[AMD_IP_GFX].queue;
}

/* A primary GFX stream of payload dwords: NOP packets of at most 16384 dwords, then the finalize
 * of the winsys, which pads it to the GFX unit and records its IBs. */
static struct ac_cmdbuf *
make_stream(struct radv_wddm2_winsys *ws, unsigned payload)
{
   struct ac_cmdbuf *cs = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   check(cs != NULL, "cs_create");
   if (!cs)
      return NULL;
   while (payload) {
      const unsigned n = MIN2(payload, 16384u);
      if (cs->cdw + n > cs->max_dw)
         ws->base.cs_grow(cs, n);
      cs->reserved_dw = MAX2(cs->reserved_dw, cs->cdw + n);
      if (n == 1) {
         cs->buf[cs->cdw++] = PKT3_NOP_PAD;
      } else {
         cs->buf[cs->cdw++] = PKT3(PKT3_NOP, n - 2, 0);
         memset(cs->buf + cs->cdw, 0, (n - 1) * 4);
         cs->cdw += n - 1;
      }
      payload -= n;
   }
   cs->reserved_dw = MAX2(cs->reserved_dw, cs->cdw);
   check(ws->base.cs_finalize(cs) == VK_SUCCESS, "cs_finalize");
   return cs;
}

static VkResult
submit(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx, unsigned count, struct ac_cmdbuf **cs)
{
   const struct radv_winsys_submit_info info = {
      .ip_type = AMD_IP_GFX,
      .cs_count = count,
      .cs_array = cs,
   };
   return ws->base.cs_submit(ctx, &info, 0, NULL, 0, NULL);
}

static uint64_t
stream_dwords(struct ac_cmdbuf *cs)
{
   uint64_t n = 0;
   for (unsigned i = 0; i < radv_winsys_cs(cs)->num_ib_buffers; i++)
      n += radv_winsys_cs(cs)->ib_buffers[i].cdw;
   return n;
}

static bool
is_ib2_call(const uint32_t *dw, const struct radv_winsys_ib *ib)
{
   return dw[0] == IB2_HEADER && dw[1] == (uint32_t)ib->va && dw[2] == (uint32_t)(ib->va >> 32) &&
          dw[3] == ib->cdw;
}

/* What the walker found in the last IB1. */
struct walk {
   bool ok;          /* type-3 packets only, the last one ending at the end of the IB1 */
   unsigned calls;   /* IB2 packets */
   bool targets_ok;  /* every IB2 target lies inside a live mapped allocation */
   uint64_t called_dw;
   unsigned other;   /* type-3 packets that are not NOP or INDIRECT_BUFFER */
};

static struct walk
walk_ib1(void)
{
   struct walk w = {.ok = h.ib1_dw > 0, .targets_ok = true};
   unsigned i = 0;
   while (w.ok && i < h.ib1_dw) {
      const uint32_t hdr = h.ib1[i];
      if ((hdr >> 30) != 3) {
         w.ok = false;
         break;
      }
      const unsigned op = (hdr >> 8) & 0xff;
      const unsigned count = (hdr >> 16) & 0x3fff;
      if (op == PKT3_NOP && count == 0x3fff) {
         i += 1; /* PKT3_NOP_PAD: the header alone */
         continue;
      }
      const unsigned len = count + 2;
      if (len > h.ib1_dw - i) {
         w.ok = false;
         break;
      }
      if (op == PKT3_INDIRECT_BUFFER) {
         const uint64_t va = h.ib1[i + 1] | ((uint64_t)h.ib1[i + 2] << 32);
         const uint32_t cdw = h.ib1[i + 3] & IB_SIZE_MAX;
         w.calls++;
         w.called_dw += cdw;
         if ((h.ib1[i + 3] & ~IB_SIZE_MAX) || !alloc_at(va, (uint64_t)cdw * 4))
            w.targets_ok = false; /* a CHAIN, VALID or VMID bit, or a target outside memory */
      } else if (op != PKT3_NOP) {
         w.other++;
      }
      i += len;
   }
   if (i != h.ib1_dw)
      w.ok = false;
   return w;
}

/* The last submission ran one IB1 of dwords dwords from a gather BO of the queue, and its blob
 * names that IB1. */
static bool
one_ib1(struct radv_wddm2_queue *q, uint32_t dwords)
{
   bool in_gather = false;
   for (unsigned i = 0; i < BC250_GATHER_SLOTS; i++)
      in_gather |= q->bc250_gather[i].bo && q->bc250_gather[i].bo->va == h.submit_va;
   return in_gather && h.submit_bytes == dwords * 4 && h.blob_magic == BC250_SUBMIT_MAGIC &&
          h.blob_num_ibs == 1 && h.blob_va == h.submit_va && h.blob_bytes == h.submit_bytes &&
          h.ib1_in_alloc && h.ib1_dw == dwords;
}

static void
finish(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx, unsigned n, struct ac_cmdbuf **cs)
{
   for (unsigned i = 0; i < n; i++)
      if (cs[i])
         ws->base.cs_destroy(cs[i]);
   if (ctx)
      ws->base.ctx_destroy(ctx);
   radv_wddm2_deferred_finish(ws);
   check(!h.unknown && !h.lost_reports, "no unexpected host call (%u) and no loss report (%u)", h.unknown,
         h.lost_reports);
   check(!h.n_allocs, "no allocation left (%u)", h.n_allocs);
   simple_mtx_destroy(&ws->deferred.lock);
   free(ws);
}

/* Two plain streams: two IB2 calls in submission order, nothing copied, and the IB1 is the
 * calls alone (8 dwords, one GFX padding unit). */
static void
test_two_calls(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   check(!GATHER_COPY(ws) && !IB_CAP(ws), "without BC250_IB_NOCOPY the IB2 calls are on");
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[2] = {make_stream(ws, 13), make_stream(ws, 20)};
   const struct radv_winsys_ib a = radv_winsys_cs(cs[0])->ib_buffers[0];
   const struct radv_winsys_ib b = radv_winsys_cs(cs[1])->ib_buffers[0];
   check(a.cdw == 16 && b.cdw == 24, "the streams are padded to 16 and 24 dwords (%u, %u)", a.cdw, b.cdw);

   check(submit(ws, ctx, 2, cs) == VK_SUCCESS && h.submits == 1, "the submission is accepted, one SubmitCommand");
   check(one_ib1(gfx(ctx), 8), "one IB1 of 8 dwords from a gather BO, and its blob names it (%u bytes)",
         h.submit_bytes);
   check(h.ib1_dw == 8 && is_ib2_call(h.ib1, &a) && is_ib2_call(h.ib1 + 4, &b),
         "an IB2 call of each stream, in submission order, at its own address and size");
   const struct walk w = walk_ib1();
   check(w.ok && w.calls == 2 && w.targets_ok && w.called_dw == 40 && !w.other,
         "the CP reads two IB2 calls of 40 dwords in all, into live memory, and nothing else");
   finish(ws, ctx, 2, cs);
}

/* A stream that calls an IB2 itself runs at IB1 level: its dwords are copied between the two
 * calls, with its own IB2 call of the secondary in them. Then the IB1 pads to the GFX unit. */
static void
test_caller_copied(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *child = ws->base.cs_create(&ws->base, AMD_IP_GFX, true);
   check(child != NULL, "a secondary stream");
   child->buf[child->cdw++] = PKT3_NOP_PAD;
   child->reserved_dw = child->cdw;
   ws->base.cs_finalize(child);
   struct ac_cmdbuf *caller = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   ws->base.cs_execute_secondary(caller, child, true);
   caller->reserved_dw = MAX2(caller->reserved_dw, caller->cdw);
   ws->base.cs_finalize(caller);
   check(CALLS_IB2(caller), "a primary that runs a secondary as an IB2 is marked");

   struct ac_cmdbuf *cs[3] = {make_stream(ws, 13), caller, make_stream(ws, 20)};
   const struct radv_winsys_ib a = radv_winsys_cs(cs[0])->ib_buffers[0];
   const struct radv_winsys_ib c = radv_winsys_cs(caller)->ib_buffers[0];
   const struct radv_winsys_ib b = radv_winsys_cs(cs[2])->ib_buffers[0];
   check(c.cdw == 8, "the caller is 8 dwords: its IB2 call and padding (%u)", c.cdw);

   check(submit(ws, ctx, 3, cs) == VK_SUCCESS, "the submission is accepted");
   const uint32_t *copy = (const uint32_t *)radv_buffer_map(&ws->base, c.bo) + (c.va - c.bo->va) / 4;
   check(one_ib1(gfx(ctx), 16) && is_ib2_call(h.ib1, &a) && !memcmp(h.ib1 + 4, copy, 8 * 4) &&
            is_ib2_call(h.ib1 + 12, &b),
         "IB1 of 16 dwords: a called, the caller copied, b called");
   const struct walk w = walk_ib1();
   const struct radv_winsys_ib s = radv_winsys_cs(child)->ib_buffers[0];
   check(w.ok && w.calls == 3 && w.targets_ok && w.called_dw == a.cdw + b.cdw + s.cdw,
         "the CP reads three IB2 calls: a, the copied call of the secondary, b");

   /* The caller twice: two copies back to back, 16 dwords, which need no padding. */
   check(submit(ws, ctx, 2, (struct ac_cmdbuf *[]){caller, caller}) == VK_SUCCESS &&
            one_ib1(gfx(ctx), 16) && !memcmp(h.ib1, copy, 32) && !memcmp(h.ib1 + 8, copy, 32),
         "two copies of the caller, back to back");
   struct ac_cmdbuf *all[4] = {cs[0], caller, cs[2], child};
   finish(ws, ctx, 4, all);
}

/* The marks on real streams: a secondary run as an IB2 marks its primary; a copied secondary
 * passes its own mark on to its primary, and an unmarked one does not; cs_execute_ib marks; a
 * reset clears the mark. */
static void
test_nested_marks(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *plain = ws->base.cs_create(&ws->base, AMD_IP_GFX, true);
   struct ac_cmdbuf *marked = ws->base.cs_create(&ws->base, AMD_IP_GFX, true);
   struct ac_cmdbuf *p_ib2 = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *p_copy = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *p_pass = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *p_exec = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *all[6] = {plain, marked, p_ib2, p_copy, p_pass, p_exec};
   bool made = true;
   for (unsigned i = 0; i < 6; i++)
      made &= all[i] != NULL;
   check(made, "six streams");
   if (!made) {
      finish(ws, ctx, 6, all);
      return;
   }
   plain->buf[plain->cdw++] = PKT3_NOP_PAD;
   plain->reserved_dw = plain->cdw;
   ws->base.cs_finalize(plain);
   /* marked: a secondary with an IB2 call of its own, as a DGC buffer gives it. */
   ws->base.cs_execute_ib(marked, NULL, radv_winsys_cs(plain)->ib_buffers[0].va,
                          radv_winsys_cs(plain)->ib_buffers[0].cdw, false);
   marked->reserved_dw = MAX2(marked->reserved_dw, marked->cdw);
   ws->base.cs_finalize(marked);
   check(CALLS_IB2(marked) && !CALLS_IB2(plain),
         "cs_execute_ib marks its stream, a plain stream is not marked");

   ws->base.cs_execute_secondary(p_ib2, plain, true);
   ws->base.cs_execute_secondary(p_copy, plain, false);
   ws->base.cs_execute_secondary(p_pass, marked, false);
   check(CALLS_IB2(p_ib2) && !CALLS_IB2(p_copy),
         "a secondary run as an IB2 marks its primary; copied, an unmarked secondary does not");
   check(CALLS_IB2(p_pass) && p_pass->cdw >= 4 && p_pass->buf[0] == IB2_HEADER,
         "a copied secondary passes its IB2 call, and its mark, on to its primary");
   ws->base.cs_execute_ib(p_exec, NULL, radv_winsys_cs(plain)->ib_buffers[0].va,
                          radv_winsys_cs(plain)->ib_buffers[0].cdw, false);
   check(CALLS_IB2(p_exec), "cs_execute_ib on a primary marks it");

   for (unsigned i = 2; i < 6; i++) {
      all[i]->reserved_dw = MAX2(all[i]->reserved_dw, all[i]->cdw);
      ws->base.cs_finalize(all[i]);
   }
   /* p_copy is called, p_pass copied (its IB2 call of plain now runs from the IB1). */
   check(submit(ws, ctx, 2, (struct ac_cmdbuf *[]){p_copy, p_pass}) == VK_SUCCESS, "the pair is accepted");
   const struct radv_winsys_ib pc = radv_winsys_cs(p_copy)->ib_buffers[0];
   const struct radv_winsys_ib pp = radv_winsys_cs(p_pass)->ib_buffers[0];
   const uint32_t *pass = (const uint32_t *)radv_buffer_map(&ws->base, pp.bo) + (pp.va - pp.bo->va) / 4;
   check(one_ib1(gfx(ctx), 4 + pp.cdw + (8 - (4 + pp.cdw) % 8) % 8) && is_ib2_call(h.ib1, &pc) &&
            !memcmp(h.ib1 + 4, pass, pp.cdw * 4),
         "the unmarked primary is called, the marked one copied (%u dwords IB1)", h.ib1_dw);
   const struct walk w = walk_ib1();
   check(w.ok && w.targets_ok && w.calls == 2, "the CP reads two IB2 calls, one level deep each");

   ws->base.cs_reset(p_pass);
   check(!CALLS_IB2(p_pass), "a reset clears the mark");
   finish(ws, ctx, 6, all);
}

/* A submission of more dwords than the IB_SIZE field holds: refused before (BD-207, the device
 * was lost), called now. Five streams of 230000 payload dwords, 1150000 dwords in all. Each stream
 * is four IBs, because radv_winsys_cs_grow doubles the IB size when a stream grows. */
static void
test_above_ib_size(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[5];
   uint64_t dwords = 0;
   unsigned ibs = 0;
   for (unsigned i = 0; i < 5; i++) {
      cs[i] = make_stream(ws, 230000);
      dwords += stream_dwords(cs[i]);
      ibs += radv_winsys_cs(cs[i])->num_ib_buffers;
   }
   check(dwords > IB_SIZE_MAX, "the streams hold %" PRIu64 " dwords in %u IBs, above the IB_SIZE field", dwords,
         ibs);
   check(submit(ws, ctx, 5, cs) == VK_SUCCESS && h.submits == 1,
         "the submission is accepted and reaches the kernel as one SubmitCommand");
   const uint32_t expect = 4u * ibs + (8u - (4u * ibs) % 8u) % 8u;
   check(one_ib1(gfx(ctx), expect), "its IB1 is %u dwords from a gather BO (%u bytes)", expect, h.submit_bytes);
   const struct walk w = walk_ib1();
   check(w.ok && w.calls == ibs && w.targets_ok && w.called_dw == dwords && !w.other,
         "the CP reads %u IB2 calls of %" PRIu64 " dwords, every one into live memory", w.calls, w.called_dw);

   /* The queue goes on: the next submission reuses the ring of gather slots. */
   for (unsigned i = 0; i < BC250_GATHER_SLOTS + 1; i++)
      submit(ws, ctx, 5, cs);
   check(h.submits == BC250_GATHER_SLOTS + 2 && one_ib1(gfx(ctx), expect),
         "and every later submission of the same streams too (%u)", h.submits);
   finish(ws, ctx, 5, cs);
}

/* One stream alone is the IB1 itself, as before: no gather BO, the stream's own address. */
static void
test_single_direct(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[1] = {make_stream(ws, 13)};
   const struct radv_winsys_ib a = radv_winsys_cs(cs[0])->ib_buffers[0];
   check(submit(ws, ctx, 1, cs) == VK_SUCCESS, "the submission is accepted");
   bool gather = false;
   for (unsigned i = 0; i < BC250_GATHER_SLOTS; i++)
      gather |= gfx(ctx)->bc250_gather[i].bo != NULL;
   check(h.submit_va == a.va && h.submit_bytes == a.cdw * 4 && h.blob_va == a.va && !gather,
         "the kernel gets the stream's own IB, and no gather BO is made");
   finish(ws, ctx, 1, cs);
}

/* The lab flavour's fallback: BC250_IB_NOCOPY=0 copies every IB, the old path. It refuses the
 * submission above IB_SIZE again, which is the negative control of above_ib_size in this build. */
static void
test_copy_fallback(void)
{
   struct radv_wddm2_winsys *ws = make_ws("0");
#if !defined(AMDGPU_WDDM_LAB_FLAVOUR)
   check(!GATHER_COPY(ws), "the release flavour ignores BC250_IB_NOCOPY");
   skip("copy fallback");
   finish(ws, NULL, 0, NULL);
#else
   check(GATHER_COPY(ws), "BC250_IB_NOCOPY=0 selects the copy of every IB");
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[5] = {make_stream(ws, 13), make_stream(ws, 20)};
   const struct radv_winsys_ib a = radv_winsys_cs(cs[0])->ib_buffers[0];
   const struct radv_winsys_ib b = radv_winsys_cs(cs[1])->ib_buffers[0];
   check(submit(ws, ctx, 2, cs) == VK_SUCCESS, "two plain streams are accepted");
   const uint32_t *pa = (const uint32_t *)radv_buffer_map(&ws->base, a.bo) + (a.va - a.bo->va) / 4;
   const uint32_t *pb = (const uint32_t *)radv_buffer_map(&ws->base, b.bo) + (b.va - b.bo->va) / 4;
   check(one_ib1(gfx(ctx), 40) && !memcmp(h.ib1, pa, 16 * 4) && !memcmp(h.ib1 + 16, pb, 24 * 4),
         "and copied, back to back (%u dwords)", h.ib1_dw);
   ws->base.cs_destroy(cs[0]);
   ws->base.cs_destroy(cs[1]);
   uint64_t dwords = 0;
   for (unsigned i = 0; i < 5; i++) {
      cs[i] = make_stream(ws, 230000);
      dwords += stream_dwords(cs[i]);
   }
   const unsigned before = h.submits;
   check(submit(ws, ctx, 5, cs) == VK_ERROR_DEVICE_LOST && h.submits == before,
         "a copy of %" PRIu64 " dwords is refused before the kernel, as before the change", dwords);
   finish(ws, ctx, 5, cs);
#endif
}

/* An IB that is not inside its BO is refused before the kernel, on the gather path and on the direct
 * path. count streams of 8 payload dwords; the first one's IB gets the address va_of(its BO). A
 * refusal also marks the queue as failed, so every case makes its own winsys and context. */
static void
refused_outside(unsigned count, uint64_t (*va_of)(const struct radeon_winsys_bo *bo), const char *what)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[2] = {make_stream(ws, 8), count > 1 ? make_stream(ws, 8) : NULL};
   struct radv_winsys_ib *ib = &radv_winsys_cs(cs[0])->ib_buffers[0];
   const uint64_t saved = ib->va;
   ib->va = va_of(ib->bo);
   const unsigned before = h.submits;
   const VkResult r = submit(ws, ctx, count, cs);
   check(r == VK_ERROR_DEVICE_LOST && h.submits == before, "%s: refused before the kernel (%d, %u submits)", what,
         r, h.submits - before);
   ib->va = saved;
   finish(ws, ctx, count, cs);
}

static uint64_t
below_base(const struct radeon_winsys_bo *bo)
{
   return bo->va - 4;
}

static uint64_t
at_end(const struct radeon_winsys_bo *bo)
{
   return bo->va + bo->size;
}

/* The copy read the start of the BO for an IB below its BO (review of the change). */
static void
test_below_bo(void)
{
   refused_outside(2, below_base, "a gathered IB that starts 4 bytes below its BO");
}

static void
test_above_bo(void)
{
   refused_outside(2, at_end, "a gathered IB of 8 dwords that starts at the end of its BO");
}

static void
test_direct_outside_bo(void)
{
   refused_outside(1, below_base, "a direct IB that starts 4 bytes below its BO");
   refused_outside(1, at_end, "a direct IB of 8 dwords that starts at the end of its BO");
}

/* A GFX stream that calls a DGC buffer runs it as an IB2, so the stream is marked and copied. */
static void
test_dgc_mark(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *child = make_stream(ws, 8);
   struct ac_cmdbuf *parent = ws->base.cs_create(&ws->base, AMD_IP_GFX, false);
   struct ac_cmdbuf *cs[2] = {child, parent};
   const struct radv_winsys_ib target = radv_winsys_cs(child)->ib_buffers[0];
   ws->base.cs_chain_dgc_ib(parent, target.va, target.cdw, 0, false);
   check(CALLS_IB2(parent), "a GFX stream that calls a DGC buffer is marked");
   parent->reserved_dw = MAX2(parent->reserved_dw, parent->cdw);
   check(ws->base.cs_finalize(parent) == VK_SUCCESS, "the DGC caller is finalized");
   check(submit(ws, ctx, 2, cs) == VK_SUCCESS, "the plain stream and the DGC caller are accepted");
   const struct walk w = walk_ib1();
   check(w.ok && w.targets_ok && w.calls == 2, "the caller is copied: two IB2 calls, one level deep each");
   finish(ws, ctx, 2, cs);
}

/* Three calls are 12 dwords: the IB1 ends on a NOP of four dwords, at the GFX padding unit. */
static void
test_odd_call_padding(void)
{
   struct radv_wddm2_winsys *ws = make_ws(NULL);
   struct radeon_winsys_ctx *ctx = make_ctx(ws);
   struct ac_cmdbuf *cs[3] = {make_stream(ws, 8), make_stream(ws, 8), make_stream(ws, 8)};
   check(submit(ws, ctx, 3, cs) == VK_SUCCESS, "three plain streams are accepted");
   check(one_ib1(gfx(ctx), 16), "three calls and four dwords of NOP padding (%u dwords)", h.ib1_dw);
   const struct walk w = walk_ib1();
   check(w.ok && w.targets_ok && w.calls == 3, "the padding ends on the end of the IB1");
   finish(ws, ctx, 3, cs);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"two_calls", test_two_calls},
   {"caller_copied", test_caller_copied},
   {"nested_marks", test_nested_marks},
   {"above_ib_size", test_above_ib_size},
   {"single_direct", test_single_direct},
   {"copy_fallback", test_copy_fallback},
   {"below_bo", test_below_bo},
   {"above_bo", test_above_bo},
   {"direct_outside_bo", test_direct_outside_bo},
   {"dgc_mark", test_dgc_mark},
   {"odd_call_padding", test_odd_call_padding},
};

int
main(int argc, char **argv)
{
   unsigned run = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      current = tests[i].name;
      tests[i].fn();
      run++;
   }
   if (argc > 1 && !run) {
      fprintf(stderr, "no test named %s\n", argv[1]);
      return 2;
   }
   printf("tests=%u checks=%u failures=%u skipped=%u\n", run, checks, failures, skipped);
   return failures ? 1 : 0;
}
