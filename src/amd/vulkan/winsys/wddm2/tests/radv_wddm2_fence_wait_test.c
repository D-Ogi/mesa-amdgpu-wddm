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
 * Scope: the tests drive vk_wddm2_fence_wait through the public
 * ctx_wait_idle hook (the vkDeviceWaitIdle path), which is one of its two
 * production callers. The other is the gather-slot reuse of
 * radv_wddm2_cs_submit, which needs a command stream and buffer objects and
 * is not built here; on this line the gather ring is the compile-time
 * BC250_GATHER_SLOTS, and gather_ring_shape below records that, because the
 * same change on the D3D ICD line reads a runtime ws->bc250_gather_slots.
 *
 * Built outside meson: compiled with the compile command of radv_wddm2_cs.c
 * and linked against the radv_wddm2_cs.c and radv_wddm2_bo.c objects and the
 * vulkan_util, amd_common, mesa_util and mesa_util_c11 libraries.
 *
 * Two negative controls, and both must fail:
 *  - BC250_TEST_OLD_BOUND=1 shortens the total bound to one slice, which is
 *    the shape before this change (one bounded wait, then a loss). The
 *    innocent-wait cases must fail under it.
 *  - BC250_TEST_SOURCE_CONTROL compiles this file against the winsys object
 *    of the source before the change, whose bounds are a hardcoded single
 *    10 s wait. It defines the two bound variables itself, because that
 *    source does not export them. past_old_bound must fail, and so must the
 *    two cases that require an early end.
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
   struct radv_wddm2_winsys *ws = make_ws();
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
