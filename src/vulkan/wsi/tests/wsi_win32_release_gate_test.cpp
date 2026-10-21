/* SPDX-License-Identifier: MIT */
/*
 * The host test of the desktop line's release gate (wsi_win32_release_gate.h, BD-213).
 *
 * The mock is a present queue that retires in order and stalls on a wait nothing satisfies, which is the
 * state the defect is about: a copy of the swapchain's image is queued behind a wait for the
 * application's own submission, and the application destroys the swapchain. Each case drives the shipped
 * decision code over that mock and asserts what the teardown did, in order.
 *
 * The cases, which are the ones the review asked for:
 *   1. a chain whose copy is still queued: the drain expires, nothing of the chain is released, and no
 *      release happens before the fence reaches its value;
 *   2. the same chain after the queue retires the copy: the drain completes and every object is released
 *      exactly once, in the order the contract wants (images, swap chain, blit fences, then the WSI base);
 *   3. the calls of the drain itself fail, one at a time (no fence, a refused Signal), with and without a
 *      removed device: a removed device releases, anything else keeps;
 *   4. a chain that queued no copy at all (a GDI chain, or a creation that failed half way): no drain is
 *      attempted and the teardown releases as it always did;
 *   5. the teardown of b164fa63 over the same mock: four releases while the queue is busy, nothing asked
 *      of the queue and nothing said. That is the defect, measured rather than described.
 *
 * --negative-control inverts every expectation, so the run must fail every case. A test that cannot fail
 * proves nothing.
 *
 * Builds on its own: the header has no Direct3D and no Windows types.
 */
#include "../wsi_win32_release_gate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static unsigned checks, failures;
static bool invert;

static void
check(bool ok, const char *what)
{
   ++checks;
   if (ok == invert) {
      ++failures;
      std::printf("FAIL %s\n", what);
   }
}

/* A present queue that retires in order. A submitted copy sits behind a wait on `wait_value` of the
 * image's shared blit timeline; `signalled` is how far the application's own submission has got. The
 * drain fence is the queue's last item, so it completes only when everything before it retired. */
struct MockQueue {
   bool have_queue = true;
   bool have_device = true;
   bool fence_creatable = true;
   bool signal_ok = true;
   bool removed = false;

   bool copy_queued = false;   /* a present copy is on the queue */
   uint64_t wait_value = 1;    /* the value that copy waits for */
   uint64_t signalled = 0;     /* what the application's submission has signalled */

   /* what the drain did */
   unsigned fences_made = 0, fences_released = 0, signals = 0, waits = 0;
   uint32_t last_deadline_ms = 0;
   bool fence_live = false;

   /* what the teardown did, and when */
   unsigned images_released = 0, swapchains_released = 0, fence_arrays_released = 0, bases_released = 0;
   unsigned pins = 0;
   enum wsi_win32_release_state pinned_state = WSI_WIN32_RELEASE_DRAINED;
   unsigned releases_while_queue_busy = 0;
   unsigned order = 0, image_order = 0, swapchain_order = 0, fences_order = 0, base_order = 0;

   bool queue_busy() const { return copy_queued && signalled < wait_value; }

   void note_release(unsigned *slot)
   {
      *slot = ++order;
      if (queue_busy())
         ++releases_while_queue_busy;
   }

   /* the adapter the gate asks for */
   bool needs_drain() const { return copy_queued; }

   bool create_fence()
   {
      if (!have_device || !have_queue || !fence_creatable)
         return false;
      ++fences_made;
      fence_live = true;
      return true;
   }

   bool signal_fence()
   {
      if (!signal_ok)
         return false;
      ++signals;
      return true;
   }

   bool wait_fence(uint32_t deadline_ms)
   {
      ++waits;
      last_deadline_ms = deadline_ms;
      /* The fence is behind the copy: it completes only once the copy is no longer waiting. */
      if (queue_busy())
         return false;
      copy_queued = false;
      return true;
   }

   void release_fence()
   {
      ++fences_released;
      fence_live = false;
   }

   bool device_removed() const { return removed; }

   void pin(enum wsi_win32_release_state state)
   {
      ++pins;
      pinned_state = state;
   }

   void release_images() { ++images_released; note_release(&image_order); }
   void release_swapchain() { ++swapchains_released; note_release(&swapchain_order); }
   void release_fences() { ++fence_arrays_released; note_release(&fences_order); }
   void release_base() { ++bases_released; note_release(&base_order); }

   unsigned releases() const
   {
      return images_released + swapchains_released + fence_arrays_released + bases_released;
   }
};

/* Case 1 and 2: one chain with a copy still queued, then the same chain once the copy can retire. */
static void
test_queued_copy()
{
   MockQueue q;
   q.copy_queued = true;
   q.wait_value = 1;
   q.signalled = 0; /* the application's submission has not signalled: the copy cannot run */

   struct wsi_win32_drain_outcome outcome = wsi_win32_release_swapchain(q);
   check(outcome.state == WSI_WIN32_RELEASE_UNPROVEN, "a queue that cannot drain is unproven");
   check(!wsi_win32_release_allowed(outcome.state), "an unproven queue does not open the gate");
   check(q.waits == 1 && q.last_deadline_ms == 2000, "the drain waited once, to the two-second deadline");
   check(q.releases() == 0, "nothing of the chain is released while the copy is queued");
   check(q.releases_while_queue_busy == 0, "no release happened before the fence reached its value");
   check(q.pins == 1 && q.pinned_state == WSI_WIN32_RELEASE_UNPROVEN, "the chain is pinned, once");
   check(outcome.fence_kept && q.fences_released == 0,
         "the drain fence stays with the queue, whose Signal of it is outstanding");

   /* The application's submission signals, so the copy retires and a second teardown may release. */
   q.signalled = q.wait_value;
   q.copy_queued = true;
   const unsigned pins_before = q.pins;
   outcome = wsi_win32_release_swapchain(q);
   check(outcome.state == WSI_WIN32_RELEASE_DRAINED, "a drained queue proves the copy retired");
   check(wsi_win32_release_allowed(outcome.state), "a drained queue opens the gate");
   check(q.pins == pins_before, "a drained teardown pins nothing");
   check(!outcome.fence_kept && q.fences_released == 1, "a completed drain releases its own fence");
   check(q.images_released == 1 && q.swapchains_released == 1 && q.fence_arrays_released == 1 &&
            q.bases_released == 1,
         "every group of objects is released exactly once");
   check(q.image_order < q.swapchain_order && q.swapchain_order < q.fences_order &&
            q.fences_order < q.base_order,
         "the release order is images, swap chain, blit fences, then the WSI base");
   check(q.releases_while_queue_busy == 0, "no release happened while the queue was busy");
}

/* The teardown of b164fa63, the code this gate replaces: every group released in the same order, with
 * nothing asked of the queue. Driving it over the same mock is what shows the defect instead of asserting
 * it in a comment: on a chain whose copy is still queued, all four groups go while the queue is busy.
 * DEFECTS BD-213 asks that the cases fail on b164fa63; this is that failure, as a value. */
template <typename Adapter>
static void
release_swapchain_b164fa63(Adapter &adapter)
{
   adapter.release_images();
   adapter.release_swapchain();
   adapter.release_fences();
   adapter.release_base();
}

static void
test_old_teardown()
{
   MockQueue q;
   q.copy_queued = true;
   q.wait_value = 1;
   q.signalled = 0;

   release_swapchain_b164fa63(q);
   check(q.releases() == 4, "the old teardown released every group");
   check(q.releases_while_queue_busy == 4,
         "every one of them went while the queue was still reading the chain");
   check(q.fences_made == 0 && q.waits == 0, "the old teardown asked the queue nothing");
   check(q.pins == 0, "and it pinned nothing, so nothing said what it had done");
}

/* Case 3: the drain's own calls fail. A removed device is the only failure that releases. */
static void
test_drain_failures()
{
   for (unsigned step = 0; step < 2; step++) {
      for (unsigned removed = 0; removed < 2; removed++) {
         MockQueue q;
         q.copy_queued = true;
         q.signalled = 0;
         q.fence_creatable = step != 0;
         q.signal_ok = step != 1;
         q.removed = removed != 0;

         const struct wsi_win32_drain_outcome outcome = wsi_win32_release_swapchain(q);
         if (removed) {
            check(outcome.state == WSI_WIN32_RELEASE_REMOVED, "a removed device is releasable");
            check(q.releases() == 4, "a removed device releases the chain");
            check(q.pins == 0, "a removed device pins nothing");
         } else {
            check(outcome.state == WSI_WIN32_RELEASE_UNPROVEN, "a failed drain call is unproven");
            check(q.releases() == 0, "a failed drain call releases nothing");
            check(q.pins == 1, "a failed drain call pins the chain");
         }
         check(q.waits == 0, "a drain that could not be started does not wait");
         if (step == 0)
            check(q.fences_made == 0 && q.fences_released == 0, "no fence, nothing to release");
         else
            check(q.fences_made == 1 && q.fences_released == 1,
                  "a refused Signal gives the fence reference back");
         check(!q.fence_live, "no drain fence is left owned by the caller");
      }
   }

   /* No present queue and no device at all: the same answer, and no call on either. */
   MockQueue q;
   q.copy_queued = true;
   q.signalled = 0;
   q.have_queue = false;
   q.have_device = false;
   const struct wsi_win32_drain_outcome outcome = wsi_win32_release_swapchain(q);
   check(outcome.state == WSI_WIN32_RELEASE_UNPROVEN, "no queue is no proof");
   check(q.releases() == 0 && q.pins == 1, "no queue keeps the chain");
}

/* Case 4: a chain that queued no copy. A GDI chain, or a route chain whose creation failed half way. */
static void
test_no_copies()
{
   MockQueue q;
   q.copy_queued = false;

   const struct wsi_win32_drain_outcome outcome = wsi_win32_release_swapchain(q);
   check(outcome.state == WSI_WIN32_RELEASE_DRAINED, "a chain with no copies needs no proof");
   check(q.fences_made == 0 && q.signals == 0 && q.waits == 0, "no drain is attempted");
   check(q.releases() == 4, "the teardown releases everything, as it always did");
   check(q.pins == 0, "a chain with no copies is never pinned");
}

/* The parts of the header that stand on their own: the names, the deadline and its conversion. */
static void
test_gate_values()
{
   check(!wsi_win32_release_allowed(WSI_WIN32_RELEASE_UNPROVEN), "unproven never releases");
   check(wsi_win32_release_allowed(WSI_WIN32_RELEASE_DRAINED), "drained releases");
   check(wsi_win32_release_allowed(WSI_WIN32_RELEASE_REMOVED), "removed releases");
   check(!std::strcmp(wsi_win32_release_state_name(WSI_WIN32_RELEASE_UNPROVEN), "unproven"),
         "unproven has its name");
   check(!std::strcmp(wsi_win32_release_state_name(WSI_WIN32_RELEASE_DRAINED), "drained"),
         "drained has its name");
   check(!std::strcmp(wsi_win32_release_state_name(WSI_WIN32_RELEASE_REMOVED), "removed"),
         "removed has its name");
   check(wsi_win32_release_wait_ms(WSI_WIN32_RELEASE_DEADLINE_NS) == 2000, "the deadline is two seconds");
   check(wsi_win32_release_wait_ms(0) == 0, "no deadline is no wait");
   check(wsi_win32_release_wait_ms(1) == 1, "a deadline under a millisecond waits one");
   check(wsi_win32_release_wait_ms(3600000000000ull) == WSI_WIN32_RELEASE_WAIT_MS_MAX,
         "a long deadline stops at the maximum wait");
}

int
main(int argc, char **argv)
{
   if (argc > 2)
      return 2;
   if (argc == 2) {
      if (std::strcmp(argv[1], "--negative-control"))
         return 2;
      invert = true;
   }

   test_gate_values();
   test_old_teardown();
   test_queued_copy();
   test_drain_failures();
   test_no_copies();

   std::printf("wsi release gate: %u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
