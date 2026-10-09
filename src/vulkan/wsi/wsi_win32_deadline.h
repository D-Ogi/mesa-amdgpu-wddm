/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * wsi_win32_deadline.h - the deadlines and the degrade rule of the Win32 DXGI present route, as
 * plain C with no Windows, Vulkan or Mesa header, so a host test drives the production rules
 * (src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_wsi_route_test.c).
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
 */
#ifndef WSI_WIN32_DEADLINE_H
#define WSI_WIN32_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
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

/* The timeout an acquire of a DXGI swapchain may really wait. A route that has not completed one
 * present is not known to work, so the application's wait (vkAcquireNextImageKHR with UINT64_MAX is
 * the normal case) becomes the route's own deadline; the caller then reports the swapchain as out of
 * date, which sends the application back through swapchain creation and onto the CPU-image path.
 * Once the route has presented, the application's timeout is its own business again.
 *
 * route_presented is wsi_win32_route_state::presented, and what sets it is the whole point of the
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

/* What the route has shown so far, per Vulkan instance (struct wsi_win32). presented means a present
 * of this route COMPLETED and an acquire after it returned an image; dead is set when a wait of the
 * route expired before that. A dead route makes every later swapchain of the instance take CPU
 * images, so an application that stops in one of the two waits this route bounds gets a usable
 * window back instead of a frozen thread, and the next swapchain does not try the route again.
 *
 * presented used to be set the moment IDXGISwapChain3::Present1 returned, and the lab arm of
 * 2026-10-09 (scratch\b27\vk-wsi-dxgi\lab-results-2026-10-09\RESULT.md) is what that cost: the first
 * present of the route ran to present1-returned, presented went true, the deadline of the acquire
 * went away with it, and the client froze one frame later in an acquire that was unbounded again.
 * Present1 only queues a present; it says nothing about the copy, the flip or the fence. The flag
 * that disables a wait is now set only by something that proves that very wait can finish.
 *
 * What this does not cover: a freeze in a call that no deadline of ours sits in front of, which is
 * everything in wsi_win32_image_init (GetBuffer, CreateCommittedResource, CreateSharedHandle, the
 * Vulkan import of the D3D12 resource, the layout check, the command list). Such a thread is still a
 * frozen thread, and the stage line names the call.
 *
 * Both fields are plain bools with no synchronisation. presented is written by the thread that
 * acquires, dead by whichever thread's wait expires, and both are read by acquire and by swapchain
 * creation, possibly on another thread. Every write is a one-way transition to true and every reader
 * that misses one only ends up being more careful (one more bounded wait, one more swapchain on the
 * route), never wrong. They are deliberately not atomics while that holds; make them atomic before
 * any reader starts to depend on the two fields together or on a write becoming visible promptly.
 */
struct wsi_win32_route_state {
   bool presented;
   bool dead;
};

/* Has the present the route queued last actually completed?
 *
 * How completion is observed, decided from this file's own code and not from a guess:
 *
 *   pending_value is the value wsi_dxgi_blit signalled on the image's SHARED BLIT FENCE after
 *   ExecuteCommandLists of the copy into the back buffer (chain->base.blit.timeline_values[i], one
 *   past the value the application's own queue signals). That fence is the D3D12 side of the
 *   Vulkan timeline semaphore of the same image, and the chain holds it in d3d12_blit_fences[i].
 *   So fence_completed_value >= pending_value says, in one read of one object the route already
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
 * pending_value 0 means no present is outstanding: the timeline values are pre-incremented, so a
 * value a present has signalled is always at least 1.
 */
static inline bool
wsi_win32_route_present_complete(uint64_t pending_value, uint64_t fence_completed_value)
{
   return pending_value != 0 && fence_completed_value >= pending_value;
}

/* An acquire of the route returned an image. This is the ONLY place that may lift the route's
 * deadlines, and it does so only when the present before it completed by the rule above. Both
 * halves are needed, and each one covers the other's blind spot:
 *
 *   the completed present says the presenter's GPU work ran at all, which Present1's return did not;
 *   the returned acquire says the wait that presented would disable can in fact finish, which a
 *   fence value on its own does not (an acquire that reaches the back buffer by another path, or a
 *   fence that completes while the application's own second submit never does, would otherwise lift
 *   the cap on a route that still freezes).
 *
 * Returns true the one time it lifts the deadlines, so the caller logs that line once. A route that
 * a wait already retired is never revived: its chains are being given back to the CPU-image path,
 * and a fence that completes after that is too late to matter.
 */
static inline bool
wsi_win32_route_note_acquired(struct wsi_win32_route_state *state,
                              uint64_t pending_value, uint64_t fence_completed_value)
{
   if (state->presented || state->dead)
      return false;
   if (!wsi_win32_route_present_complete(pending_value, fence_completed_value))
      return false;
   state->presented = true;
   return true;
}

/* A bounded wait of the route expired. Before the first present this retires the route for the
 * instance; after it, one late wait is a slow frame, not a broken route, and the route stays.
 * Returns true when the route is now dead.
 */
static inline bool
wsi_win32_route_wait_expired(struct wsi_win32_route_state *state)
{
   if (!state->presented)
      state->dead = true;
   return state->dead;
}

static inline bool
wsi_win32_route_usable(const struct wsi_win32_route_state *state)
{
   return !state->dead;
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
