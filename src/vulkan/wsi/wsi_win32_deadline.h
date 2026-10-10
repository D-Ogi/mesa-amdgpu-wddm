/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * wsi_win32_deadline.h - the deadlines, the degrade rule and the retirement rules of the Win32 DXGI
 * present route, as plain C with no Windows, Vulkan or Mesa header, so a host test drives the
 * production rules (src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_wsi_route_test.c).
 *
 * Why the route has deadlines at all: on 2026-10-09 the first lab trial of the DXGI route froze
 * every client that used it (Quake II RTX and vkcube) before their first present, with the GPU
 * idle, no packet outstanding and therefore no TDR to end it (BD-105). A wait with no deadline
 * between the application's Vulkan device and the presenter's D3D12 device cannot be recovered
 * from by anything: the process stays alive and shows nothing. The rule this header carries is
 * that no CPU wait this route owns may be unbounded, and that a route whose first present never
 * completes gives the window back to the GDI path instead of keeping a dead swapchain.
 *
 * The D3D12 queue's own GPU-side wait (ID3D12CommandQueue::Wait) takes no deadline; it is covered
 * from the other end, because the acquire that waits for that copy is bounded here.
 *
 * Round 4b rewrites three rules after an independent audit of round 4 (the four-day audit of
 * 2026-10-10, findings V1, V3 and V5). What they have in common is that a deadline that expired
 * was being read as a fact about the presenter, and it is not one:
 *
 *   V1  a timeout proves neither that the presenter finished nor that it is dead, so a host signal
 *       of a shared timeline on expiry could pass a Vulkan signal that was still pending, which
 *       VUID-VkSemaphoreSignalInfo-value-03258/03259 forbid. The three presenter states are now
 *       separate (live-but-slow, proved removed, unproven), and a host signal needs the removal
 *       proved AND the preceding Vulkan signal completed.
 *   V5  a drain wait that expired is not proof that the queue stopped referencing the resources the
 *       caller was about to release (D3D12 object lifetime). The flush now answers drained, removed
 *       or unproven, and unproven keeps the whole dependent set alive.
 *   V3  the two route flags were plain bools written and read by threads driving independent
 *       swapchains. They are one word now, and every access is an atomic operation.
 */
#ifndef WSI_WIN32_DEADLINE_H
#define WSI_WIN32_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The atomic word this header's state lives in.
 *
 * Why not <stdatomic.h> or <atomic>: this header is included by a C11 translation unit (the host
 * test, cl /TC /std:c11) and by a C++ one (wsi_common_win32.cpp), and MSVC's C11 atomics and C++
 * <atomic> are not the same spelling. Two compiler intrinsics are, and they need no library, no
 * Windows header and no MSVC version test. The LLVM memory model is the rule being honoured here
 * (ref/llvm-project-19.1.7/llvm/docs/Atomics.rst:148-165: a plain load or store shared by threads
 * must be protected by a lock or other synchronisation); a one-way bool transition is not an
 * exemption from it, whatever the hardware does with a byte store.
 *
 * The load is an interlocked OR with a no-op operand, which is a read-modify-write and therefore
 * costs a locked instruction. The paths that read it are one per acquire and one per swapchain
 * creation, so the cost is a few hundred locked instructions a second at frame rate, against a
 * present whose own measured cost is milliseconds.
 */
#if defined(_MSC_VER)
#include <intrin.h>
static inline uint32_t
wsi_win32_atomic_load32(uint32_t *word)
{
   return (uint32_t)_InterlockedOr((volatile long *)word, 0);
}
static inline uint32_t
wsi_win32_atomic_cas32(uint32_t *word, uint32_t expect, uint32_t desired)
{
   return (uint32_t)_InterlockedCompareExchange((volatile long *)word, (long)desired, (long)expect);
}
#else
static inline uint32_t
wsi_win32_atomic_load32(uint32_t *word)
{
   return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}
static inline uint32_t
wsi_win32_atomic_cas32(uint32_t *word, uint32_t expect, uint32_t desired)
{
   __atomic_compare_exchange_n(word, &expect, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
   return expect;
}
#endif

/* The deadline of a wait the route owns, in nanoseconds. Two seconds is a chosen bound, not a
 * measured one: no present of this route has ever completed, so the cost of one is unknown. What is
 * measured is the GDI path of the same file at 1920x1200, whose median present costs 3.18 ms of CPU
 * (copy 1343 us plus BitBlt 1832 us over 640 rows, the b26 present log). A GPU copy and a Present1
 * have no reason to need three orders of magnitude more than that, so two seconds sits far above any
 * present a working route should need and far below the three-minute bound of a lab trial or a
 * player's patience. Arm A2 of the lab plan is the measurement that first says what a present of
 * this route really costs; if one ever needs more than this bound, the bound is wrong and that arm
 * is where it shows.
 */
#define WSI_WIN32_ROUTE_DEADLINE_NS 2000000000ull

/* The deadline of the one wait the RETIREMENT itself makes: a bounded wait on the Vulkan side of a
 * shared blit timeline, for the value the application's own first submission signals, before a host
 * signal of the next value is allowed (V1 below). It sits inside the route's own deadline, because
 * by the time it runs the route has already spent that deadline once. The wait is for work on the
 * application's own healthy device, so it is expected to be satisfied immediately or never: a tenth
 * of the route deadline is a bound, not a budget.
 */
#define WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS 200000000ull

/* WaitForSingleObject takes milliseconds and reads 0xFFFFFFFF as INFINITE, which is the value this
 * route must never pass. The cap keeps one millisecond of head room below it.
 */
#define WSI_WIN32_WAIT_MS_MAX 0xfffffffeu

/* Milliseconds for a Win32 wait, from a deadline in nanoseconds: never INFINITE, and never 0 for a
 * deadline that is not 0 (a sub-millisecond deadline still waits once).
 */
static inline uint32_t
wsi_win32_wait_ms(uint64_t ns)
{
   uint64_t ms;
   if (!ns)
      return 0;
   ms = ns / 1000000ull;
   if (!ms)
      return 1;
   if (ms > (uint64_t)WSI_WIN32_WAIT_MS_MAX)
      return WSI_WIN32_WAIT_MS_MAX;
   return (uint32_t)ms;
}

/* What the route has shown so far, per Vulkan instance (struct wsi_win32), as one atomic word.
 *
 * PRESENTED: a present of this route COMPLETED and an acquire after it returned an image. DEAD: a
 * wait of the route expired before that. A dead route makes every later swapchain of the instance
 * take CPU images, so an application that stops in one of the two waits this route bounds gets a
 * usable window back instead of a frozen thread, and the next swapchain does not try the route again.
 *
 * PRESENTED used to be set the moment IDXGISwapChain3::Present1 returned, and the lab arm of
 * 2026-10-09 (scratch\b27\vk-wsi-dxgi\lab-results-2026-10-09\RESULT.md) is what that cost: the first
 * present of the route ran to present1-returned, presented went true, the deadline of the acquire
 * went away with it, and the client froze one frame later in an acquire that was unbounded again.
 * Present1 only queues a present; it says nothing about the copy, the flip or the fence. The flag
 * that disables a wait is now set only by something that proves that very wait can finish.
 *
 * One word, and every access an atomic operation (V3). Before round 4b these were two plain bools,
 * written by whichever thread acquired or whichever thread's wait expired, and read by acquire and by
 * swapchain creation on any thread, with a comment that called the race benign because every
 * transition is one way. C++ grants no such exemption, and the two flags are in fact read together
 * (the expiry rule below asks whether PRESENTED is set before it sets DEAD), which is exactly the
 * dependency that comment said to make atomic first. One word also makes both transitions
 * compare-and-swap, so note_acquired returns true to exactly one caller even when two threads
 * acquire two swapchains at the same moment.
 *
 * What this does not cover: a freeze in a call that no deadline of ours sits in front of, which is
 * everything in wsi_win32_image_init (GetBuffer, CreateCommittedResource, CreateSharedHandle, the
 * Vulkan import of the D3D12 resource, the layout check, the command list). Such a thread is still a
 * frozen thread, and the stage line names the call.
 */
#define WSI_WIN32_ROUTE_PRESENTED 0x1u
#define WSI_WIN32_ROUTE_DEAD      0x2u

struct wsi_win32_route_state {
   uint32_t flags;
};

static inline uint32_t
wsi_win32_route_flags(struct wsi_win32_route_state *state)
{
   return wsi_win32_atomic_load32(&state->flags);
}

static inline bool
wsi_win32_route_presented(struct wsi_win32_route_state *state)
{
   return (wsi_win32_route_flags(state) & WSI_WIN32_ROUTE_PRESENTED) != 0;
}

static inline bool
wsi_win32_route_usable(struct wsi_win32_route_state *state)
{
   return (wsi_win32_route_flags(state) & WSI_WIN32_ROUTE_DEAD) == 0;
}

/* The timeout an acquire of a DXGI swapchain may really wait. A route that has not completed one
 * present is not known to work, so the application's wait (vkAcquireNextImageKHR with UINT64_MAX is
 * the normal case) becomes the route's own deadline; the caller then reports the swapchain as out of
 * date, which sends the application back through swapchain creation and onto the CPU-image path.
 * Once the route has presented, the application's timeout is its own business again.
 *
 * route_presented is wsi_win32_route_presented(), and what sets it is the whole point of the
 * round-3 fix: see the comment on wsi_win32_route_note_acquired below.
 */
static inline uint64_t
wsi_win32_acquire_timeout_ns(uint64_t app_timeout_ns, bool route_presented)
{
   if (route_presented || app_timeout_ns <= WSI_WIN32_ROUTE_DEADLINE_NS)
      return app_timeout_ns;
   return WSI_WIN32_ROUTE_DEADLINE_NS;
}

static inline bool
wsi_win32_acquire_timeout_capped(uint64_t app_timeout_ns, bool route_presented)
{
   return wsi_win32_acquire_timeout_ns(app_timeout_ns, route_presented) != app_timeout_ns;
}

/* Has the present of one image of the route actually completed?
 *
 * How completion is observed, decided from this file's own code and not from a guess:
 *
 *   image_present_value is the value wsi_dxgi_blit signalled on that image's SHARED BLIT FENCE after
 *   ExecuteCommandLists of the copy into the back buffer (chain->base.blit.timeline_values[i], one
 *   past the value the application's own queue signals). That fence is the D3D12 side of the
 *   Vulkan timeline semaphore of the same image, and the chain holds it in d3d12_blit_fences[i].
 *   So fence_completed_value >= image_present_value says, in one read of one object the route already
 *   owns: the presenter queue's Wait for the application's signal was satisfied, the copy executed,
 *   and the queue retired the Signal. That is the end of the route's own GPU work and it is exactly
 *   the thing BD-105 says never happens (the kernel driver counted blits 0 while both stacks waited).
 *
 * The two alternatives were rejected from the code, not on taste:
 *
 *   DXGI frame statistics (IDXGISwapChain::GetFrameStatistics, PresentCount) describe the
 *   presentation engine, not our copy, are documented to fail with DXGI_ERROR_FRAME_STATISTICS_
 *   DISJOINT on a first call, and this file also creates composition swap chains shown through a
 *   DirectComposition visual (hwnd_target false), for which they report nothing useful. A proof of
 *   the route that can fail for a reason unrelated to the route would either hold the deadline on a
 *   working route for ever or be read as a success; neither is acceptable for the one flag that
 *   turns an unbounded wait back on.
 *
 *   A fresh present fence (an ID3D12Fence signalled on the presenter queue after Present1) would
 *   prove only that the queue drained past the copy, which the blit fence already proves, and
 *   wsi_win32_flush_d3d12_queue in wsi_common_win32.cpp shows what that costs per frame: an object,
 *   an event and a wait.
 *
 * What the blit fence does NOT prove is that DWM or the display consumed the frame. The proof that
 * would is DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT with GetFrameLatencyWaitableObject,
 * and that is a change at swap-chain creation (the chains are created with ALLOW_TEARING today, not
 * the waitable flag) with a buffer count to match. It is the next step, not this one, and until the
 * route completes one copy at all it would answer a question nobody has reached yet.
 *
 * image_present_value 0 means the image was never presented: the timeline values are
 * pre-incremented, so a value a present has signalled is always at least 1. UINT64_MAX as a
 * completed value is NOT a completion: it is the documented answer of ID3D12Fence::GetCompletedValue
 * for a REMOVED device (sdk-api nf-d3d12-id3d12fence-getcompletedvalue.md:61), and reading it as a
 * very high fence value is how a removed presenter could otherwise prove the route works.
 */
static inline bool
wsi_win32_route_present_complete(uint64_t image_present_value, uint64_t fence_completed_value)
{
   if (!image_present_value || fence_completed_value == UINT64_MAX)
      return false;
   return fence_completed_value >= image_present_value;
}

/* An acquire of the route returned an image. This is the ONLY place that may lift the route's
 * deadlines, and it does so only when the last present OF THAT IMAGE completed by the rule above.
 * The image matters: an image cannot be presented again before it is acquired, so the acquired
 * image's own timeline value is the value its last blit signalled, while the chain's most recent
 * present belongs to another image and would stay one frame behind for ever on a two-image chain.
 * Both halves are needed, and each one covers the other's blind spot:
 *
 *   the completed present says the presenter's GPU work ran at all, which Present1's return did not;
 *   the returned acquire says the wait that presented would disable can in fact finish, which a
 *   fence value on its own does not (an acquire that reaches the back buffer by another path, or a
 *   fence that completes while the application's own second submit never does, would otherwise lift
 *   the cap on a route that still freezes).
 *
 * Returns true the one time it lifts the deadlines, so the caller logs that line once; the
 * compare-and-swap is what makes "the one time" true across threads. A route that a wait already
 * retired is never revived: its chains are being given back to the CPU-image path, and a fence that
 * completes after that is too late to matter.
 */
static inline bool
wsi_win32_route_note_acquired(struct wsi_win32_route_state *state,
                              uint64_t image_present_value, uint64_t fence_completed_value)
{
   if (!wsi_win32_route_present_complete(image_present_value, fence_completed_value))
      return false;
   for (;;) {
      const uint32_t have = wsi_win32_atomic_load32(&state->flags);
      if (have & (WSI_WIN32_ROUTE_PRESENTED | WSI_WIN32_ROUTE_DEAD))
         return false;
      if (wsi_win32_atomic_cas32(&state->flags, have, have | WSI_WIN32_ROUTE_PRESENTED) == have)
         return true;
   }
}

/* A bounded wait of the route expired. Before the first present this retires the route for the
 * instance; after it, one late wait is a slow frame, not a broken route, and the route stays.
 * Returns true when the route is now dead.
 */
static inline bool
wsi_win32_route_wait_expired(struct wsi_win32_route_state *state)
{
   for (;;) {
      const uint32_t have = wsi_win32_atomic_load32(&state->flags);
      if (have & WSI_WIN32_ROUTE_PRESENTED)
         return (have & WSI_WIN32_ROUTE_DEAD) != 0;
      if (have & WSI_WIN32_ROUTE_DEAD)
         return true;
      if (wsi_win32_atomic_cas32(&state->flags, have, have | WSI_WIN32_ROUTE_DEAD) == have)
         return true;
   }
}

/* V1. What is KNOWN about the presenter's D3D12 device, which a wait that expired does not say.
 *
 * Round 4 had two states, alive and gone, and inferred the second one from a timeout. The audit of
 * 2026-10-10 is right that a timeout separates nothing: a presenter that is merely slow, a presenter
 * whose device was removed, and a presenter nothing could be read from all look the same from inside
 * an expired wait. Only two readings say anything, and each one can be absent:
 *
 *   ID3D12Device::GetDeviceRemovedReason, which answers S_OK for a live device and the removal
 *   HRESULT for a removed one. It can be unavailable: the driver may not have given the WSI the
 *   get_d3d12_device hook at all, and then there is no device to ask.
 *   ID3D12Fence::GetCompletedValue, whose documented answer for a removed device is UINT64_MAX
 *   (local sdk-api nf-d3d12-id3d12fence-getcompletedvalue.md:61). It can be unavailable too: a chain
 *   with no blit fences has nothing to read.
 *
 * reason_read/removed_reason is the first reading, fence_read/fence_completed_value the second.
 * REMOVED needs one of them to say so. LIVE needs the removal reason to have been read and to be
 * S_OK. Everything else is UNPROVEN, which is a state of its own and not a synonym for either: it is
 * the state in which this round refuses to act.
 */
enum wsi_win32_presenter_state {
   WSI_WIN32_PRESENTER_UNPROVEN = 0, /* nothing could be read: neither alive nor gone is established */
   WSI_WIN32_PRESENTER_LIVE,         /* GetDeviceRemovedReason answered S_OK: live, however slow */
   WSI_WIN32_PRESENTER_REMOVED,      /* proved gone by the removal reason or by the UINT64_MAX sentinel */
};

static inline enum wsi_win32_presenter_state
wsi_win32_presenter_state(bool reason_read, uint32_t removed_reason,
                          bool fence_read, uint64_t fence_completed_value)
{
   if (reason_read && removed_reason != 0u)
      return WSI_WIN32_PRESENTER_REMOVED;
   if (fence_read && fence_completed_value == UINT64_MAX)
      return WSI_WIN32_PRESENTER_REMOVED;
   if (reason_read)
      return WSI_WIN32_PRESENTER_LIVE;
   return WSI_WIN32_PRESENTER_UNPROVEN;
}

static inline const char *
wsi_win32_presenter_state_name(enum wsi_win32_presenter_state state)
{
   switch (state) {
   case WSI_WIN32_PRESENTER_LIVE: return "live";
   case WSI_WIN32_PRESENTER_REMOVED: return "removed";
   case WSI_WIN32_PRESENTER_UNPROVEN: return "unproven";
   default: return "unknown";
   }
}

/* The work a dead route leaves behind, and when the host may release it.
 *
 * The route's synchronisation, read from wsi_common.c (wsi_common_queue_present) and
 * wsi_common_win32.cpp (wsi_dxgi_blit), for one image i of a DXGI swapchain:
 *
 *   1. the application's own queue submission waits for the application's present semaphores and
 *      signals the image's SHARED blit timeline semaphore to V (chain->blit.semaphores[i],
 *      ++chain->blit.timeline_values[i], wsi_common.c:2764-2769);
 *   2. wsi_dxgi_blit makes the presenter's D3D12 queue Wait for V on the same object seen as an
 *      ID3D12Fence (chain->d3d12_blit_fences[i]), execute the copy into the back buffer, and
 *      Signal V+1, which becomes timeline_values[i];
 *   3. a SECOND submission on the application's queue waits for V+1 on that semaphore and signals
 *      the image's Vulkan fence and semaphores - the fence the next acquire of that image waits on.
 *
 * Only the presenter's D3D12 queue can take the semaphore to V+1. When that presenter is gone, step
 * 3 waits for a value nothing will ever signal, and the submission stays on the application's queue
 * for the life of the process. The lab round of 2026-10-09 measured exactly that: the route retired
 * itself after 2000 ms as it was designed to, vkcube took VK_ERROR_OUT_OF_DATE_KHR, entered its
 * recreate path, and froze in vkDeviceWaitIdle - which carries no timeout in the API, so no deadline
 * of this route reaches it. Retiring the route is not enough; the work the route queued has to be
 * retired too.
 *
 * The release is a CPU signal of the Vulkan side of that timeline semaphore to V+1
 * (vkSignalSemaphore). It satisfies step 3's wait, the submission runs - it carries no command
 * buffer on this path, because the copy was the D3D12 queue's - and it signals the image's fence and
 * semaphores. There is no CPU signal for a VkFence in Vulkan and none is needed: the released
 * submission signals them.
 *
 * WHAT ROUND 4 GOT WRONG, and what this rule now admits (V1). Round 4 signalled V+1 whenever the
 * semaphore read below it, on the argument that a timeline takes the same value twice without harm.
 * It does not: ref/Vulkan-Docs/chapters/synchronization.adoc:4824-4826 makes
 * VUID-VkSemaphoreSignalInfo-value-03258 require the value to be GREATER than the semaphore's
 * current value, and 03259 require it to be LESS than the value of any PENDING semaphore signal
 * operation. Step 1's signal of V is such a pending operation while that submission has not retired,
 * and the lab's own reading (semaphore 0, present value 2) is precisely that case: signalling 2 with
 * 1 pending breaks 03259. So two things must hold before a host signal, and a timeout establishes
 * neither of them:
 *
 *   the presenter must be PROVED removed, not merely slow or unreadable - otherwise the value is
 *   still the presenter's to signal and a host signal would call a frame presented that never was;
 *   the preceding Vulkan signal of V must have COMPLETED on our own device, which is proved by the
 *   semaphore reading exactly V (a bounded vkWaitSemaphores on V is how the caller gets there). Then
 *   there is no pending signal on the object at all, 03259 is vacuous, and V+1 > V satisfies 03258.
 *
 * When either half is missing the answer is REFUSE: the caller reports the loss and does not
 * manufacture completion. NOTHING is the honest answer for an image that was never presented, and
 * for a timeline the presenter did reach.
 */
enum wsi_win32_retire_action {
   WSI_WIN32_RETIRE_NOTHING = 0, /* no outstanding wait of ours behind this image */
   WSI_WIN32_RETIRE_SIGNAL,      /* proved safe: host-signal *out_value */
   WSI_WIN32_RETIRE_REFUSE,      /* an outstanding wait, and no proof that a host signal is allowed */
};

static inline enum wsi_win32_retire_action
wsi_win32_route_retire_action(enum wsi_win32_presenter_state presenter,
                              uint64_t image_present_value, uint64_t semaphore_value,
                              uint64_t *out_value)
{
   if (!image_present_value || semaphore_value >= image_present_value)
      return WSI_WIN32_RETIRE_NOTHING;
   if (presenter != WSI_WIN32_PRESENTER_REMOVED)
      return WSI_WIN32_RETIRE_REFUSE;
   /* The only admitted gap is exactly one: the presenter's Signal of V+1. A semaphore below V means
    * step 1's signal of V is still pending, which 03259 forbids us to pass.
    */
   if (semaphore_value != image_present_value - 1u)
      return WSI_WIN32_RETIRE_REFUSE;
   if (out_value)
      *out_value = image_present_value;
   return WSI_WIN32_RETIRE_SIGNAL;
}

/* The value the caller waits for before it asks again: V, the application's own signal, which is one
 * below the value the presenter should have reached. 0 for an image with nothing outstanding.
 */
static inline uint64_t
wsi_win32_route_retire_wait_value(uint64_t image_present_value, uint64_t semaphore_value)
{
   if (!image_present_value || semaphore_value >= image_present_value - 1u)
      return 0;
   return image_present_value - 1u;
}

static inline const char *
wsi_win32_retire_action_name(enum wsi_win32_retire_action action)
{
   switch (action) {
   case WSI_WIN32_RETIRE_NOTHING: return "nothing";
   case WSI_WIN32_RETIRE_SIGNAL: return "signal";
   case WSI_WIN32_RETIRE_REFUSE: return "refuse";
   default: return "unknown";
   }
}

/* What the route answers an application whose acquire expired, from what the retirement could prove.
 *
 * OUT_OF_DATE is the answer that works: the chain is gone, the application recreates it, and the
 * next chain of this instance takes CPU images because the route is retired. It is only honest when
 * nothing of ours is left outstanding on the application's queue - otherwise the application's own
 * recreate path waits for that submission in vkDeviceWaitIdle, which has no timeout, and the process
 * freezes in its own code (BD-105 round 3).
 *
 * DEVICE_LOST is the answer for the state round 4b refuses to fix by force: a submission of this
 * device waits for a value only a presenter that cannot be proved removed could signal. Then no
 * recreate can succeed, and VK_ERROR_DEVICE_LOST is what the specification has for a device that
 * cannot be used any more. It ends the client instead of freezing it, which is the lesser of the two
 * and the only one that does not break a signal rule to get there.
 */
enum wsi_win32_route_report {
   WSI_WIN32_REPORT_OUT_OF_DATE = 0, /* nothing of ours stays outstanding: the chain is out of date */
   WSI_WIN32_REPORT_DEVICE_LOST,     /* a submission of this device can never retire */
};

static inline enum wsi_win32_route_report
wsi_win32_route_report(bool work_outstanding)
{
   return work_outstanding ? WSI_WIN32_REPORT_DEVICE_LOST : WSI_WIN32_REPORT_OUT_OF_DATE;
}

/* V5. What a bounded drain of the presenter's D3D12 queue established, which decides whether the
 * caller may release what that queue might still be reading.
 *
 * ref/win32-docs/desktop-src/direct3d12/binding-model.md:35 is the rule: before freeing a resource,
 * the application must make sure the GPU has finished referencing it. Round 4's flush returned void,
 * and returned early on a fence that could not be created and on a Signal that failed, after which
 * its callers released the back buffers, the copy command lists, the imported image memory and the
 * D3D12 resources, or handed the DXGI chain to a new swapchain and resized it. A route marked dead
 * is not a statement about a queue.
 *
 * DRAINED is the proof: the drain fence reached its value, so everything submitted before it retired.
 * REMOVED is the other releasable state: a device the runtime has removed executes nothing further
 * and will not read those objects again, and the alternative - pinning every resource of every chain
 * of a process whose presenter died - leaks without bound for no gain. This round treats REMOVED as
 * releasable and says so here rather than in a commit message, because it is an assumption about
 * removal and not a line of the lifetime page.
 * UNPROVEN is the state the audit found missing: the wait expired, or there was no fence, or the
 * Signal failed, and the queue may still name the objects. Nothing dependent is released then.
 */
enum wsi_win32_flush_result {
   WSI_WIN32_FLUSH_UNPROVEN = 0, /* the queue may still reference this chain's resources */
   WSI_WIN32_FLUSH_DRAINED,      /* the drain fence completed: everything submitted before it retired */
   WSI_WIN32_FLUSH_REMOVED,      /* the presenter's device is proved removed */
};

static inline bool
wsi_win32_flush_releases(enum wsi_win32_flush_result result)
{
   return result == WSI_WIN32_FLUSH_DRAINED || result == WSI_WIN32_FLUSH_REMOVED;
}

static inline const char *
wsi_win32_flush_result_name(enum wsi_win32_flush_result result)
{
   switch (result) {
   case WSI_WIN32_FLUSH_DRAINED: return "drained";
   case WSI_WIN32_FLUSH_REMOVED: return "removed";
   case WSI_WIN32_FLUSH_UNPROVEN: return "unproven";
   default: return "unknown";
   }
}

/* V2. The FIRST D3D12 call of the route that failed, kept once per Vulkan instance and written once.
 *
 * Round 4 dropped the HRESULT of ID3D12GraphicsCommandList::Close, of the queue's Wait and of the
 * queue's Signal, and wsi_dxgi_blit returned VK_SUCCESS whatever they answered. The local contract
 * says each of them can fail and what it means: recording errors are returned from Close
 * (sdk-api nf-d3d12-id3d12graphicscommandlist-close.md:59-75), ExecuteCommandLists returns void and
 * the runtime removes the device for an invalid list (:71-79), and the queue's Wait returns before
 * the GPU does anything at all (nf-d3d12-id3d12commandqueue-wait.md:50), so its own HRESULT is the
 * only thing it says. A refusal at any of them left Vulkan waiting for a copy that was never
 * enqueued, and the first diagnostic came two seconds later from a timeout that could name nothing.
 *
 * This structure is what the route keeps instead of nothing: the call's own name, its HRESULT, the
 * presenter's removal reason read immediately afterwards, and the image the call was for. One per
 * process, claimed by compare-and-swap, so a cascade of later refusals cannot bury the first one -
 * and the first one is the one that caused the cascade. It is always on and it costs one log line.
 */
struct wsi_win32_route_error {
   uint32_t taken;          /* 0 until a failure is recorded; claimed by compare-and-swap */
   const char *call;        /* a string literal: the name of the call that failed */
   uint32_t hr;             /* its HRESULT */
   uint32_t removed_reason; /* GetDeviceRemovedReason read right after it, 0 when it could not be read */
   uint32_t image;          /* the image index the call was for, or WSI_WIN32_ROUTE_ERROR_NO_IMAGE */
};

#define WSI_WIN32_ROUTE_ERROR_NO_IMAGE 0xffffffffu

/* Records the failure and returns true to the one caller that recorded it, which is the caller that
 * logs it. The claim comes first, so the writer of the fields is unique.
 */
static inline bool
wsi_win32_route_note_error(struct wsi_win32_route_error *error, const char *call, uint32_t hr,
                           uint32_t removed_reason, uint32_t image)
{
   if (wsi_win32_atomic_cas32(&error->taken, 0u, 1u) != 0u)
      return false;
   error->call = call;
   error->hr = hr;
   error->removed_reason = removed_reason;
   error->image = image;
   return true;
}

static inline bool
wsi_win32_route_error_taken(struct wsi_win32_route_error *error)
{
   return wsi_win32_atomic_load32(&error->taken) != 0u;
}

/* The stages of a first present, in the order the route runs them, and then the acquire of the next
 * frame, which is where the route froze once the present itself ran to the end (BD-105, the lab arm
 * of 2026-10-09). The route logs each stage once per chain while it has not proved itself, so a
 * freeze names the call that did not return instead of needing a debugger on the lab.
 *
 * ACQUIRE is the seventh name and the one the round-2 arm lacked. Its line is written only when the
 * acquire is really about to WAIT: the first acquires of a chain take an idle image and return at
 * once, and a stage line there would say nothing about a call that can block.
 */
enum wsi_win32_present_stage {
   WSI_WIN32_STAGE_IMAGES = 0, /* the swapchain's images and their D3D12 blit contexts are ready */
   WSI_WIN32_STAGE_FENCE_WAIT, /* ID3D12CommandQueue::Wait on the image's shared blit fence */
   WSI_WIN32_STAGE_EXECUTE,    /* ExecuteCommandLists of the copy into the back buffer */
   WSI_WIN32_STAGE_SIGNAL,     /* ID3D12CommandQueue::Signal of the shared blit fence */
   WSI_WIN32_STAGE_PRESENT,    /* IDXGISwapChain3::Present1 */
   WSI_WIN32_STAGE_DONE,       /* Present1 returned */
   WSI_WIN32_STAGE_ACQUIRE,    /* vkAcquireNextImageKHR of the next frame, about to wait on the
                                * image's fence (vk_common_WaitForFences, the frozen call) */
   WSI_WIN32_STAGE_COUNT,
};

static inline const char *
wsi_win32_present_stage_name(enum wsi_win32_present_stage stage)
{
   switch (stage) {
   case WSI_WIN32_STAGE_IMAGES: return "images";
   case WSI_WIN32_STAGE_FENCE_WAIT: return "queue-wait";
   case WSI_WIN32_STAGE_EXECUTE: return "execute";
   case WSI_WIN32_STAGE_SIGNAL: return "queue-signal";
   case WSI_WIN32_STAGE_PRESENT: return "present1";
   case WSI_WIN32_STAGE_DONE: return "present1-returned";
   case WSI_WIN32_STAGE_ACQUIRE: return "acquire-wait";
   default: return "unknown";
   }
}

/* One line per stage per chain. The old gate was the route's presented flag, which went true after
 * the first present and silenced every stage from the second frame on - including the acquire that
 * then froze. A bit per stage keeps the log at seven lines a chain and keeps the stage that blocks
 * visible however late in the chain's life it is first reached.
 */
static inline bool
wsi_win32_stage_first(uint32_t *logged, enum wsi_win32_present_stage stage)
{
   uint32_t bit;
   if ((unsigned)stage >= (unsigned)WSI_WIN32_STAGE_COUNT)
      return false;
   bit = 1u << (unsigned)stage;
   if (*logged & bit)
      return false;
   *logged |= bit;
   return true;
}

#ifdef __cplusplus
}
#endif

#endif /* WSI_WIN32_DEADLINE_H */
