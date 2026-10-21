/* SPDX-License-Identifier: MIT
 *
 * Deferred destruction of the desktop BC250 winsys (radv_wddm2_bo.c, BD-201),
 * linked as compiled for vulkan_radeon.dll of the desktop RADV line. A scripted host dispatch stands in
 * for the kernel: it records every call in order, hands out allocation handles
 * and gpu addresses, and plays the GPU by completing the value a CPU wait asks
 * for, unless the test makes that wait fail.
 *
 * What the tests are for (R v2, DESIGN.md 6): an ordinary BO is released at its
 * destroy through DestroyAllocation2 with flags 0 alone, because the OS defers
 * that destruction until the commands queued before it finish and frees the
 * API-allocated range with the allocation (d3dukmdt.md:3398-3402,
 * d3dkmthk.md:13810). What the OS does not cover is a range this driver owns by
 * itself, a REPLAYABLE BO's address or a virtual BO's reservation: such a BO is
 * held until the progress values captured at its destroy are reached, because
 * after D3DKMTFreeGpuVirtualAddress a new range may take the freed address at
 * once, under work that still reads it. Nothing is ever released without proof:
 * a failed wait, a missing entry and teardown are not proof, and the answer to
 * a missing proof is to keep the range or, at teardown, to leak it and count
 * it. The hold needs one allocation, of the entry that records those values,
 * and radv_wddm2_deferred_entry_fail forces it to fail, as host memory
 * pressure would; the recorded call order is the proof of what the destroy did
 * instead.
 *
 * The cases drive the real ws->buffer_destroy of the winsys on an internal BO
 * made by the real ws->buffer_create. The queue is one attached tracker with a
 * published value, which is what radv_wddm2_queue_init attaches and what the
 * submit path publishes; no command stream is needed to reach the destroy.
 *
 * Built outside meson: compiled with the compile command of radv_wddm2_bo.c and
 * linked against the radv_wddm2_cs.c and radv_wddm2_bo.c objects and the
 * vulkan_util, amd_common, mesa_util and mesa_util_c11 libraries
 * (scratch/bd201-fix/tools/winsys-tests.py).
 *
 * The negative control is the source before the fix: there every destroy evicts
 * the BO and frees its range at once, and a destroy that gets no entry does so
 * under running work. plain_destroy, no_entry_leaks, held_until_retired and
 * teardown_leak must fail against it, and they must pass with the fix.
 *
 * Usage: radv_wddm2_deferred_destroy_test [test], no argument runs all.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "util/bc250_host_bootstrap.h"
#include "vk_wddm2_monitored_fence.h"

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
#define PROGRESS_H   0x40u
#define CONTEXT_H    0x50u
#define ALLOC_BASE   0x100u
#define BO_SIZE      65536u
#define MAX_EV       512u
#define MAX_ALLOC    32u

struct event {
   uint32_t op;
   uint32_t handle; /* the allocation or the fence the call names */
   uint64_t value;
};

static struct {
   struct event ev[MAX_EV];
   unsigned n_ev, dropped;

   uint32_t allocs[MAX_ALLOC]; /* the live allocation handles */
   unsigned n_allocs, next_alloc;
   uint64_t next_va;

   uint64_t fence_value; /* the progress fence of the one queue */
   unsigned unknown;

   uint32_t fail_op; /* this op fails while fail_calls is positive */
   unsigned fail_calls;
   int32_t fail_status;
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

/* The release flavour reads no BC250_* variable and writes no log (BD-167), so a
 * check of a log line is reported as skipped there. */
static void
check_log(bool ok, const char *fmt, ...)
{
   va_list ap;
#if !defined(AMDGPU_WDDM_LAB_FLAVOUR)
   skipped++;
   printf("SKIP %-18s (release flavour: no log) ", current);
#else
   checks++;
   if (!ok)
      failures++;
   printf("%s %-18s ", ok ? "PASS" : "FAIL", current);
#endif
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

static void
record(uint32_t op, uint32_t handle, uint64_t value)
{
   if (h.n_ev < MAX_EV)
      h.ev[h.n_ev++] = (struct event){op, handle, value};
   else
      h.dropped++;
}

static unsigned
count_op(unsigned mark, uint32_t op)
{
   unsigned n = 0;
   for (unsigned i = mark; i < h.n_ev; i++)
      if (h.ev[i].op == op)
         n++;
   return n;
}

/* The index of the first call of op at or after mark, or -1. */
static int
find_op(unsigned mark, uint32_t op)
{
   for (unsigned i = mark; i < h.n_ev; i++)
      if (h.ev[i].op == op)
         return (int)i;
   return -1;
}

static int
find_handle(unsigned mark, uint32_t op, uint32_t handle)
{
   for (unsigned i = mark; i < h.n_ev; i++)
      if (h.ev[i].op == op && h.ev[i].handle == handle)
         return (int)i;
   return -1;
}

static bool
cpu_wait_is(int index, uint32_t fence, uint64_t value)
{
   return index >= 0 && h.ev[index].op == BC250_HOST_WaitForSynchronizationObjectFromCpu &&
          h.ev[index].handle == fence && h.ev[index].value == value;
}

static bool
alloc_live(uint32_t handle)
{
   for (unsigned i = 0; i < h.n_allocs; i++)
      if (h.allocs[i] == handle)
         return true;
   return false;
}

static void
alloc_add(uint32_t handle)
{
   if (h.n_allocs < MAX_ALLOC)
      h.allocs[h.n_allocs++] = handle;
}

static void
alloc_del(uint32_t handle)
{
   for (unsigned i = 0; i < h.n_allocs; i++)
      if (h.allocs[i] == handle) {
         h.allocs[i] = h.allocs[--h.n_allocs];
         return;
      }
}

static bool
failing(uint32_t op)
{
   if (op != h.fail_op || !h.fail_calls)
      return false;
   h.fail_calls--;
   return true;
}

static void
fail_calls(uint32_t op, unsigned n)
{
   h.fail_op = op;
   h.fail_calls = n;
   h.fail_status = (int32_t)0xC0000001; /* STATUS_UNSUCCESSFUL */
}

static void
no_failures(void)
{
   check(!h.fail_calls, "every injected failure was used");
   h.fail_op = 0;
}

static int32_t
fake_dispatch(void *userdata, uint32_t op, void *arg)
{
   (void)userdata;
   if (op == BC250_HOST_CHECK_STATUS)
      return 0; /* not recorded: the fence helpers ask on every read */

   switch (op) {
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *a = arg;
      const uint32_t handle = ALLOC_BASE + ++h.next_alloc;
      record(op, handle, a->pAllocationInfo2 ? a->pAllocationInfo2[0].PrivateDriverDataSize : 0);
      if (failing(op))
         return h.fail_status;
      a->pAllocationInfo2[0].hAllocation = handle;
      alloc_add(handle);
      return STATUS_SUCCESS;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      record(op, m->hAllocation, m->BaseAddress);
      if (failing(op))
         return h.fail_status;
      m->VirtualAddress = m->BaseAddress ? m->BaseAddress : (h.next_va += 0x10000000ull);
      m->PagingFenceValue = 2;
      return STATUS_PENDING;
   }
   case BC250_HOST_MakeResident: {
      D3DDDI_MAKERESIDENT *r = arg;
      record(op, r->AllocationList ? r->AllocationList[0] : 0, r->NumAllocations);
      if (failing(op))
         return h.fail_status;
      r->PagingFenceValue = 3;
      return STATUS_PENDING;
   }
   case BC250_HOST_Evict: {
      D3DKMT_EVICT *e = arg;
      record(op, e->AllocationList ? e->AllocationList[0] : 0, e->NumAllocations);
      return failing(op) ? h.fail_status : STATUS_SUCCESS;
   }
   case BC250_HOST_FreeGpuVirtualAddress: {
      const D3DKMT_FREEGPUVIRTUALADDRESS *f = arg;
      record(op, 0, f->BaseAddress);
      return failing(op) ? h.fail_status : STATUS_SUCCESS;
   }
   case BC250_HOST_DestroyAllocation2: {
      const D3DKMT_DESTROYALLOCATION2 *d = arg;
      const uint32_t handle = d->phAllocationList ? d->phAllocationList[0] : 0;
      record(op, handle, d->AllocationCount);
      if (failing(op))
         return h.fail_status;
      alloc_del(handle);
      return STATUS_SUCCESS;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      const uint32_t fence = w->ObjectCount ? w->ObjectHandleArray[0] : 0;
      const uint64_t value = w->ObjectCount ? w->FenceValueArray[0] : 0;
      record(op, fence, value);
      if (failing(op))
         return h.fail_status;
      /* The GPU of the test: the work the caller waits for completes now. */
      if (fence == PROGRESS_H && value > h.fence_value)
         h.fence_value = value;
      if (w->hAsyncEvent)
         SetEvent(w->hAsyncEvent);
      return STATUS_SUCCESS;
   }
   case BC250_HOST_GetDeviceState: {
      D3DKMT_GETDEVICESTATE *g = arg;
      record(op, 0, 0);
      g->ExecutionState = D3DKMT_DEVICEEXECUTION_ACTIVE;
      return STATUS_SUCCESS;
   }
   default:
      record(op, 0, 0);
      h.unknown++;
      return (int32_t)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
   }
}

/* The log of the winsys, when the lab flavour was asked for one. */
static const char *
log_file(void)
{
   const char *path = getenv("BC250_DEFERRED_LOG");
   return path && *path ? path : NULL;
}

static char *
log_read(size_t *size)
{
   const char *path = log_file();
   *size = 0;
   if (!path)
      return NULL;
   HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return NULL;
   LARGE_INTEGER end;
   if (!GetFileSizeEx(f, &end) || end.QuadPart < 0 || end.QuadPart > (1 << 24)) {
      CloseHandle(f);
      return NULL;
   }
   char *text = calloc(1, (size_t)end.QuadPart + 1);
   DWORD got = 0;
   if (text && end.QuadPart)
      ReadFile(f, text, (DWORD)end.QuadPart, &got, NULL);
   CloseHandle(f);
   if (text)
      text[got] = 0;
   *size = got;
   return text;
}

/* The log's length now: the lines a case asks about are the ones written after it. */
static size_t
log_mark(void)
{
   size_t size = 0;
   free(log_read(&size));
   return size;
}

/* The lines after mark that hold needle; the first of them in out, when given. */
static unsigned
log_lines(size_t mark, const char *needle, char *out, size_t out_size)
{
   size_t size = 0;
   char *text = log_read(&size);
   unsigned found = 0;
   if (out && out_size)
      out[0] = 0;
   if (!text)
      return 0;
   char *line = text + (mark < size ? mark : size);
   while (*line) {
      char *next = strchr(line, '\n');
      if (next)
         *next = 0;
      if (strstr(line, needle)) {
         if (!found && out && out_size)
            snprintf(out, out_size, "%s", line);
         found++;
      }
      if (!next)
         break;
      line = next + 1;
   }
   free(text);
   return found;
}

static struct radv_wddm2_winsys *
make_ws(void)
{
   memset(&h, 0, sizeof(h));
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
   ws->gpu_info.pte_fragment_size = 0x200000;
   ws->gpu_info.gart_page_size = 4096;
   radv_wddm2_bo_init_functions(ws);
   radv_wddm2_cs_init_functions(ws);
   return ws;
}

/* The one queue of these cases: the tracker radv_wddm2_queue_init attaches, on
 * the progress fence the submit path publishes to. */
static struct radv_wddm2_tracker *
attach_queue(struct radv_wddm2_winsys *ws)
{
   struct radv_wddm2_tracker *tracker = radv_wddm2_tracker_attach(ws, PROGRESS_H, &h.fence_value, CONTEXT_H);
   check(tracker != NULL, "the queue's tracker is attached");
   return tracker;
}

#define PLAIN_FLAGS RADEON_FLAG_NO_INTERPROCESS_SHARING
/* A BO that owns its GPU address range (radv_wddm2_bo_owns_va): capture/replay promises the application
 * the same address again, so this driver frees the range itself and needs proof that it is idle. These
 * are the only BOs an R2 hold keeps. */
#define HELD_FLAGS (PLAIN_FLAGS | RADEON_FLAG_REPLAYABLE)

static struct radeon_winsys_bo *
make_bo_flags(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags)
{
   struct radeon_winsys_bo *bo = NULL;
   const VkResult result =
      ws->base.buffer_create(&ws->base, BO_SIZE, 4096, RADEON_DOMAIN_GTT, flags, 0, 0, NULL, &bo);
   check(result == VK_SUCCESS && bo && bo->handle, "an internal BO is made (%d)", result);
   return bo;
}

static struct radeon_winsys_bo *
make_bo(struct radv_wddm2_winsys *ws)
{
   return make_bo_flags(ws, PLAIN_FLAGS);
}

static void
finish(struct radv_wddm2_winsys *ws, struct radv_wddm2_tracker *tracker)
{
   radv_wddm2_tracker_detach(ws, tracker, true);
   radv_wddm2_deferred_finish(ws);
   check(!h.unknown && !h.dropped, "no unexpected host call and no dropped record");
   check(!h.n_allocs, "no allocation left (%u)", h.n_allocs);
   simple_mtx_destroy(&ws->deferred.lock);
   free(ws);
}

/* R1 (DESIGN.md 6.4): an ordinary BO is released at its destroy, through DestroyAllocation2 with flags
 * 0 alone. The OS defers that destruction until the commands queued before it finish and frees the
 * API-allocated range with the allocation, so there is nothing to record, nothing to wait for and
 * nothing to allocate: radv_wddm2_deferred_entry_fail is still armed after the destroy, which is the
 * proof that the destroy path asked for no memory at all. */
static void
test_plain_destroy(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;

   radv_wddm2_tracker_publish(tracker, 1);
   check(h.fence_value == 0 && tracker->published == 1, "the queue published 1, completed 0");

   const unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   ws->base.buffer_destroy(&ws->base, bo);
   check(radv_wddm2_deferred_entry_fail == 1, "an ordinary BO's destroy asks for no entry");
   radv_wddm2_deferred_entry_fail = 0;
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0,
         "the allocation goes to the OS, which defers its destruction");
   check(!count_op(mark, BC250_HOST_Evict) && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress) &&
            !count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu),
         "and nothing else: no Evict, no VA free, no CPU wait");
   check(!ws->deferred.count && !ws->deferred.held_va_bytes && !ws->deferred.leaked_ranges &&
            ws->deferred.immediate == 1,
         "nothing is held and no range is leaked");

   h.fence_value = 1;
   finish(ws, tracker);
}

/* R2: the BO owns its range, the published 1 has not retired, and the entry allocation fails. Nothing
 * can ever prove the range idle then, so it is given up unfreed and counted: a FreeGpuVirtualAddress
 * here would let the next allocation take the address under the running work (d3dkmthk.md:12632), and a
 * CPU wait in a destroy is what R v2 removes. The allocation itself still goes to the OS. */
static void
test_no_entry_leaks(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo_flags(ws, HELD_FLAGS);
   const uint32_t bo_h = bo->handle;
   char line[1024];

   radv_wddm2_tracker_publish(tracker, 1);
   const size_t lm = log_mark();
   const unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   ws->base.buffer_destroy(&ws->base, bo);
   check(radv_wddm2_deferred_entry_fail == 0, "the destroy of a BO with its own range made one entry "
                                              "allocation, and it failed");
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) &&
            !count_op(mark, BC250_HOST_Evict) && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress),
         "no CPU wait, no Evict and no VA free");
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0 &&
            !ws->deferred.count,
         "the allocation goes to the OS, which defers it; nothing is held");
   check(ws->deferred.leaked_ranges == 1 && ws->deferred.leaked_va_bytes == BO_SIZE,
         "the range is counted as leaked (%" PRIu64 " ranges, %" PRIu64 " bytes)", ws->deferred.leaked_ranges,
         ws->deferred.leaked_va_bytes);
   check_log(log_lines(lm, "no memory to hold a 65536-byte BO's own", line, sizeof(line)) == 1 &&
                strstr(line, "leaked, not freed"),
             "and logged: %.180s", line);

   h.fence_value = 1;
   finish(ws, tracker);
}

/* R2 with every published value reached: the same failed entry allocation needs no entry. The proof is
 * there without one, so the range is freed at once and nothing is leaked or waited for. */
static void
test_no_entry_retired(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo_flags(ws, HELD_FLAGS);
   const uint32_t bo_h = bo->handle;

   radv_wddm2_tracker_publish(tracker, 1);
   h.fence_value = 1;
   const unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   ws->base.buffer_destroy(&ws->base, bo);
   check(!radv_wddm2_deferred_entry_fail, "the entry allocation was made and failed");
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0 &&
            count_op(mark, BC250_HOST_FreeGpuVirtualAddress) == 1,
         "the range is freed and the allocation destroyed at once");
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) && !count_op(mark, BC250_HOST_Evict),
         "with nothing in flight, no CPU wait and no Evict");
   check(!ws->deferred.leaked_ranges && ws->deferred.immediate == 1,
         "counted as an immediate destroy, with no leaked range");

   finish(ws, tracker);
}

/* The ordinary R2 hold: with an entry the BO's range waits for the value captured at the destroy, and no
 * call of its destroy reaches the host until that value is reached. */
static void
test_held_until_retired(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo_flags(ws, HELD_FLAGS);
   const uint32_t bo_h = bo->handle;

   radv_wddm2_tracker_publish(tracker, 3);
   unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(alloc_live(bo_h) && ws->deferred.count == 1 && ws->deferred.held_va_bytes == BO_SIZE,
         "the BO is held with its range (%u BOs, %" PRIu64 " bytes of ranges)", ws->deferred.count,
         ws->deferred.held_va_bytes);
   check(!count_op(mark, BC250_HOST_Evict) && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress) &&
            !count_op(mark, BC250_HOST_DestroyAllocation2) &&
            !count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu),
         "and nothing of it reached the host: no Evict, no VA free, no destroy, no wait");

   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(bo_h) && ws->deferred.count == 1, "a drain with the 3 in flight holds it");

   h.fence_value = 3;
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   const int free_va = find_op(mark, BC250_HOST_FreeGpuVirtualAddress);
   const int destroy = find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h);
   check(!alloc_live(bo_h) && free_va >= 0 && destroy > free_va && !count_op(mark, BC250_HOST_Evict),
         "once the 3 retired the drain frees the range, then the allocation, and never evicts (%d %d)", free_va,
         destroy);
   check(!ws->deferred.count && !ws->deferred.held_va_bytes && ws->deferred.released == 1 &&
            !ws->deferred.leaked_ranges,
         "nothing is held or leaked afterwards");

   finish(ws, tracker);
}

/* R7: a DestroyAllocation2 the host refuses leaves the allocation with the ICD. It is counted, kept on a
 * list threaded through the BO's own link, and retried at the drain points: nothing is ever written down
 * as released while the ICD still holds it. */
static void
test_refused_kept(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;
   char line[1024];

   const size_t lm = log_mark();
   unsigned mark = h.n_ev;
   fail_calls(BC250_HOST_DestroyAllocation2, 1);
   ws->base.buffer_destroy(&ws->base, bo);
   no_failures();
   check(alloc_live(bo_h) && count_op(mark, BC250_HOST_DestroyAllocation2) == 1 &&
            ws->deferred.refused_count == 1 && ws->deferred.refused_bytes == BO_SIZE &&
            ws->deferred.refused_total == 1,
         "a refused destroy keeps the allocation, counted (%u kept)", ws->deferred.refused_count);
   check_log(log_lines(lm, "DestroyAllocation2 refused a 65536-byte allocation", line, sizeof(line)) == 1,
             "and logged: %.160s", line);

   /* The retry interval has not passed, so a drain leaves it alone. */
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(bo_h) && ws->deferred.refused_count == 1 && !count_op(mark, BC250_HOST_DestroyAllocation2),
         "a drain inside the retry interval leaves it kept");

   ws->deferred.refused_next_ns = 0;
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0 &&
            !ws->deferred.refused_count && !ws->deferred.refused_bytes && ws->deferred.refused_retries == 1,
         "the next drain retries it and the allocation goes");

   finish(ws, tracker);
}

/* R5: teardown is not proof either. A held range whose wait fails at teardown is given up unfreed and
 * counted, while its allocation goes to the OS: the alternative would be a FreeGpuVirtualAddress under
 * work that may still run. */
static void
test_teardown_leak(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo_flags(ws, HELD_FLAGS);
   const uint32_t bo_h = bo->handle;
   char line[1024];

   radv_wddm2_tracker_publish(tracker, 2);
   ws->base.buffer_destroy(&ws->base, bo);
   check(alloc_live(bo_h) && ws->deferred.count == 1, "the BO is held against the published 2");

   const size_t lm = log_mark();
   const unsigned mark = h.n_ev;
   fail_calls(BC250_HOST_WaitForSynchronizationObjectFromCpu, 1);
   radv_wddm2_deferred_finish(ws);
   no_failures();
   check(!count_op(mark, BC250_HOST_FreeGpuVirtualAddress), "the failed teardown wait frees no range");
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0,
         "the allocation is left to the OS, which defers its destruction");
   check(ws->deferred.leaked_ranges == 1 && ws->deferred.leaked_va_bytes == BO_SIZE && !ws->deferred.released,
         "the range is counted as leaked, and the BO is not counted as released");
   check_log(log_lines(lm, "leaked, not freed", line, sizeof(line)) == 1, "and logged: %.180s", line);

   h.fence_value = 2;
   radv_wddm2_tracker_detach(ws, tracker, true);
   check(!h.unknown && !h.dropped, "no unexpected host call and no dropped record");
   check(!h.n_allocs, "no allocation left (%u)", h.n_allocs);
   simple_mtx_destroy(&ws->deferred.lock);
   free(ws);
}

/* R3 (BD-212): a queue that goes while a value it published has not retired leaves its tracker listed
 * and referenced, so a destroy after it still records a wait for that work. A drain point reaps the
 * tracker once the value retires. Dropping it at the detach is the BD-212 breach: the destroy below
 * would then record no wait at all. */
static void
test_tracker_abandoned(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);

   radv_wddm2_tracker_publish(tracker, 1);
   radv_wddm2_tracker_detach(ws, tracker, true);
   check(p_atomic_read(&ws->deferred.abandoned) == 1 && !list_is_empty(&ws->deferred.trackers),
         "the queue went with its 1 unretired: the tracker stays listed");

   struct radeon_winsys_bo *bo = make_bo_flags(ws, HELD_FLAGS);
   const uint32_t bo_h = bo->handle;
   const unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(alloc_live(bo_h) && ws->deferred.count == 1 && !count_op(mark, BC250_HOST_FreeGpuVirtualAddress),
         "a destroy after it still waits for that queue's 1");

   h.fence_value = 1;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(bo_h) && !ws->deferred.count && ws->deferred.released == 1,
         "once the 1 retires the drain releases the BO");
   check(!p_atomic_read(&ws->deferred.abandoned) && ws->deferred.reaped == 1 &&
            list_is_empty(&ws->deferred.trackers),
         "and reaps the abandoned tracker (%" PRIu64 " reaped)", ws->deferred.reaped);

   radv_wddm2_deferred_finish(ws);
   check(!h.unknown && !h.dropped, "no unexpected host call and no dropped record");
   check(!h.n_allocs, "no allocation left (%u)", h.n_allocs);
   simple_mtx_destroy(&ws->deferred.lock);
   free(ws);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"plain_destroy", test_plain_destroy},
   {"no_entry_leaks", test_no_entry_leaks},
   {"no_entry_retired", test_no_entry_retired},
   {"held_until_retired", test_held_until_retired},
   {"refused_kept", test_refused_kept},
   {"teardown_leak", test_teardown_leak},
   {"tracker_abandoned", test_tracker_abandoned},
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
