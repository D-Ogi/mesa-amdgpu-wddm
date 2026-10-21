/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 */

#ifndef WSI_WIN32_IFLIP_H
#define WSI_WIN32_IFLIP_H

/* M15.14 for the Vulkan WSI DXGI route: the swap-chain shape that lets the operating system scan
 * out the chain's own buffers instead of composing them.
 *
 * What the lab measured on 2026-10-11 (BD-105 dxgi-r1, arms A5w and A5g). A borderless
 * 1920x1200 chain on this route presented a correct picture and stayed COMPOSED: 2440 of 2443
 * presents were Windowed_Dx_Flip_Consumed, the compositor's own surface update said direct flip
 * TRUE and independent FALSE, and the kernel driver's scan-out counters did not move at all
 * (scanout_requests 0, scanout_flips 0).
 *
 * Why the chain was composed is NOT established. The first reading of that round inferred from the
 * absence of a "M15.14 scanout" line and a "heap import refused" line that the D3D12 runtime had
 * created the back buffers on a heap without D3D12_HEAP_FLAG_ALLOW_DISPLAY. That inference is
 * withdrawn (CONTRACT.md of this round, C3): the shell writes its scan-out note with
 * OutputDebugStringA, so it never reaches the AMDGPU_WDDM_LOG file that was read, and the refusal
 * lines that were there belong to the route's own shared images, not to the chain's back buffers.
 * The zero scan-out counters are per flip transaction, so they are equally consistent with a
 * composed chain whose buffers were primaries: they do not decide it either. The heap flags of the
 * back buffers were not read in that round. The arm that decides it captures the debugger channel
 * and the route's own readback of those flags.
 *
 * So the question this header answers is not "may the kernel driver flip this buffer" - that
 * contract already holds and is measured (M844: The Witcher 3 flipped 2028 of 2028 and 1696 of
 * 1696) - but "does the swap chain have the shape that makes DXGI allocate displayable primaries
 * at all". The one configuration measured to reach independent flip on this hardware is the
 * D3D12 flip client of increment 2 (tools/win/d3d12queue/interactive-flip.h, 600 of 600 frames):
 *
 *   WS_POPUP window at exactly the output rectangle, shown and foreground, no WS_EX_TOPMOST
 *   DXGI_SWAP_CHAIN_DESC1: Width/Height = the output, FLIP_DISCARD, ALPHA_MODE_IGNORE,
 *                          Scaling = DXGI_SCALING_NONE, BufferCount = 3, Flags = 0,
 *                          BufferUsage = RENDER_TARGET_OUTPUT
 *   MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)
 *
 * The b27 route chain differed from it in exactly four inputs, all of them the WSI's own:
 *
 *   1. Scaling          DXGI_SCALING_STRETCH, not NONE.
 *   2. Flags            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING on every chain, whatever the present
 *                       mode, so a FIFO chain was made tearing-capable without asking. The flag
 *                       does NOT keep a chain from an independent flip, and this header no longer
 *                       treats it as a refusal (BD-197): arm A5g (2026-10-11, a kernel driver
 *                       without FlipImmediateMmIo) measured an IMMEDIATE Quake II RTX chain with the
 *                       flag in independent flip for 637 of 640 packets, and BD-197 (2026-10-21,
 *                       0.7.216.103) 631 of 633 presents. The 31v3 kernel driver declares
 *                       FlipImmediateMmIo with FlipOnVSyncMmIo and FlipIndependent (BD-170,
 *                       driver/kmd/wddm.c:4237-4239 at e08e3bb0), so such a chain's flips are
 *                       immediate and may tear, which is what IMMEDIATE asks for. The earlier
 *                       reading here ("this driver has no immediate flip, so a tearing chain is
 *                       not a flip candidate") is withdrawn.
 *   3. BufferCount      2, because wsi_win32_surface_get_capabilities pinned
 *                       minImageCount = maxImageCount = 2 for the DXGI path. Upstream's reason is
 *                       that a DXGI chain must be presented in acquire order, which held only
 *                       while image index and back-buffer index were the same number.
 *   4. Window hook      MakeWindowAssociation with DXGI_MWA_NO_WINDOW_CHANGES as well, which
 *                       takes DXGI off the window's message queue. The default drops that flag only
 *                       for a chain this header found eligible, which is the chain under test; every
 *                       other chain keeps it (wsi_iflip_plan_of).
 *
 * Which of the four withholds the primary is NOT established: no lab arm has moved one at a time,
 * and the D3D12 runtime's rule is not documented in any local reference. This header therefore
 * gives the chain the whole measured-good shape by default and keeps one switch per input, so one
 * lab session can bisect the four without a new binary. Nothing here changes the kernel driver's
 * admission or the shell's scan-out rule; both already hold.
 *
 * The clause names below are the vocabulary of the route log and of the host gate. An eligibility
 * answer never changes behaviour on its own: it is what the chain's INFO line and the one WARN
 * line report. A chain that is not eligible still presents, correctly, composed.
 *
 * One signal in here is a reading and NOT a calibrated verdict (review of r1, finding 4):
 * WSI_IFLIP_NOT_DISPLAYABLE comes from ID3D12Resource::GetHeapProperties on back buffer 0, and no
 * local reference states what a flip-model back buffer reports back to the application
 * (ref/sdk-api-docs nf-d3d12-id3d12resource-getheapproperties.md says only that the out parameter
 * receives "any miscellaneous heap flags"). No measurement of this project has read that value on a
 * chain that is known to reach independent flip, so "NO" and "this flag is not observable this way"
 * cannot yet be told apart. The route therefore reports the reading, the WARN says what was read,
 * and the verdict of an arm stays with the ETW witness and the kernel driver's counters
 * (LAB-PLAN.md section 3).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* DXGI's own numbers, so that the rules compile in a host test without dxgi1_2.h. The call site
 * static_asserts them against the real enumerators.
 */
#define WSI_IFLIP_SCALING_STRETCH 0u
#define WSI_IFLIP_SCALING_NONE    1u

/* The flip-model minimum is 2 (ref/win32-docs for-best-performance--use-dxgi-flip-model.md:71).
 * 3 is the count the measured-good client used. The maximum is 3 and not more: every buffer costs
 * one copy command list per image (buffer_count * image_count lists), and no reference names a
 * fourth buffer as useful on this part.
 */
#define WSI_IFLIP_BUFFERS_MIN     2u
#define WSI_IFLIP_BUFFERS_MAX     3u
#define WSI_IFLIP_BUFFERS_DEFAULT 3u

/* The scan-out primary rows of the shared format table, by DXGI number
 * (driver/contract/amdgpu_wddm_surface_format.h, SCANOUT_PRIMARY). BGRA8 is the firmware's own
 * format; RGBA8 and RGB10A2 also need BC250_SCANOUT_CAPS_PLANE_FORMATS in the kernel driver's
 * trailer (bc250_scanout_format_admitted, driver/contract/bc250_scanout_caps.h). The driver reads
 * the trailer for the WSI (wsi_device.win32.scanout_plane_formats) and the answer is the input
 * plane_formats; a driver without that hook gives true, and the clause then admits them as before.
 */
#define WSI_IFLIP_DXGI_B8G8R8A8_UNORM   87u
#define WSI_IFLIP_DXGI_R8G8B8A8_UNORM   28u
#define WSI_IFLIP_DXGI_R10G10B10A2_UNORM 24u

enum wsi_iflip_reason {
   WSI_IFLIP_ELIGIBLE = 0,
   WSI_IFLIP_NOT_WINDOW,       /* a composition chain: there is no window to cover an output */
   WSI_IFLIP_NO_OUTPUT,        /* the window's monitor rectangle could not be read */
   WSI_IFLIP_NOT_COVERED,      /* the client area does not cover the output exactly */
   WSI_IFLIP_EXTENT,           /* the chain's buffers are not the size of the output */
   WSI_IFLIP_FORMAT,           /* not a SCANOUT_PRIMARY row of the shared format table */
   WSI_IFLIP_ALPHA,            /* the chain asks the compositor to blend it */
   WSI_IFLIP_SCALING,          /* the chain stretches, so its buffer is not the plane's surface */
   WSI_IFLIP_BUFFERS,          /* fewer buffers than the flip model's minimum */
   WSI_IFLIP_NOT_DISPLAYABLE,  /* DXGI gave the back buffers no ALLOW_DISPLAY heap */
   WSI_IFLIP_REASON_COUNT
};

static inline const char *
wsi_iflip_reason_text(enum wsi_iflip_reason reason)
{
   switch (reason) {
   case WSI_IFLIP_ELIGIBLE:          return "eligible";
   case WSI_IFLIP_NOT_WINDOW:        return "not-window";
   case WSI_IFLIP_NO_OUTPUT:         return "no-output";
   case WSI_IFLIP_NOT_COVERED:       return "not-covered";
   case WSI_IFLIP_EXTENT:            return "extent";
   case WSI_IFLIP_FORMAT:            return "format";
   case WSI_IFLIP_ALPHA:             return "alpha";
   case WSI_IFLIP_SCALING:           return "scaling";
   case WSI_IFLIP_BUFFERS:           return "buffers";
   case WSI_IFLIP_NOT_DISPLAYABLE:   return "not-displayable";
   default:                          return "unknown";
   }
}

/* One switch per input, plus the one that turns the whole shape off. A switch may only subtract
 * from the measured-good default, so that a rollback is always the b27 chain byte for byte.
 */
struct wsi_iflip_knobs {
   bool shape;              /* false: the b27 chain exactly, every clause below ignored */
   bool scaling_none;       /* false: always DXGI_SCALING_STRETCH, as b27 */
   bool tearing_on_demand;  /* false: ALLOW_TEARING on every chain, as b27 */
   bool window_hook;        /* false: pass DXGI_MWA_NO_WINDOW_CHANGES as well, as b27 */
   unsigned buffers;        /* the buffer count asked for; 0 keeps the application's own */
};

/* Everything the plan needs, and nothing a Windows header is needed for: the host gate drives the
 * production function over the whole table.
 */
struct wsi_iflip_inputs {
   bool hwnd_target;        /* CreateSwapChainForHwnd, not CreateSwapChainForComposition */
   bool output_known;       /* the monitor rectangle was read */
   bool covers_output;      /* the window's client area is the output rectangle */
   bool alpha_ignore;       /* DXGI_ALPHA_MODE_IGNORE */
   bool may_tear;           /* the create-time present mode, or a compatible one, is IMMEDIATE */
   bool system_tearing;     /* IDXGIFactory5 reports DXGI_FEATURE_PRESENT_ALLOW_TEARING */
   bool own_copy;           /* the route's own D3D12 copy per present, so image and back buffer
                             * indices can be told apart (see wsi_iflip_buffer_count) */
   bool plane_formats;      /* the scan-out plane takes RGBA8 and RGB10A2 as well as BGRA8 */
   uint32_t dxgi_format;
   uint32_t extent_width, extent_height;
   uint32_t client_width, client_height;
   uint32_t output_width, output_height;
   uint32_t min_image_count;
};

struct wsi_iflip_plan {
   unsigned scaling;        /* WSI_IFLIP_SCALING_* */
   unsigned buffer_count;   /* the DXGI buffer count AND the Vulkan image count: one back buffer
                             * per image, which is what keeps GetBuffer(i) the image's own */
   bool allow_tearing;      /* put DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING on the chain */
   bool window_changes;     /* true: MakeWindowAssociation without NO_WINDOW_CHANGES */
   bool eligible;
   enum wsi_iflip_reason reason;
};

/* The D3D12 shell's FORMAT clause for this chain (bc250_scanout_primary.h): BGRA8 always, the
 * other two rows only on a start whose plane takes them. Without plane_formats the shell stands an
 * RGBA8 chain down, and the chain is composed although every other input is at its scan-out value
 * (BD-197: the 31v3 kernel driver withholds PLANE_FORMATS while its connector drives the output).
 */
static inline bool
wsi_iflip_format_is_primary(uint32_t dxgi_format, bool plane_formats)
{
   if (dxgi_format == WSI_IFLIP_DXGI_B8G8R8A8_UNORM)
      return true;
   return plane_formats && (dxgi_format == WSI_IFLIP_DXGI_R8G8B8A8_UNORM ||
                            dxgi_format == WSI_IFLIP_DXGI_R10G10B10A2_UNORM);
}

/* The buffer count, which is also the swapchain's image count. Raising it above the application's
 * own count is allowed by the Vulkan contract (minImageCount is a minimum and
 * vkGetSwapchainImagesKHR reports what the implementation made), and it needs the route's own
 * D3D12 copy: the copy is what can be recorded once per (image, back buffer) pair, so a present
 * out of acquire order still copies the acquired image into the buffer DXGI is about to show.
 * Without that copy the Vulkan image IS back buffer i and the two indices cannot be told apart,
 * which is the upstream reason for the hard limit of two.
 */
static inline unsigned
wsi_iflip_buffer_count(const struct wsi_iflip_knobs *knobs, const struct wsi_iflip_inputs *in)
{
   unsigned count = in->min_image_count;

   if (count < WSI_IFLIP_BUFFERS_MIN)
      count = WSI_IFLIP_BUFFERS_MIN;
   if (knobs->shape && knobs->buffers && in->hwnd_target && in->own_copy &&
       knobs->buffers > count)
      count = knobs->buffers;
   /* The clamp is unconditional (review of r1, regression finding on the array bound): every chain
    * that reaches the copy path indexes cmd_lists[WSI_IFLIP_BUFFERS_MAX], and a count above that
    * bound used to refuse the whole chain and lose the DXGI route to the GDI fallback. A count the
    * array cannot hold is not a count this route may ask DXGI for, whatever the switches say.
    */
   if (count > WSI_IFLIP_BUFFERS_MAX)
      count = WSI_IFLIP_BUFFERS_MAX;
   return count;
}

/* What the surface advertises as maxImageCount, which is the only lawful place to make a client ask
 * for fewer images: a swapchain may never have fewer images than the application's own
 * minImageCount, so a switch that shapes the chain has to shape the caps as well, or it cannot
 * subtract (review of r1, regression finding on BC250_WSI_IFLIP_BUFFERS). vkcube asks for three
 * images and clamps to maxImageCount (ref/Vulkan-Tools cube/cube.c:1475-1483), and so does DXVK
 * (ref/dxvk src/dxvk/dxvk_presenter.cpp), so without this the b27 control arm would present with
 * three back buffers and measure the wrong shape.
 *
 * own_copy is the route's own D3D12 copy per present (see wsi_iflip_buffer_count); a driver that
 * renders into the back buffer keeps upstream's two, whatever the switches say.
 */
static inline unsigned
wsi_iflip_caps_max_images(const struct wsi_iflip_knobs *knobs, bool own_copy)
{
   unsigned count;

   if (!knobs->shape || !knobs->buffers || !own_copy)
      return WSI_IFLIP_BUFFERS_MIN;
   count = knobs->buffers;
   if (count < WSI_IFLIP_BUFFERS_MIN)
      count = WSI_IFLIP_BUFFERS_MIN;
   if (count > WSI_IFLIP_BUFFERS_MAX)
      count = WSI_IFLIP_BUFFERS_MAX;
   return count;
}

/* The plan, as a pure function of the switches and the inputs. The eligibility clauses are asked
 * in the order of the table above; the first one that fails is the reason, and the reason is what
 * the chain's line prints. `displayable` is asked separately, after the chain exists
 * (wsi_iflip_displayable_reason), because only DXGI's answer settles it.
 */
static inline struct wsi_iflip_plan
wsi_iflip_plan_of(const struct wsi_iflip_knobs *knobs, const struct wsi_iflip_inputs *in)
{
   struct wsi_iflip_plan plan;

   plan.buffer_count = wsi_iflip_buffer_count(knobs, in);
   plan.scaling = WSI_IFLIP_SCALING_STRETCH;
   plan.allow_tearing = in->system_tearing;
   plan.window_changes = false;
   plan.eligible = false;
   plan.reason = WSI_IFLIP_ELIGIBLE;

   if (knobs->shape) {
      /* Scaling matters only when the buffer and the presentation target differ in size
       * (ref/sdk-api-docs ne-dxgi1_2-dxgi_scaling.md:50-60). NONE is asked for only when they are
       * the same size, so a chain smaller than its window keeps the behaviour it had.
       */
      if (knobs->scaling_none && in->extent_width && in->extent_height &&
          in->extent_width == in->client_width && in->extent_height == in->client_height)
         plan.scaling = WSI_IFLIP_SCALING_NONE;
      if (knobs->tearing_on_demand)
         plan.allow_tearing = in->system_tearing && in->may_tear;
   }

   if (!in->hwnd_target)
      plan.reason = WSI_IFLIP_NOT_WINDOW;
   else if (!in->output_known)
      plan.reason = WSI_IFLIP_NO_OUTPUT;
   else if (!in->covers_output)
      plan.reason = WSI_IFLIP_NOT_COVERED;
   else if (!in->extent_width || in->extent_width != in->output_width ||
            in->extent_height != in->output_height)
      plan.reason = WSI_IFLIP_EXTENT;
   else if (!wsi_iflip_format_is_primary(in->dxgi_format, in->plane_formats))
      plan.reason = WSI_IFLIP_FORMAT;
   else if (!in->alpha_ignore)
      plan.reason = WSI_IFLIP_ALPHA;
   /* No tearing clause: ALLOW_TEARING does not keep a chain from an independent flip (item 2). */
   else if (plan.scaling != WSI_IFLIP_SCALING_NONE)
      plan.reason = WSI_IFLIP_SCALING;
   else if (plan.buffer_count < WSI_IFLIP_BUFFERS_MIN)
      plan.reason = WSI_IFLIP_BUFFERS;
   else
      plan.eligible = true;

   /* DXGI_MWA_NO_WINDOW_CHANGES is dropped only for the chain under test, which is a chain that
    * covers its output and asks for nothing that forbids a flip (review of r1, regression finding on
    * the window hook). The flag exists so that an application can handle window changes itself
    * ("ensures that DXGI will not interfere with application's handling of window mode changes",
    * ref/sdk-api-docs nf-dxgi-idxgifactory-makewindowassociation.md:91), and a WSI layer under an
    * unknowing Vulkan application is not that application: a windowed chain, a composition chain and
    * every chain of a D3D11 or DXVK front therefore keep the flag they had in b27. Alt+Enter stays
    * refused on every chain through DXGI_MWA_NO_ALT_ENTER.
    */
   plan.window_changes = knobs->shape && knobs->window_hook && plan.eligible;

   return plan;
}

/* The copy list the present executes: cmd_lists[b] of the acquired image was recorded with back
 * buffer b as its destination, so the list of the buffer IDXGISwapChain3::GetCurrentBackBufferIndex
 * names is the one that copies into the buffer the next Present1 will show. A buffer outside the
 * chain's own count, or outside the recorded array, has no list: the present fails instead of
 * copying into the wrong buffer. The production path calls this; the host gate drives it.
 */
#define WSI_IFLIP_LIST_NONE 0xffffffffu

static inline unsigned
wsi_iflip_list_index(unsigned buffer, unsigned buffer_count)
{
   if (!buffer_count || buffer_count > WSI_IFLIP_BUFFERS_MAX)
      return WSI_IFLIP_LIST_NONE;
   if (buffer >= buffer_count)
      return WSI_IFLIP_LIST_NONE;
   return buffer;
}

/* The acquire fallback of a DXGI chain, as a rule instead of one step forward. Up to b27 the route
 * took GetCurrentBackBufferIndex as an image index and, when that image was still being drawn,
 * stepped exactly one place with an assert that the next image was QUEUED. That single step is
 * provable only at two images, which is what upstream's maxImageCount = 2 guaranteed; with three
 * images and two images legally acquired at once the step can land on a second DRAWING image, and
 * the shipped build is NDEBUG, so the assert is gone and the application is handed an image it
 * already holds (review of r1, regression finding on the acquire fallback).
 *
 * The rule: the oldest QUEUED image, which is the one whose fence is nearest to signalling, and
 * WSI_IFLIP_IMAGE_NONE when no image is QUEUED. `serial` is the present serial the chain stamped on
 * the image when it queued it; it is read only for a QUEUED image.
 */
enum wsi_iflip_image_state {
   WSI_IFLIP_IMAGE_IDLE = 0,
   WSI_IFLIP_IMAGE_DRAWING,
   WSI_IFLIP_IMAGE_QUEUED,
};

#define WSI_IFLIP_IMAGE_NONE 0xffffffffu

static inline unsigned
wsi_iflip_pick_queued(const unsigned char *state, const uint64_t *serial, unsigned count)
{
   unsigned best = WSI_IFLIP_IMAGE_NONE;
   uint64_t best_serial = 0;

   for (unsigned i = 0; i < count; i++) {
      if (state[i] != (unsigned char)WSI_IFLIP_IMAGE_QUEUED)
         continue;
      if (best == WSI_IFLIP_IMAGE_NONE || serial[i] < best_serial) {
         best = i;
         best_serial = serial[i];
      }
   }
   return best;
}

/* What DXGI answered about the buffers it made. `heap_queried` is the HRESULT of
 * ID3D12Resource::GetHeapProperties having succeeded, and `allow_display` whether the heap flags
 * it returned carry D3D12_HEAP_FLAG_ALLOW_DISPLAY (0x8), which is the API side of
 * D3D12DDI_HEAP_FLAG_PRIMARY and therefore of the shell's scan-out primary. A query that failed is
 * not evidence of either answer, so it keeps the plan's own reason.
 */
static inline enum wsi_iflip_reason
wsi_iflip_displayable_reason(const struct wsi_iflip_plan *plan, bool heap_queried,
                            bool allow_display)
{
   if (!plan->eligible)
      return plan->reason;
   if (!heap_queried)
      return WSI_IFLIP_ELIGIBLE;
   return allow_display ? WSI_IFLIP_ELIGIBLE : WSI_IFLIP_NOT_DISPLAYABLE;
}

/* What the chain IS, read back from it after it exists (IDXGISwapChain1::GetDesc1 plus the heap
 * query). A chain taken over from an oldSwapchain keeps the Scaling it was created with -
 * ResizeBuffers cannot change that field (ref/sdk-api-docs nf-dxgi-idxgiswapchain-resizebuffers.md
 * takes BufferCount, Width, Height, NewFormat and SwapChainFlags only) - so the plan is what was
 * asked for and this is what was given. The reason the route logs, and the lab's parser reads, comes
 * from here: no chain may report a shape it does not have (review of r1).
 */
struct wsi_iflip_got {
   bool known;              /* GetDesc1 succeeded; without it only the plan is known */
   unsigned scaling;
   unsigned buffer_count;
   bool allow_tearing;      /* read back for the chain's line; not a refusal (item 2 above) */
   bool heap_queried;       /* ID3D12Resource::GetHeapProperties succeeded */
   bool allow_display;
};

static inline enum wsi_iflip_reason
wsi_iflip_chain_reason(const struct wsi_iflip_plan *plan, const struct wsi_iflip_got *got)
{
   if (!plan->eligible)
      return plan->reason;
   if (got->known) {
      if (got->scaling != WSI_IFLIP_SCALING_NONE)
         return WSI_IFLIP_SCALING;
      if (got->buffer_count < WSI_IFLIP_BUFFERS_MIN)
         return WSI_IFLIP_BUFFERS;
   }
   return wsi_iflip_displayable_reason(plan, got->heap_queried, got->allow_display);
}

/* The switches, parsed from strings so that the host gate drives the production parser. Every
 * value is read once per process at the call site. The spellings are the ones a lab arm writes:
 *   BC250_WSI_IFLIP=off              the b27 chain, byte for byte
 *   BC250_WSI_IFLIP_SCALING=stretch  only clause 1 back to b27
 *   BC250_WSI_IFLIP_TEARING=always   only clause 2 back to b27
 *   BC250_WSI_IFLIP_BUFFERS=2        only clause 3 back to b27 (0 also means "the application's");
 *                                    it moves the advertised maxImageCount as well, which is what
 *                                    makes a client ask for fewer images (wsi_iflip_caps_max_images)
 *   BC250_WSI_IFLIP_WINDOWHOOK=off   only clause 4 back to b27
 * An unset variable, and any value that is not one of the spellings above, leaves the default.
 */
static inline bool
wsi_iflip_word_off(const char *value)
{
   return value && (!strcmp(value, "off") || !strcmp(value, "0") || !strcmp(value, "no"));
}

static inline struct wsi_iflip_knobs
wsi_iflip_knobs_parse(const char *shape, const char *scaling, const char *tearing,
                      const char *buffers, const char *hook)
{
   struct wsi_iflip_knobs knobs;

   knobs.shape = !wsi_iflip_word_off(shape);
   knobs.scaling_none = !(scaling && (!strcmp(scaling, "stretch") || wsi_iflip_word_off(scaling)));
   knobs.tearing_on_demand = !(tearing && (!strcmp(tearing, "always") ||
                                           !strcmp(tearing, "1") || !strcmp(tearing, "on")));
   knobs.window_hook = !wsi_iflip_word_off(hook);
   knobs.buffers = WSI_IFLIP_BUFFERS_DEFAULT;
   if (buffers && buffers[0]) {
      const long asked = strtol(buffers, NULL, 10);
      if (asked <= 0)
         knobs.buffers = 0;                 /* the application's own count */
      else if (asked < (long)WSI_IFLIP_BUFFERS_MIN)
         knobs.buffers = WSI_IFLIP_BUFFERS_MIN;
      else if (asked > (long)WSI_IFLIP_BUFFERS_MAX)
         knobs.buffers = WSI_IFLIP_BUFFERS_MAX;
      else
         knobs.buffers = (unsigned)asked;
   }
   return knobs;
}

/* The rectangle test, kept here so that the host gate drives the same arithmetic the route does.
 * Coverage is exact: a client area larger than the output is a window the compositor must clip,
 * and one smaller leaves desktop around it. Both compose.
 */
static inline bool
wsi_iflip_covers(int32_t client_left, int32_t client_top, int32_t client_right,
                 int32_t client_bottom, int32_t out_left, int32_t out_top, int32_t out_right,
                 int32_t out_bottom)
{
   if (client_right <= client_left || client_bottom <= client_top)
      return false;
   return client_left == out_left && client_top == out_top &&
          client_right == out_right && client_bottom == out_bottom;
}

/* Whether a stand-down is this route's own doing. A chain in a window the application sized, or
 * one whose buffers the application deliberately made smaller than the output, is composed because
 * it asked to be; nothing is wrong and nothing warns. The clauses below are the ones where the
 * route gave the chain a shape that cannot be scanned out, which is what a WARN is for.
 *
 * WSI_IFLIP_FORMAT is NOT one of them (review of r1): the swapchain format is the application's own
 * choice, exactly like the window it sized and the extent it asked for, so an application that
 * chose an FP16 HDR chain must not read a WARN that blames this route.
 */
static inline bool
wsi_iflip_reason_is_ours(enum wsi_iflip_reason reason)
{
   switch (reason) {
   case WSI_IFLIP_ALPHA:
   case WSI_IFLIP_SCALING:
   case WSI_IFLIP_BUFFERS:
   case WSI_IFLIP_NOT_DISPLAYABLE:
      return true;
   default:
      return false;
   }
}

/* The bounded window of the one WARN line: a chain that covers the output and has presented this
 * many frames without being eligible says so once. 180 frames is three seconds at 60 Hz, which is
 * past every start-up resize and inside the shortest lab arm.
 */
#define WSI_IFLIP_WARN_PRESENTS 180u

/* The frame-statistics sample interval, in presents. DXGI_FRAME_STATISTICS says nothing about the
 * present mode, so the sample is a record of what the chain and the display did, not a verdict:
 * the verdict comes from the ETW witness (tools/win/etw/etw-present-mode.py).
 */
#define WSI_IFLIP_STATS_EVERY 300u

#endif /* WSI_WIN32_IFLIP_H */
