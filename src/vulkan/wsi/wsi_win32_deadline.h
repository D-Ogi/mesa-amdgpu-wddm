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

/* What the route has shown so far, per Vulkan instance (struct wsi_win32). presented is set by the
 * first present that Present1 accepted; dead is set when a wait of the route expired before that.
 * A dead route makes every later swapchain of the instance take CPU images, so an application that
 * stops in one of the two waits this route bounds gets a usable window back instead of a frozen
 * thread, and the next swapchain does not try the route again.
 *
 * What this does not cover: a freeze in a call that no deadline of ours sits in front of, which is
 * everything in wsi_win32_image_init (GetBuffer, CreateCommittedResource, CreateSharedHandle, the
 * Vulkan import of the D3D12 resource, the layout check, the command list). Such a thread is still a
 * frozen thread, and BD-105 cannot yet say which of the two it is: that is outcome 3 of arm A2.
 *
 * Both fields are plain bools with no synchronisation. presented is written by the thread that
 * presents, dead by whichever thread's wait expires, and both are read by acquire and by swapchain
 * creation, possibly on another thread. Every write is a one-way transition to true and every reader
 * that misses one only ends up being more careful (one more bounded wait, one more swapchain on the
 * route), never wrong. They are deliberately not atomics while that holds; make them atomic before
 * any reader starts to depend on the two fields together or on a write becoming visible promptly.
 */
struct wsi_win32_route_state {
   bool presented;
   bool dead;
};

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

/* The stages of a first present, in the order the route runs them. The route logs the stage it is
 * about to enter while no present has completed yet (one line each, for the first chain only), so a
 * freeze names the call that did not return instead of needing a debugger on the lab.
 */
enum wsi_win32_present_stage {
   WSI_WIN32_STAGE_IMAGES = 0, /* the swapchain's images and their D3D12 blit contexts are ready */
   WSI_WIN32_STAGE_FENCE_WAIT, /* ID3D12CommandQueue::Wait on the image's shared blit fence */
   WSI_WIN32_STAGE_EXECUTE,    /* ExecuteCommandLists of the copy into the back buffer */
   WSI_WIN32_STAGE_SIGNAL,     /* ID3D12CommandQueue::Signal of the shared blit fence */
   WSI_WIN32_STAGE_PRESENT,    /* IDXGISwapChain3::Present1 */
   WSI_WIN32_STAGE_DONE,       /* Present1 returned */
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
   default: return "unknown";
   }
}

#ifdef __cplusplus
}
#endif

#endif /* WSI_WIN32_DEADLINE_H */
