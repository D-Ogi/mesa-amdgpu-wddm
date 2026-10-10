/* SPDX-License-Identifier: MIT
 *
 * Tests of the CPU fence wait of the system BC250 winsys (vk_wddm2_fence_wait
 * in radv_wddm2_cs.c, linked as compiled for vulkan_radeon.dll). A scripted
 * host dispatch stands in for the kernel: it records every call, accepts the
 * kernel wait without completing it, and lets a GPU thread of the test decide
 * what happens to the waiting work.
 *
 * What the tests are for (K225, lab trial D1 of KMD 0.7.216.17 with TdrDelay
 * 10): a single 10 s wait reported a device loss while the work of this device
 * was only waiting behind another device's hang. The scheduler resets the
 * engine and resubmits the innocent render packets with new fence ids, and
 * that takes TdrDelay and the reset - 14.6 s in that trial. The wait is now
 * done in slices, and it ends early only when the device really is lost.
 *
 * The tests run in both configurations of the winsys. A hosted winsys (zink
 * or the D3D ICD hosts the device) answers the liveness checks itself; the
 * shipped system ICD has no host, and then the kernel device-state query
 * between the slices is the only liveness mechanism, so the native_* tests
 * fill in a dispatch table of their own and read that mechanism.
 *
 * Scope: the tests drive vk_wddm2_fence_wait through the ws->ctx_wait_idle
 * hook. On this line that hook is called from src/amd/vulkan/tools/
 * radv_debug_hang.c only (the hang report and the trap-handler dump), so it
 * is a debug-gated entry and NOT the vkDeviceWaitIdle path: vkDeviceWaitIdle
 * and vkQueueWaitIdle go through vk_wddm2_monitored_fence_wait_many, which
 * honours the caller's timeout and never reaches this wait. The wait's own
 * callers are that hook, the queue teardown and the gather-slot reuse of
 * radv_wddm2_cs_submit, which is the one an application reaches (through
 * vkQueueSubmit); the reuse needs a command stream and buffer objects and is
 * not built here, so gather_ring_shape below only records this line's ring,
 * because the same change on the D3D ICD line reads a runtime
 * ws->bc250_gather_slots. All of them run the same body.
 *
 * Built outside meson: compiled with the compile command of radv_wddm2_cs.c
 * and linked against the radv_wddm2_cs.c and radv_wddm2_bo.c objects and the
 * vulkan_util, amd_common, mesa_util and mesa_util_c11 libraries.
 *
 * Three negative controls, and each must fail:
 *  - BC250_TEST_OLD_BOUND=1 shortens the total bound to one slice, which is
 *    the shape before this change (one bounded wait, then a loss). The
 *    innocent-wait cases must fail under it.
 *  - BC250_TEST_SOURCE_CONTROL compiles this file against the winsys object
 *    of the source before the slices, whose bound is a hardcoded single 10 s
 *    wait. It defines the two bound variables itself, because that source
 *    does not export them. past_old_bound must fail, and so must the cases
 *    that require an early end and the native cases that require a query.
 *  - the same program linked against the winsys object of the revision that
 *    had the slices but read a failed device-state query as "still
 *    executing": native_device_removed and native_unanswered_state must fail
 *    there, because that source waits out the whole total bound.
 *
 * Usage: radv_wddm2_fence_wait_test [test], no argument runs all.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bc250.h"
#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "util/bc250_host_bootstrap.h"
#include "util/macros.h"
#include "vk_sync.h"

#ifdef Status
#undef Status
#endif
#include <windows.h>
#include "d3dkmthk.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel of the native tests. BC250_WDDM_CALL takes the native path only
 * while host.dispatch is NULL, which is the shipped configuration of the
 * system ICD: the Vulkan loader makes the device, there is no host, and the
 * device-state query between slices is the only liveness mechanism left. The
 * hosted tests cannot reach it, so these stand in for the kernel instead. */
static struct {
   bool in_use;
   NTSTATUS fail_status;   /* what a failing GetDeviceState returns */
   unsigned fail_queries;  /* the first N queries fail; UINT_MAX: all of them */
   D3DKMT_DEVICEEXECUTION_STATE state; /* what an answering query reports */
   unsigned queries, waits;
} k;

static NTSTATUS APIENTRY
test_kernel_GetDeviceState(D3DKMT_GETDEVICESTATE *arg)
{
   k.queries++;
   if (k.queries <= k.fail_queries)
      return k.fail_status;
   arg->ExecutionState = k.state;
   return STATUS_SUCCESS;
}

/* Accepts the wait and does not complete it, like the hosted dispatch: the GPU
 * thread of the test acts on the async event later. */
static NTSTATUS APIENTRY
test_kernel_WaitForSynchronizationObjectFromCpu(CONST D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *arg);

static struct vk_wddm2_dispatch_table test_table;

struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   if (!k.in_use) {
      fprintf(stderr, "native dispatch table requested by a hosted test: test setup error\n");
      abort();
   }
   return &test_table;
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

/* The winsys names the monitored-fence sync type; the wait does not use it. */
const struct vk_sync_type vk_wddm2_monitored_fence_type = {0};

#define DEVICE_H 0x20u
#define SYNC_H   0x40u
#define MAX_EV   4096

/* The bounds of the wait under test. The source before this change does not
 * export them, so the control build defines them here and they are ignored. */
#ifdef BC250_TEST_SOURCE_CONTROL
uint64_t radv_wddm2_fence_wait_slice_ns = 1000000000ull;
uint64_t radv_wddm2_fence_wait_total_ns = 120000000000ull;
#else
extern uint64_t radv_wddm2_fence_wait_slice_ns;
extern uint64_t radv_wddm2_fence_wait_total_ns;
#endif

struct event {
   uint32_t op;
   uint32_t sync;
   uint64_t value;
};

static struct {
   struct event ev[MAX_EV];
   unsigned n_ev;
   DWORD thread;
   unsigned wrong_thread, unexpected;

   int32_t status; /* CHECK_STATUS; negative: the device is lost */

   /* The fence the wait reads, and the kernel wait the host accepted but did
    * not complete: the GPU thread of the test acts on it later. */
   uint64_t fence_value;
   HANDLE held_event, held_armed;
   uint64_t held_value;
} h;

static unsigned checks, failures;
static const char *current = "";

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

static void
record(uint32_t op, uint32_t sync, uint64_t value)
{
   if (GetCurrentThreadId() != h.thread)
      h.wrong_thread++;
   if (h.n_ev < MAX_EV)
      h.ev[h.n_ev++] = (struct event){op, sync, value};
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

static int32_t
fake_dispatch(void *userdata, uint32_t op, void *arg)
{
   (void)userdata;
   switch (op) {
   case BC250_HOST_CHECK_STATUS:
      record(op, 0, 0);
      return h.status;
   case BC250_HOST_REPORT_LOST:
      record(op, 0, 0);
      return 0;
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      record(op, w->ObjectCount ? w->ObjectHandleArray[0] : 0, w->ObjectCount ? w->FenceValueArray[0] : 0);
      /* Held: the work waits behind another device's hang. */
      h.held_event = w->hAsyncEvent;
      h.held_value = w->ObjectCount ? w->FenceValueArray[0] : 0;
      SetEvent(h.held_armed);
      return 0;
   }
   case BC250_HOST_GetDeviceState: {
      D3DKMT_GETDEVICESTATE *g = arg;
      record(op, 0, 0);
      g->ExecutionState = D3DKMT_DEVICEEXECUTION_ACTIVE;
      return 0;
   }
   default:
      record(op, 0, 0);
      h.unexpected++;
      return (int32_t)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
   }
}

static NTSTATUS APIENTRY
test_kernel_WaitForSynchronizationObjectFromCpu(CONST D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *arg)
{
   k.waits++;
   h.held_event = arg->hAsyncEvent;
   h.held_value = arg->ObjectCount ? arg->FenceValueArray[0] : 0;
   SetEvent(h.held_armed);
   return STATUS_SUCCESS;
}

/* A winsys and a context with nothing in them but what the wait reads. */
static struct radv_wddm2_winsys *
make_ws(void)
{
   struct radv_wddm2_winsys *ws = calloc(1, sizeof(*ws));
   if (!ws)
      abort();
   ws->host.sType = BC250_HOST_STYPE;
   ws->host.version = BC250_HOST_VERSION;
   ws->host.size = sizeof(ws->host);
   ws->host.dispatch = fake_dispatch;
   ws->host.userdata = &h;
   ws->device_h = DEVICE_H;
   ws->bc250 = true;
   radv_wddm2_cs_init_functions(ws);
   return ws;
}

/* The same winsys in the shipped configuration: no host, so every WDDM call
 * goes to the dispatch table above. */
static struct radv_wddm2_winsys *
make_ws_native(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   ws->host.dispatch = NULL;
   ws->host.userdata = NULL;
   test_table.GetDeviceState = test_kernel_GetDeviceState;
   test_table.WaitForSynchronizationObjectFromCpu = test_kernel_WaitForSynchronizationObjectFromCpu;
   k.in_use = true;
   return ws;
}

static struct radv_wddm2_ctx *
make_ctx(struct radv_wddm2_winsys *ws, uint64_t wait_value)
{
   struct radv_wddm2_ctx *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      abort();
   ctx->ws = ws;
   ctx->per_ip[AMD_IP_GFX].last_submission.handle = SYNC_H;
   ctx->per_ip[AMD_IP_GFX].last_submission.wait_value = wait_value;
   ctx->per_ip[AMD_IP_GFX].last_submission.value_map = &h.fence_value;
   return ctx;
}

enum gpu_action { GPU_COMPLETE, GPU_LOSE, GPU_FENCE_MAX, GPU_NOTHING };

static struct {
   enum gpu_action action;
   DWORD delay_ms;
} gpu;

/* Which winsys held_wait() makes: the hosted one, or the shipped native one. */
static bool use_native;

/* The scheduler of the test. It waits until the winsys has made its kernel
 * wait, sleeps, and then completes the work, loses the device, writes
 * UINT64_MAX into the fence, or does nothing. It never calls the host
 * dispatch, so the one-thread rule still holds. */
static DWORD WINAPI
gpu_thread(void *arg)
{
   (void)arg;
   if (WaitForSingleObject(h.held_armed, 30000) != WAIT_OBJECT_0)
      return 1;
   Sleep(gpu.delay_ms);
   switch (gpu.action) {
   case GPU_COMPLETE:
      InterlockedExchange64((volatile LONG64 *)&h.fence_value, (LONG64)h.held_value);
      SetEvent(h.held_event);
      break;
   case GPU_LOSE:
      InterlockedExchange((volatile LONG *)&h.status, -1);
      break;
   case GPU_FENCE_MAX:
      InterlockedExchange64((volatile LONG64 *)&h.fence_value, (LONG64)-1);
      break;
   case GPU_NOTHING:
      break;
   }
   return 0;
}

static void
reset_state(void)
{
   memset(&h, 0, sizeof(h));
   h.thread = GetCurrentThreadId();
   memset(&k, 0, sizeof(k));
   k.state = D3DKMT_DEVICEEXECUTION_ACTIVE;
   k.fail_status = STATUS_UNSUCCESSFUL;
   use_native = false;
}

static void
restore_bounds(void)
{
   radv_wddm2_fence_wait_slice_ns = 1000000000ull;
   radv_wddm2_fence_wait_total_ns = 120000000000ull;
}

/* Waits for the queue's own fence with the wait held, and reports what the
 * wait answered and how long it took. slice_ms or total_ms of 0 keeps the
 * shipped default. */
static bool
held_wait(enum gpu_action action, DWORD delay_ms, uint64_t slice_ms, uint64_t total_ms, unsigned *mark,
          double *elapsed_ms)
{
   struct radv_wddm2_winsys *ws = use_native ? make_ws_native() : make_ws();
   struct radv_wddm2_ctx *ctx = make_ctx(ws, 1);
   if (slice_ms)
      radv_wddm2_fence_wait_slice_ns = slice_ms * 1000000ull;
   if (total_ms)
      radv_wddm2_fence_wait_total_ns = total_ms * 1000000ull;
   const char *old = getenv("BC250_TEST_OLD_BOUND");
   if (old && *old == '1') {
      radv_wddm2_fence_wait_total_ns = radv_wddm2_fence_wait_slice_ns;
      printf("NOTE %-16s negative control: total bound = one slice (%llu ms)\n", current,
             (unsigned long long)(radv_wddm2_fence_wait_slice_ns / 1000000ull));
   }
   gpu.action = action;
   gpu.delay_ms = delay_ms;
   h.held_armed = CreateEventA(NULL, TRUE, FALSE, NULL);
   HANDLE t = CreateThread(NULL, 0, gpu_thread, NULL, 0, NULL);
   LARGE_INTEGER f, t0, t1;
   QueryPerformanceFrequency(&f);
   *mark = h.n_ev;
   QueryPerformanceCounter(&t0);
   bool ok = ws->base.ctx_wait_idle((struct radeon_winsys_ctx *)ctx, AMD_IP_GFX, 0);
   QueryPerformanceCounter(&t1);
   SetEvent(h.held_armed); /* a GPU thread that never saw the wait ends too */
   WaitForSingleObject(t, INFINITE);
   CloseHandle(t);
   CloseHandle(h.held_armed);
   h.held_armed = NULL;
   *elapsed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
   restore_bounds();
   free(ctx);
   free(ws);
   return ok;
}

static void
contract(void)
{
   check(!h.wrong_thread, "every host call on the test's thread (%u off it)", h.wrong_thread);
   check(!h.unexpected, "no host operation outside the wait's own set (%u)", h.unexpected);
}

/* A native test went to the kernel and to no host at all. */
static void
native_contract(void)
{
   check(!h.n_ev, "no host dispatch call on the native path (%u)", h.n_ev);
   check(k.waits == 1, "one kernel wait issued (%u)", k.waits);
}

/* The quick poll: a fence already at its wait value needs no kernel call at
 * all, and no device-state query (the BD-102 wait shape). */
static void
test_quick_poll(void)
{
   reset_state();
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_ctx *ctx = make_ctx(ws, 1);
   h.fence_value = 1;
   unsigned mark = h.n_ev;
   bool ok = ws->base.ctx_wait_idle((struct radeon_winsys_ctx *)ctx, AMD_IP_GFX, 0);
   check(ok, "a fence already at its wait value is retired work");
   check(!count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu), "no kernel wait (%u)",
         count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu));
   check(!count_op(mark, BC250_HOST_GetDeviceState), "no device-state query (%u)",
         count_op(mark, BC250_HOST_GetDeviceState));
   free(ctx);
   free(ws);
   contract();
}

/* An ordinary completion inside the first slice: the wait answers true and
 * asks the kernel for no device state. This is the b25 fence-wait shape
 * (BD-102 review finding 1) and the slices must not have cost it. */
static void
test_ordinary_completion(void)
{
   reset_state();
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 0, 1000, 120000, &mark, &ms);
   check(ok, "work that completes inside the first slice is not a loss (%.0f ms)", ms);
   check(count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 1, "one kernel wait (%u)",
         count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu));
   check(!count_op(mark, BC250_HOST_GetDeviceState),
         "no device-state query on an ordinary completion (%u)", count_op(mark, BC250_HOST_GetDeviceState));
   check(!count_op(mark, BC250_HOST_REPORT_LOST), "no loss reported");
   contract();
}

/* The innocent wait: the work completes after about 15 slices, as DWM's did
 * after 14.6 s behind a 10 s wait in trial D1. */
static void
test_innocent_wait(void)
{
   reset_state();
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 300, 20, 5000, &mark, &ms);
   check(ok, "the wait outlives about 15 slices and answers retired (%.0f ms)", ms);
   check(ms >= 280.0, "it waited for the work (%.0f ms, completes at 300)", ms);
   check(count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 1,
         "one kernel wait, waited on again per slice and not issued again (%u)",
         count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu));
   check(count_op(mark, BC250_HOST_CHECK_STATUS) >= 5, "the device state is checked between slices (%u checks)",
         count_op(mark, BC250_HOST_CHECK_STATUS));
   check(!count_op(mark, BC250_HOST_REPORT_LOST) && !count_op(mark, BC250_HOST_GetDeviceState),
         "no loss is reported and the hosted device is not asked for GetDeviceState");
   contract();
}

/* The same claim against the bound that shipped: work that completes 10.5 s
 * after the wait started. The source before this change waits once for 10 s,
 * so this case is the one that reads the semantics and not only the shape. */
static void
test_past_old_bound(void)
{
   reset_state();
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 10500, 0, 0, &mark, &ms);
   check(ok, "work that completes 10.5 s into the wait is not a loss (%.0f ms)", ms);
   check(ms >= 10000.0, "it waited past the 10 s bound that shipped (%.0f ms)", ms);
   check(count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu) == 1, "one kernel wait (%u)",
         count_op(mark, BC250_HOST_WaitForSynchronizationObjectFromCpu));
   check(!count_op(mark, BC250_HOST_REPORT_LOST), "no loss reported");
   contract();
}

/* A real loss ends the wait at the next slice, not at the total bound. */
static void
test_lost_during_wait(void)
{
   static const struct {
      enum gpu_action action;
      const char *what;
   } cases[] = {
      {GPU_LOSE, "the host reports the loss"},
      {GPU_FENCE_MAX, "the fence reads UINT64_MAX"},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      reset_state();
      unsigned mark;
      double ms;
      bool ok = held_wait(cases[i].action, 100, 20, 5000, &mark, &ms);
      check(!ok, "%s during the wait: the wait answers lost", cases[i].what);
      check(ms < 1000.0, "at the next slice, not at the total bound (%.0f ms, bound 5000)", ms);
      check(cases[i].action != GPU_FENCE_MAX || count_op(mark, BC250_HOST_REPORT_LOST) >= 1,
            "a UINT64_MAX fence is reported to the host");
      contract();
   }
}

/* Work that never completes on a live device still ends, at the total bound. */
static void
test_wait_bound(void)
{
   reset_state();
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_NOTHING, 0, 20, 300, &mark, &ms);
   check(!ok, "work that never completes on a live device ends at the total bound");
   check(ms >= 280.0 && ms < 2000.0, "after the total bound (%.0f ms, bound 300)", ms);
   contract();
}

/* The native path, which is the shipped one: no host, so the only liveness
 * mechanism between slices is the kernel device-state query. An innocent wait
 * on a device that keeps answering ACTIVE is retired, and the query really is
 * made, once per slice. The hosted innocent_wait above asserts the opposite
 * (no query at all), because a hosted device has no kernel device of its own,
 * so this is the case that reads the production mechanism. */
static void
test_native_live_wait(void)
{
   reset_state();
   use_native = true;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 300, 20, 5000, &mark, &ms);
   check(ok, "work that completes after about 15 slices is retired (%.0f ms)", ms);
   check(ms >= 280.0, "it waited for the work (%.0f ms, completes at 300)", ms);
   check(k.queries >= 5, "the kernel device state is queried between slices (%u queries)", k.queries);
   native_contract();
}

/* The ordinary completion on the native path: a wait that returns inside its
 * first slice pays no device-state query. This is the b25 wait shape (BD-102
 * review finding 1) read where it actually runs. */
static void
test_native_ordinary(void)
{
   reset_state();
   use_native = true;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 0, 1000, 120000, &mark, &ms);
   check(ok, "work that completes inside the first slice is not a loss (%.0f ms)", ms);
   check(!k.queries, "no device-state query on an ordinary completion (%u)", k.queries);
   native_contract();
}

/* A removed device: the adapter was stopped or the display device was reset,
 * which D3DKMTGetDeviceState answers with STATUS_DEVICE_REMOVED. Nothing will
 * signal that wait any more, so it must end at the next slice. Reading a
 * failed query as "still executing" holds the caller for the whole total
 * bound instead, which is the b28 review's blocking item. */
static void
test_native_device_removed(void)
{
   reset_state();
   use_native = true;
   k.fail_status = STATUS_DEVICE_REMOVED;
   k.fail_queries = UINT_MAX;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_NOTHING, 0, 20, 5000, &mark, &ms);
   check(!ok, "a removed device answers lost");
   check(ms < 1000.0, "at the next slice, not at the total bound (%.0f ms, bound 5000)", ms);
   check(k.queries >= 1, "the device state was queried (%u)", k.queries);
   native_contract();
}

/* A device-state query that keeps failing with a status that does not name a
 * removal. One such query is no evidence of a loss, so the wait tolerates
 * RADV_WDDM2_UNANSWERED_STATE_MAX - 1 of them and then ends: a device that
 * never answers cannot be waited out to the total bound either. */
static void
test_native_unanswered_state(void)
{
   reset_state();
   use_native = true;
   k.fail_status = STATUS_UNSUCCESSFUL;
   k.fail_queries = UINT_MAX;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_NOTHING, 0, 20, 5000, &mark, &ms);
   check(!ok, "a device that stops answering ends the wait");
   check(ms < 1000.0, "well before the total bound (%.0f ms, bound 5000)", ms);
   check(k.queries >= 3, "it tolerated the first unanswered queries (%u queries)", k.queries);
   check(ms >= 40.0, "and did not end on the first one (%.0f ms, one slice is 20)", ms);
   native_contract();
}

/* The other side of that tolerance: two unanswered queries in a row on a
 * device that is only slow must not become a loss. */
static void
test_native_transient_state(void)
{
   reset_state();
   use_native = true;
   k.fail_status = STATUS_UNSUCCESSFUL;
   k.fail_queries = 2;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_COMPLETE, 300, 20, 5000, &mark, &ms);
   check(ok, "two unanswered queries do not lose a live device (%.0f ms)", ms);
   check(ms >= 280.0, "the work was waited for (%.0f ms, completes at 300)", ms);
   check(k.queries > 2, "the query was made again after the failures (%u)", k.queries);
   native_contract();
}

/* A device that answers, and is not executing: the hung device of a TDR. It
 * is lost at the next slice, as it was before the slices. */
static void
test_native_error_state(void)
{
   reset_state();
   use_native = true;
   k.state = D3DKMT_DEVICEEXECUTION_HUNG;
   unsigned mark;
   double ms;
   bool ok = held_wait(GPU_NOTHING, 0, 20, 5000, &mark, &ms);
   check(!ok, "a device that is not ACTIVE answers lost");
   check(ms < 1000.0, "at the next slice, not at the total bound (%.0f ms, bound 5000)", ms);
   native_contract();
}

/* The gather ring of this line is a compile-time constant, not the runtime
 * ws->bc250_gather_slots of the D3D ICD line the change came from. The slot
 * reuse of cs_submit is the wait's other caller, so the shape is recorded
 * here even though this program does not drive a submission. */
static void
test_gather_ring_shape(void)
{
   reset_state();
   struct radv_wddm2_winsys *ws = make_ws();
   struct radv_wddm2_ctx *ctx = make_ctx(ws, 1);
   check(BC250_GATHER_SLOTS >= 2u, "the gather ring has %u slots, so a submission can reuse one",
         (unsigned)BC250_GATHER_SLOTS);
   check(ARRAY_SIZE(ctx->per_ip[AMD_IP_GFX].queue.bc250_gather) == BC250_GATHER_SLOTS,
         "the queue's ring is that many slots (%u)",
         (unsigned)ARRAY_SIZE(ctx->per_ip[AMD_IP_GFX].queue.bc250_gather));
   free(ctx);
   free(ws);
}

static const struct {
   const char *name;
   void (*run)(void);
} tests[] = {
   {"quick_poll", test_quick_poll},
   {"ordinary_completion", test_ordinary_completion},
   {"innocent_wait", test_innocent_wait},
   {"past_old_bound", test_past_old_bound},
   {"lost_during_wait", test_lost_during_wait},
   {"wait_bound", test_wait_bound},
   {"native_live_wait", test_native_live_wait},
   {"native_ordinary", test_native_ordinary},
   {"native_device_removed", test_native_device_removed},
   {"native_unanswered_state", test_native_unanswered_state},
   {"native_transient_state", test_native_transient_state},
   {"native_error_state", test_native_error_state},
   {"gather_ring_shape", test_gather_ring_shape},
};

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   unsigned ran = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      current = tests[i].name;
      tests[i].run();
      ran++;
   }
   if (argc > 1 && !ran) {
      fprintf(stderr, "no test named %s\n", argv[1]);
      return 2;
   }
   printf("tests=%u checks=%u failures=%u\n", ran, checks, failures);
   return failures ? 1 : 0;
}
