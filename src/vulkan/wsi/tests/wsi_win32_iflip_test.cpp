/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 *
 * The host gate of M15.14 for the Vulkan WSI DXGI route. It drives the production rules of
 * wsi_win32_iflip.h, never a copy of them, and it needs no Vulkan, no DXGI and no lab.
 *
 *   wsi_win32_iflip_test.exe                  every case must pass
 *   wsi_win32_iflip_test.exe --b27-control    the same cases against the b27 shape, which must
 *                                             fail exactly the cases the lab measured as COMPOSED
 *
 * The cases are numbered in the order of the rules: the switches (1), the advertised image count
 * (1b), the two arms of 2026-10-11 (2), the formats (3), one case per refusal clause (4), the
 * scaling and extent interaction (5), the buffer counts and the array bound (6), the heap reading
 * (7), the chain as it stands against the chain as it was asked for (7b), the coverage arithmetic
 * (8), the reason names (9), the copy-list selection and its bounds (10) and the acquire fallback
 * (11).
 *
 * The control is the negative control the gate needs: it reproduces the swap-chain shape that
 * arm A5w and arm A5g presented with on 2026-10-11 (STRETCH, ALLOW_TEARING on every chain, the
 * application's two buffers, NO_WINDOW_CHANGES) and asserts that the eligibility clauses refuse
 * it. A gate that passed both shapes would be measuring nothing.
 */

#include "../wsi_win32_iflip.h"

#include <cstdio>
#include <cstring>

static unsigned checks;
static unsigned failures;

static void
check(bool ok, const char *what)
{
   checks++;
   if (!ok) {
      failures++;
      std::printf("FAIL %s\n", what);
   }
}

/* The inputs of the two arms the lab measured, and of the one configuration measured to reach
 * independent flip (the D3D12 flip client of increment 2). own_copy is true: this route copies
 * each image into the back buffer with its own D3D12 list.
 */
static struct wsi_iflip_inputs
borderless_1920x1200(uint32_t dxgi_format, uint32_t min_image_count)
{
   struct wsi_iflip_inputs in = {};

   in.hwnd_target = true;
   in.output_known = true;
   in.covers_output = true;
   in.alpha_ignore = true;
   in.may_tear = false;              /* FIFO, as both arms presented */
   in.system_tearing = true;         /* the lab's DXGI reports tearing support */
   in.own_copy = true;
   in.dxgi_format = dxgi_format;
   in.extent_width = 1920;
   in.extent_height = 1200;
   in.client_width = 1920;
   in.client_height = 1200;
   in.output_width = 1920;
   in.output_height = 1200;
   in.min_image_count = min_image_count;
   return in;
}

/* The destination back buffer of a recorded copy list. The production rule picks the list by
 * wsi_iflip_list_index(buffer, buffer_count) and that list was recorded with that same buffer as
 * its destination, so this is the only part of the pair the test owns (review of r1: case 10 used
 * to assert an identity between two of its own functions and drove no production code).
 * The rule up to b27 had one list per image, recorded with back buffer image_index.
 */
static unsigned
iflip_dst_of_list(unsigned list)
{
   return list;
}

static unsigned
old_dst_of_image(unsigned image)
{
   return image;
}

/* What a conformant client ends up with: it asks for some number of images and clamps to the
 * maxImageCount the surface advertised (vkcube: ref/Vulkan-Tools cube/cube.c:1475-1483; DXVK:
 * ref/dxvk src/dxvk/dxvk_presenter.cpp), and the route then plans the chain's buffers from that.
 * Both halves below the clamp are production rules.
 */
static unsigned
chain_buffers_for_client(const struct wsi_iflip_knobs *knobs, unsigned asked, bool own_copy);

static struct wsi_iflip_knobs
knobs_default(void)
{
   return wsi_iflip_knobs_parse(NULL, NULL, NULL, NULL, NULL);
}

/* The b27 shape, as the switches spell it. */
static struct wsi_iflip_knobs
knobs_b27(void)
{
   return wsi_iflip_knobs_parse("off", NULL, NULL, NULL, NULL);
}

static unsigned
chain_buffers_for_client(const struct wsi_iflip_knobs *knobs, unsigned asked, bool own_copy)
{
   const unsigned maximum = wsi_iflip_caps_max_images(knobs, own_copy);
   struct wsi_iflip_inputs in =
      borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, asked > maximum ? maximum : asked);

   in.own_copy = own_copy;
   return wsi_iflip_buffer_count(knobs, &in);
}

int
main(int argc, char **argv)
{
   const bool control = argc > 1 && !std::strcmp(argv[1], "--b27-control");
   const struct wsi_iflip_knobs knobs = control ? knobs_b27() : knobs_default();

   /* 1. The default switches are the measured-good shape, and "off" is the b27 chain. */
   {
      const struct wsi_iflip_knobs on = knobs_default();
      const struct wsi_iflip_knobs off = knobs_b27();

      check(on.shape && on.scaling_none && on.tearing_on_demand && on.window_hook &&
            on.buffers == WSI_IFLIP_BUFFERS_DEFAULT, "default switches are the whole shape");
      check(!off.shape, "BC250_WSI_IFLIP=off turns the shape off");
      check(!wsi_iflip_knobs_parse(NULL, "stretch", NULL, NULL, NULL).scaling_none,
            "SCALING=stretch");
      check(!wsi_iflip_knobs_parse(NULL, NULL, "always", NULL, NULL).tearing_on_demand,
            "TEARING=always");
      check(!wsi_iflip_knobs_parse(NULL, NULL, NULL, NULL, "off").window_hook, "WINDOWHOOK=off");
      check(wsi_iflip_knobs_parse(NULL, NULL, NULL, "2", NULL).buffers == 2u, "BUFFERS=2");
      check(wsi_iflip_knobs_parse(NULL, NULL, NULL, "0", NULL).buffers == 0u,
            "BUFFERS=0 is the application's own count");
      check(wsi_iflip_knobs_parse(NULL, NULL, NULL, "9", NULL).buffers == WSI_IFLIP_BUFFERS_MAX,
            "BUFFERS is clamped to the maximum");
      check(wsi_iflip_knobs_parse(NULL, NULL, NULL, "1", NULL).buffers == WSI_IFLIP_BUFFERS_MIN,
            "BUFFERS is clamped to the flip model's minimum");
      check(wsi_iflip_knobs_parse(NULL, "nonsense", "nonsense", "nonsense", "nonsense").shape,
            "a value that is not a spelling leaves the default");
   }

   /* 1b. The advertised maxImageCount follows the same switches, which is the only lawful place to
    * make a client ask for fewer images (review of r1: without this BC250_WSI_IFLIP=off and
    * BC250_WSI_IFLIP_BUFFERS=2 could not subtract, because the client asks for three by itself and
    * the plan only ever raises).
    */
   {
      const struct wsi_iflip_knobs on = knobs_default();
      const struct wsi_iflip_knobs off = knobs_b27();
      const struct wsi_iflip_knobs two = wsi_iflip_knobs_parse(NULL, NULL, NULL, "2", NULL);
      const struct wsi_iflip_knobs own = wsi_iflip_knobs_parse(NULL, NULL, NULL, "0", NULL);

      check(wsi_iflip_caps_max_images(&on, true) == WSI_IFLIP_BUFFERS_MAX,
            "the default caps offer three images");
      check(wsi_iflip_caps_max_images(&off, true) == WSI_IFLIP_BUFFERS_MIN,
            "shape off: the caps offer two");
      check(wsi_iflip_caps_max_images(&two, true) == 2u, "BUFFERS=2: the caps offer two");
      check(wsi_iflip_caps_max_images(&own, true) == WSI_IFLIP_BUFFERS_MIN,
            "BUFFERS=0: the caps offer two, so the client's own count is at most two");
      check(wsi_iflip_caps_max_images(&on, false) == WSI_IFLIP_BUFFERS_MIN,
            "a driver that renders into the back buffer: two, whatever the switches say");
      /* The two cases the review asked for by name: a client that asks for three. */
      check(chain_buffers_for_client(&off, 3, true) == 2u, "shape off, app asked 3 -> 2 buffers");
      check(chain_buffers_for_client(&two, 3, true) == 2u, "BUFFERS=2, app asked 3 -> 2 buffers");
      check(chain_buffers_for_client(&on, 3, true) == 3u, "the default, app asked 3 -> 3 buffers");
      check(chain_buffers_for_client(&on, 2, true) == 3u, "the default, app asked 2 -> 3 buffers");
      check(chain_buffers_for_client(&on, 3, false) == 2u,
            "no copy of this route's own, app asked 3 -> 2 buffers");
   }

   /* 2. The two arms of 2026-10-11: a borderless BGRA8 chain at the output's size. The default
    * shape must be eligible; the b27 shape must not be, and its reason must be one of the inputs
    * this route controls.
    */
   {
      const struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      const struct wsi_iflip_plan plan = wsi_iflip_plan_of(&knobs, &in);

      check(plan.eligible, "A5w's chain is eligible");
      check(plan.reason == WSI_IFLIP_ELIGIBLE, "and stands down on nothing");
      check(plan.scaling == WSI_IFLIP_SCALING_NONE, "the buffer is not stretched");
      check(!plan.allow_tearing, "a FIFO chain is not a tearing chain");
      check(plan.buffer_count == WSI_IFLIP_BUFFERS_DEFAULT, "three buffers");
      check(plan.window_changes, "DXGI may watch the window");
      /* The b27 shape fails every line above. What it must also do is say so under a clause this
       * route owns, so that the WARN of a composed chain names something actionable.
       */
      if (control)
         check(wsi_iflip_reason_is_ours(plan.reason), "the b27 reason is this route's own doing");
   }

   /* 3. A5g's format (RGBA8) and the 10-bit row are scan-out rows as well; FP16 is not. */
   {
      struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_R8G8B8A8_UNORM, 2);
      check(wsi_iflip_plan_of(&knobs, &in).eligible, "A5g RGBA8");
      in.dxgi_format = WSI_IFLIP_DXGI_R10G10B10A2_UNORM;
      check(wsi_iflip_plan_of(&knobs, &in).eligible, "RGB10A2");
      in.dxgi_format = 10u;                               /* R16G16B16A16_FLOAT */
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_FORMAT, "FP16 is no scan-out row");
      in.dxgi_format = 0u;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_FORMAT, "an unknown format");
   }

   /* 4. One clause per refusal, in the order the rules ask them. Each case moves one input of the
    * eligible chain and nothing else, so a clause that stopped being asked shows up here.
    */
   {
      struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);

      in.hwnd_target = false;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_NOT_WINDOW, "composition chain");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.output_known = false;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_NO_OUTPUT, "no monitor rectangle");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.covers_output = false;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_NOT_COVERED, "a window");
      /* Only the chain under test drops DXGI_MWA_NO_WINDOW_CHANGES (review of r1): a windowed
       * chain, a composition chain and the chains of a D3D11 or DXVK front keep it.
       */
      check(!wsi_iflip_plan_of(&knobs, &in).window_changes,
            "a windowed chain keeps NO_WINDOW_CHANGES");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.hwnd_target = false;
      check(!wsi_iflip_plan_of(&knobs, &in).window_changes,
            "a composition chain keeps NO_WINDOW_CHANGES");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.may_tear = true;
      check(!wsi_iflip_plan_of(&knobs, &in).window_changes,
            "a tearing chain is not eligible, so it keeps the flag too");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.extent_width = 1280;
      in.extent_height = 720;
      in.client_width = 1280;                             /* a chain smaller than the output */
      in.client_height = 720;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_EXTENT, "not the output's size");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.alpha_ignore = false;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_ALPHA, "blended by the compositor");
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.may_tear = true;
      check(wsi_iflip_plan_of(&knobs, &in).reason == WSI_IFLIP_TEARING,
            "an IMMEDIATE chain keeps its tearing and loses the flip");
      in.system_tearing = false;
      check(wsi_iflip_plan_of(&knobs, &in).eligible,
            "IMMEDIATE without system tearing support is not a tearing chain");
   }

   /* 5. The scaling clause follows the sizes: a buffer that is not its window's size keeps the
    * stretch it needs, and then it is not a flip candidate either.
    */
   {
      struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      in.extent_width = 1920;
      in.extent_height = 1200;
      in.client_width = 1920;
      in.client_height = 1181;                            /* A5w's first chain, before the restyle */
      const struct wsi_iflip_plan plan = wsi_iflip_plan_of(&knobs, &in);
      check(plan.scaling == WSI_IFLIP_SCALING_STRETCH, "a chain that is not its client's size");
      check(!plan.eligible, "and it is not eligible");
   }

   /* 6. The buffer count. The route's own copy is what allows more than two; without it the
    * chain keeps the application's count, which is what upstream limited to two.
    */
   {
      struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      check(wsi_iflip_buffer_count(&knobs, &in) == WSI_IFLIP_BUFFERS_DEFAULT,
            "two images become three buffers");
      in.own_copy = false;
      check(wsi_iflip_buffer_count(&knobs, &in) == 2u,
            "a driver that renders into the back buffer keeps two");
      in.own_copy = true;
      in.min_image_count = 1;
      check(wsi_iflip_buffer_count(&knobs, &in) >= WSI_IFLIP_BUFFERS_MIN,
            "never below the flip model's minimum");
      in.min_image_count = 3;
      check(wsi_iflip_buffer_count(&knobs, &in) == 3u, "an application that asks for three");
      in.hwnd_target = false;
      in.min_image_count = 2;
      check(wsi_iflip_buffer_count(&knobs, &in) == 2u, "a composition chain is left alone");
      const struct wsi_iflip_knobs own = wsi_iflip_knobs_parse(NULL, NULL, NULL, "0", NULL);
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      check(wsi_iflip_buffer_count(&own, &in) == 2u, "BUFFERS=0 keeps the application's count");

      /* The array bound holds on every path, switches included (review of r1): cmd_lists has
       * WSI_IFLIP_BUFFERS_MAX entries, and a count above it used to refuse the chain and lose the
       * DXGI route altogether. Reachable only above the advertised maximum, so it needs a client
       * that ignores maxImageCount; the clamp costs nothing and the bound cannot disagree.
       */
      const struct wsi_iflip_knobs off = knobs_b27();
      in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 4);
      check(wsi_iflip_buffer_count(&off, &in) == WSI_IFLIP_BUFFERS_MAX,
            "shape off, a client that asks for four is still clamped to the array bound");
      check(wsi_iflip_buffer_count(&own, &in) == WSI_IFLIP_BUFFERS_MAX,
            "BUFFERS=0, four asked for, clamped to the array bound");
      in.hwnd_target = false;
      check(wsi_iflip_buffer_count(&knobs, &in) == WSI_IFLIP_BUFFERS_MAX,
            "a composition chain that asks for four is clamped as well");
      in.hwnd_target = true;
      in.own_copy = false;
      check(wsi_iflip_buffer_count(&knobs, &in) == WSI_IFLIP_BUFFERS_MAX,
            "no copy of this route's own, four asked for, clamped as well");
   }

   /* 7. What DXGI answered about the buffers. A query that failed is not an answer, and a chain
    * that was not eligible keeps its own reason: the displayable clause is the last one.
    */
   {
      const struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      const struct wsi_iflip_plan plan = wsi_iflip_plan_of(&knobs, &in);

      check(wsi_iflip_displayable_reason(&plan, true, true) == WSI_IFLIP_ELIGIBLE,
            "ALLOW_DISPLAY buffers");
      check(wsi_iflip_displayable_reason(&plan, true, false) == WSI_IFLIP_NOT_DISPLAYABLE,
            "buffers the display cannot read");
      check(wsi_iflip_displayable_reason(&plan, false, false) == WSI_IFLIP_ELIGIBLE,
            "a query that failed says nothing");
      check(wsi_iflip_reason_is_ours(WSI_IFLIP_NOT_DISPLAYABLE), "and it warns");
      if (control)
         check(wsi_iflip_displayable_reason(&plan, true, true) == plan.reason,
               "an ineligible chain keeps its own reason");
      check(!wsi_iflip_reason_is_ours(WSI_IFLIP_NOT_COVERED), "a window does not warn");
      check(!wsi_iflip_reason_is_ours(WSI_IFLIP_EXTENT), "a smaller chain does not warn");
      check(!wsi_iflip_reason_is_ours(WSI_IFLIP_ELIGIBLE), "an eligible chain does not warn");
      /* The format is the application's choice, like the window and the extent, so it does not
       * warn either (review of r1): an FP16 HDR chain is composed because the application asked
       * for a format the display cannot scan out, not because this route gave it one.
       */
      check(!wsi_iflip_reason_is_ours(WSI_IFLIP_FORMAT), "the application's format does not warn");
   }

   /* 7b. The chain as it stands, not as it was asked for. A chain taken over from an oldSwapchain
    * keeps the Scaling it was created with, which ResizeBuffers cannot change, so the reason the
    * route logs is computed from GetDesc1 (review of r1).
    */
   {
      const struct wsi_iflip_inputs in = borderless_1920x1200(WSI_IFLIP_DXGI_B8G8R8A8_UNORM, 2);
      const struct wsi_iflip_knobs on = knobs_default();
      const struct wsi_iflip_plan plan = wsi_iflip_plan_of(&on, &in);
      struct wsi_iflip_got got = {};

      got.known = true;
      got.scaling = WSI_IFLIP_SCALING_NONE;
      got.buffer_count = WSI_IFLIP_BUFFERS_DEFAULT;
      got.allow_tearing = false;
      got.heap_queried = true;
      got.allow_display = true;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_ELIGIBLE,
            "the chain is what the plan asked for");
      got.scaling = WSI_IFLIP_SCALING_STRETCH;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_SCALING,
            "a reused chain that kept STRETCH is reported as stretching, not as eligible");
      got.scaling = WSI_IFLIP_SCALING_NONE;
      got.allow_tearing = true;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_TEARING,
            "a reused chain that kept ALLOW_TEARING is reported as tearing");
      got.allow_tearing = false;
      got.buffer_count = 1;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_BUFFERS,
            "a chain with one buffer is no flip-model chain");
      got.buffer_count = WSI_IFLIP_BUFFERS_DEFAULT;
      got.allow_display = false;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_NOT_DISPLAYABLE,
            "the heap reading is the last clause");
      got.heap_queried = false;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_ELIGIBLE,
            "a heap query that failed says nothing");
      got.known = false;
      got.scaling = WSI_IFLIP_SCALING_STRETCH;
      got.allow_tearing = true;
      check(wsi_iflip_chain_reason(&plan, &got) == WSI_IFLIP_ELIGIBLE,
            "GetDesc1 that failed leaves the plan's own answer");
      /* A chain that was not eligible keeps its own reason whatever the read-back says. */
      struct wsi_iflip_inputs windowed = in;
      windowed.covers_output = false;
      const struct wsi_iflip_plan windowed_plan = wsi_iflip_plan_of(&on, &windowed);
      got.known = true;
      got.scaling = WSI_IFLIP_SCALING_NONE;
      got.allow_tearing = false;
      got.heap_queried = true;
      got.allow_display = true;
      check(wsi_iflip_chain_reason(&windowed_plan, &got) == WSI_IFLIP_NOT_COVERED,
            "a windowed chain keeps not-covered");
   }

   /* 8. The coverage arithmetic, which is the client area against the monitor. */
   {
      check(wsi_iflip_covers(0, 0, 1920, 1200, 0, 0, 1920, 1200), "exactly the output");
      check(!wsi_iflip_covers(0, 0, 1920, 1181, 0, 0, 1920, 1200), "19 rows short");
      check(!wsi_iflip_covers(0, 0, 1921, 1200, 0, 0, 1920, 1200), "one column too wide");
      check(!wsi_iflip_covers(1, 0, 1921, 1200, 0, 0, 1920, 1200), "the right size, moved");
      check(!wsi_iflip_covers(0, 0, 0, 0, 0, 0, 1920, 1200), "an empty client area");
      check(wsi_iflip_covers(-1920, 0, 0, 1200, -1920, 0, 0, 1200), "a monitor left of the first");
   }

   /* 9. Every reason has a name, and no two share one: the route log and the lab's parser read
    * the name and never the number.
    */
   {
      bool ok = true;
      for (unsigned i = 0; i < WSI_IFLIP_REASON_COUNT; i++) {
         const char *text = wsi_iflip_reason_text((enum wsi_iflip_reason)i);
         if (!text || !text[0] || !std::strcmp(text, "unknown"))
            ok = false;
         for (unsigned j = 0; j < i; j++) {
            if (!std::strcmp(text, wsi_iflip_reason_text((enum wsi_iflip_reason)j)))
               ok = false;
         }
      }
      check(ok, "every reason has its own name");
      check(!std::strcmp(wsi_iflip_reason_text(WSI_IFLIP_REASON_COUNT), "unknown"),
            "a number past the table");
   }

   /* 10. The copy list the present picks. The production rule is "the list of the buffer DXGI is
    * about to show", which is what makes an acquire order other than the buffer order correct.
    * The old rule - the image's single list, whose destination was back buffer image_index - is
    * driven here as well and must copy into the wrong buffer as soon as the two differ. This is
    * why upstream could expose two images only.
    */
   {
      /* (acquired image, back buffer the next Present1 will show) */
      const unsigned sequence[][2] = { {0,0}, {1,1}, {2,2}, {0,0}, {2,1}, {1,2}, {0,1}, {1,0} };
      unsigned right = 0, wrong_under_the_old_rule = 0;

      for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
         const unsigned image = sequence[i][0], buffer = sequence[i][1];
         /* The production rule: wsi_iflip_list_index names the list of the buffer that will be
          * shown, and that list was recorded with that same buffer as its destination.
          */
         const unsigned list = wsi_iflip_list_index(buffer, 3);
         if (list != WSI_IFLIP_LIST_NONE && iflip_dst_of_list(list) == buffer)
            right++;
         /* The old rule: one list per image, recorded with the image's own buffer. */
         if (old_dst_of_image(image) != buffer)
            wrong_under_the_old_rule++;
      }
      check(right == 8u, "every present copies into the buffer it will show");
      check(wrong_under_the_old_rule == 4u,
            "the old one-list rule would have copied into the wrong buffer four times");

      /* The bounds half of the same rule: a buffer the chain does not have, and a count the
       * recorded array cannot hold, both have no list, and the present fails instead of copying
       * into the wrong buffer.
       */
      check(wsi_iflip_list_index(2, 2) == WSI_IFLIP_LIST_NONE,
            "a buffer index outside the chain's count has no list");
      check(wsi_iflip_list_index(0, 0) == WSI_IFLIP_LIST_NONE, "a chain with no buffers");
      check(wsi_iflip_list_index(3, WSI_IFLIP_BUFFERS_MAX + 1) == WSI_IFLIP_LIST_NONE,
            "a count past the recorded array");
      check(wsi_iflip_list_index(1, 2) == 1u && wsi_iflip_list_index(0, 3) == 0u,
            "and an index inside the chain is the list of that buffer");
   }

   /* 11. The acquire fallback of a DXGI chain. The b27 rule took the current back-buffer index as
    * an image index and stepped one place when that image was DRAWING, asserting the next one was
    * QUEUED; the assert is compiled out of the shipped build, and with three images and two of them
    * legally acquired the step could hand back an image the application already holds. The schedule
    * below is the one the review found by exhaustive search (acquire 0, acquire 1, acquire 2,
    * present 0, acquire): the production rule must answer "nothing is queued" there, and never an
    * image whose state is DRAWING.
    */
   {
      unsigned char state[WSI_IFLIP_BUFFERS_MAX];
      uint64_t serial[WSI_IFLIP_BUFFERS_MAX] = { 0, 0, 0 };

      /* image 0 presented (QUEUED, serial 1), images 1 and 2 held by the application */
      state[0] = (unsigned char)WSI_IFLIP_IMAGE_QUEUED;
      serial[0] = 1;
      state[1] = (unsigned char)WSI_IFLIP_IMAGE_DRAWING;
      state[2] = (unsigned char)WSI_IFLIP_IMAGE_DRAWING;
      check(wsi_iflip_pick_queued(state, serial, 3) == 0u, "the one queued image is the answer");

      /* The aliasing schedule: all three held, nothing queued. The old rule would have returned
       * (current back buffer + 1), which is DRAWING.
       */
      state[0] = (unsigned char)WSI_IFLIP_IMAGE_DRAWING;
      check(wsi_iflip_pick_queued(state, serial, 3) == WSI_IFLIP_IMAGE_NONE,
            "nothing is queued: the acquire answers not-ready and hands out no image");

      /* Two queued images: the oldest, which is the one whose fence is nearest to signalling. */
      state[0] = (unsigned char)WSI_IFLIP_IMAGE_QUEUED;
      serial[0] = 7;
      state[1] = (unsigned char)WSI_IFLIP_IMAGE_QUEUED;
      serial[1] = 4;
      state[2] = (unsigned char)WSI_IFLIP_IMAGE_DRAWING;
      check(wsi_iflip_pick_queued(state, serial, 3) == 1u, "the oldest queued image");
      serial[1] = 9;
      check(wsi_iflip_pick_queued(state, serial, 3) == 0u, "and it follows the serial, not the index");

      /* Two images, the count upstream allowed: the rule is the same one. */
      state[0] = (unsigned char)WSI_IFLIP_IMAGE_DRAWING;
      state[1] = (unsigned char)WSI_IFLIP_IMAGE_QUEUED;
      serial[1] = 2;
      check(wsi_iflip_pick_queued(state, serial, 2) == 1u, "two images, one queued");
      state[1] = (unsigned char)WSI_IFLIP_IMAGE_IDLE;
      check(wsi_iflip_pick_queued(state, serial, 2) == WSI_IFLIP_IMAGE_NONE,
            "an idle image is not a queued one (the idle search runs before this rule)");
   }

   std::printf("iflip%s: %u checks, %u failures\n", control ? " (b27 control)" : "", checks,
               failures);
   return failures ? 1 : 0;
}
