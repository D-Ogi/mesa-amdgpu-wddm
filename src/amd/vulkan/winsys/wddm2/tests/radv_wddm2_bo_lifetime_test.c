/* SPDX-License-Identifier: MIT
 *
 * BD-201: the BO lifetime contract of the WDDM2 winsys, tested on the real
 * winsys code (radv_wddm2_bo.c and radv_wddm2_cs.c, compiled as the ICD
 * compiles them) through a mocked D3DKMT host.
 *
 * THE GUARANTEE
 *
 * For every BO the winsys destroys, neither D3DKMTEvict nor
 * D3DKMTFreeGpuVirtualAddress of it runs before each queue's progress fence
 * has reached the value that queue PUBLISHED AT THE MOMENT OF THE DESTROY.
 * The target set is fixed at the destroy: later, unrelated submissions may
 * not move it.
 *
 * Why those two calls and not DestroyAllocation2 (ref\ddi-display\d3dkmthk.md):
 * once an eviction is queued "it is illegal to access the underlying
 * allocation" (:12514); after a VA free "a new virtual address range can be
 * allocated in place of the freed one immediately" (:12632) and a separate VA
 * free "needs to be synchronized with allocation destruction" (:13810).
 * DestroyAllocation2 with flags 0 waits for queued use (d3dukmdt.md:3402), so
 * it protects the allocation and not the address range that was already freed.
 *
 * THE RULES THE MOCK ENFORCES, BY CLASS OF OBJECT
 * (lead, 2026-10-21 15:05Z, design R v2)
 *
 * R1 objects: an internal BO, not virtual, not REPLAYABLE, not borrowed. Its
 *   legal release is DestroyAllocation2 with flags 0 and nothing else. A
 *   breach is: any Evict of it; any FreeGpuVirtualAddress of its address;
 *   DestroyAllocation2 with a non-zero flag; and DestroyAllocation2 issued
 *   while a submission has not RETURNED from the host's SubmitCommand, which
 *   is the handoff the OS's own deferral counts from (6.2). A
 *   DestroyAllocation2 BEFORE the destroy-time values complete is NOT a
 *   breach: the mock models VidMm deferring it, so the allocation keeps its
 *   memory and its address until those values are reached, and a reuse of
 *   either in the meantime is a breach of its own.
 * R2 objects: a virtual BO's reservation (reserved_va, sparse_high_va) and a
 *   REPLAYABLE BO's replay address. No Evict of them, ever.
 *   FreeGpuVirtualAddress only after the values CAPTURED at the destroy are
 *   reached; the values published later do not count. Unproven at teardown
 *   means leaked and counted, never freed.
 * Kept lists: a BO whose release the host refused (a CPU-locked allocation
 *   here) is kept whole with a counter and retried at drain points, and never
 *   released because a wait failed.
 *
 * The strict clauses of R1 (no Evict and no address free AT ALL, and no
 * destroy inside the submit window) are the contract of the frozen candidate:
 * they are assertions under BD201_V2 and counted observations otherwise,
 * because V0 and V1 release an R1 object with an Evict and an address free
 * after the proof, which the rule below allows them. Everything else below
 * holds on every version.
 *
 * THE RULES THAT HOLD ON EVERY VERSION
 *
 * 1. Evict and FreeGpuVirtualAddress of a destroyed BO, if they happen at all,
 *    only after its destroy-time values are reached. This holds THROUGH
 *    TEARDOWN: a timeout, a failed wait, a missing tracker and the end of the
 *    process are not proof that the work completed. The hosted line does not
 *    even call DestroyDevice (radv_wddm2_winsys.c:796), and on the lines that
 *    do, these two calls come before it.
 * 2. DestroyAllocation2 is allowed at any time, but only with
 *    AssumeNotInUse = 0, which is what makes VidMm defer the destruction until
 *    the commands queued before the request finish (d3dukmdt.md:3398-3402).
 *    The test asserts the flag, and that no Evict and no VA free of that
 *    handle follows it.
 * 3. A virtual BO's own reservation (ReserveGpuVirtualAddress, sparse_high_va)
 *    is freed only after proof, or never. Nobody frees it for us, so leaking
 *    it is the safe end state.
 * There is no required order of the three calls. d3dkmthk.md:13810 says the
 * API-allocated range is freed WITH the allocation destroy, and that a
 * separate free must be synchronized with it; it does not ask for
 * Evict, then VA free, then destroy. An earlier version of this test checked
 * such an order; the lead rejected both the rule and its source.
 *
 * The legal case that needs the guarantee: RADV destroys a queue's replaced
 * scratch, compute-scratch, ESGS, GSVS and descriptor BOs on the submit path
 * (radv_update_preambles, radv_queue.c) while earlier submissions that carry
 * the old addresses may still be queued or on the ring. The test never
 * reproduces an application freeing memory the GPU still uses, which Vulkan
 * forbids.
 *
 * HOW THE TEST IS INDEPENDENT OF THE FIX
 *
 * - The published value of a queue is read from the winsys's own declarations
 *   to the host: BC250_HOST_PUBLISH_PROGRESS, and the value the queue asks the
 *   GPU to signal on its progress fence (SignalSynchronizationObjectFromGpu2).
 *   The mock takes the larger of the two. No field of the implementation is
 *   read, and no test-only hook of it is used.
 * - Host memory pressure is injected by the test's own interposition: the
 *   winsys translation unit is compiled with malloc, calloc and realloc
 *   renamed to the test's wrappers (-Dmalloc=..., see the build script), and a
 *   wrapper refuses every allocation while the test arms it, for the duration
 *   of one call. Mesa's MALLOC, CALLOC and CALLOC_STRUCT are macros over the
 *   same three functions, so they are covered as well.
 * - Every Evict, FreeGpuVirtualAddress and DestroyAllocation2 is checked
 *   against the guarantee by the mock itself, for every BO the test destroys,
 *   so a breach anywhere in a test is counted, not only where a check looks.
 *
 * Usage: radv_wddm2_bo_lifetime_test [test], no argument runs all.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "winsys/common/radv_winsys_cs.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/bc250_host_bootstrap.h"
#include "util/macros.h"
#include "util/u_math.h"

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

/* The lines of this winsys differ in one way this test must adapt to: whether
 * the tree has the deferred-destruction entry points at all. The build script
 * reads the source and passes the answer; 1 is the state of a line that holds
 * the fix. Nothing else in the test is line-specific: the queue is created
 * through ws->base.ctx_create, which every line has. */
#ifndef BD201_DEFERRED_API
#define BD201_DEFERRED_API 1
#endif

/* The second difference, also answered by the build script from the header: a
 * line that keeps a pool of BO structs frees it after the deferred queue
 * (radv_wddm2_bo_pool_finish); a line without the pool has no such call. The
 * teardown case calls it where it exists, because a teardown that touches a
 * released BO would show up there. */
#ifndef BD201_BO_POOL
#define BD201_BO_POOL BD201_DEFERRED_API
#endif

/* The third: buffer_from_hosted takes the import flags on the newer lines and
 * not on the older system one (radv_radeon_winsys.h). The build script reads
 * the declaration and answers. */
#ifndef BD201_HOSTED_IMPORT_FLAGS
#define BD201_HOSTED_IMPORT_FLAGS 1
#endif

/* The fourth answer, and the only one that is not about the shape of a tree:
 * whether the tree under test is the frozen BD-201 candidate. The build script
 * passes 1 only when it is told to (--expect v2). The requirements of the
 * decided design that V0 and V1 are not expected to meet are assertions then
 * and observations otherwise (lead's decision E, 2026-10-21), so the same
 * source runs against the old lines without false failures and asserts the
 * design the moment the candidate is frozen. */
#ifndef BD201_V2
#define BD201_V2 0
#endif

/* The winsys objects reference these; the test provides them. BC250_WDDM_CALL
 * never takes the native path while host.dispatch is set. */
struct vk_wddm2_dispatch_table;
struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   fprintf(stderr, "native dispatch table requested: test setup error\n");
   abort();
}

void
print_hex_data(FILE *fp, const void *data, uint32_t size)
{
   (void)fp;
   (void)data;
   (void)size;
   fprintf(stderr, "non-BC250 submission path reached: test setup error\n");
   abort();
}

/* BD-226: the local budget query of radv_wddm2_winsys.c, which this test does
 * not link, on the lines whose command streams can go to CPU-visible VRAM. It
 * fails, so radv_wddm2_cs_domain answers GTT; a line without the query does not
 * reference it. */
bool radv_wddm2_query_local(struct radeon_winsys *ws, uint64_t *budget, uint64_t *usage);
bool
radv_wddm2_query_local(struct radeon_winsys *ws, uint64_t *budget, uint64_t *usage)
{
   (void)ws;
   *budget = 0;
   *usage = 0;
   return false;
}

/* The application fences of a queue carry these types; no test makes one. */
const struct vk_sync_type vk_wddm2_monitored_fence_type = {0};
const struct vk_sync_type vk_wddm2_monitored_fence_hosted_type = {0};

/* The CPU wait bounds of the winsys (radv_wddm2_cs.c). The host tests shorten
 * them; nothing else writes them. */
extern uint64_t radv_wddm2_fence_wait_slice_ns;
extern uint64_t radv_wddm2_fence_wait_total_ns;

#define HOST_FAIL       ((int32_t)0xC0000001) /* STATUS_UNSUCCESSFUL */
#define HOST_UNEXPECTED ((int32_t)0xC0000002) /* STATUS_NOT_IMPLEMENTED */
#define DEVICE_H        0x20u
#define PAGING_QUEUE_H  0x30u
#define PAGING_FENCE_H  0x34u
#define MAX_OBJ         256
#define MAX_EV          8192
#define MAX_WATCH       64
#define MAX_Q           4
#define LOCK_BYTES      (256 * 1024)
#define PLAIN_FLAGS     (RADEON_FLAG_NO_INTERPROCESS_SHARING)

/* ------------------------------------------------------------------ *
 * The test's allocator, which the winsys translation unit calls
 * ------------------------------------------------------------------ */

/* Thread-local, so that arming it covers the calls of the destroying thread
 * alone: a test that destroys while another thread submits must not refuse the
 * submitter's allocations too. */
static __declspec(thread) struct {
   /* While armed, every allocation of the winsys on this thread fails. */
   bool armed;
   unsigned attempts; /* allocation calls while armed */
   unsigned total;    /* allocation calls at all */
} alloc_hook;

void *
bd201_test_malloc(size_t size)
{
   alloc_hook.total++;
   if (alloc_hook.armed) {
      alloc_hook.attempts++;
      return NULL;
   }
   return malloc(size);
}

void *
bd201_test_calloc(size_t count, size_t size)
{
   alloc_hook.total++;
   if (alloc_hook.armed) {
      alloc_hook.attempts++;
      return NULL;
   }
   return calloc(count, size);
}

void *
bd201_test_realloc(void *p, size_t size)
{
   alloc_hook.total++;
   if (alloc_hook.armed) {
      alloc_hook.attempts++;
      return NULL;
   }
   return realloc(p, size);
}

static unsigned
arm_allocator(void)
{
   alloc_hook.armed = true;
   alloc_hook.attempts = 0;
   return alloc_hook.total;
}

static unsigned
disarm_allocator(void)
{
   alloc_hook.armed = false;
   return alloc_hook.attempts;
}

/* ------------------------------------------------------------------ *
 * The mocked host
 * ------------------------------------------------------------------ */

struct obj {
   uint32_t handle;
   bool live;
   uint64_t value; /* fences: what the CPU mapping shows (the completed value) */
   uint64_t va;    /* allocations: the address the test last saw mapped */
   void *mem;      /* allocations: Lock2 memory */
   bool locked;
   bool resident;
   /* The OS's own deferral: DestroyAllocation2 with flags 0 was accepted while
    * work that may use the allocation had not completed, so VidMm keeps the
    * memory and the address valid until it has. mock_settle() finishes it. */
   bool pending_destroy;
   unsigned destroy_refusals; /* releases the host refused (a locked allocation) */
};

/* One queue, as the mock knows it: its progress fence, the largest value the
 * winsys has published or asked the GPU to signal on it, the value of a
 * submission the kernel is holding right now, and whether a submit call of it
 * has yet returned. The winsys publishes the value to its own tracker BEFORE
 * the IB reaches the kernel (radv_wddm2_cs.c:1817) and to the host only after
 * the submit call returns (radv_wddm2_cs.c:1660, called at :1843, :1869 and
 * :1974), so the kernel's view of the value must come from the handoff, not
 * from the later publication, or a destroy inside that window would capture a
 * target of zero. */
struct queue_view {
   uint32_t fence;
   uint32_t context;
   uint64_t published;
   uint64_t pending;     /* the value of a submission handed to the kernel */
   unsigned in_submit;   /* submit calls of this queue that have not returned */
};

/* A BO the test destroyed, with the target set fixed at that destroy. The mock
 * checks every release call of it against this. */
struct watch {
   const char *where;
   uint32_t alloc;  /* the allocation handle, 0 once destroyed */
   uint64_t va[2];  /* base.va or reserved_va, and sparse_high_va */
   uint64_t size;
   uint64_t target[MAX_Q];
   unsigned n_target;
   bool evicted, va_freed;
   bool released; /* DestroyAllocation2 seen */
   bool assume_not_in_use; /* its Flags.AssumeNotInUse, which must be 0 */
   /* A borrowed host import names an allocation and an address that belong to
    * the host, and a plain BO of the test may name the same ones. Release
    * calls are the owner's, so they are matched against the owner's watch. */
   bool foreign;
   /* The class of the object (see the head of this file): R2 covers a virtual
    * BO's own reservation and a REPLAYABLE BO's replay address, R1 every other
    * internal BO. A borrowed import is neither: its allocation and address are
    * the host's. */
   bool r2;
   /* When each of the three calls first came, in release calls of the whole
    * run. There is no required order (see the head of this file); the sequence
    * only tells whether an Evict or a VA free FOLLOWED the destroy, which is a
    * call on a handle the OS may already have reused. */
   unsigned seq_evict, seq_va, seq_destroy;
};

struct release_note {
   const char *where;
   const char *op;
   unsigned queue;
   uint64_t completed, target;
};

static struct {
   struct obj fences[MAX_OBJ], allocs[MAX_OBJ];
   unsigned n_fences, n_allocs;
   struct queue_view queues[MAX_Q];
   unsigned n_queues;
   struct watch watch[MAX_WATCH];
   unsigned n_watch;
   struct release_note notes[32];
   unsigned n_notes;
   unsigned violations;

   unsigned n_ev; /* host calls, for "no host call happened" checks */
   unsigned evicts, va_frees, destroys, cpu_waits, creates;
   unsigned dead_alloc_use, dead_fence_use, unexpected;
   unsigned seq;          /* release calls so far, which orders them */
   unsigned assume_flag;  /* DestroyAllocation2 calls with AssumeNotInUse set */
   unsigned r1_evict;     /* Evicts of an R1 object, at any time (R1 allows none) */
   unsigned r1_va_free;   /* address frees of an R1 object, at any time */
   unsigned r2_evict;     /* Evicts of an R2 object, which the rule never allows */
   unsigned in_window;    /* releases issued while a submit call had not returned */
   unsigned va_reuse;     /* an address handed out again while its work may run */
   unsigned refusals, retries; /* kept list: refused releases and later attempts */
   unsigned after_destroy; /* Evict or VA free of a handle after its own destroy */
   uint32_t dead_fence_h; /* the first fence handle a call named that the mock does not know */

   uint64_t next_va;
   int32_t status; /* CHECK_STATUS; negative: the device is lost */

   /* A CPU wait that the GPU never completes: the fence does not advance and
    * the event is never set, so the winsys's bounded wait times out. */
   bool stuck_cpu_wait;
   uint64_t last_cpu_wait_value;
   uint32_t last_cpu_wait_fence;

   /* The failing call of fail_op. */
   uint32_t fail_op;
   int32_t fail_status;
   unsigned fail_left;

   /* Opened imports (OpenResourceFromNtHandle): the resource handle the mock
    * handed out and the allocation it holds, so a DestroyAllocation2 that
    * names the resource alone is matched to the right allocation. */
   uint32_t res_handle[8], res_alloc[8];
   unsigned n_res;

   /* What GetDeviceState reports, and how often the winsys asked. */
   uint32_t execution_state;
   unsigned device_state_asked;

   /* A host call parked inside the mock: park_op waits for park_release while
    * the test runs, so a submission is in the kernel and not finished. The
    * wait happens outside the mock's lock. */
   uint32_t park_op;
   unsigned park_left;
   HANDLE parked_in, park_release;

   DWORD thread;
} h;

static unsigned checks, failures;
static const char *current = "";

/* The mock's own state is one device's kernel state: a test that submits from
 * another thread while it destroys BOs would otherwise race the mock itself,
 * not the winsys. Every host call and every write of a fence value takes this
 * lock; no call of the mock waits for anything while it holds it, so a
 * deadlock of the winsys still shows as one. */
static CRITICAL_SECTION mock_lock;

static void
mock_enter(void)
{
   EnterCriticalSection(&mock_lock);
}

static void
mock_leave(void)
{
   LeaveCriticalSection(&mock_lock);
}

static void
check(bool ok, const char *fmt, ...)
{
   va_list ap;
   checks++;
   if (!ok)
      failures++;
   printf("%s %-20s ", ok ? "PASS" : "FAIL", current);
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
   fflush(stdout);
}

/* An observation the test records without judging it. */
static void
note(const char *fmt, ...)
{
   va_list ap;
   printf("NOTE %-20s ", current);
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
   fflush(stdout);
}

/* A requirement of the decided design that the old lines are not expected to
 * meet: an assertion against the frozen candidate, an observation otherwise.
 * The NOTE wording says which, so a V0 record states what the old code does
 * without counting it as a failure of the contract the review asked for. */
static void
v2_req(bool ok, const char *fmt, ...)
{
   va_list ap;
#if BD201_V2
   checks++;
   if (!ok)
      failures++;
   printf("%s %-20s ", ok ? "PASS" : "FAIL", current);
#else
   printf("NOTE %-20s a V2 requirement, %s here: ", current, ok ? "met" : "NOT met");
#endif
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
   fflush(stdout);
}

/* An observation of accounting or logging quality, never an assertion on any
 * version: the decided design does not require it, and whether it becomes a
 * requirement is the lead's call after bd201-fix's DESIGN-NOW.md (lead,
 * 2026-10-21 15:15Z, the G3 split). The line is printed so that a record of
 * any version says what the code does. */
static void
quality(bool ok, const char *fmt, ...)
{
   va_list ap;
   printf("NOTE %-20s accounting quality, %s: ", current, ok ? "met" : "not met");
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
   fflush(stdout);
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

static struct queue_view *
queue_of_fence(uint32_t fence)
{
   for (unsigned i = 0; i < h.n_queues; i++) {
      if (h.queues[i].fence == fence)
         return &h.queues[i];
   }
   return NULL;
}

static struct queue_view *
queue_of_context(uint32_t context)
{
   for (unsigned i = 0; i < h.n_queues; i++) {
      if (h.queues[i].context == context)
         return &h.queues[i];
   }
   return NULL;
}

/* Submissions in the kernel right now, over all queues. */
static unsigned
submit_window(void)
{
   unsigned n = 0;
   for (unsigned i = 0; i < h.n_queues; i++)
      n += h.queues[i].in_submit;
   return n;
}

/* The value the kernel can be asked about for this queue: what the winsys
 * published to the host, or the value of a submission it is holding. */
static uint64_t
queue_value(unsigned i)
{
   return MAX2(h.queues[i].published, h.queues[i].pending);
}

static struct queue_view *
new_queue_view(uint32_t fence, uint32_t context)
{
   if (h.n_queues >= MAX_Q)
      abort();
   struct queue_view *q = &h.queues[h.n_queues++];
   memset(q, 0, sizeof(*q));
   q->fence = fence;
   q->context = context;
   return q;
}

/* The winsys told the host which value this queue's work will reach. The view
 * may already exist from the handoff of its first submission, where only the
 * context handle was known, so a view of this context adopts the fence. */
static void
publish(uint32_t fence, uint32_t context, uint64_t value)
{
   struct queue_view *q = queue_of_fence(fence);
   if (!q && context)
      q = queue_of_context(context);
   if (!q)
      q = new_queue_view(fence, context);
   if (!q->fence)
      q->fence = fence;
   if (context)
      q->context = context;
   if (value > q->published)
      q->published = value;
}

/* The object a call names, or NULL. The winsys's paging fence is a handle the
 * mock did not create (ws->paging_fence_h), so a call naming it is expected and
 * counts as nothing. */
static struct obj *
fence_use(uint32_t handle)
{
   if (handle == PAGING_FENCE_H)
      return NULL;
   struct obj *o = find(h.fences, h.n_fences, handle);
   if (!o || !o->live) {
      h.dead_fence_use++;
      if (!h.dead_fence_h)
         h.dead_fence_h = handle;
      return NULL;
   }
   return o;
}

static uint64_t
completed(unsigned i)
{
   const struct obj *o = find(h.fences, h.n_fences, h.queues[i].fence);
   return o ? o->value : 0;
}

/* An allocation the OS is holding for its own deferral: its memory and its
 * address stay valid until the values of the destroy are reached. Called
 * before every host call and after every write of a fence value, so the
 * kernel's state never shows a destroyed object whose work still runs, and
 * never shows a live one whose work finished long ago. */
static void
mock_settle(void)
{
   for (unsigned i = 0; i < h.n_allocs; i++) {
      struct obj *o = &h.allocs[i];
      if (!o->pending_destroy || !o->live)
         continue;
      bool done = true;
      for (unsigned w = 0; w < h.n_watch && done; w++) {
         const struct watch *s = &h.watch[w];
         if (s->alloc != o->handle)
            continue;
         for (unsigned q = 0; q < s->n_target && q < h.n_queues; q++) {
            const uint64_t c = completed(q);
            if (c == UINT64_MAX || c < s->target[q])
               done = false;
         }
      }
      if (!done)
         continue;
      o->live = false;
      o->pending_destroy = false;
      free(o->mem);
      o->mem = NULL;
   }
}

/* Work of a watched allocation that has not completed: the OS's deferral of a
 * destroy lasts until there is none. */
static bool
watch_unproven(uint32_t alloc)
{
   for (unsigned w = 0; w < h.n_watch; w++) {
      const struct watch *s = &h.watch[w];
      if (s->alloc != alloc || s->foreign)
         continue;
      for (unsigned q = 0; q < s->n_target && q < h.n_queues; q++) {
         const uint64_t c = completed(q);
         if (c == UINT64_MAX || c < s->target[q])
            return true;
      }
   }
   return false;
}

/* Every release call of a watched BO is checked here, whoever made it. */
static void
release_call(const char *op, uint32_t alloc, uint64_t address)
{
   for (unsigned w = 0; w < h.n_watch; w++) {
      struct watch *s = &h.watch[w];
      const bool mine = !s->foreign && ((alloc && s->alloc == alloc) ||
                                        (address && (s->va[0] == address || s->va[1] == address)));
      if (!mine)
         continue;
      h.seq++;
      if (!strcmp(op, "Evict")) {
         s->evicted = true;
         if (!s->seq_evict)
            s->seq_evict = h.seq;
      } else if (!strcmp(op, "FreeGpuVirtualAddress")) {
         s->va_freed = true;
         if (!s->seq_va)
            s->seq_va = h.seq;
      } else {
         s->released = true;
         if (!s->seq_destroy)
            s->seq_destroy = h.seq;
      }
      /* The class rules of design R v2. An Evict or an address free of an R1
       * object is never part of its release; of an R2 object an Evict never
       * is. A release of any kind inside the submit window is a release
       * against work the OS has taken but not acknowledged (6.2). */
      if (!strcmp(op, "Evict")) {
         if (s->r2)
            h.r2_evict++;
         else
            h.r1_evict++;
      } else if (!strcmp(op, "FreeGpuVirtualAddress")) {
         if (!s->r2)
            h.r1_va_free++;
      }
      if (submit_window())
         h.in_window++;
      /* A destroy with AssumeNotInUse = 0 is the OS's own deferral, so it is
       * legal before the values complete; the other two calls are not. A call
       * on this handle AFTER its destroy is a call on a handle the OS may have
       * reused. */
      if (!strcmp(op, "DestroyAllocation2"))
         return;
      if (s->seq_destroy) {
         h.after_destroy++;
         if (h.n_notes < ARRAY_SIZE(h.notes))
            h.notes[h.n_notes++] = (struct release_note){s->where, op, MAX_Q, 0, 0};
      }
      for (unsigned i = 0; i < s->n_target && i < h.n_queues; i++) {
         const uint64_t done = completed(i);
         /* UINT64_MAX is the lost-device marker, not a value the GPU wrote, so
          * it is no proof that the work completed (design R v2, R4(c) in its
          * fence form rejected: native-gpu-fence-objects.md:46 is another
          * field). A release on it is a breach like any other. */
         if (done != UINT64_MAX && done >= s->target[i])
            continue;
         h.violations++;
         if (h.n_notes < ARRAY_SIZE(h.notes))
            h.notes[h.n_notes++] = (struct release_note){s->where, op, i, done, s->target[i]};
      }
      return;
   }
}

/* An address the kernel is about to hand to a new allocation. If it is inside
 * the range of a destroyed BO whose work has not completed, the OS has given
 * away an address the GPU may still read through (d3dkmthk.md:12632 says a
 * freed range may be taken immediately, which is why the free itself is
 * bounded; a range still in use must never be handed out). */
static void
address_handed_out(uint64_t base, uint64_t size)
{
   for (unsigned w = 0; w < h.n_watch; w++) {
      const struct watch *s = &h.watch[w];
      if (s->foreign || !watch_unproven(s->alloc))
         continue;
      for (unsigned i = 0; i < 2; i++) {
         if (!s->va[i] || !s->size)
            continue;
         if (base < s->va[i] + s->size && s->va[i] < base + MAX2(size, 1u)) {
            h.va_reuse++;
            if (h.n_notes < ARRAY_SIZE(h.notes))
               h.notes[h.n_notes++] = (struct release_note){s->where, "address handed out again", MAX_Q, base,
                                                            s->va[i]};
         }
      }
   }
}

static int32_t
dispatch_locked(void *userdata, uint32_t op, void *arg)
{
   (void)userdata;
   h.n_ev++;
   mock_settle();
   if (h.fail_op == op && h.fail_left) {
      h.fail_left--;
      return h.fail_status ? h.fail_status : HOST_FAIL;
   }

   switch (op) {
   case BC250_HOST_CHECK_STATUS:
      return h.status;
   case BC250_HOST_REPORT_LOST:
      return 0;
   case BC250_HOST_PUBLISH_PROGRESS: {
      const struct bc250_host_progress *p = arg;
      publish(p->sync, p->context, p->value);
      return 0;
   }
   case BC250_HOST_CreateContextVirtual: {
      D3DKMT_CREATECONTEXTVIRTUAL *c = arg;
      c->hContext = new_obj(h.fences, &h.n_fences, 0x1000)->handle; /* a handle space of its own */
      return 0;
   }
   case BC250_HOST_DestroyContext:
      return 0;
   case BC250_HOST_CreateSynchronizationObject2: {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *c = arg;
      struct obj *o = new_obj(h.fences, &h.n_fences, 0x2000);
      if (c->hDevice != DEVICE_H || c->Info.Type != D3DDDI_MONITORED_FENCE)
         h.unexpected++;
      c->hSyncObject = o->handle;
      c->Info.MonitoredFence.FenceValueCPUVirtualAddress = &o->value;
      /* No GPU address: the queue's progress fence takes the kernel signal,
       * which the mock sees and which fixes the published value. */
      c->Info.MonitoredFence.FenceValueGPUVirtualAddress = 0;
      return 0;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *d = arg;
      struct obj *o = fence_use(d->hSyncObject);
      if (o)
         o->live = false;
      return 0;
   }
   case BC250_HOST_SignalSynchronizationObjectFromGpu2: {
      const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *s = arg;
      for (unsigned i = 0; i < s->ObjectCount; i++) {
         fence_use(s->ObjectHandleArray[i]);
         /* The work in flight of that queue reaches this value. */
         if (queue_of_fence(s->ObjectHandleArray[i]))
            publish(s->ObjectHandleArray[i], s->BroadcastContextCount ? s->BroadcastContextArray[0] : 0,
                    s->MonitoredFenceValueArray[i]);
      }
      return 0;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromGpu:
      return 0;
   case BC250_HOST_SignalSynchronizationObjectFromCpu:
      return 0;
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      h.cpu_waits++;
      if (w->ObjectCount) {
         h.last_cpu_wait_fence = w->ObjectHandleArray[0];
         h.last_cpu_wait_value = w->FenceValueArray[0];
      }
      if (h.stuck_cpu_wait)
         return 0; /* accepted, never completed: the GPU made no progress */
      for (unsigned i = 0; i < w->ObjectCount; i++) {
         struct obj *o = fence_use(w->ObjectHandleArray[i]);
         if (!o)
            continue;
         if (o->value < w->FenceValueArray[i])
            o->value = w->FenceValueArray[i];
      }
      if (w->hAsyncEvent)
         SetEvent(w->hAsyncEvent);
      return 0;
   }
   case BC250_HOST_SubmitCommand:
      return 0;
   case BC250_HOST_UpdateGpuVirtualAddress:
      return 0;
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *c = arg;
      h.creates++;
      struct obj *o = new_obj(h.allocs, &h.n_allocs, 0x3000);
      c->pAllocationInfo2[0].hAllocation = o->handle;
      return 0;
   }
   case BC250_HOST_DestroyAllocation2: {
      const D3DKMT_DESTROYALLOCATION2 *d = arg;
      h.destroys++;
      /* Rule 2: only Flags.Value 0 makes VidMm defer the destruction until the
       * commands queued before this request finish (d3dukmdt.md:3398-3402).
       * The D3D12 shell's HostedDispatch refuses any other value. */
      if (d->Flags.Value)
         h.assume_flag++;
      /* An opened import is destroyed by its resource handle, with no
       * allocation list at all (radv_wddm2_bo.c:1418-1422). */
      if (!d->AllocationCount && d->hResource) {
         uint32_t alloc = 0;
         for (unsigned i = 0; i < h.n_res; i++) {
            if (h.res_handle[i] == d->hResource)
               alloc = h.res_alloc[i];
         }
         struct obj *o = find(h.allocs, h.n_allocs, alloc);
         if (!o || !o->live) {
            h.dead_alloc_use++;
            return 0;
         }
         for (unsigned w = 0; w < h.n_watch; w++) {
            if (h.watch[w].alloc == o->handle)
               h.watch[w].assume_not_in_use = !!d->Flags.AssumeNotInUse;
         }
         release_call("DestroyAllocation2", o->handle, 0);
         if (watch_unproven(o->handle)) {
            o->pending_destroy = true;
         } else {
            o->live = false;
            free(o->mem);
            o->mem = NULL;
         }
         return 0;
      }
      for (unsigned i = 0; i < d->AllocationCount; i++) {
         struct obj *o = find(h.allocs, h.n_allocs, d->phAllocationList[i]);
         if (!o || !o->live) {
            h.dead_alloc_use++;
            continue;
         }
         if (o->locked) {
            /* A host refuses to destroy a CPU-locked allocation, as the D3D12
             * shell's HostedDispatch does. Rule 3: the BO goes on a kept list
             * whole, with a counter, and is retried at a drain point. */
            h.refusals++;
            o->destroy_refusals++;
            return HOST_FAIL;
         }
         if (o->destroy_refusals)
            h.retries++;
         for (unsigned w = 0; w < h.n_watch; w++) {
            if (h.watch[w].alloc == o->handle)
               h.watch[w].assume_not_in_use = !!d->Flags.AssumeNotInUse;
         }
         release_call("DestroyAllocation2", o->handle, 0);
         /* VidMm defers the destruction of an allocation named in commands
          * queued before this request (d3dukmdt.md:3398-3402): its memory and
          * its address stay valid until that work completes, which is what
          * makes a destroy before the values legal in the first place. */
         if (watch_unproven(o->handle)) {
            o->pending_destroy = true;
         } else {
            o->live = false;
            free(o->mem);
            o->mem = NULL;
         }
      }
      return 0;
   }
   case BC250_HOST_Evict: {
      const D3DKMT_EVICT *e = arg;
      h.evicts++;
      for (unsigned i = 0; i < e->NumAllocations; i++) {
         struct obj *o = find(h.allocs, h.n_allocs, e->AllocationList[i]);
         if (!o || !o->live) {
            h.dead_alloc_use++;
            continue;
         }
         o->resident = false;
         release_call("Evict", o->handle, 0);
      }
      return 0;
   }
   case BC250_HOST_FreeGpuVirtualAddress: {
      const D3DKMT_FREEGPUVIRTUALADDRESS *f = arg;
      h.va_frees++;
      release_call("FreeGpuVirtualAddress", 0, f->BaseAddress);
      return 0;
   }
   case BC250_HOST_ReserveGpuVirtualAddress: {
      D3DDDI_RESERVEGPUVIRTUALADDRESS *r = arg;
      if (r->BaseAddress) {
         address_handed_out(r->BaseAddress, r->Size);
         r->VirtualAddress = r->BaseAddress;
      } else {
         h.next_va = MAX2(align64(h.next_va, 65536), align64(r->MinimumAddress, 65536));
         r->VirtualAddress = h.next_va;
         h.next_va += align64(r->Size, 65536);
      }
      return 0;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      if (m->BaseAddress) {
         address_handed_out(m->BaseAddress, m->SizeInPages * 4096ull);
         m->VirtualAddress = m->BaseAddress;
      } else {
         h.next_va = MAX2(align64(h.next_va, 65536), align64(m->MinimumAddress, 65536));
         m->VirtualAddress = h.next_va;
         h.next_va += align64(m->SizeInPages * 4096ull, 65536);
      }
      m->PagingFenceValue = 0;
      struct obj *o = find(h.allocs, h.n_allocs, m->hAllocation);
      if (o)
         o->va = m->VirtualAddress;
      return 0;
   }
   case BC250_HOST_MakeResident: {
      D3DDDI_MAKERESIDENT *r = arg;
      r->PagingFenceValue = 0;
      for (uint32_t i = 0; i < r->NumAllocations; i++) {
         struct obj *o = find(h.allocs, h.n_allocs, r->AllocationList[i]);
         if (o)
            o->resident = true;
      }
      return 0;
   }
   case BC250_HOST_Lock2: {
      D3DKMT_LOCK2 *l = arg;
      struct obj *o = find(h.allocs, h.n_allocs, l->hAllocation);
      if (!o || !o->live) {
         h.dead_alloc_use++;
         return HOST_FAIL;
      }
      o->locked = true;
      if (!o->mem)
         o->mem = calloc(1, LOCK_BYTES);
      l->pData = o->mem;
      return 0;
   }
   case BC250_HOST_Unlock2: {
      const D3DKMT_UNLOCK2 *u = arg;
      struct obj *o = find(h.allocs, h.n_allocs, u->hAllocation);
      if (!o || !o->live || !o->locked)
         h.unexpected++;
      if (o)
         o->locked = false;
      return 0;
   }
   case BC250_HOST_QueryResourceInfoFromNtHandle: {
      D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *q = arg;
      /* One allocation, and private driver data of exactly one LB7A v1
       * surface: the native linear-surface sharing contract the winsys
       * validates (radv_wddm2_bo.c:1048-1114). */
      q->NumAllocations = 1;
      q->TotalPrivateDriverDataSize = 32;
      q->ResourcePrivateDriverDataSize = 0;
      q->PrivateRuntimeDataSize = 0;
      return 0;
   }
   case BC250_HOST_OpenResourceFromNtHandle: {
      D3DKMT_OPENRESOURCEFROMNTHANDLE *o = arg;
      if (o->NumAllocations != 1 || !o->pOpenAllocationInfo2 || !o->pTotalPrivateDriverDataBuffer ||
          o->TotalPrivateDriverDataBufferSize < 32)
         return HOST_FAIL;
      struct bc250_linear_surface {
         uint32_t magic, version, width, height, pitch, format;
         uint64_t size;
      } surface = {
         .magic = 0x4137424c, /* 'LB7A' */
         .version = 1,
         .width = 256,
         .height = 256,
         .pitch = 1024,
         .format = D3DDDIFMT_A8R8G8B8,
         .size = 1024ull * 256,
      };
      memcpy(o->pTotalPrivateDriverDataBuffer, &surface, sizeof(surface));
      struct obj *a = new_obj(h.allocs, &h.n_allocs, 0x3000);
      o->pOpenAllocationInfo2[0].hAllocation = a->handle;
      o->pOpenAllocationInfo2[0].pPrivateDriverData = o->pTotalPrivateDriverDataBuffer;
      o->pOpenAllocationInfo2[0].PrivateDriverDataSize = sizeof(surface);
      o->hResource = 0x5001 + h.n_res;
      if (h.n_res < ARRAY_SIZE(h.res_handle)) {
         h.res_handle[h.n_res] = o->hResource;
         h.res_alloc[h.n_res] = a->handle;
         h.n_res++;
      }
      return 0;
   }
   case BC250_HOST_GetDeviceState: {
      D3DKMT_GETDEVICESTATE *g = arg;
      h.device_state_asked++;
      if (g->StateType == D3DKMT_DEVICESTATE_EXECUTION)
         g->ExecutionState = h.execution_state ? (D3DKMT_DEVICEEXECUTION_STATE)h.execution_state
                                               : D3DKMT_DEVICEEXECUTION_ACTIVE;
      return 0;
   }
   default:
      h.unexpected++;
      return HOST_UNEXPECTED;
   }
}

static int32_t
dispatch(void *userdata, uint32_t op, void *arg)
{
   /* A parked call waits OUTSIDE the mock's lock: the point of parking is to
    * leave a submission in the kernel while the test destroys a BO, not to
    * stop the mock from answering the other thread. */
   mock_enter();
   const bool park = h.park_op == op && h.park_left;
   if (park)
      h.park_left--;
   HANDLE parked_in = h.parked_in, release = h.park_release;
   /* The submit window: from the moment the kernel is given a submission until
    * the call returns. The value it will signal is the queue's next one, which
    * is what the winsys published to its own tracker before this call
    * (radv_wddm2_cs.c:1817), so the kernel's view of the queue rises here and
    * not at the later publication to the host. */
   struct queue_view *sq = NULL;
   if (op == (uint32_t)BC250_HOST_SubmitCommand) {
      const D3DKMT_SUBMITCOMMAND *s = arg;
      const uint32_t ctx = s->BroadcastContextCount ? s->BroadcastContext[0] : 0;
      /* The first submission of a queue reaches the kernel before the winsys
       * has told the host anything about it, so the view is created here. Its
       * fence is unknown until the publication, and nothing has completed on
       * an unknown fence, which is the honest kernel state. */
      sq = ctx ? queue_of_context(ctx) : NULL;
      if (!sq && ctx)
         sq = new_queue_view(0, ctx);
      if (sq) {
         sq->in_submit++;
         if (sq->pending <= sq->published)
            sq->pending = sq->published + 1;
      }
   }
   mock_leave();
   if (park) {
      if (parked_in)
         SetEvent(parked_in);
      if (release)
         WaitForSingleObject(release, 30000);
   }
   mock_enter();
   const int32_t status = dispatch_locked(userdata, op, arg);
   if (sq) {
      sq->in_submit--;
      /* A submission the kernel rejected will never signal its value, so the
       * kernel's view of the queue goes back to what was published. */
      if (status < 0)
         sq->pending = sq->published;
   }
   mock_leave();
   return status;
}

/* ------------------------------------------------------------------ *
 * The winsys under test
 * ------------------------------------------------------------------ */

static struct radv_wddm2_winsys *
make_ws(void)
{
   memset(&h, 0, sizeof(h));
   memset(&alloc_hook, 0, sizeof(alloc_hook));
   h.thread = GetCurrentThreadId();

   struct radv_wddm2_winsys *ws = calloc(1, sizeof(*ws));
   if (!ws)
      abort();
   ws->bc250 = true;
   ws->host.sType = BC250_HOST_STYPE;
   ws->host.version = BC250_HOST_VERSION;
   ws->host.size = sizeof(ws->host);
   ws->host.identity = (void *)(uintptr_t)0x1;
   ws->host.userdata = &h;
   ws->host.dispatch = dispatch;
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

static struct radeon_winsys_ctx *
new_ctx(struct radv_wddm2_winsys *ws)
{
   struct radeon_winsys_ctx *ctx = NULL;
   if (ws->base.ctx_create(&ws->base, RADEON_CTX_PRIORITY_MEDIUM, &ctx) != VK_SUCCESS || !ctx) {
      fprintf(stderr, "ctx_create failed\n");
      exit(2);
   }
   return ctx;
}

static struct radv_wddm2_queue *
gfx(struct radeon_winsys_ctx *ctx)
{
   return &radv_wddm2_ctx(ctx)->per_ip[AMD_IP_GFX].queue;
}

/* Laid out as radv_wddm2_cs: the stream, then its BO set (none here). The IB
 * names a stand-in BO that holds it exactly, because the system line refuses
 * an IB outside its BO before the kernel (BD-207). No line maps that BO on the
 * one-IB path, and the mock never sees it. The stand-in is a whole
 * radv_wddm2_bo: the deferred-destroy witness writes its last-use fields on
 * every submit, and a bare radeon_winsys_bo let it write past the struct. */
struct fake_cs {
   struct radv_winsys_cs cs;
   struct set *buffers;
   struct radv_winsys_ib ib;
   struct radv_wddm2_bo ib_bo;
};

static struct ac_cmdbuf *
fake_cs(struct fake_cs *f, uint64_t va, unsigned cdw)
{
   memset(f, 0, sizeof(*f));
   f->ib_bo.base.va = va;
   f->ib_bo.base.size = (uint64_t)cdw * 4;
   f->ib.bo = &f->ib_bo.base;
   f->ib.va = va;
   f->ib.cdw = cdw;
   f->cs.ib_buffers = &f->ib;
   f->cs.num_ib_buffers = 1;
   f->cs.hw_ip = AMD_IP_GFX;
   return &f->cs.base;
}

static VkResult
submit_one(struct radv_wddm2_winsys *ws, struct radeon_winsys_ctx *ctx)
{
   struct fake_cs f;
   struct ac_cmdbuf *cs = fake_cs(&f, 0x100000, 8);
   const struct radv_winsys_submit_info info = {
      .ip_type = AMD_IP_GFX,
      .cs_count = 1,
      .cs_array = &cs,
   };
   return ws->base.cs_submit(ctx, &info, 0, NULL, 0, NULL);
}

static struct radeon_winsys_bo *
new_bo(struct radv_wddm2_winsys *ws, uint64_t size)
{
   struct radeon_winsys_bo *bo = NULL;
   if (ws->base.buffer_create(&ws->base, size, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &bo) !=
          VK_SUCCESS ||
       !bo) {
      fprintf(stderr, "buffer_create failed\n");
      exit(2);
   }
   return bo;
}

static bool
alloc_live(uint32_t handle)
{
   const struct obj *o = find(h.allocs, h.n_allocs, handle);
   return o && o->live;
}

static bool
alloc_resident(uint32_t handle)
{
   const struct obj *o = find(h.allocs, h.n_allocs, handle);
   return o && o->resident;
}

/* The GPU completed value of a queue's progress fence; the test writes it to
 * retire that queue's work. */
static void
retire(struct radv_wddm2_queue *q, uint64_t value)
{
   mock_enter();
   struct obj *o = find(h.fences, h.n_fences, q->bc250_progress.handle);
   if (!o)
      abort();
   o->value = value;
   mock_settle();
   mock_leave();
}

/* The same, by fence handle, for a queue whose context is already gone: the
 * mock keeps the kernel state the winsys left behind. */
static void
retire_fence(uint32_t fence, uint64_t value)
{
   mock_enter();
   struct obj *o = find(h.fences, h.n_fences, fence);
   if (!o)
      abort();
   o->value = value;
   mock_settle();
   mock_leave();
}

static bool
fence_live(uint32_t fence)
{
   mock_enter();
   const struct obj *o = find(h.fences, h.n_fences, fence);
   const bool live = o && o->live;
   mock_leave();
   return live;
}

static uint64_t
completed_of_fence(uint32_t fence)
{
   mock_enter();
   const struct obj *o = find(h.fences, h.n_fences, fence);
   const uint64_t value = o ? o->value : 0;
   mock_leave();
   return value;
}

static uint64_t
published_of_fence(uint32_t fence)
{
   mock_enter();
   const struct queue_view *v = queue_of_fence(fence);
   const uint64_t value = v ? v->published : 0;
   mock_leave();
   return value;
}

/* Every CPU wait of the winsys fails while this is on. A failed wait is not
 * proof that the work completed (design R v2, R4), so a release after one is a
 * breach wherever it happens, teardown included. */
static void
fail_cpu_waits(bool on)
{
   mock_enter();
   h.fail_op = on ? (uint32_t)BC250_HOST_WaitForSynchronizationObjectFromCpu : 0u;
   h.fail_status = HOST_FAIL;
   h.fail_left = on ? (1u << 20) : 0u;
   mock_leave();
}

/* The next call of one host operation fails. R6 needs a rejected submit and a
 * failed GPU signal, each exactly once. */
static void
fail_once(uint32_t op)
{
   mock_enter();
   h.fail_op = op;
   h.fail_status = HOST_FAIL;
   h.fail_left = 1;
   mock_leave();
}

static unsigned
live_allocs(void)
{
   unsigned live = 0;
   mock_enter();
   for (unsigned i = 0; i < h.n_allocs; i++)
      live += h.allocs[i].live;
   mock_leave();
   return live;
}

static uint64_t
published_of(struct radv_wddm2_queue *q)
{
   mock_enter();
   const struct queue_view *v = queue_of_fence(q->bc250_progress.handle);
   const uint64_t value = v ? v->published : 0;
   mock_leave();
   return value;
}

/* Destroys a BO and fixes the target set of the guarantee at that moment:
 * every queue's published value, as the winsys itself declared it. */
static struct watch *
destroy_bo(struct radv_wddm2_winsys *ws, struct radeon_winsys_bo *bo, const char *where)
{
   mock_enter();
   if (h.n_watch >= MAX_WATCH)
      abort();
   struct radv_wddm2_bo *w = radv_wddm2_bo(bo);
   struct watch *s = &h.watch[h.n_watch++];
   memset(s, 0, sizeof(*s));
   s->where = where;
   s->alloc = bo->handle;
   s->va[0] = w->reserved_va ? w->reserved_va : bo->va;
   s->va[1] = w->sparse_high_va;
   s->size = bo->size;
   s->foreign = w->borrowed;
   /* R2: the reservation of a virtual BO and the replay address of a
    * REPLAYABLE one are ours to free, and nobody frees them for us. */
   s->r2 = bo->is_virtual || w->reserved_va || w->sparse_high_va || (w->flags & RADEON_FLAG_REPLAYABLE);
   s->n_target = h.n_queues;
   for (unsigned i = 0; i < h.n_queues; i++)
      s->target[i] = queue_value(i);
   mock_leave();
   ws->base.buffer_destroy(&ws->base, bo);
   return s;
}

/* Every host call of the run holds the contract; counted once per test. */
static void
contract(void)
{
   check(!h.violations, "rule 1: no Evict and no VA free of a destroyed BO before its destroy-time values, teardown "
                        "included (%u breaches)",
         h.violations);
   for (unsigned i = 0; i < h.n_notes; i++) {
      const struct release_note *n = &h.notes[i];
      if (n->queue == MAX_Q)
         printf("     breach: %s %s of the BO after its own DestroyAllocation2\n", n->where, n->op);
      else
         printf("     breach: %s %s queue %u completed %" PRIu64 " < target %" PRIu64 "\n", n->where, n->op, n->queue,
                n->completed, n->target);
   }
   check(!h.assume_flag && !h.after_destroy,
         "rule 2: every DestroyAllocation2 had flags 0, and no Evict or VA free followed one (%u flagged, %u after)",
         h.assume_flag, h.after_destroy);
   check(!h.r2_evict, "rule 2 (R2): no Evict of a virtual reservation or a replay address, ever (%u)", h.r2_evict);
   check(!h.va_reuse, "no address was handed out again while the work that may use it had not completed (%u)",
         h.va_reuse);
   if (h.refusals)
      note("kept list: the host refused %u release(s); %u later attempt(s) on the same allocation", h.refusals,
           h.retries);
   v2_req(!h.r1_evict && !h.r1_va_free,
          "rule 1 (R1): an internal BO is released by DestroyAllocation2 alone (%u Evicts, %u address frees)",
          h.r1_evict, h.r1_va_free);
#if BD201_V2
   /* The decided design (6.7) needs no handoff wait: every submission that
    * names an internal BO is handed to the OS before the BO's destroy (per-queue
    * order I1 and the client lifetime contract I2 of the order proof). No
    * submission of this test names a BO it destroys, so a DestroyAllocation2
    * while another submission is inside SubmitCommand is legal here. An Evict
    * or an address free there is still a breach of rule 1 above. */
   if (h.in_window)
      note("%u release call(s) came while a submission that does not name the BO had not returned from the "
           "host's SubmitCommand (legal under I1 and I2)",
           h.in_window);
#else
   check(!h.in_window,
         "rule 1: nothing was released while a submission had not returned from the host's SubmitCommand (%u)",
         h.in_window);
#endif
   check(!h.unexpected && !h.dead_alloc_use && !h.dead_fence_use,
         "no unknown host call, no call naming a destroyed object (%u unknown, %u allocations, %u fences, first "
         "fence handle 0x%X)",
         h.unexpected, h.dead_alloc_use, h.dead_fence_use, h.dead_fence_h);
   fflush(stdout);
}

/* ------------------------------------------------------------------ *
 * A watchdog, so that a wait that never ends fails instead of hanging
 * ------------------------------------------------------------------ */

static HANDLE watchdog_done;
static volatile LONG watchdog_ms;
static const char *watchdog_what;

static DWORD WINAPI
watchdog_thread(void *arg)
{
   (void)arg;
   if (WaitForSingleObject(watchdog_done, (DWORD)watchdog_ms) == WAIT_TIMEOUT) {
      printf("FAIL %-20s %s did not return within %ld ms: a deadlock or an unbounded wait\n", current,
             watchdog_what, (long)watchdog_ms);
      printf("tests=%u failures=%u\n", checks + 1, failures + 1);
      fflush(stdout);
      ExitProcess(1);
   }
   return 0;
}

static HANDLE
watchdog_start(const char *what, unsigned ms)
{
   watchdog_done = CreateEventW(NULL, TRUE, FALSE, NULL);
   watchdog_ms = (LONG)ms;
   watchdog_what = what;
   HANDLE t = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
   if (!t)
      abort();
   return t;
}

static void
watchdog_stop(HANDLE t)
{
   SetEvent(watchdog_done);
   WaitForSingleObject(t, INFINITE);
   CloseHandle(t);
   CloseHandle(watchdog_done);
   watchdog_done = NULL;
}

/* ------------------------------------------------------------------ *
 * The tests
 * ------------------------------------------------------------------ */

/* The plain case of BD-201, with no injection at all: a BO destroyed while a
 * submission of the queue is in flight. The guarantee alone decides. */
static void
test_in_flight(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);

   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   check(submit_one(ws, a) == VK_SUCCESS && published_of(qa) == 1,
         "the queue submitted; it published progress 1 (completed %" PRIu64 ")",
         *qa->bc250_progress.value_map);

   const unsigned evicts = h.evicts, va_frees = h.va_frees, waits = h.cpu_waits;
   struct watch *s = destroy_bo(ws, bo, "in_flight");
   check(!s->evicted && !s->va_freed, "the destroy evicted nothing and freed no VA (%u evicts, %u VA frees)",
         h.evicts - evicts, h.va_frees - va_frees);
   /* RADV destroys replaced scratch and ring BOs on the submit path, so a
    * destroy that can hold the BO may not wait for the GPU there. */
   check(h.cpu_waits == waits, "and it made no CPU wait on the submit path (%u)", h.cpu_waits - waits);
   if (s->released)
      note("the allocation went to the OS's own deferral at the destroy (flags 0, AssumeNotInUse=%d)",
           s->assume_not_in_use);
   else
      check(alloc_live(handle) && alloc_resident(handle), "the allocation is live and resident");

   /* An unrelated later submission must not evict it or free its VA either. */
   check(submit_one(ws, a) == VK_SUCCESS, "the queue submits again, progress 2");
   check(!s->evicted && !s->va_freed, "still no Evict and no VA free: progress 1 has not retired");

   retire(qa, 1);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue submits once progress 1 retired");
   check(s->released && !alloc_live(handle), "the BO's allocation is destroyed by then");

   /* Design R v2, R1: an internal, non-virtual BO is released through the OS
    * alone. DestroyAllocation2 with flags 0 already makes VidMm hold the
    * allocation until the commands queued before it finish, so V2 needs no
    * Evict and no VA free of such a BO at all, at any time: the address range
    * of an API-allocated mapping goes with the allocation (d3dkmthk.md:13810)
    * and an Evict only takes residency the OS can take itself. */
   v2_req(!s->evicted && !s->va_freed,
          "R1: the BO was released through DestroyAllocation2 alone, with no Evict (%d) and no VA free (%d)",
          s->evicted, s->va_freed);
   v2_req(s->released && !s->assume_not_in_use, "and that destroy carried AssumeNotInUse 0 (released %d, flag %d)",
          s->released, s->assume_not_in_use);

   retire(qa, 3);
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* The same, with every allocation of the winsys refused for the duration of
 * the destroy call: the destroy path may not need host memory to keep the
 * guarantee (BD-201, the hole of the hosted deferral). */
static void
test_no_host_memory(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);

   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   check(submit_one(ws, a) == VK_SUCCESS && published_of(qa) == 1 && !*qa->bc250_progress.value_map,
         "the queue published progress 1, completed 0");

   HANDLE dog = watchdog_start("a destroy with no host memory", 30000);
   arm_allocator();
   struct watch *s = destroy_bo(ws, bo, "no_host_memory");
   const unsigned attempts = disarm_allocator();
   watchdog_stop(dog);
   note("the destroy made %u allocation attempts, all refused", attempts);

   /* Either route is right: hold the BO, or wait for the published value
    * inside the destroy and release it after that. What may not happen is a
    * release before the value, which the mock counts. */
   check(!h.violations, "neither Evict nor VA free of the BO before A's published 1 (%u breaches, completed %" PRIu64
                        ")",
         h.violations, *qa->bc250_progress.value_map);
   if (s->evicted || s->va_freed)
      check(*qa->bc250_progress.value_map >= 1,
            "an Evict or a VA free happened only after the value (completed %" PRIu64 ")",
            *qa->bc250_progress.value_map);
   else if (s->released)
      note("the allocation went to the OS's own deferral with no Evict and no VA free (AssumeNotInUse=%d)",
           s->assume_not_in_use);
   else
      check(alloc_live(handle), "the BO keeps its allocation and its VA");

   /* Whatever the route, the BO is gone once the queue retires. */
   retire(qa, 1);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue submits once progress 1 retired");
   check(!alloc_live(handle) && s->released, "the BO is released by then");

   retire(qa, 2);
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* If the fix reserves the hold at BO creation, a refused reservation must fail
 * the create cleanly, with no allocation, VA or residency left behind. If it
 * reserves nothing, the create succeeds: the test records which it is. */
static void
test_create_rollback(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue submitted; progress 1 is in flight");

   const unsigned live_before = 0u + h.n_allocs;
   const unsigned creates_before = h.creates, destroys_before = h.destroys;
   struct radeon_winsys_bo *bo = NULL;
   arm_allocator();
   const VkResult result =
      ws->base.buffer_create(&ws->base, 65536, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &bo);
   const unsigned attempts = disarm_allocator();

   if (result == VK_SUCCESS) {
      note("buffer_create needs no allocation of its own (%u attempts refused): nothing to roll back", attempts);
      check(bo != NULL, "the create succeeded");
      struct watch *s = destroy_bo(ws, bo, "create_rollback");
      check(!s->evicted && !s->va_freed, "and its destroy keeps the guarantee");
      retire(qa, 1);
      check(submit_one(ws, a) == VK_SUCCESS && s->released, "released once progress 1 retired");
   } else {
      note("buffer_create allocates (%u attempts refused) and failed with %d", attempts, (int)result);
      check(result == VK_ERROR_OUT_OF_HOST_MEMORY, "a refused host allocation fails the create with "
                                                   "VK_ERROR_OUT_OF_HOST_MEMORY (%d)",
            (int)result);
      check(!bo, "no BO is returned");
      unsigned live = 0;
      for (unsigned i = live_before; i < h.n_allocs; i++)
         live += h.allocs[i].live;
      check(!live, "nothing is left behind: no live kernel allocation, no VA, no residency (%u created, %u "
                   "destroyed, %u live)",
            h.creates - creates_before, h.destroys - destroys_before, live);
   }

   retire(qa, 2);
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* Two queues, one retired: the BO waits for the other one's destroy-time
 * value as well. */
static void
test_two_queues(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);

   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS,
         "both queues submitted; each published progress 1");
   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   struct watch *s = destroy_bo(ws, bo, "two_queues");
   check(!s->evicted && !s->va_freed, "the destroy evicted nothing and freed no VA");

   retire(qa, 1);
   check(ws->base.ctx_wait_idle(a, AMD_IP_GFX, 0), "queue A waits idle");
   check(!s->evicted && !s->va_freed, "A alone is not enough: B's value is in the target set");

   retire(qb, 1);
   check(ws->base.ctx_wait_idle(b, AMD_IP_GFX, 0), "queue B waits idle");
   check(s->released && !alloc_live(handle), "then the BO is released");

   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
}

/* The target set is fixed at the destroy: while the BO is held, another queue
 * keeps submitting with rising values, and the BO is released once the
 * destroy-time values are reached, not later. */
static void
test_fixed_target(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);

   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS, "A and B each published progress 1");
   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   struct watch *s = destroy_bo(ws, bo, "fixed_target");
   check(s->n_target >= 2 && s->target[0] == 1 && s->target[1] == 1, "the target set is A 1, B 1");

   /* B works on, A does not. The target set must not grow with B's new work. */
   for (unsigned i = 0; i < 4; i++) {
      check(submit_one(ws, b) == VK_SUCCESS, "B submits again (progress %u)", i + 2);
      retire(qb, i + 2);
   }
   check(!s->evicted && !s->va_freed, "the BO still waits for A's 1");
   check(published_of(qb) == 5, "B has published 5 by now");

   retire(qa, 1);
   check(submit_one(ws, b) == VK_SUCCESS, "a drain point once A's 1 retired");
   check(s->released && !alloc_live(handle),
         "the BO is released there, at the destroy-time values and no later");

   retire(qb, 6);
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
}

/* A device that makes no progress: the fence never advances, so every CPU wait
 * of the winsys times out. A destroy must return within a bound, and neither
 * the destroy nor the teardown may evict the BO or free its VA: a timeout is
 * not proof that the work completed (design R v2, R4 and R5; lead's correction
 * of 2026-10-21). An allocation handed to the OS's deferred destroy with
 * flags 0 is legal here, because VidMm then waits for the queued commands
 * itself. */
static void
test_device_lost(void)
{
   const uint64_t slice = radv_wddm2_fence_wait_slice_ns, total = radv_wddm2_fence_wait_total_ns;
   radv_wddm2_fence_wait_slice_ns = 20000000ull;  /* 20 ms */
   radv_wddm2_fence_wait_total_ns = 200000000ull; /* 200 ms */

   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1");
   h.stuck_cpu_wait = true;

   struct radeon_winsys_bo *plain = new_bo(ws, 65536);
   const uint32_t plain_h = plain->handle;
   HANDLE dog = watchdog_start("a destroy with a stuck fence", 30000);
   struct watch *p = destroy_bo(ws, plain, "device_lost/plain");
   watchdog_stop(dog);
   check(!p->evicted && !p->va_freed, "a destroy with a stuck fence returns, evicts nothing and frees no VA");
   if (!p->released)
      check(alloc_live(plain_h) && alloc_resident(plain_h), "and the allocation stays live and resident");

   struct radeon_winsys_bo *starved = new_bo(ws, 65536);
   const uint32_t starved_h = starved->handle;
   dog = watchdog_start("a destroy with a stuck fence and no host memory", 30000);
   arm_allocator();
   struct watch *q = destroy_bo(ws, starved, "device_lost/no_memory");
   const unsigned attempts = disarm_allocator();
   watchdog_stop(dog);
   note("the destroy made %u allocation attempts and %u CPU waits", attempts, h.cpu_waits);
   check(!q->evicted && !q->va_freed, "with no host memory either, it evicts nothing and frees no VA");
   if (!q->released)
      check(alloc_live(starved_h) && alloc_resident(starved_h), "and that allocation stays live and resident too");
   check(!h.violations, "no release before the destroy-time values while the device runs (%u breaches)",
         h.violations);
   for (unsigned i = 0; i < h.n_notes; i++)
      printf("     breach: %s %s queue %u completed %" PRIu64 " < target %" PRIu64 "\n", h.notes[i].where,
             h.notes[i].op, h.notes[i].queue, h.notes[i].completed, h.notes[i].target);
   const unsigned before_teardown = h.violations;

#if BD201_DEFERRED_API
   /* Teardown with the fence still stuck. This is not an exemption: the hosted
    * line does not call DestroyDevice at all (radv_wddm2_winsys.c:796), and on
    * the lines that do, deferred_finish's Evict and FreeGpuVirtualAddress come
    * before it. An unproven range is leaked, which costs only VA. */
   dog = watchdog_start("teardown with a stuck fence", 60000);
   radv_wddm2_deferred_finish(ws);
   watchdog_stop(dog);
   note("teardown with a stuck fence: plain BO evicted=%d va_freed=%d released=%d; "
        "no-memory BO evicted=%d va_freed=%d released=%d; %u CPU waits in all",
        p->evicted, p->va_freed, p->released, q->evicted, q->va_freed, q->released, h.cpu_waits);
   check(!p->evicted && !p->va_freed && !q->evicted && !q->va_freed,
         "teardown is no exemption: neither BO is evicted and no VA of theirs is freed without proof");
   check(h.violations == before_teardown, "and no new breach at teardown (%u)", h.violations - before_teardown);
#endif

   h.stuck_cpu_wait = false;
   retire(qa, 1);
   ws->base.ctx_destroy(a);
   check(!h.unexpected && !h.dead_alloc_use && !h.dead_fence_use,
         "no unknown host call, no use of a dead object (%u/%u/%u, first fence handle 0x%X)", h.unexpected,
         h.dead_alloc_use, h.dead_fence_use, h.dead_fence_h);
   free(ws);
   radv_wddm2_fence_wait_slice_ns = slice;
   radv_wddm2_fence_wait_total_ns = total;
}

/* The deadlock detector: the destroy runs on one thread while another thread
 * submits on a second queue, so a synchronous wait inside the destroy competes
 * with a submitter for the winsys's own locks. The watchdog fails the test
 * instead of letting it hang. */
struct racer {
   struct radv_wddm2_winsys *ws;
   struct radeon_winsys_ctx *ctx;
   struct radv_wddm2_queue *queue;
   volatile LONG stop;
   unsigned submits;
   HANDLE ready;
};

static DWORD WINAPI
racer_thread(void *arg)
{
   struct racer *r = arg;
   SetEvent(r->ready);
   while (!InterlockedCompareExchange(&r->stop, 0, 0)) {
      if (submit_one(r->ws, r->ctx) != VK_SUCCESS)
         break;
      r->submits++;
      retire(r->queue, r->submits);
   }
   return 0;
}

static void
test_wait_deadlock(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   check(submit_one(ws, a) == VK_SUCCESS, "A published progress 1, and keeps it in flight");

   struct racer r = {ws, b, qb, 0, 0, CreateEventW(NULL, TRUE, FALSE, NULL)};
   HANDLE t = CreateThread(NULL, 0, racer_thread, &r, 0, NULL);
   if (!t || WaitForSingleObject(r.ready, 5000) != WAIT_OBJECT_0)
      abort();

   HANDLE dog = watchdog_start("destroys racing a submitter", 60000);
   unsigned held = 0;
   for (unsigned i = 0; i < 64; i++) {
      struct radeon_winsys_bo *bo = new_bo(ws, 65536);
      arm_allocator(); /* the synchronous route, which is the one that can block */
      struct watch *s = destroy_bo(ws, bo, "wait_deadlock");
      disarm_allocator();
      held += !s->released;
   }
   InterlockedExchange(&r.stop, 1);
   WaitForSingleObject(t, 30000);
   watchdog_stop(dog);
   CloseHandle(t);
   CloseHandle(r.ready);
   check(true, "64 destroys raced %u submissions of another queue without a deadlock (%u held)", r.submits, held);

   retire(qa, published_of(qa));
   retire(qb, published_of(qb));
   check(submit_one(ws, a) == VK_SUCCESS, "a drain point with everything retired");
   unsigned unreleased = 0;
   for (unsigned i = 0; i < h.n_watch; i++)
      unreleased += !h.watch[i].released;
   check(!unreleased, "every destroyed BO is released once its work retired (%u left)", unreleased);

   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
}

/* Teardown. Two phases in one winsys:
 *  - with proof available, a destroyed BO really goes: the test must not
 *    accept a leak where the work retired;
 *  - then a teardown whose every CPU wait FAILS. A failed wait is not proof
 *    (design R v2, R4), so nothing of the two unproven BOs may be evicted and
 *    no VA of theirs may be freed. Their ranges are leaked instead, which
 *    costs only VA, and the hosted line keeps the host's device anyway. */
static void
test_teardown(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1");

   /* Phase 1: proof arrives, so the BO must not be kept. */
   struct radeon_winsys_bo *proven = new_bo(ws, 65536);
   const uint32_t proven_h = proven->handle;
   struct watch *done = destroy_bo(ws, proven, "teardown/proven");
   retire(qa, 1);
   check(submit_one(ws, a) == VK_SUCCESS, "a drain point once progress 1 retired");
   check(done->released && !alloc_live(proven_h), "the proven BO is released, not leaked");

   /* Phase 2: two BOs whose work never retires, and waits that fail. */
   struct radeon_winsys_bo *held = new_bo(ws, 65536);
   struct watch *s = destroy_bo(ws, held, "teardown/held");

   struct radeon_winsys_bo *starved = new_bo(ws, 65536);
   arm_allocator();
   struct watch *q = destroy_bo(ws, starved, "teardown/no_memory");
   const unsigned attempts = disarm_allocator();
   note("the second destroy made %u allocation attempts, all refused", attempts);
   fail_cpu_waits(true);

#if BD201_DEFERRED_API
   HANDLE dog = watchdog_start("teardown whose waits fail", 60000);
   radv_wddm2_deferred_finish(ws);
   watchdog_stop(dog);
   note("teardown with failing waits: held evicted=%d va_freed=%d released=%d; kept evicted=%d va_freed=%d "
        "released=%d",
        s->evicted, s->va_freed, s->released, q->evicted, q->va_freed, q->released);
   check(!s->evicted && !s->va_freed && !q->evicted && !q->va_freed,
         "a teardown whose waits fail evicts neither BO and frees no VA of theirs");
#if BD201_BO_POOL
   radv_wddm2_bo_pool_finish(ws);
#endif
#else
   note("this line has no deferred-destroy teardown entry point");
   check(!s->evicted && !s->va_freed && !q->evicted && !q->va_freed,
         "no Evict and no VA free of the two unproven BOs");
#endif
   fail_cpu_waits(false);

   note("%u of the mock's allocations are still live at the end (an unproven BO may keep its own)", live_allocs());
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* BD-212, review finding C. A queue is destroyed while its published value is
 * unretired, so its tracker detaches with work outstanding. A BO destroyed
 * AFTER that must still wait for that queue's value: the GPU may still run the
 * submissions that named it.
 *
 * Expected: V0 and V1 fail here. Both enumerate ws->deferred.trackers to find
 * what to wait for, and radv_wddm2_tracker_detach dropped the abandoned
 * queue's last reference (ctx_destroy -> tracker_detach, refs 1 -> 0, list_del
 * and FREE), so the destroy sees no queue at all, waits for nothing and calls
 * radv_wddm2_bo_destroy_now. No entry failure is needed: this is the shipped
 * code's own path. */
static void
test_abandoned_queue(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS && published_of(qa) == 1,
         "queue A published progress 1 and keeps it in flight (completed %" PRIu64 ")",
         *qa->bc250_progress.value_map);
   const uint32_t fence_a = qa->bc250_progress.handle;

   /* The application destroys the context with that work outstanding. The
    * queue's own wait inside ctx_destroy fails, which is what leaves the
    * kernel objects to the device ("abandoned") and detaches the tracker while
    * its published value is unretired. */
   fail_cpu_waits(true);
   ws->base.ctx_destroy(a);
   fail_cpu_waits(false);
   check(fence_live(fence_a) && published_of_fence(fence_a) == 1 && completed_of_fence(fence_a) == 0,
         "the context is gone, its progress fence lives on at published 1, completed 0");

   /* Now an internal BO is destroyed, as radv_update_preambles does. */
   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   struct watch *s = destroy_bo(ws, bo, "abandoned_queue");
   check(s->n_target >= 1 && s->target[0] == 1, "its target set still holds the abandoned queue's 1");
   check(!s->evicted && !s->va_freed, "the destroy evicts nothing and frees no VA while that value is unreached");

   /* A later drain point must not release it either. */
   struct radeon_winsys_ctx *b = new_ctx(ws);
   check(submit_one(ws, b) == VK_SUCCESS, "another queue submits, which is a drain point");
   check(!s->evicted && !s->va_freed, "still no Evict and no VA free of it");

   /* Once the abandoned queue's value completes, a release is allowed. */
   retire_fence(fence_a, 1);
   check(submit_one(ws, b) == VK_SUCCESS, "a drain point once the abandoned queue's 1 completed");
   if (s->released)
      note("the BO was released after the proof (evicted=%d va_freed=%d)", s->evicted, s->va_freed);
   else
      note("the BO is still held after the proof (allocation live=%d)", alloc_live(handle));

   retire(gfx(b), published_of(gfx(b)));
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
}

/* Review finding A against the rejected first fix (V1). A BO that the winsys
 * KEEPS because its destroy could neither record nor prove anything must not
 * be released by a later "the device looks idle" walk.
 *
 * The sequence: queue A publishes 1 and never retires it; BO X is destroyed
 * with every allocation refused AND every CPU wait failing, so the fix can
 * neither record an entry nor prove anything; the context is then destroyed
 * with that work outstanding, which drops the tracker; then drain points run.
 *
 * Expected: V1 fails. radv_wddm2_starved_retry decides "idle" by walking
 * ws->deferred.trackers, which no longer names queue A, and then calls
 * radv_wddm2_bo_destroy_now on X: Evict and FreeGpuVirtualAddress under work
 * that may still run. V0 fails earlier, at the destroy itself, having no keep
 * at all. */
static void
test_regression_a(void)
{
   const uint64_t slice = radv_wddm2_fence_wait_slice_ns, total = radv_wddm2_fence_wait_total_ns;
   radv_wddm2_fence_wait_slice_ns = 20000000ull;  /* 20 ms */
   radv_wddm2_fence_wait_total_ns = 200000000ull; /* 200 ms */

   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS && published_of(qa) == 1, "queue A published progress 1, completed 0");
   const uint32_t fence_a = qa->bc250_progress.handle;

   struct radeon_winsys_bo *x = new_bo(ws, 65536);
   const uint32_t x_h = x->handle;
   fail_cpu_waits(true);
   HANDLE dog = watchdog_start("a destroy with no memory and failing waits", 30000);
   arm_allocator();
   struct watch *s = destroy_bo(ws, x, "regression_a");
   const unsigned attempts = disarm_allocator();
   watchdog_stop(dog);
   note("the destroy made %u allocation attempts, all refused, and every CPU wait failed", attempts);
   check(!s->evicted && !s->va_freed, "the destroy itself evicts nothing and frees no VA");
   note("the BO is %s after that destroy", s->released ? "already released" : "kept by the winsys");

   /* The context goes with its work outstanding: the tracker detaches. The
    * wait inside ctx_destroy still fails, which is what abandons the queue. */
   ws->base.ctx_destroy(a);
   fail_cpu_waits(false);
   check(fence_live(fence_a) && completed_of_fence(fence_a) == 0,
         "the context is gone and its value 1 is still unreached");

   /* Drain points, where an "idle" walk would run. */
   struct radeon_winsys_ctx *b = new_ctx(ws);
   check(submit_one(ws, b) == VK_SUCCESS, "another queue submits (a drain point)");
   struct radeon_winsys_bo *pressure = new_bo(ws, 65536); /* a create is a drain point too */
   check(!s->evicted && !s->va_freed,
         "the kept BO is not evicted and keeps its VA while the abandoned queue's 1 is unreached");
   check(alloc_live(x_h) || s->released, "and nothing of it is half gone");

   destroy_bo(ws, pressure, "regression_a/pressure");
   retire_fence(fence_a, 1);
   retire(gfx(b), published_of(gfx(b)));
   check(submit_one(ws, b) == VK_SUCCESS, "a drain point once every value completed");
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
   radv_wddm2_fence_wait_slice_ns = slice;
   radv_wddm2_fence_wait_total_ns = total;
}

/* The positive control. A borrowed BO is a host import: the allocation is the
 * host's, not ours, and BD-042 destroys it at once with no Evict, no VA free
 * and no DestroyAllocation2. The test must not flag that, and the underlying
 * allocation must survive. This is the case that proves the suite does not
 * simply forbid every immediate destroy. */
static void
test_borrowed_ok(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1 and keeps it in flight");

   /* The host's allocation, which the test owns through a plain BO. */
   struct radeon_winsys_bo *owner = new_bo(ws, 65536);
   const uint32_t handle = owner->handle;
   const uint64_t va = owner->va;

   struct radeon_winsys_bo *borrowed = NULL;
#if BD201_HOSTED_IMPORT_FLAGS
   const VkResult result = ws->base.buffer_from_hosted(&ws->base, ws->host.identity, handle, 0, va, 65536, &borrowed);
#else
   const VkResult result = ws->base.buffer_from_hosted(&ws->base, ws->host.identity, handle, va, 65536, &borrowed);
#endif
   check(result == VK_SUCCESS && borrowed, "a host import of that allocation (%d)", (int)result);

   const unsigned evicts = h.evicts, va_frees = h.va_frees, destroys = h.destroys;
   struct watch *s = destroy_bo(ws, borrowed, "borrowed_ok");
   check(h.evicts == evicts && h.va_frees == va_frees && h.destroys == destroys,
         "destroying the import makes no kernel call at all (%u evicts, %u VA frees, %u destroys)",
         h.evicts - evicts, h.va_frees - va_frees, h.destroys - destroys);
   check(!s->evicted && !s->va_freed && !s->released, "and the mock saw no release call for it");
   check(alloc_live(handle) && alloc_resident(handle), "the host's allocation is untouched");

   /* The owning BO still behaves as before. */
   struct watch *o = destroy_bo(ws, owner, "borrowed_ok/owner");
   check(!o->evicted && !o->va_freed, "the owning BO is held, its queue's 1 in flight");
   retire(qa, 1);
   check(submit_one(ws, a) == VK_SUCCESS && o->released, "and released once the value completed");

   retire(qa, published_of(qa));
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* Design R v2, R4 as revised by the lead on 2026-10-21: neither a fence
 * reading UINT64_MAX nor a non-ACTIVE device state is proof that the work
 * completed, so a BO destroyed with an unreached value keeps its VA and its
 * residency in both states.
 *
 * Expected: V0 and V1 fail the UINT64_MAX half, because
 * radv_wddm2_tracker_retired_locked compares p_atomic_read(value_map) with the
 * wanted value and UINT64_MAX is above every value (the comment at
 * radv_wddm2_bo.c:1790 says so). The device-state half cannot be reached from
 * here on any line: radv_wddm2_device_executing returns true at once while
 * ws->host.dispatch is set (radv_wddm2_cs.c:797), and this test always sets
 * it. The case records whether the winsys asked at all, which is the signal
 * for V2: the design wants a GetDeviceState through the host seam. */
static void
test_lost_proof(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS && published_of(qa) == 1, "queue A published progress 1, completed 0");
   const uint32_t fence_a = qa->bc250_progress.handle;

   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   struct watch *s = destroy_bo(ws, bo, "lost_proof");
   check(!s->evicted && !s->va_freed, "the destroy holds the BO");

   /* The device reports a non-ACTIVE execution state. */
   mock_enter();
   h.execution_state = D3DKMT_DEVICEEXECUTION_HUNG;
   const unsigned asked_before = h.device_state_asked;
   mock_leave();
   check(submit_one(ws, b) == VK_SUCCESS, "another queue submits, which is a drain point");
   mock_enter();
   const unsigned asked = h.device_state_asked - asked_before;
   mock_leave();
   note("the winsys asked GetDeviceState %u times at that drain point", asked);
   check(!s->evicted && !s->va_freed, "a non-ACTIVE device state is not proof: no Evict, no VA free");
   /* The SAFETY half of R4 is the assertion above and holds on every version: a
    * non-ACTIVE state is not proof, so nothing is released. Whether the winsys
    * ASKS the device state at all is accounting quality, not a requirement
    * (lead, 2026-10-21, the G3 split): while ws->host.dispatch is set,
    * radv_wddm2_device_executing returns true at once (radv_wddm2_cs.c:797), so
    * the question never reaches the seam on any line and the answer is
    * untested here. If bd201-fix adds a device-state seam, this line says so. */
   quality(asked > 0, "R4: a drain point with an unreached value asks the device state through the host seam (%u)",
           asked);

   /* Now the fence itself reads the lost-device value. */
   retire_fence(fence_a, UINT64_MAX);
   check(submit_one(ws, b) == VK_SUCCESS, "a drain point with queue A's fence reading UINT64_MAX");
   check(!s->evicted && !s->va_freed,
         "a fence reading UINT64_MAX is not proof either (allocation live=%d)", alloc_live(handle));

   retire(gfx(b), published_of(gfx(b)));
   ws->base.ctx_destroy(b);
   fail_cpu_waits(true); /* queue A's value never really completed */
   ws->base.ctx_destroy(a);
   fail_cpu_waits(false);
   contract();
   free(ws);
}

/* Design R v2, R6. The queue publishes the value BEFORE it submits, so a
 * destroy always counts it; what must not happen is a release on a value that
 * no accepted submission will ever write, and what must not happen either is
 * an endless hold once a later value of the same in-order queue completes
 * (R4(b)).
 *
 * Two shapes: a submission the kernel rejects outright, and one it accepts
 * whose GPU signal fails. Expected: V0 fails both through the plain BD-201
 * hole wherever it cannot record; V1 holds the BO and then releases it on the
 * later completed value, which is correct, so V1 may pass. The case is here to
 * pin V2's behaviour and to prove no value is ever marked complete
 * artificially. */
static void
test_failed_submit(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);

   /* (a) rejected before acceptance. */
   fail_once(BC250_HOST_SubmitCommand);
   const VkResult rejected = submit_one(ws, a);
   note("a rejected submit returned %d; the value the host has seen published is %" PRIu64, (int)rejected,
        published_of(qa));
   check(published_of(qa) == 0, "the kernel never saw a value for it, so nothing of the GPU's can use the BO");

   struct radeon_winsys_bo *x = new_bo(ws, 65536);
   struct watch *s = destroy_bo(ws, x, "failed_submit/rejected");
   check(!s->evicted && !s->va_freed,
         "a value published for a rejected submit still holds the BO: nothing marks it complete");

   /* The queue works on. Proof comes from a later value of the same queue. */
   check(submit_one(ws, a) == VK_SUCCESS, "the queue submits again");
   retire(qa, published_of(qa));
   check(submit_one(ws, a) == VK_SUCCESS, "a drain point once a later value of that queue completed");
   check(s->released, "and the BO is released there, by R4(b), not before");

   /* (b) accepted, but the GPU signal fails. */
   fail_once(BC250_HOST_SignalSynchronizationObjectFromGpu2);
   const VkResult signalled = submit_one(ws, a);
   note("a submit whose GPU signal failed returned %d", (int)signalled);
   const uint64_t target = published_of(qa);
   struct radeon_winsys_bo *y = new_bo(ws, 65536);
   struct watch *q = destroy_bo(ws, y, "failed_submit/signal");
   check(!q->evicted && !q->va_freed, "the BO waits for the unproven value %" PRIu64, target);

   retire(qa, target);
   /* A failed GPU signal loses the device here, so a later submit may fail;
    * destroying another BO is a drain point that does not need one. */
   struct radeon_winsys_bo *z = new_bo(ws, 65536);
   destroy_bo(ws, z, "failed_submit/drain");
   note("the second BO is %s once that value completed on the queue", q->released ? "released" : "still held");

   retire(qa, published_of(qa));
   fail_cpu_waits(true);
   ws->base.ctx_destroy(a);
   fail_cpu_waits(false);
   contract();
   free(ws);
}

/* Design R v2, R2 and R1-a. A virtual (sparse) BO's range is OUR reservation
 * (ReserveGpuVirtualAddress, radv_wddm2_reserve_va_range): nobody frees it for
 * us, and a new range may take it the moment it is freed
 * (d3dkmthk.md:12632). So FreeGpuVirtualAddress of it may happen only after
 * the destroy-time values are reached, and never without them, teardown
 * included. Leaking it costs VA only.
 *
 * Expected: V0 fails, radv_wddm2_bo_destroy_now frees the reservation at once.
 * V1 holds the recorded entry, so it may pass the first half and fail the
 * second, where the entry cannot be allocated. */
static void
test_virtual_bo(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS,
         "two queues published progress 1 and keep it in flight");

   struct radeon_winsys_bo *v = NULL;
   VkResult result = ws->base.buffer_create(&ws->base, 1024 * 1024, 65536, RADEON_DOMAIN_GTT,
                                            RADEON_FLAG_VIRTUAL, 0, 0, NULL, &v);
   check(result == VK_SUCCESS && v && v->is_virtual && v->va,
         "a virtual BO reserved a range of its own (%d, va 0x%" PRIx64 ")", (int)result, v ? v->va : 0);
   if (!v) {
      ws->base.ctx_destroy(a);
      ws->base.ctx_destroy(b);
      free(ws);
      return;
   }
   const unsigned frees = h.va_frees;
   struct watch *s = destroy_bo(ws, v, "virtual_bo/held");
   check(!s->va_freed, "it is not freed while the two values are unreached (%u VA frees)", h.va_frees - frees);

   /* Proof arrives for both queues: freeing it now is correct. */
   retire(qa, 1);
   retire(qb, 1);
   check(submit_one(ws, a) == VK_SUCCESS, "a drain point once both values retired");
   note("after the proof the held reservation is %s", s->va_freed ? "freed" : "still reserved");

   /* A second virtual BO, whose destroy can neither record nor prove anything,
    * and whose queues never retire again. */
   check(submit_one(ws, b) == VK_SUCCESS, "queue B submits again and keeps progress 2 in flight");
   struct radeon_winsys_bo *w = NULL;
   result = ws->base.buffer_create(&ws->base, 1024 * 1024, 65536, RADEON_DOMAIN_GTT, RADEON_FLAG_VIRTUAL, 0, 0,
                                   NULL, &w);
   check(result == VK_SUCCESS && w, "a second virtual BO (%d)", (int)result);
   fail_cpu_waits(true);
   arm_allocator();
   struct watch *q = w ? destroy_bo(ws, w, "virtual_bo/no_memory") : NULL;
   const unsigned attempts = disarm_allocator();
   note("its destroy made %u allocation attempts, all refused, and every CPU wait failed", attempts);
   if (q)
      check(!q->va_freed, "its reservation is not freed: the destroy could neither record nor prove");

#if BD201_DEFERRED_API
   HANDLE dog = watchdog_start("teardown with an unproven reservation", 60000);
   radv_wddm2_deferred_finish(ws);
   watchdog_stop(dog);
   if (q)
      check(!q->va_freed, "and teardown leaks it rather than freeing it unproven");
#if BD201_BO_POOL
   radv_wddm2_bo_pool_finish(ws);
#endif
#else
   if (q)
      check(!q->va_freed, "and nothing else freed it either (this line has no teardown entry point)");
#endif
   fail_cpu_waits(false);

   retire(qa, published_of(qa));
   retire(qb, published_of(qb));
   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
}

/* Design R v2, 6.2: the handoff race at the OS seam. DestroyAllocation2 with
 * flags 0 protects only what was queued to the OS BEFORE the destroy request,
 * so the mock counts protection from the HANDOFF, which is the host's
 * SubmitCommand. Here a submission is parked inside that call, on another
 * thread, while the main thread destroys a BO: the value is published, the
 * kernel has the submission in its hands and nothing has completed. Neither an
 * Evict nor a VA free may happen, and the destroy may not deadlock against the
 * parked call.
 *
 * This is the OS seam alone. The driver's own software queues (the vk_queue
 * submit thread and the hosted queue) are a different handoff, covered by the
 * code proof in bd201-fix's ORDER-PROOF.md; a RADV-level test of them is a
 * later item (lead's decision E, 2026-10-21).
 *
 * The two publications of a value, which this case separates (lead's item c):
 *  - the winsys's own tracker, radv_wddm2_tracker_publish
 *    (radv_wddm2_bo.h:108-111, field published at radv_wddm2_bo.h:92), called
 *    at radv_wddm2_cs.c:1817 BEFORE radv_wddm2_bc250_submit at :1823. This is
 *    what a destroy of any thread reads, so the window is covered on the
 *    winsys's side from before the handoff.
 *  - the host's view, BC250_HOST_PUBLISH_PROGRESS in
 *    radv_wddm2_progress_signalled (radv_wddm2_cs.c:1652-1667, the dispatch at
 *    :1660, which also writes queue->bc250_progress.wait_value at :1657),
 *    called only after the submit call returned, at :1843, :1869 and :1974.
 * They are two different variables, and the mock therefore raises the kernel's
 * view of a queue at the handoff itself (queue_view::pending), not at the
 * later publication, or a destroy inside the window would capture a target of
 * zero and this case would assert nothing.
 *
 * The V2 requirement (design 6.2, with the handoff wait): while the submission
 * is in the kernel, there is no DestroyAllocation2 of the BO either, because
 * the OS's deferral counts only what was queued BEFORE the request.
 * Expected: V0 and V1 MEET the two 6.2 clauses here, for a reason of their
 * own rather than by design. Their destroy records a deferred entry while the
 * host allocation succeeds and then issues nothing at all, so no call of any
 * kind falls inside the window; neither knows about a submit call that has not
 * returned. They do not meet the other R1 clause, because they release the BO
 * later with an Evict and an address free. A version that destroyed the
 * allocation at once, as both do whenever they cannot record an entry, would
 * fail the two clauses here, which is what they are for.
 *
 * The decided design (6.7, after the order proof and its review) dropped the
 * handoff wait: on every line a submission is handed to the OS inline, under
 * the queue lock, before its vkQueueSubmit returns (I1), and the client may
 * destroy a BO only after the submissions that name it completed (I2). The
 * parked submission here names no BO of the test, so under BD201_V2 a
 * DestroyAllocation2 inside its window is legal and is a note; the destroy
 * must still capture the value the kernel is holding, and an Evict or an
 * address free inside the window is still a breach. */
struct parker {
   struct radv_wddm2_winsys *ws;
   struct radeon_winsys_ctx *ctx;
   VkResult result;
};

static DWORD WINAPI
parker_thread(void *arg)
{
   struct parker *p = arg;
   p->result = submit_one(p->ws, p->ctx);
   return 0;
}

static void
test_handoff_race(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);

   mock_enter();
   h.parked_in = CreateEventW(NULL, TRUE, FALSE, NULL);
   h.park_release = CreateEventW(NULL, TRUE, FALSE, NULL);
   h.park_op = BC250_HOST_SubmitCommand;
   h.park_left = 1;
   mock_leave();

   struct parker p = {ws, a, VK_SUCCESS};
   HANDLE t = CreateThread(NULL, 0, parker_thread, &p, 0, NULL);
   if (!t || WaitForSingleObject(h.parked_in, 10000) != WAIT_OBJECT_0)
      abort();
   check(h.park_left == 0, "a submission is parked inside the host's submit call: the kernel has it");
   note("the value the host has seen published at that moment is %" PRIu64
        " (this winsys publishes to the host after the submit call returns)", published_of(qa));

   struct radeon_winsys_bo *bo = new_bo(ws, 65536);
   const uint32_t handle = bo->handle;
   const unsigned window_before = h.in_window;
   HANDLE dog = watchdog_start("a destroy while a submission is parked in the kernel", 30000);
   struct watch *s = destroy_bo(ws, bo, "handoff_race");
   watchdog_stop(dog);
   check(s->n_target >= 1 && s->target[0] >= 1,
         "the destroy captured the value the kernel is holding (target %" PRIu64 "), not the one the host has been "
         "told (%" PRIu64 ")",
         s->n_target ? s->target[0] : 0, published_of(qa));
   check(!s->evicted && !s->va_freed,
         "the destroy evicts nothing and frees no VA while that submission is in the kernel");
   note("the allocation is %s and the destroy %s the OS", alloc_live(handle) ? "live" : "gone",
        s->released ? "was handed to" : "was not handed to");
#if BD201_V2
   note("6.7: the parked submission does not name this BO, so its DestroyAllocation2 may go to the OS now "
        "(released %d, %u calls inside the submit window)",
        s->released, h.in_window - window_before);
#else
   check(h.in_window == window_before, "6.2: nothing of it was released inside the submit window (%u calls)",
         h.in_window - window_before);
#endif

   SetEvent(h.park_release);
   if (WaitForSingleObject(t, 30000) != WAIT_OBJECT_0)
      abort();
   CloseHandle(t);
   check(p.result == VK_SUCCESS, "the parked submission then completed its call (%d)", (int)p.result);
   check(published_of(qa) >= 1,
         "and the host's own view of the value follows once that call returned (%" PRIu64 "): two different "
         "variables, the tracker's and the host's",
         published_of(qa));
   check(!s->evicted && !s->va_freed, "the handoff alone released nothing");

   retire(qa, published_of(qa));
   check(submit_one(ws, a) == VK_SUCCESS, "a drain point once the value completed");
   check(s->released, "the BO is released there");

   /* The half that bites: the same window, with no host memory for a deferred
    * entry and every CPU wait failing, which is where both old lines fall
    * through to destroying the allocation at once. Nothing of the BO may be
    * released while the kernel holds a submission that may name it. */
   struct radeon_winsys_bo *starved = new_bo(ws, 65536);
   mock_enter();
   ResetEvent(h.parked_in);
   ResetEvent(h.park_release);
   h.park_op = BC250_HOST_SubmitCommand;
   h.park_left = 1;
   const unsigned window_starved = h.in_window;
   mock_leave();
   struct parker p2 = {ws, a, VK_SUCCESS};
   HANDLE t2 = CreateThread(NULL, 0, parker_thread, &p2, 0, NULL);
   if (!t2 || WaitForSingleObject(h.parked_in, 10000) != WAIT_OBJECT_0)
      abort();
   fail_cpu_waits(true);
   arm_allocator();
   dog = watchdog_start("a starved destroy while a submission is parked in the kernel", 30000);
   struct watch *q = destroy_bo(ws, starved, "handoff_race/starved");
   watchdog_stop(dog);
   const unsigned attempts = disarm_allocator();
   fail_cpu_waits(false);
   note("that destroy made %u allocation attempts, all refused, with the kernel holding a submission", attempts);
#if BD201_V2
   check(!q->evicted && !q->va_freed,
         "a destroy that can record nothing evicts nothing and frees no address there (%d/%d)", q->evicted,
         q->va_freed);
   note("6.7: its DestroyAllocation2 %s inside the submit window (%u calls), legal for a BO the parked "
        "submission does not name",
        q->released ? "went to the OS" : "did not go to the OS", h.in_window - window_starved);
#else
   check(h.in_window == window_starved,
         "a destroy that can record nothing still releases nothing inside the submit window (%u calls)",
         h.in_window - window_starved);
   check(!q->evicted && !q->va_freed && !q->released,
         "no Evict, no address free and no DestroyAllocation2 of it there (%d/%d/%d)", q->evicted, q->va_freed,
         q->released);
#endif
   SetEvent(h.park_release);
   if (WaitForSingleObject(t2, 30000) != WAIT_OBJECT_0)
      abort();
   CloseHandle(t2);
   note("the second parked submission returned %d", (int)p2.result);
   retire(qa, published_of(qa));

   mock_enter();
   CloseHandle(h.parked_in);
   CloseHandle(h.park_release);
   h.parked_in = h.park_release = NULL;
   mock_leave();
   retire(qa, published_of(qa));
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* A wall clock for the budget case; nothing else times anything. */
static double
now_ms(void)
{
   LARGE_INTEGER f, c;
   QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Design R v2, R5: the wait budget. A destroy may wait, but the waiting is
 * bounded, and the bound is a budget of the winsys, not a cost per queue and
 * not a cost per destroy. A frame that replaces a queue's scratch BOs
 * (radv_update_preambles) destroys several BOs in a row while several queues
 * are busy; if each destroy pays the full wait budget for each queue, that
 * frame stalls for the product.
 *
 * The case: two queues whose fence never advances (nothing can be proved), six
 * destroys in a row, each timed. Every BO must be kept, no destroy may hang,
 * and the six together must not cost a multiple of the first one: the design
 * asks for a total budget.
 *
 * Expected: V0 and V1 both pass. Both record an entry whenever the host
 * allocation succeeds, and neither waits inside the destroy at all, so the
 * measured cost is near zero and the budget is never spent. The case therefore
 * does not catch V0: it states the bound that V2 may not exceed once it
 * starts waiting, and the measured numbers are reported as notes whatever the
 * verdict. The holes of V0 and V1 are caught by no_host_memory, device_lost,
 * teardown, abandoned_queue, regression_a, lost_proof and virtual_bo. */
static void
test_wait_budget(void)
{
   const uint64_t slice = radv_wddm2_fence_wait_slice_ns, total = radv_wddm2_fence_wait_total_ns;
   radv_wddm2_fence_wait_slice_ns = 20000000ull;  /* 20 ms */
   radv_wddm2_fence_wait_total_ns = 200000000ull; /* 200 ms */

   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS,
         "two queues published progress 1 and neither will retire it");
   h.stuck_cpu_wait = true;

   enum { N = 6 };
   struct watch *s[N];
   double cost[N], first = 0.0, sum = 0.0;
   HANDLE dog = watchdog_start("six destroys with a stuck fence", 60000);
   for (unsigned i = 0; i < N; i++) {
      struct radeon_winsys_bo *bo = new_bo(ws, 1024 * 1024);
      const double t0 = now_ms();
      s[i] = destroy_bo(ws, bo, "wait_budget");
      cost[i] = now_ms() - t0;
      sum += cost[i];
      if (!i)
         first = cost[i];
   }
   watchdog_stop(dog);
   note("the six destroys cost %.0f, %.0f, %.0f, %.0f, %.0f, %.0f ms (%.0f ms in all, %u CPU waits)", cost[0],
        cost[1], cost[2], cost[3], cost[4], cost[5], sum, h.cpu_waits);

   unsigned kept = 0;
   for (unsigned i = 0; i < N; i++)
      kept += !s[i]->evicted && !s[i]->va_freed;
   check(kept == N, "all six BOs are kept: nothing was proved, so nothing may be evicted or freed (%u of %u)", kept,
         N);
   /* The budget: the first destroy may pay it, the rest may not pay it again.
    * 4x the first plus 200 ms of slack is generous for six destroys. V0 and V1
    * do not wait in a destroy at all once they can record an entry, so this is
    * the bound V2 may not exceed, not a test of the old lines. */
   v2_req(sum <= 4.0 * first + 200.0,
          "R5: the six destroys share a wait budget instead of each paying it (%.0f ms against the bound %.0f ms)",
          sum, 4.0 * first + 200.0);

   h.stuck_cpu_wait = false;
   retire(qa, 1);
   retire(qb, 1);
   struct radeon_winsys_bo *drain = new_bo(ws, 65536);
   destroy_bo(ws, drain, "wait_budget/drain");
   unsigned released = 0;
   for (unsigned i = 0; i < N; i++)
      released += s[i]->released;
   note("after the proof %u of the six are released", released);

   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
   radv_wddm2_fence_wait_slice_ns = slice;
   radv_wddm2_fence_wait_total_ns = total;
}

/* Design R v2, R7: the create-time sweep. A BO the winsys keeps holds memory
 * and VA the application already gave back, so an allocation that fails for
 * want of device memory may wait for held BOs and release what retired
 * (radv_wddm2_bo.c:955-990 describes exactly that). What it may not do is
 * release a BO whose work is unproven in order to make room: the OS would hand
 * that memory and that address to the new allocation while the GPU may still
 * read them. A create that cannot make room must fail instead.
 *
 * Two halves: an allocation refused by the kernel (which the winsys maps to
 * VK_ERROR_OUT_OF_DEVICE_MEMORY and which drives the sweep), and a mapping
 * refused after the allocation succeeded (the rollback path).
 *
 * Expected: V0 and V1 both pass. Both keep the two BOs (the host allocation
 * of the entry succeeds here), the sweep finds nothing retired and the create
 * fails with VK_ERROR_OUT_OF_DEVICE_MEMORY instead of taking their memory. The
 * case does not catch V0; it pins the sweep and the rollback for V2. */
static void
test_create_sweep(void)
{
   const uint64_t slice = radv_wddm2_fence_wait_slice_ns, total = radv_wddm2_fence_wait_total_ns;
   radv_wddm2_fence_wait_slice_ns = 20000000ull;  /* 20 ms */
   radv_wddm2_fence_wait_total_ns = 200000000ull; /* 200 ms */

   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws), *b = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a), *qb = gfx(b);
   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS, "two queues published progress 1");
   /* A stuck wait, so that only the values the test retires itself count as
    * completed: a mock that granted every wait would prove everything. */
   h.stuck_cpu_wait = true;

   struct radeon_winsys_bo *y = new_bo(ws, 1024 * 1024);
   struct watch *sy = destroy_bo(ws, y, "create_sweep/older");
   struct radeon_winsys_bo *x = new_bo(ws, 1024 * 1024);
   struct watch *sx = destroy_bo(ws, x, "create_sweep/newer");

   /* Nothing is proved: the sweep must find nothing to release and the create
    * must fail rather than take an unproven BO's memory. */
   struct radeon_winsys_bo *want = NULL;
   fail_once((uint32_t)BC250_HOST_CreateAllocation2);
   HANDLE dog = watchdog_start("a create whose allocation is refused with unproven BOs held", 60000);
   VkResult result = ws->base.buffer_create(&ws->base, 1024 * 1024, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL,
                                            &want);
   watchdog_stop(dog);
   note("the create returned %d with two unproven BOs held (%u CPU waits so far)", (int)result, h.cpu_waits);
   check(result != VK_SUCCESS && !want, "a create that cannot make room fails instead of taking unproven memory");
   check(!sx->evicted && !sx->va_freed && !sy->evicted && !sy->va_freed,
         "and neither held BO is evicted and no VA of theirs is freed");

   /* Now the work of both queues really completed. The same failure must sweep
    * the held BOs and succeed. */
   retire(qa, 1);
   retire(qb, 1);
   fail_once((uint32_t)BC250_HOST_CreateAllocation2);
   dog = watchdog_start("the same create once the held work retired", 60000);
   result = ws->base.buffer_create(&ws->base, 1024 * 1024, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &want);
   watchdog_stop(dog);
   note("with the held work retired the create returned %d; older released=%d newer released=%d", (int)result,
        sy->released, sx->released);
   /* bo_create drains first (radv_wddm2_bo.c:960), so by the time a refused
    * allocation is seen the held BOs are already released and
    * ws->deferred.count is zero: the sweep loop is skipped and the create
    * fails although the room exists. R7 does not require that retry; it
    * requires only that a retry never assume the memory is back, which the
    * assertions above cover (nothing unproven is released to make room). So
    * this is an observation on every version (lead, 2026-10-21 15:15Z). */
   quality(result == VK_SUCCESS,
           "R7: a create retries itself once the drain released held BOs (%d, older released %d, newer %d)",
           (int)result, sy->released, sx->released);
   if (result == VK_SUCCESS && want)
      ws->base.buffer_destroy(&ws->base, want);

   /* The rollback half: the allocation succeeds and the mapping is refused.
    * Both queues publish 2 and never retire it, so the BO destroyed next is
    * unproven while the failing create runs. */
   check(submit_one(ws, a) == VK_SUCCESS && submit_one(ws, b) == VK_SUCCESS, "both queues published progress 2");
   struct radeon_winsys_bo *z = new_bo(ws, 1024 * 1024);
   struct watch *sz = destroy_bo(ws, z, "create_sweep/rollback");
   const unsigned live_before = live_allocs();
   want = NULL;
   fail_once((uint32_t)BC250_HOST_MapGpuVirtualAddress);
   dog = watchdog_start("a create whose mapping is refused", 60000);
   result = ws->base.buffer_create(&ws->base, 1024 * 1024, 4096, RADEON_DOMAIN_GTT, PLAIN_FLAGS, 0, 0, NULL, &want);
   watchdog_stop(dog);
   note("a create whose mapping was refused returned %d (%u live allocations before, %u after)", (int)result,
        live_before, live_allocs());
   check(result != VK_SUCCESS && !want, "it fails");
   check(live_allocs() == live_before, "and leaves no allocation of its own behind");
   check(!sz->evicted && !sz->va_freed, "and the unproven BO it could not make room with is untouched");

   h.stuck_cpu_wait = false;
   retire(qa, published_of(qa));
   retire(qb, published_of(qb));
   struct radeon_winsys_bo *drain = new_bo(ws, 65536);
   destroy_bo(ws, drain, "create_sweep/drain");
   note("after the proof the rollback BO is %s", sz->released ? "released" : "still held");

   ws->base.ctx_destroy(a);
   ws->base.ctx_destroy(b);
   contract();
   free(ws);
   radv_wddm2_fence_wait_slice_ns = slice;
   radv_wddm2_fence_wait_total_ns = total;
}

/* Design R v2, R7: the accounting of kept bytes, and what happens over the
 * cap. A kept BO stays in the winsys's byte accounting until it is released,
 * and over a cap of kept bytes (BC250_DEFERRED_CAP_MB, 512 MiB by default) the
 * winsys waits for the oldest kept BO and releases what retired. The cap is a
 * budget, not a licence: when nothing can be proved, the kept bytes exceed the
 * cap and every BO stays. The observable accounting is the kernel's: every
 * kept BO's allocation is still live and still resident, and their sizes sum to
 * the bytes the winsys is holding.
 *
 * Then the shape that catches a version which uses the cap as a licence: one
 * more destroy over the cap with every host allocation refused, so the winsys
 * cannot record an entry for it. It must still evict nothing and free no VA,
 * of that BO or of the nine already kept.
 *
 * Expected: V0 and V1 pass the first half (both record an entry while the
 * allocation succeeds, and the cap wait returns without progress), and both
 * fail the starved tenth destroy: with no entry to record they fall through to
 * radv_wddm2_bo_destroy_now, which is the BD-201 hole under cap pressure. */
static void
test_kept_bytes(void)
{
   const uint64_t slice = radv_wddm2_fence_wait_slice_ns, total = radv_wddm2_fence_wait_total_ns;
   radv_wddm2_fence_wait_slice_ns = 20000000ull;  /* 20 ms */
   radv_wddm2_fence_wait_total_ns = 200000000ull; /* 200 ms */

   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1 and will not retire it");
   h.stuck_cpu_wait = true;

   /* Nine BOs of 64 MiB: 576 MiB, over the 512 MiB default cap. */
   enum { N = 9, MB = 64 };
   struct watch *s[N];
   uint64_t bytes = 0;
   HANDLE dog = watchdog_start("nine destroys over the cap of kept bytes", 60000);
   const double t0 = now_ms();
   for (unsigned i = 0; i < N; i++) {
      struct radeon_winsys_bo *bo = new_bo(ws, (uint64_t)MB << 20);
      bytes += bo->size;
      s[i] = destroy_bo(ws, bo, "kept_bytes");
   }
   const double cost = now_ms() - t0;
   watchdog_stop(dog);
   note("nine destroys of %u MiB (%" PRIu64 " MiB in all, cap 512 MiB) cost %.0f ms and %u CPU waits", MB,
        bytes >> 20, cost, h.cpu_waits);

   unsigned kept = 0, live = 0, resident = 0;
   for (unsigned i = 0; i < N; i++) {
      kept += !s[i]->evicted && !s[i]->va_freed;
      live += alloc_live(s[i]->alloc);
      resident += alloc_resident(s[i]->alloc);
   }
   check(kept == N, "over its own cap the winsys still evicts nothing and frees no VA (%u of %u kept)", kept, N);
   check(live == N && resident == N,
         "and the accounting is the kernel's: all nine allocations are live and resident (%u live, %u resident)",
         live, resident);

   /* The tenth destroy, still over the cap, with no host memory for its entry
    * and every CPU wait failing: nothing about it can be recorded or proved. */
   struct radeon_winsys_bo *tenth = new_bo(ws, (uint64_t)MB << 20);
   fail_cpu_waits(true);
   arm_allocator();
   dog = watchdog_start("a starved destroy over the cap", 60000);
   struct watch *s10 = destroy_bo(ws, tenth, "kept_bytes/starved");
   watchdog_stop(dog);
   const unsigned attempts = disarm_allocator();
   fail_cpu_waits(false);
   note("the tenth destroy made %u allocation attempts, all refused, with %" PRIu64 " MiB already kept", attempts,
        bytes >> 20);
   check(!s10->evicted && !s10->va_freed,
         "a destroy the winsys cannot record keeps its BO even over the cap: the cap is a budget, not a licence");
   kept = 0;
   for (unsigned i = 0; i < N; i++)
      kept += !s[i]->evicted && !s[i]->va_freed;
   check(kept == N, "and the nine already kept are untouched by that pressure (%u of %u)", kept, N);

   h.stuck_cpu_wait = false;
   retire(qa, 1);
   struct radeon_winsys_bo *drain = new_bo(ws, 65536);
   struct watch *sd = destroy_bo(ws, drain, "kept_bytes/drain");
   unsigned released = 0;
   for (unsigned i = 0; i < N; i++)
      released += s[i]->released;
   note("after the proof %u of the nine are released, %u allocations live in all", released, live_allocs());
   check(released == N, "the kept bytes are given back once the work is proved (%u of %u)", released, N);
   (void)sd;

   ws->base.ctx_destroy(a);
   contract();
   free(ws);
   radv_wddm2_fence_wait_slice_ns = slice;
   radv_wddm2_fence_wait_total_ns = total;
}

/* Design R v2, R1-a: a REPLAYABLE BO, whose address the application supplies
 * (vkGetBufferOpaqueCaptureAddress replay). Its range comes from the replay
 * heap at the exact address asked for (radv_wddm2_bo.c:840-857), so freeing it
 * before proof is worse than freeing an ordinary range: a replaying
 * application asks for that one address again and the OS may already have
 * given it to something else. The destroy-time rule is the same, and the case
 * also records whether the address was honoured exactly.
 *
 * Then the same with every host allocation refused and every CPU wait failing,
 * which is where a version that cannot record an entry falls through to
 * radv_wddm2_bo_destroy_now.
 *
 * Expected: V0 and V1 pass the first half (the entry is recorded) and fail the
 * starved half, freeing a replay address under work that may still run. The
 * teardown check is part of it: a failed wait is not proof (R4). */
static void
test_replayable(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1 and keeps it in flight");

   const uint64_t wanted = RADV_WDDM2_REPLAY_HEAP_START + (16ull << 20);
   struct radeon_winsys_bo *r = NULL;
   VkResult result = ws->base.buffer_create(&ws->base, 1024 * 1024, 65536, RADEON_DOMAIN_GTT,
                                            PLAIN_FLAGS | RADEON_FLAG_REPLAYABLE, 0, wanted, NULL, &r);
   check(result == VK_SUCCESS && r, "a replayable BO at an address the application supplied (%d)", (int)result);
   if (!r) {
      ws->base.ctx_destroy(a);
      free(ws);
      return;
   }
   check(r->va == wanted, "at exactly that address (asked 0x%" PRIx64 ", got 0x%" PRIx64 ")", wanted, r->va);

   const uint32_t alloc = r->handle;
   struct watch *s = destroy_bo(ws, r, "replayable");
   check(!s->evicted && !s->va_freed,
         "its destroy neither evicts it nor frees the replay address while the queue's 1 is unreached");
   if (!s->released)
      check(alloc_live(alloc) && alloc_resident(alloc), "and the allocation stays live and resident");

   retire(qa, 1);
   struct radeon_winsys_bo *drain = new_bo(ws, 65536);
   destroy_bo(ws, drain, "replayable/drain");
   note("after the proof the replay address is %s", s->va_freed ? "freed" : "still mapped");

   /* The starved half: a second replay address, destroyed while the queue's
    * next value is unreached, with nothing the winsys can record or prove. */
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 2 and keeps it in flight");
   const uint64_t second = RADV_WDDM2_REPLAY_HEAP_START + (64ull << 20);
   struct radeon_winsys_bo *r2 = NULL;
   result = ws->base.buffer_create(&ws->base, 1024 * 1024, 65536, RADEON_DOMAIN_GTT,
                                   PLAIN_FLAGS | RADEON_FLAG_REPLAYABLE, 0, second, NULL, &r2);
   check(result == VK_SUCCESS && r2 && r2->va == second, "a second replayable BO at 0x%" PRIx64 " (%d)", second,
         (int)result);
   if (r2) {
      fail_cpu_waits(true);
      arm_allocator();
      HANDLE dog = watchdog_start("a starved destroy of a replayable BO", 60000);
      struct watch *s2 = destroy_bo(ws, r2, "replayable/starved");
      watchdog_stop(dog);
      const unsigned attempts = disarm_allocator();
      note("its destroy made %u allocation attempts, all refused, and every CPU wait failed", attempts);
      check(!s2->evicted && !s2->va_freed,
            "a replay address is not freed by a destroy that can neither record nor prove anything");
#if BD201_DEFERRED_API
      dog = watchdog_start("teardown with an unproven replay address", 60000);
      radv_wddm2_deferred_finish(ws);
      watchdog_stop(dog);
      check(!s2->evicted && !s2->va_freed, "and teardown does not free it either: a failed wait is not proof");
#if BD201_BO_POOL
      radv_wddm2_bo_pool_finish(ws);
#endif
#endif
      fail_cpu_waits(false);
   }

   retire(qa, published_of(qa));
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

/* Lead's decision F, 2026-10-21, in place of a COM-mocked D3D12 harness: the
 * winsys seam of an OPENED import. radv_wddm2_bo_from_handle opens a shared
 * NT handle (QueryResourceInfoFromNtHandle, OpenResourceFromNtHandle), maps it
 * into our address space itself and makes it resident, so the ADDRESS and the
 * residency are ours even though the allocation is the application's or another
 * process's. A destroy must therefore keep the same promise as for an internal
 * BO: no Evict and no FreeGpuVirtualAddress of it before the destroy-time
 * values. The destroy itself names the resource and no allocation list at all
 * (radv_wddm2_bo.c:1418-1422), which is how the mock recognises it.
 *
 * Under V2 there is no Evict of it at all (R1 again: the OS owns the residency
 * of the allocation we opened).
 *
 * The hosted and the 32-bit lines refuse an NT-handle import while the host
 * seam is active (radv_wddm2_bo.c:1022-1026, "BC250 hosted: NT-handle memory
 * import refused"), so on those lines the case records the refusal and tests
 * nothing: the seam itself has the two operations (BC250_HOST ops 37 and 38),
 * so admitting them through a mocked host is a V2 decision, not a capability.
 *
 * Expected: on the system and desktop lines V0 fails, destroy_now evicting the
 * opened allocation and freeing our mapping under unretired work. On hosted
 * and 32-bit it is a note. */
static void
test_opened_import(void)
{
   struct radv_wddm2_winsys *ws = make_ws();
   struct radeon_winsys_ctx *a = new_ctx(ws);
   struct radv_wddm2_queue *qa = gfx(a);
   check(submit_one(ws, a) == VK_SUCCESS, "the queue published progress 1 and keeps it in flight");

   struct radeon_winsys_bo *bo = NULL;
   uint64_t alloc_size = 0;
   const VkResult result = ws->base.buffer_from_handle(&ws->base, (void *)(uintptr_t)0x7001, 0, &bo, &alloc_size);
   if (result != VK_SUCCESS || !bo) {
      note("this line refuses to open an NT handle while the host seam is active (%d): the case tests nothing here",
           (int)result);
      retire(qa, 1);
      ws->base.ctx_destroy(a);
      contract();
      free(ws);
      return;
   }
   check(bo->va && alloc_size, "an opened import, mapped at 0x%" PRIx64 " (%" PRIu64 " bytes)", bo->va, alloc_size);

   const uint32_t handle = bo->handle;
   const unsigned evicts = h.evicts, va_frees = h.va_frees;
   struct watch *s = destroy_bo(ws, bo, "opened_import");
   check(!s->evicted && !s->va_freed,
         "its destroy neither evicts the opened allocation nor frees our mapping (%u evicts, %u VA frees)",
         h.evicts - evicts, h.va_frees - va_frees);
   if (!s->released)
      check(alloc_live(handle), "the allocation is still live");

   check(submit_one(ws, a) == VK_SUCCESS, "the queue submits again, progress 2");
   check(!s->evicted && !s->va_freed, "and still nothing of it is evicted or freed: progress 1 has not retired");

   retire(qa, 1);
   struct radeon_winsys_bo *drain = new_bo(ws, 65536);
   destroy_bo(ws, drain, "opened_import/drain");
   note("after the proof the opened import is %s (evicted=%d va_freed=%d)", s->released ? "released" : "still held",
        s->evicted, s->va_freed);
   v2_req(!s->evicted, "R1: an opened allocation is never evicted by us (%d)", s->evicted);

   retire(qa, published_of(qa));
   ws->base.ctx_destroy(a);
   contract();
   free(ws);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"in_flight", test_in_flight},
   {"no_host_memory", test_no_host_memory},
   {"create_rollback", test_create_rollback},
   {"two_queues", test_two_queues},
   {"fixed_target", test_fixed_target},
   {"device_lost", test_device_lost},
   {"wait_deadlock", test_wait_deadlock},
   {"teardown", test_teardown},
   {"abandoned_queue", test_abandoned_queue},
   {"regression_a", test_regression_a},
   {"borrowed_ok", test_borrowed_ok},
   {"lost_proof", test_lost_proof},
   {"failed_submit", test_failed_submit},
   {"virtual_bo", test_virtual_bo},
   {"handoff_race", test_handoff_race},
   {"wait_budget", test_wait_budget},
   {"create_sweep", test_create_sweep},
   {"kept_bytes", test_kept_bytes},
   {"replayable", test_replayable},
   {"opened_import", test_opened_import},
};

int
main(int argc, char **argv)
{
   bool ran = false;
   InitializeCriticalSection(&mock_lock);
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      current = tests[i].name;
      tests[i].fn();
      ran = true;
   }
   if (!ran) {
      fprintf(stderr, "no such test: %s\n", argv[1]);
      return 2;
   }
   printf("tests=%u failures=%u\n", checks, failures);
   return failures ? 1 : 0;
}
