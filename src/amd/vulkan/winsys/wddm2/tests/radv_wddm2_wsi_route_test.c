/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_wsi_route.h and wsi_win32_deadline.h: the present route switch, the
 * report of application-local runtime modules, the D3D12 implementation check, the LB7A checks of
 * the shared-resource import, and the deadlines and the degrade rule of the DXGI route (BD-105).
 * No GPU, no Vulkan, no Windows header. Run through
 * bc250-win tools/build/build-radv-wsi-route-test.ps1.
 *
 * Usage: radv_wddm2_wsi_route_test [--negative-control]
 * The negative control runs every case with one expectation inverted; it must fail, which shows
 * that a broken rule is reported.
 */
#include "../radv_wddm2_wsi_route.h"
#include "../../../../../vulkan/wsi/wsi_win32_deadline.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
static bool invert;

#define CHECK(cond)                                                                                \
   do {                                                                                            \
      checks++;                                                                                    \
      if (!(cond) != invert) {                                                                     \
         failures++;                                                                               \
         printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                    \
      }                                                                                            \
   } while (0)

static void
test_parse(void)
{
   enum radv_wddm2_wsi_route r = RADV_WDDM2_WSI_ROUTE_GDI;
   CHECK(radv_wddm2_wsi_route_parse("dxgi", &r) && r == RADV_WDDM2_WSI_ROUTE_DXGI);
   CHECK(radv_wddm2_wsi_route_parse("DXGI", &r) && r == RADV_WDDM2_WSI_ROUTE_DXGI);
   CHECK(radv_wddm2_wsi_route_parse("Dxgi-Composition", &r) &&
         r == RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION);
   CHECK(radv_wddm2_wsi_route_parse("gdi", &r) && r == RADV_WDDM2_WSI_ROUTE_GDI);
   CHECK(!radv_wddm2_wsi_route_parse("dxgi ", &r));
   CHECK(!radv_wddm2_wsi_route_parse("dxg", &r));
   CHECK(!radv_wddm2_wsi_route_parse("", &r));
   CHECK(!radv_wddm2_wsi_route_parse(NULL, &r));
   CHECK(!strcmp(radv_wddm2_wsi_route_name(RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION),
                 "dxgi-composition"));
}

static void
test_choose(void)
{
   struct radv_wddm2_wsi_route_choice c;

   /* Nothing set: the default is GDI while the DXGI route has never completed a present on the lab
    * (BD-105). An application reaches the DXGI route only by asking for it.
    */
   c = radv_wddm2_wsi_route_choose(NULL, NULL);
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT &&
         !c.invalid);
   CHECK(RADV_WDDM2_WSI_ROUTE_DEFAULT == RADV_WDDM2_WSI_ROUTE_GDI);
   c = radv_wddm2_wsi_route_choose("", "");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT);

   /* "gdi" is the rollback, from either source, and the environment overrides the registry. */
   c = radv_wddm2_wsi_route_choose("gdi", NULL);
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_ENV && !c.invalid);
   c = radv_wddm2_wsi_route_choose(NULL, "GDI");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_REGISTRY &&
         !c.invalid);
   c = radv_wddm2_wsi_route_choose("gdi", "dxgi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_ENV);

   /* The environment wins over the registry. */
   c = radv_wddm2_wsi_route_choose("dxgi", "gdi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_ENV);
   c = radv_wddm2_wsi_route_choose(NULL, "dxgi-composition");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION &&
         c.source == RADV_WDDM2_WSI_SOURCE_REGISTRY);

   /* An empty environment value clears the override: the registry decides. */
   c = radv_wddm2_wsi_route_choose("", "dxgi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_REGISTRY);

   /* A wrong value never selects a GPU route. */
   c = radv_wddm2_wsi_route_choose("dxgi2", "dxgi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.invalid && c.source == RADV_WDDM2_WSI_SOURCE_ENV);
   c = radv_wddm2_wsi_route_choose(NULL, "invalid");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.invalid);
}

static void
test_path_in_dir(void)
{
   const wchar_t *sys = L"C:\\Windows\\system32";
   CHECK(radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System32\\dxgi.dll", sys));
   CHECK(radv_wddm2_wsi_path_in_dir(L"c:/windows/system32/D3D12.DLL", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System32\\sub\\dxgi.dll", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System32dxgi.dll", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System32\\", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System3", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"D:\\Games\\Foo\\dxgi.dll", sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(NULL, sys));
   CHECK(!radv_wddm2_wsi_path_in_dir(L"C:\\Windows\\System32\\dxgi.dll", L""));
}

/* Application-local copies are reported, and the route stays DXGI: the old module gate cases
 * (DXVK dxgi.dll, vkd3d-proton d3d12.dll and d3d12core.dll next to the game) now only give a mask
 * and a log text. There is no route input in these functions, by design.
 */
static void
test_app_local(void)
{
   const wchar_t *sys = L"C:\\Windows\\System32";
   char text[96];

   CHECK(radv_wddm2_wsi_app_local(sys, NULL, NULL, NULL) == 0);
   CHECK(radv_wddm2_wsi_app_local(sys, L"C:\\Windows\\System32\\dxgi.dll",
                                  L"C:\\Windows\\System32\\d3d12.dll",
                                  L"C:\\Windows\\System32\\D3D12Core.dll") == 0);
   /* DXVK next to the game (facts M792, M793). */
   CHECK(radv_wddm2_wsi_app_local(sys, L"D:\\Games\\Foo\\dxgi.dll", NULL, NULL) ==
         RADV_WDDM2_WSI_LOCAL_DXGI);
   /* vkd3d-proton next to the game (facts M793, M794). */
   CHECK(radv_wddm2_wsi_app_local(sys, L"C:\\Windows\\System32\\dxgi.dll",
                                  L"D:\\Games\\Foo\\d3d12.dll",
                                  L"D:\\Games\\Foo\\d3d12core.dll") ==
         (RADV_WDDM2_WSI_LOCAL_D3D12 | RADV_WDDM2_WSI_LOCAL_D3D12CORE));

   CHECK(!strcmp(radv_wddm2_wsi_app_local_text(0, 0, text, sizeof(text)), "none"));
   CHECK(!strcmp(radv_wddm2_wsi_app_local_text(RADV_WDDM2_WSI_LOCAL_DXGI, 0, text, sizeof(text)),
                 "dxgi.dll(loaded)"));
   CHECK(!strcmp(radv_wddm2_wsi_app_local_text(RADV_WDDM2_WSI_LOCAL_DXGI,
                                               RADV_WDDM2_WSI_LOCAL_DXGI |
                                                  RADV_WDDM2_WSI_LOCAL_D3D12CORE,
                                               text, sizeof(text)),
                 "dxgi.dll(loaded),d3d12core.dll(file)"));
   /* A short buffer is cut, never overrun. */
   char small[8];
   radv_wddm2_wsi_app_local_text(RADV_WDDM2_WSI_LOCAL_D3D12CORE, 0, small, sizeof(small));
   CHECK(strlen(small) < sizeof(small));
}

static void
test_d3d12_impl(void)
{
   const wchar_t *sys = L"C:\\Windows\\System32";
   const wchar_t *agility = L"D:\\Games\\Foo\\D3D12";

   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, NULL, L"C:\\Windows\\System32\\d3d12core.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM);
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, NULL, L"C:\\Windows\\System32\\D3D12.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM);
   /* The Agility SDK core in the directory the game's D3D12SDKPath names. */
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, agility, L"D:\\Games\\Foo\\D3D12\\D3D12Core.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_AGILITY);
   /* The debug layer or a capture tool wraps the device: accepted. */
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, agility,
                                         L"C:\\Windows\\System32\\d3d12SDKLayers.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED);
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, NULL, L"C:\\Tools\\renderdoc.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED);
   /* vkd3d-proton next to the game: the only case that leaves the route. */
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, NULL, L"D:\\Games\\Foo\\d3d12core.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN);
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, agility, L"D:\\Games\\Foo\\d3d12core.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN);
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, agility, L"D:\\Games\\Foo\\D3D12\\d3d12.dll") ==
         RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN);
   CHECK(radv_wddm2_wsi_d3d12_impl_class(sys, NULL, NULL) == RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN);

   CHECK(!radv_wddm2_wsi_d3d12_impl_usable(RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN));
   CHECK(radv_wddm2_wsi_d3d12_impl_usable(RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM));
   CHECK(radv_wddm2_wsi_d3d12_impl_usable(RADV_WDDM2_WSI_D3D12_IMPL_AGILITY));
   CHECK(radv_wddm2_wsi_d3d12_impl_usable(RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED));
   CHECK(radv_wddm2_wsi_d3d12_impl_usable(RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN));
   CHECK(!strcmp(radv_wddm2_wsi_d3d12_impl_name(RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN), "foreign"));

   CHECK(radv_wddm2_wsi_base_name_is(L"D:/Games/Foo/D3D12Core.DLL", L"d3d12core.dll"));
   CHECK(!radv_wddm2_wsi_base_name_is(L"D:\\Games\\d3d12core.dll.bak", L"d3d12core.dll"));
}

static struct radv_wddm2_lb7a
lb7a(uint32_t width, uint32_t height, uint32_t pitch, uint32_t format)
{
   struct radv_wddm2_lb7a s = {RADV_WDDM2_LB7A_MAGIC, 1, width, height, pitch, format, 0};
   s.size = (uint64_t)pitch * height;
   return s;
}

static void
test_lb7a(void)
{
   struct radv_wddm2_lb7a s;

   CHECK(radv_wddm2_lb7a_bytes_per_pixel(RADV_WDDM2_D3DDDIFMT_A8R8G8B8) == 4);
   CHECK(radv_wddm2_lb7a_bytes_per_pixel(RADV_WDDM2_D3DDDIFMT_A8B8G8R8) == 4);
   CHECK(radv_wddm2_lb7a_bytes_per_pixel(RADV_WDDM2_D3DDDIFMT_A2B10G10R10) == 4);
   CHECK(radv_wddm2_lb7a_bytes_per_pixel(RADV_WDDM2_D3DDDIFMT_A16B16G16R16F) == 8);
   CHECK(radv_wddm2_lb7a_bytes_per_pixel(0) == 0);

   /* The formats a Vulkan swapchain image can alias, at 1920x1200 with a 256-byte pitch. */
   s = lb7a(1920, 1200, 7680, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   CHECK(radv_wddm2_lb7a_valid(&s));
   s = lb7a(1920, 1200, 7680, RADV_WDDM2_D3DDDIFMT_A8B8G8R8);
   CHECK(radv_wddm2_lb7a_valid(&s));
   s = lb7a(1920, 1200, 7680, RADV_WDDM2_D3DDDIFMT_A2B10G10R10);
   CHECK(radv_wddm2_lb7a_valid(&s));
   s = lb7a(1920, 1200, 15360, RADV_WDDM2_D3DDDIFMT_A16B16G16R16F);
   CHECK(radv_wddm2_lb7a_valid(&s));

   /* FP16 at a 4-byte pitch is too short: the old 8-bit-only rule would have read past rows. */
   s = lb7a(1920, 1200, 7680, RADV_WDDM2_D3DDDIFMT_A16B16G16R16F);
   CHECK(!radv_wddm2_lb7a_valid(&s));
   /* An odd width with a 16-byte-aligned pitch. */
   s = lb7a(1001, 3, 4016, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   CHECK(radv_wddm2_lb7a_valid(&s));
   s = lb7a(1001, 3, 4004, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   CHECK(!radv_wddm2_lb7a_valid(&s)); /* pitch not a multiple of 16 */

   s = lb7a(64, 64, 256, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   s.magic = 0x41374241;
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(64, 64, 256, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   s.version = 2;
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(64, 64, 256, 77);
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(0, 64, 256, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(8193, 1, 32784, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(64, 64, 256, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   s.size -= 1;
   CHECK(!radv_wddm2_lb7a_valid(&s));
   s = lb7a(64, 64, 256, RADV_WDDM2_D3DDDIFMT_A8R8G8B8);
   s.size = UINT64_MAX;
   CHECK(!radv_wddm2_lb7a_valid(&s));
}

/* The deadlines of the DXGI route (wsi_win32_deadline.h). The rule under test is the one the route
 * broke on 2026-10-09: no CPU wait it owns may be unbounded, whatever the application asks for.
 */
static void
test_deadline(void)
{
   /* INFINITE is never the answer of a Win32 wait of this route, whatever the deadline. */
   CHECK(wsi_win32_wait_ms(UINT64_MAX) != 0xffffffffu);
   CHECK(wsi_win32_wait_ms(UINT64_MAX) == WSI_WIN32_WAIT_MS_MAX);
   CHECK(wsi_win32_wait_ms(WSI_WIN32_ROUTE_DEADLINE_NS) == 2000);
   CHECK(wsi_win32_wait_ms(1500000ull) == 1); /* 1.5 ms: truncated, never rounded to no wait */
   CHECK(wsi_win32_wait_ms(1) == 1);          /* under a millisecond still waits once */
   CHECK(wsi_win32_wait_ms(0) == 0);          /* a poll stays a poll */

   /* The deadline is a chosen lab bound, not a measured one: no present of this route has ever
    * completed. It sits far above the only present cost this file has shown, the GDI path's median
    * 3.18 ms at 1920x1200 (b26), and far below the three minutes a lab trial has.
    */
   CHECK(WSI_WIN32_ROUTE_DEADLINE_NS >= 100000000ull);
   CHECK(WSI_WIN32_ROUTE_DEADLINE_NS <= 10000000000ull);

   /* An application timeout at or under the route's deadline passes through unchanged, and the
    * vkAcquireNextImageKHR poll (timeout 0) stays a poll, so it still answers VK_NOT_READY.
    */
   CHECK(wsi_win32_acquire_timeout_ns(0, false) == 0);
   CHECK(!wsi_win32_acquire_timeout_capped(0, false));
   CHECK(wsi_win32_acquire_timeout_ns(1000000ull, false) == 1000000ull);
   CHECK(wsi_win32_acquire_timeout_ns(WSI_WIN32_ROUTE_DEADLINE_NS, false) ==
         WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_ns(WSI_WIN32_ROUTE_DEADLINE_NS + 1, false) ==
         WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_capped(WSI_WIN32_ROUTE_DEADLINE_NS + 1, false));

   /* Every stage of a first present has a name of its own, so one log line names the call that did
    * not return.
    */
   CHECK(!strcmp(wsi_win32_present_stage_name(WSI_WIN32_STAGE_IMAGES), "images"));
   CHECK(!strcmp(wsi_win32_present_stage_name(WSI_WIN32_STAGE_FENCE_WAIT), "queue-wait"));
   CHECK(!strcmp(wsi_win32_present_stage_name(WSI_WIN32_STAGE_PRESENT), "present1"));
   CHECK(strcmp(wsi_win32_present_stage_name(WSI_WIN32_STAGE_DONE),
                wsi_win32_present_stage_name(WSI_WIN32_STAGE_PRESENT)) != 0);
}

/* The round-3 rule, and the one the lab arm of 2026-10-09 bought with a frozen message pump: the
 * route's deadlines are lifted by a present that COMPLETED, never by Present1 returning. A present
 * that never completes must leave the bounded wait and the fallback in place.
 */
static void
test_present_completion(void)
{
   const uint64_t forever = UINT64_MAX;
   /* The value wsi_dxgi_blit signalled on the image's shared blit fence after the copy. */
   const uint64_t queued = 7;

   /* Present1 returned, the route recorded that value, and the fence has not reached it: nothing
    * completed, so the next acquire still gets the route's own deadline and not the application's
    * UINT64_MAX. This is the state the b27 round-2 ICD lifted the deadline in.
    */
   struct wsi_win32_route_state queued_only = {false, false};
   CHECK(!wsi_win32_route_present_complete(queued, 0));
   CHECK(!wsi_win32_route_present_complete(queued, queued - 1));
   CHECK(!wsi_win32_route_note_acquired(&queued_only, queued, queued - 1));
   CHECK(!queued_only.presented);
   CHECK(wsi_win32_acquire_timeout_ns(forever, queued_only.presented) ==
         WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_capped(forever, queued_only.presented));
   /* And when that bounded wait expires, the route is retired and the next swapchain takes CPU
    * images: outcome 2 of the lab plan (a usable window), not outcome 3 (a dead message pump).
    */
   CHECK(wsi_win32_route_wait_expired(&queued_only));
   CHECK(queued_only.dead && !wsi_win32_route_usable(&queued_only));

   /* The same present, completed: the fence reached the queued value and the acquire returned. Only
    * then are the deadlines lifted, and only once.
    */
   struct wsi_win32_route_state proved = {false, false};
   CHECK(wsi_win32_route_present_complete(queued, queued));
   CHECK(wsi_win32_route_present_complete(queued, queued + 3));
   CHECK(wsi_win32_route_note_acquired(&proved, queued, queued));
   CHECK(proved.presented && !proved.dead);
   CHECK(!wsi_win32_route_note_acquired(&proved, queued, queued));
   CHECK(wsi_win32_acquire_timeout_ns(forever, proved.presented) == forever);
   CHECK(!wsi_win32_route_wait_expired(&proved));
   CHECK(wsi_win32_route_usable(&proved));

   /* An image that was never presented has timeline value 0: an acquire that returns it proves
    * nothing. The first acquires of a chain take an idle image and never wait, so they must not lift
    * anything, whatever any fence reads.
    */
   struct wsi_win32_route_state fresh = {false, false};
   CHECK(!wsi_win32_route_present_complete(0, 0));
   CHECK(!wsi_win32_route_present_complete(0, 99));
   CHECK(!wsi_win32_route_note_acquired(&fresh, 0, 99));
   CHECK(!fresh.presented);
   CHECK(wsi_win32_acquire_timeout_capped(forever, fresh.presented));

   /* The value must be the ACQUIRED image's own, not the chain's most recent present. A two-image
    * chain alternates, so the most recent present always belongs to the other image and its fence
    * lags by one frame: a route proved against it would stay behind for ever. Played through: image
    * 0 was presented at value 2 and its copy is done, image 1 was presented at 4 and its copy is
    * not. The acquire of image 0 lifts the deadlines, and the lagging value would not have.
    */
   struct wsi_win32_route_state alternating = {false, false};
   const uint64_t image0_value = 2, image0_fence = 2;
   const uint64_t image1_value = 4, image1_fence = 3;
   CHECK(!wsi_win32_route_present_complete(image1_value, image1_fence));
   CHECK(!wsi_win32_route_note_acquired(&alternating, image1_value, image1_fence));
   CHECK(!alternating.presented);
   CHECK(wsi_win32_route_present_complete(image0_value, image0_fence));
   CHECK(wsi_win32_route_note_acquired(&alternating, image0_value, image0_fence));
   CHECK(alternating.presented);

   /* A route a wait already retired is never revived by a fence that completes afterwards. */
   struct wsi_win32_route_state retired = {false, true};
   CHECK(!wsi_win32_route_note_acquired(&retired, queued, queued + 1));
   CHECK(!retired.presented && !wsi_win32_route_usable(&retired));
   CHECK(wsi_win32_acquire_timeout_capped(forever, retired.presented));
}

/* Retiring the work a dead route queued, which is what round 3 left undone: the route retired itself
 * on its deadline, the application took the error, and then it froze in its own vkDeviceWaitIdle
 * waiting for the submission whose shared blit timeline only the dead presenter could signal
 * (BD-105, lab round 3 of 2026-10-09, stack-vkcube-12308).
 */
static void
test_retire_waits(void)
{
   uint64_t value = 0;

   /* The shape of one presented image: the application signalled V = 1, wsi_dxgi_blit signalled
    * V + 1 = 2, and the second submission waits for 2. The presenter is gone, so the semaphore
    * still reads 1 and the release value is 2.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_value(2, 1, &value) && value == 2);
   /* Signalled: the same image asks for nothing a second time, because vkSignalSemaphore may only
    * raise a timeline semaphore and 2 -> 2 is not a raise.
    */
   value = 0;
   CHECK(!wsi_win32_route_retire_value(2, 2, &value) && value == 0);
   CHECK(!wsi_win32_route_retire_value(2, 3, &value));

   /* An image that was never presented has timeline value 0 and no wait of ours behind it. */
   value = 0;
   CHECK(!wsi_win32_route_retire_value(0, 0, &value) && value == 0);
   CHECK(!wsi_win32_route_retire_value(0, 7, &value));

   /* Both images of the lab's two-image chain, with the readings arm A2f printed (want 2 on image 0
    * and a queued present of image 1 at value 2): every image with an outstanding value is
    * released, not only the one whose acquire expired.
    */
   const uint64_t want[2] = {2, 2};
   uint64_t have[2] = {1, 1};
   unsigned released = 0;
   for (unsigned i = 0; i < 2; i++) {
      if (wsi_win32_route_retire_value(want[i], have[i], &value)) {
         have[i] = value;
         released++;
      }
   }
   CHECK(released == 2 && have[0] == 2 && have[1] == 2);
   /* And the chain's teardown, which runs the same release again, finds nothing left to do. */
   for (unsigned i = 0; i < 2; i++)
      CHECK(!wsi_win32_route_retire_value(want[i], have[i], &value));

   /* The deadline rules are unchanged by the release: a route a wait retired stays retired, and the
    * release is not a proof of anything. An expiry releases the waits and keeps the route dead.
    */
   struct wsi_win32_route_state dead = {false, false};
   CHECK(wsi_win32_route_wait_expired(&dead));
   CHECK(dead.dead && !wsi_win32_route_usable(&dead));
   CHECK(wsi_win32_route_retire_value(2, 1, &value) && value == 2);
   CHECK(!wsi_win32_route_note_acquired(&dead, 2, 2));
   CHECK(!dead.presented && !wsi_win32_route_usable(&dead));

   /* A live route must not be released from the CPU: there the value is the presenter's to signal,
    * and the rule is only ever reached on a path that has already given the route up. The rule
    * itself still answers for the state, which is what the caller's gate rests on.
    */
   struct wsi_win32_route_state alive = {true, false};
   CHECK(wsi_win32_route_usable(&alive));
}

/* The stage lines. One bit per stage per chain, so a stage first reached on the tenth frame is
 * still named; the round-2 gate was the route's presented flag, which silenced every stage from the
 * second frame on - including the acquire that froze.
 */
static void
test_stage_bits(void)
{
   uint32_t bits = 0;

   CHECK(wsi_win32_stage_first(&bits, WSI_WIN32_STAGE_IMAGES));
   CHECK(!wsi_win32_stage_first(&bits, WSI_WIN32_STAGE_IMAGES));
   /* Every stage has a bit of its own: one stage already written never hides another. */
   for (int s = WSI_WIN32_STAGE_IMAGES; s < WSI_WIN32_STAGE_COUNT; s++)
      CHECK(wsi_win32_stage_first(&bits, (enum wsi_win32_present_stage)s) ==
            (s != WSI_WIN32_STAGE_IMAGES));
   CHECK(bits == (1u << (unsigned)WSI_WIN32_STAGE_COUNT) - 1u);

   /* Seven stages, and the seventh is the acquire of the next frame, with a name of its own. */
   CHECK((int)WSI_WIN32_STAGE_COUNT == 7);
   CHECK((int)WSI_WIN32_STAGE_ACQUIRE == (int)WSI_WIN32_STAGE_DONE + 1);
   CHECK(!strcmp(wsi_win32_present_stage_name(WSI_WIN32_STAGE_ACQUIRE), "acquire-wait"));
   for (int s = WSI_WIN32_STAGE_IMAGES; s < (int)WSI_WIN32_STAGE_ACQUIRE; s++)
      CHECK(strcmp(wsi_win32_present_stage_name((enum wsi_win32_present_stage)s),
                   wsi_win32_present_stage_name(WSI_WIN32_STAGE_ACQUIRE)) != 0);

   /* A stage out of range writes nothing at all. */
   uint32_t none = 0;
   CHECK(!wsi_win32_stage_first(&none, WSI_WIN32_STAGE_COUNT));
   CHECK(none == 0);
}

/* The b26 lab failure, played through the rules: a swapchain takes the DXGI route, its first
 * present never completes, and the application waits for the image with UINT64_MAX (Quake II RTX
 * and vkcube both do). The old route waited there for ever, with the GPU idle and no TDR to end it.
 */
static void
test_first_present_freeze(void)
{
   struct wsi_win32_route_state route = {false, false};
   struct wsi_win32_route_state healthy = {true, false};
   const uint64_t forever = UINT64_MAX;

   CHECK(wsi_win32_route_usable(&route)); /* a fresh instance may take the route */
   CHECK(wsi_win32_acquire_timeout_ns(forever, route.presented) == WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_capped(forever, route.presented));
   /* The wait expired before any present: the route is retired for the process, and the swapchain
    * is reported out of date so that the application creates the next one on CPU images.
    */
   CHECK(wsi_win32_route_wait_expired(&route));
   CHECK(route.dead && !wsi_win32_route_usable(&route));
   /* It stays retired; nothing re-arms it inside the process. */
   CHECK(wsi_win32_route_wait_expired(&route));
   CHECK(!wsi_win32_route_usable(&route));

   /* A route that has shown a frame keeps the application's own timeout, and one late wait after
    * that is a slow frame, not a broken route.
    */
   CHECK(wsi_win32_acquire_timeout_ns(forever, healthy.presented) == forever);
   CHECK(!wsi_win32_acquire_timeout_capped(forever, healthy.presented));
   CHECK(!wsi_win32_route_wait_expired(&healthy));
   CHECK(wsi_win32_route_usable(&healthy));
   CHECK(!healthy.dead);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"parse", test_parse},
   {"choose", test_choose},
   {"path_in_dir", test_path_in_dir},
   {"app_local", test_app_local},
   {"d3d12_impl", test_d3d12_impl},
   {"lb7a", test_lb7a},
   {"deadline", test_deadline},
   {"present_completion", test_present_completion},
   {"retire_waits", test_retire_waits},
   {"stage_bits", test_stage_bits},
   {"first_present_freeze", test_first_present_freeze},
};

int
main(int argc, char **argv)
{
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   int failed_tests = 0;
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      const int before = failures;
      tests[i].fn();
      const bool ok = failures == before;
      failed_tests += !ok;
      printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
   }
   printf("wsi route: %u tests, %d checks, %d failed checks%s\n",
          (unsigned)(sizeof(tests) / sizeof(tests[0])), checks, failures,
          invert ? " (negative control)" : "");
   return failed_tests ? 1 : 0;
}
