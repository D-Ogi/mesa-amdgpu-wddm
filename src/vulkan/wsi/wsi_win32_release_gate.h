/* SPDX-License-Identifier: MIT */
/*
 * The release gate of the desktop line's DXGI present route (BD-213).
 *
 * `wsi_win32_swapchain_destroy` used to release everything the route holds the moment the application
 * destroyed the swapchain: the imported image memory a queued copy reads, the DXGI swap chain whose back
 * buffer that copy writes, the shared blit fences whose Wait and Signal are still queued, and the WSI's
 * own semaphores and fences that a pending submission of the application still waits on and signals.
 * Vulkan does not ask the application to wait for that copy, so the queue may name all of it.
 * `ref\win32-docs\desktop-src\direct3d12\binding-model.md:35` is the rule: the application must make sure
 * the GPU has finished referencing a resource before it frees it.
 *
 * This header holds the decision, not the Direct3D calls, so a host test drives the same code the driver
 * runs. The caller gives one adapter that can create a fence on the present queue's device, signal it,
 * wait for it, read the device's removal state and release the objects of the chain. The adapter of the
 * driver is in `wsi_common_win32.cpp`; the adapter of the test is a mock queue that retires in order and
 * stalls on an unsatisfied wait.
 *
 * The system line (`wsi_win32_deadline.h`, `wsi_win32_flush_d3d12_queue`) decided the same question
 * first; the states and the rule that an expired wait releases nothing are the same there. The deadline
 * machinery of that line, its route retirement and its presenter state are not here: this line has
 * none of it, and the gate does not need it.
 */
#ifndef WSI_WIN32_RELEASE_GATE_H
#define WSI_WIN32_RELEASE_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* What a bounded drain of the present queue proved, which decides what the caller may release.
 *
 * DRAINED is the proof: the drain fence reached its value, so everything submitted before it retired,
 * including the copies of this chain.
 * REMOVED is the other releasable state: a device the runtime removed executes nothing further and will
 * not read these objects again. The alternative, pinning every resource of every chain of a process
 * whose device died, leaks without bound for no gain. No local page states that the queue of a removed
 * device has stopped reading, so this is an assumption, written here and not in a commit message. It is
 * the same assumption as the system line's, and the same lab measurement (a TDR) decides both.
 * UNPROVEN is every other end: no fence, a failed Signal, or a wait that expired. The queue may still
 * name the objects, so nothing of the chain is released.
 */
enum wsi_win32_release_state {
   WSI_WIN32_RELEASE_UNPROVEN = 0, /* the queue may still read this chain's resources */
   WSI_WIN32_RELEASE_DRAINED,      /* the drain fence completed: everything before it retired */
   WSI_WIN32_RELEASE_REMOVED,      /* the present queue's device is removed */
};

static inline bool
wsi_win32_release_allowed(enum wsi_win32_release_state state)
{
   return state == WSI_WIN32_RELEASE_DRAINED || state == WSI_WIN32_RELEASE_REMOVED;
}

static inline const char *
wsi_win32_release_state_name(enum wsi_win32_release_state state)
{
   switch (state) {
   case WSI_WIN32_RELEASE_DRAINED: return "drained";
   case WSI_WIN32_RELEASE_REMOVED: return "removed";
   case WSI_WIN32_RELEASE_UNPROVEN: return "unproven";
   default: return "unknown";
   }
}

/* The drain waits two seconds, the deadline the system line's route uses for the same wait. A present
 * copy of one image of one chain is a few hundred microseconds of work, so two seconds is already the
 * sign of a queue that is not making progress; past it the chain is pinned rather than waited for. */
#define WSI_WIN32_RELEASE_DEADLINE_NS 2000000000ull
#define WSI_WIN32_RELEASE_WAIT_MS_MAX 60000u

static inline uint32_t
wsi_win32_release_wait_ms(uint64_t ns)
{
   uint64_t ms;

   if (!ns)
      return 0;
   ms = ns / 1000000ull;
   if (!ms)
      return 1;
   if (ms > (uint64_t)WSI_WIN32_RELEASE_WAIT_MS_MAX)
      return WSI_WIN32_RELEASE_WAIT_MS_MAX;
   return (uint32_t)ms;
}

struct wsi_win32_drain_outcome {
   enum wsi_win32_release_state state;
   /* True when the queue keeps the last reference of the drain fence: its Signal of that fence is still
    * outstanding, so the fence is one object this teardown deliberately gives up instead of releasing
    * under the scheduler. One fence for a route that is being abandoned anyway. */
   bool fence_kept;
};

/* The bounded drain. The adapter gives:
 *   bool create_fence();          a fence on the present queue's device, at value 0
 *   bool signal_fence();          the queue signals that fence behind everything already submitted
 *   bool wait_fence(uint32_t ms); true when the fence reached its value inside the deadline
 *   void release_fence();         drops the caller's reference
 *   bool device_removed();        the present queue's device is removed
 */
template <typename Adapter>
static inline struct wsi_win32_drain_outcome
wsi_win32_drain_queue(Adapter &adapter, uint32_t deadline_ms)
{
   struct wsi_win32_drain_outcome outcome = {WSI_WIN32_RELEASE_UNPROVEN, false};

   if (!adapter.create_fence()) {
      outcome.state = adapter.device_removed() ? WSI_WIN32_RELEASE_REMOVED : WSI_WIN32_RELEASE_UNPROVEN;
      return outcome;
   }
   if (!adapter.signal_fence()) {
      /* Nothing of ours is queued on this fence, so the reference comes back here. */
      adapter.release_fence();
      outcome.state = adapter.device_removed() ? WSI_WIN32_RELEASE_REMOVED : WSI_WIN32_RELEASE_UNPROVEN;
      return outcome;
   }
   if (adapter.wait_fence(deadline_ms)) {
      adapter.release_fence();
      outcome.state = WSI_WIN32_RELEASE_DRAINED;
      return outcome;
   }
   outcome.fence_kept = true;
   outcome.state = adapter.device_removed() ? WSI_WIN32_RELEASE_REMOVED : WSI_WIN32_RELEASE_UNPROVEN;
   return outcome;
}

/* The teardown of one swapchain, in the order the contract asks for: prove first, release after. The
 * adapter adds to the five calls above:
 *   bool needs_drain();                      this chain queued copies on a present queue
 *   void pin(enum wsi_win32_release_state);  nothing was released; say so once
 *   void release_images();                   the copy lists, the blit resources, the imported memory
 *                                            and the back buffers of every image
 *   void release_swapchain();                the DXGI swap chain
 *   void release_fences();                   the shared blit fences
 *   void release_base();                     the WSI base: its semaphores and its own fences
 *
 * A chain that queued no copy needs no drain: either it is a GDI chain, whose images are CPU copies that
 * no D3D12 queue reads, or it is a route chain that never presented, and a creation that failed half way
 * must not leak over a queue that has nothing of it. Every release call runs only after the gate opened, and `pin` is the only thing a refused gate
 * does, so a caller cannot release half of a chain the queue still reads.
 */
template <typename Adapter>
static inline struct wsi_win32_drain_outcome
wsi_win32_release_swapchain(Adapter &adapter)
{
   struct wsi_win32_drain_outcome outcome = {WSI_WIN32_RELEASE_DRAINED, false};

   if (adapter.needs_drain())
      outcome = wsi_win32_drain_queue(adapter, wsi_win32_release_wait_ms(WSI_WIN32_RELEASE_DEADLINE_NS));
   if (!wsi_win32_release_allowed(outcome.state)) {
      adapter.pin(outcome.state);
      return outcome;
   }
   adapter.release_images();
   adapter.release_swapchain();
   adapter.release_fences();
   adapter.release_base();
   return outcome;
}

#endif /* WSI_WIN32_RELEASE_GATE_H */
