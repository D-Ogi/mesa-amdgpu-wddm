/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * The present overrides of the per-application graphics settings on the Win32 WSI routes (the
 * amdgpu-wddm BC-250 port): what VSync, FrameRateLimit and MaxFrameLatency change in a present. The
 * driver gives the values in wsi_device.win32.present_settings (radv_wddm2_wsi.c); all zero keeps
 * upstream behaviour. The rules follow the D3D11 shell of the same driver (bc250-win
 * driver/umd/app-settings/app-settings.h): VSync is the sync interval of every present, the frame
 * rate limit is a paced wait per present, and MaxFrameLatency is a wait, after a present is queued,
 * for the GPU work of the present that many presents back, on a route that queues frames (the DXGI
 * route of the x64 system ICD; a GDI present copies a finished frame and never queues one).
 *
 * No Windows header, so that the host test (src/amd/vulkan/winsys/wddm2/tests/
 * radv_wddm2_app_settings_test.c) can run the rules.
 */
#ifndef WSI_WIN32_PRESENT_POLICY_H
#define WSI_WIN32_PRESENT_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

struct wsi_win32_present_settings {
   bool vsync_set;             /* false: the application's present mode stays */
   bool vsync;                 /* true: every present waits for a vertical blank */
   uint32_t frame_rate_limit;  /* frames per second, 0 for no limit */
   uint32_t max_frame_latency; /* 1 to 3 presents queued ahead of the GPU, 0 for no limit */
};

/* The present mode that a chain uses. VSync 1 is FIFO. VSync 0 is IMMEDIATE: interval 0, with
 * tearing where the chain may tear; both routes list FIFO and IMMEDIATE
 * (wsi_win32_surface_get_present_modes), so the result is always a mode the surface supports.
 */
static inline VkPresentModeKHR
wsi_win32_settings_present_mode(const struct wsi_win32_present_settings *s, VkPresentModeKHR app)
{
   if (!s || !s->vsync_set)
      return app;
   return s->vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
}

/* The clock of a frame rate limit, in the ticks of any monotonic clock. last: the release time of
 * the previous frame (0 = none yet). Returns the time to wait for, or 0 to go on at once. A frame
 * more than one interval late restarts the clock, so that a stall is not followed by a burst of
 * frames above the cap. The same rule as limiter_target of the D3D shells.
 */
static inline uint64_t
wsi_win32_limiter_target(uint64_t now, uint64_t interval, uint64_t *last)
{
   uint64_t target;

   if (!interval)
      return 0;
   if (!*last || now < *last) {
      *last = now;
      return 0;
   }
   target = *last + interval;
   if (now >= target) {
      *last = now - target > interval ? now : target;
      return 0;
   }
   *last = target;
   return target;
}

/* The presents of one chain that MaxFrameLatency may wait for: the image and the blit fence value
 * that says its copy completed, for the last WSI_WIN32_LATENCY_RING presents.
 */
#define WSI_WIN32_LATENCY_RING 4

struct wsi_win32_latency {
   uint32_t image[WSI_WIN32_LATENCY_RING];
   uint64_t value[WSI_WIN32_LATENCY_RING];
   uint64_t count; /* presents recorded */
};

static inline void
wsi_win32_latency_push(struct wsi_win32_latency *l, uint32_t image, uint64_t value)
{
   const unsigned slot = (unsigned)(l->count % WSI_WIN32_LATENCY_RING);
   l->image[slot] = image;
   l->value[slot] = value;
   l->count++;
}

/* After a push: the present `latency` presents before the newest one, whose copy must complete
 * before this present returns, so that at most `latency` presents are queued ahead of the GPU.
 * False: no wait (no limit, a limit out of 1 to 3, or not enough presents yet).
 */
static inline bool
wsi_win32_latency_target(const struct wsi_win32_latency *l, uint32_t latency, uint32_t *image,
                         uint64_t *value)
{
   uint64_t index;

   if (!latency || latency >= WSI_WIN32_LATENCY_RING || l->count <= latency)
      return false;
   index = l->count - 1 - latency;
   *image = l->image[index % WSI_WIN32_LATENCY_RING];
   *value = l->value[index % WSI_WIN32_LATENCY_RING];
   return true;
}

#endif /* WSI_WIN32_PRESENT_POLICY_H */
