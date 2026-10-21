/* SPDX-License-Identifier: MIT
 *
 * Deferred destruction of the desktop BC250 winsys (radv_wddm2_bo.c, BD-201),
 * linked as compiled for vulkan_radeon.dll of the desktop RADV line. A scripted host dispatch stands in
 * for the kernel: it records every call in order, hands out allocation handles
 * and gpu addresses, and plays the GPU by completing the value a CPU wait asks
 * for, unless the test makes that wait fail.
 *
 * What the tests are for: a destroyed BO keeps its allocation, its VA and its
 * residency until every queue reaches the progress value published when it was
 * destroyed, because D3DKMTEvict makes an allocation illegal to access once the
 * request is queued and after D3DKMTFreeGpuVirtualAddress a new range may take
 * the freed VA at once, under work that still reads it. The hold needs one
 * allocation, of the entry that records the waits, and the destroy path may not
 * depend on it (BD-201): the answer to "no host memory" cannot be an eviction
 * and a VA free under running work. radv_wddm2_deferred_entry_fail forces that
 * one allocation to fail, as host memory pressure would, and the recorded call
 * order is the proof of what the destroy did instead.
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
 * The negative control is the source before the fix: there a destroy that gets
 * no entry calls radv_wddm2_bo_destroy_now at once. no_entry_waits and
 * no_entry_kept must fail against it, and they must pass with the fix.
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

static struct radeon_winsys_bo *
make_bo(struct radv_wddm2_winsys *ws)
{
   struct radeon_winsys_bo *bo = NULL;
   const VkResult result = ws->base.buffer_create(&ws->base, BO_SIZE, 4096, RADEON_DOMAIN_GTT,
                                                 RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &bo);
   check(result == VK_SUCCESS && bo && bo->handle, "an internal BO is made (%d)", result);
   return bo;
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

/* The published value has not retired and the entry allocation fails: the
 * destroy waits for that value itself, and only then evicts the BO and frees
 * its VA. Before the fix it evicted and freed at once. */
static void
test_no_entry_waits(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;
   char line[1024];

   radv_wddm2_tracker_publish(tracker, 1);
   check(h.fence_value == 0 && tracker->published == 1, "the queue published 1, completed 0");

   const size_t lm = log_mark();
   const unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   ws->base.buffer_destroy(&ws->base, bo);
   check(radv_wddm2_deferred_entry_fail == 0, "the destroy made one entry allocation, and it failed");

   const int w = find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu);
   const int evict = find_op(mark, BC250_HOST_Evict);
   const int free_va = find_op(mark, BC250_HOST_FreeGpuVirtualAddress);
   const int destroy = find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h);
   check(cpu_wait_is(w, PROGRESS_H, 1), "with no entry the destroy waits for the published 1 itself (%d)", w);
   check(w >= 0 && evict > w && free_va > w && destroy > w,
         "no Evict, no VA free and no allocation destroy before that wait (w=%d, %d %d %d)", w, evict, free_va,
         destroy);
   check(evict >= 0 && evict < free_va && free_va < destroy, "then in that order (%d %d %d)", evict, free_va, destroy);
   check(h.fence_value >= 1, "and the value was reached first (%" PRIu64 ")", h.fence_value);
   check(!alloc_live(bo_h) && !ws->deferred.count && !ws->deferred.starved_count, "the BO is gone, nothing held");
   check(ws->deferred.starved_total == 1 && ws->deferred.starved_waits == 1 && !ws->deferred.starved_failed,
         "one entry-less destroy counted, one CPU wait, none kept");
   check_log(log_lines(lm, "no memory to hold a 65536-byte BO", line, sizeof(line)) == 1 &&
                strstr(line, "1 CPU wait") && strstr(line, "destroyed after it"),
             "and logged: %.180s", line);

   finish(ws, tracker);
}

/* Every published value retired: the same failed allocation needs no wait. */
static void
test_no_entry_retired(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;

   radv_wddm2_tracker_publish(tracker, 1);
   h.fence_value = 1;
   const unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   ws->base.buffer_destroy(&ws->base, bo);
   check(!radv_wddm2_deferred_entry_fail, "the entry allocation was made and failed");
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0,
         "the BO is destroyed at once");
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu), "with nothing in flight, no CPU wait");
   check(!ws->deferred.starved_total && !ws->deferred.starved_waits && ws->deferred.immediate == 1,
         "counted as an immediate destroy, not as an entry-less one");

   finish(ws, tracker);
}

/* The entry allocation fails and the CPU wait fails too: the BO keeps its
 * allocation, its VA and its residency, and a drain point retries it. Nothing
 * of it is freed while the published value is unreached. */
static void
test_no_entry_kept(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;
   char line[1024];

   radv_wddm2_tracker_publish(tracker, 2);
   const size_t lm = log_mark();
   unsigned mark = h.n_ev;
   radv_wddm2_deferred_entry_fail = 1;
   fail_calls(BC250_HOST_WaitForSynchronizationObjectFromCpu, 1);
   ws->base.buffer_destroy(&ws->base, bo);
   no_failures();
   check(cpu_wait_is(find_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu), PROGRESS_H, 2),
         "the destroy waited for the published 2");
   check(alloc_live(bo_h) && !count_op(mark, BC250_HOST_Evict) &&
            !count_op(mark, BC250_HOST_FreeGpuVirtualAddress) &&
            find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) < 0,
         "the failed wait freed nothing: no Evict, no VA free, no allocation destroy");
   check(ws->deferred.starved_count == 1 && ws->deferred.starved_bytes == BO_SIZE &&
            ws->deferred.starved_failed == 1 && ws->deferred.starved_waits == 1,
         "the BO is kept whole (%u BOs, %" PRIu64 " bytes)", ws->deferred.starved_count,
         ws->deferred.starved_bytes);
   check_log(log_lines(lm, "kept whole", line, sizeof(line)) == 1, "and logged: %.180s", line);

   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(alloc_live(bo_h) && ws->deferred.starved_count == 1 && !count_op(mark, BC250_HOST_Evict),
         "a drain with the 2 still in flight keeps it");

   h.fence_value = 2;
   mark = h.n_ev;
   radv_wddm2_deferred_drain(ws);
   check(!alloc_live(bo_h) && find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h) >= 0 &&
            !ws->deferred.starved_count && !ws->deferred.starved_bytes,
         "once the 2 retired a drain releases it");

   finish(ws, tracker);
}

/* The ordinary hold, for the contrast: with an entry the BO waits, and no call
 * of its destroy reaches the host until the published value retires. */
static void
test_held_until_retired(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_tracker *tracker = attach_queue(ws);
   struct radeon_winsys_bo *bo = make_bo(ws);
   const uint32_t bo_h = bo->handle;

   radv_wddm2_tracker_publish(tracker, 3);
   unsigned mark = h.n_ev;
   ws->base.buffer_destroy(&ws->base, bo);
   check(alloc_live(bo_h) && ws->deferred.count == 1 && ws->deferred.bytes == BO_SIZE,
         "the BO is held (%u BOs, %" PRIu64 " bytes)", ws->deferred.count, ws->deferred.bytes);
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
   const int evict = find_op(mark, BC250_HOST_Evict);
   const int destroy = find_handle(mark, BC250_HOST_DestroyAllocation2, bo_h);
   check(!alloc_live(bo_h) && evict >= 0 && destroy > evict && !ws->deferred.count && ws->deferred.released == 1,
         "once the 3 retired the drain destroys it (%d %d)", evict, destroy);

   finish(ws, tracker);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"no_entry_waits", test_no_entry_waits},
   {"no_entry_retired", test_no_entry_retired},
   {"no_entry_kept", test_no_entry_kept},
   {"held_until_retired", test_held_until_retired},
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
