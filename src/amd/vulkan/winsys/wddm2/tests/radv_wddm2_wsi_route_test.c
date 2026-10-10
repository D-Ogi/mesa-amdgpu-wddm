/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_wsi_route.h and wsi_win32_deadline.h: the present route switch, the
 * report of application-local runtime modules, the D3D12 implementation check, the LB7A checks of
 * the shared-resource import, and the deadlines, the degrade rule and the retirement rules of the
 * DXGI route (BD-105). No GPU, no Vulkan and no Windows header: the one thing outside the C library
 * is _beginthread from <process.h>, which the concurrency case of round 4b needs to run the route
 * flags from two threads at once (V3). Run through
 * bc250-win tools/build/build-radv-wsi-route-test.ps1.
 *
 * Usage: radv_wddm2_wsi_route_test [--negative-control]
 * The negative control runs every case with one expectation inverted; it must fail, which shows
 * that a broken rule is reported.
 */
#include "../radv_wddm2_wsi_route.h"
#include "../../../../../vulkan/wsi/wsi_win32_deadline.h"

#include <process.h>
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

   /* The retirement's own wait sits INSIDE the route deadline: by the time it runs, the route has
    * already spent that deadline once, and the thing it waits for is work on the application's own
    * healthy device. It is still a wait, so it still has a bound and the bound is still never
    * INFINITE.
    */
   CHECK(WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS > 0);
   CHECK(WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS < WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_wait_ms(WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS) == 200);
   CHECK(wsi_win32_wait_ms(WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS) != 0xffffffffu);

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
   struct wsi_win32_route_state queued_only = {0};
   CHECK(!wsi_win32_route_present_complete(queued, 0));
   CHECK(!wsi_win32_route_present_complete(queued, queued - 1));
   CHECK(!wsi_win32_route_note_acquired(&queued_only, queued, queued - 1));
   CHECK(!wsi_win32_route_presented(&queued_only));
   CHECK(wsi_win32_acquire_timeout_ns(forever, wsi_win32_route_presented(&queued_only)) ==
         WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&queued_only)));
   /* And when that bounded wait expires, the route is retired and the next swapchain takes CPU
    * images: outcome 2 of the lab plan (a usable window), not outcome 3 (a dead message pump).
    */
   CHECK(wsi_win32_route_wait_expired(&queued_only));
   CHECK(!wsi_win32_route_usable(&queued_only));

   /* The same present, completed: the fence reached the queued value and the acquire returned. Only
    * then are the deadlines lifted, and only once.
    */
   struct wsi_win32_route_state proved = {0};
   CHECK(wsi_win32_route_present_complete(queued, queued));
   CHECK(wsi_win32_route_present_complete(queued, queued + 3));
   CHECK(wsi_win32_route_note_acquired(&proved, queued, queued));
   CHECK(wsi_win32_route_presented(&proved) && wsi_win32_route_usable(&proved));
   CHECK(!wsi_win32_route_note_acquired(&proved, queued, queued));
   CHECK(wsi_win32_acquire_timeout_ns(forever, wsi_win32_route_presented(&proved)) == forever);
   CHECK(!wsi_win32_route_wait_expired(&proved));
   CHECK(wsi_win32_route_usable(&proved));

   /* UINT64_MAX is NOT a very high fence value: it is the documented answer of
    * ID3D12Fence::GetCompletedValue for a REMOVED device (sdk-api
    * nf-d3d12-id3d12fence-getcompletedvalue.md:61), which is exactly what lab round 3 read. Round 4
    * compared it with >= and would have called the route proved by a presenter that had died.
    */
   struct wsi_win32_route_state removed = {0};
   CHECK(!wsi_win32_route_present_complete(queued, UINT64_MAX));
   CHECK(!wsi_win32_route_note_acquired(&removed, queued, UINT64_MAX));
   CHECK(!wsi_win32_route_presented(&removed));
   CHECK(wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&removed)));

   /* An image that was never presented has timeline value 0: an acquire that returns it proves
    * nothing. The first acquires of a chain take an idle image and never wait, so they must not lift
    * anything, whatever any fence reads.
    */
   struct wsi_win32_route_state fresh = {0};
   CHECK(!wsi_win32_route_present_complete(0, 0));
   CHECK(!wsi_win32_route_present_complete(0, 99));
   CHECK(!wsi_win32_route_note_acquired(&fresh, 0, 99));
   CHECK(!wsi_win32_route_presented(&fresh));
   CHECK(wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&fresh)));

   /* The value must be the ACQUIRED image's own, not the chain's most recent present. A two-image
    * chain alternates, so the most recent present always belongs to the other image and its fence
    * lags by one frame: a route proved against it would stay behind for ever. Played through: image
    * 0 was presented at value 2 and its copy is done, image 1 was presented at 4 and its copy is
    * not. The acquire of image 0 lifts the deadlines, and the lagging value would not have.
    */
   struct wsi_win32_route_state alternating = {0};
   const uint64_t image0_value = 2, image0_fence = 2;
   const uint64_t image1_value = 4, image1_fence = 3;
   CHECK(!wsi_win32_route_present_complete(image1_value, image1_fence));
   CHECK(!wsi_win32_route_note_acquired(&alternating, image1_value, image1_fence));
   CHECK(!wsi_win32_route_presented(&alternating));
   CHECK(wsi_win32_route_present_complete(image0_value, image0_fence));
   CHECK(wsi_win32_route_note_acquired(&alternating, image0_value, image0_fence));
   CHECK(wsi_win32_route_presented(&alternating));

   /* A route a wait already retired is never revived by a fence that completes afterwards. */
   struct wsi_win32_route_state retired = {WSI_WIN32_ROUTE_DEAD};
   CHECK(!wsi_win32_route_note_acquired(&retired, queued, queued + 1));
   CHECK(!wsi_win32_route_presented(&retired) && !wsi_win32_route_usable(&retired));
   CHECK(wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&retired)));
}

/* V1, the first half: a wait that expired says nothing about the presenter, and the three states it
 * could be in are told apart by the two readings that exist. The audit of 2026-10-10 found round 4
 * inferring "removed" from a timeout and then host-signalling a Vulkan timeline on that inference.
 */
static void
test_presenter_state(void)
{
   /* The reading lab round 3 took: GetDeviceRemovedReason answered DXGI_ERROR_DEVICE_REMOVED
    * (0x887A0005) and the blit fence answered the UINT64_MAX sentinel. Either one alone proves it.
    */
   CHECK(wsi_win32_presenter_state(true, 0x887A0005u, true, UINT64_MAX) ==
         WSI_WIN32_PRESENTER_REMOVED);
   CHECK(wsi_win32_presenter_state(true, 0x887A0005u, false, 0) == WSI_WIN32_PRESENTER_REMOVED);
   CHECK(wsi_win32_presenter_state(false, 0, true, UINT64_MAX) == WSI_WIN32_PRESENTER_REMOVED);
   CHECK(wsi_win32_presenter_state(true, 0x887A0006u, true, 1) == WSI_WIN32_PRESENTER_REMOVED);

   /* A LIVE presenter that is merely slow: the removal reason was read and it is S_OK, and the fence
    * sits at a real value below the one the copy should have reached. This is the state round 4
    * could not tell from a removed one, and the one in which a host signal is forbidden.
    */
   CHECK(wsi_win32_presenter_state(true, 0u, true, 1) == WSI_WIN32_PRESENTER_LIVE);
   CHECK(wsi_win32_presenter_state(true, 0u, false, 0) == WSI_WIN32_PRESENTER_LIVE);

   /* UNPROVEN: nothing answered. A driver that gave the WSI no get_d3d12_device hook, or a chain
    * with no blit fences. Round 4's reader returned S_OK here, which reads as a live device on
    * evidence nobody has; the state is its own now.
    */
   CHECK(wsi_win32_presenter_state(false, 0u, false, 0) == WSI_WIN32_PRESENTER_UNPROVEN);
   CHECK(wsi_win32_presenter_state(false, 0x887A0005u, false, 0) == WSI_WIN32_PRESENTER_UNPROVEN);
   /* A fence that reads a real value is not a proof of life: only the removal reason is. */
   CHECK(wsi_win32_presenter_state(false, 0u, true, 2) == WSI_WIN32_PRESENTER_UNPROVEN);

   /* Three names, all different, so a log line says which one it was. */
   CHECK(!strcmp(wsi_win32_presenter_state_name(WSI_WIN32_PRESENTER_LIVE), "live"));
   CHECK(!strcmp(wsi_win32_presenter_state_name(WSI_WIN32_PRESENTER_REMOVED), "removed"));
   CHECK(!strcmp(wsi_win32_presenter_state_name(WSI_WIN32_PRESENTER_UNPROVEN), "unproven"));
   CHECK((int)WSI_WIN32_PRESENTER_UNPROVEN == 0); /* a zeroed state is "nothing is known" */
}

/* V1, the second half: retiring the work a dead route queued, which round 3 left undone and round 4
 * did by breaking a signal rule. The route retired itself on its deadline, the application took the
 * error, and then it froze in its own vkDeviceWaitIdle waiting for the submission whose shared blit
 * timeline only the dead presenter could signal (BD-105, lab round 3 of 2026-10-09,
 * stack-vkcube-12308). The host may release that wait, and only with the two proofs below.
 */
static void
test_retire_action(void)
{
   uint64_t value = 0;
   const enum wsi_win32_presenter_state removed = WSI_WIN32_PRESENTER_REMOVED;
   const enum wsi_win32_presenter_state live = WSI_WIN32_PRESENTER_LIVE;
   const enum wsi_win32_presenter_state unproven = WSI_WIN32_PRESENTER_UNPROVEN;
   /* The presenter's Signal of the value the image carries was accepted, which is what makes that
    * value the presenter's. Every case below this constant models a cycle whose blit succeeded; the
    * cases for a blit that failed pass false and are the second half of this test.
    */
   const bool owed = true;

   /* The shape of one presented image: the application signalled V = 1, wsi_dxgi_blit signalled
    * V + 1 = 2, and the second submission waits for 2. With the presenter PROVED removed and the
    * semaphore reading exactly 1, there is no pending Vulkan signal left on the object, so 2 is
    * greater than the current value (03258) and below no pending one (03259).
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 1, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 2);

   /* The delayed first signal, which is the case VUID-VkSemaphoreSignalInfo-value-03259 forbids and
    * round 4 signalled anyway: the semaphore still reads 0, so the application's own signal of 1 is
    * PENDING, and a host signal of 2 would pass it. Refused, and the value the caller must wait for
    * first is 1.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 0, &value) == WSI_WIN32_RETIRE_REFUSE);
   CHECK(value == 0); /* nothing is handed back for a refusal */
   CHECK(wsi_win32_route_retire_wait_value(2, 0) == 1);
   /* Once that wait is satisfied the same reading becomes a signal: this is the whole bounded-wait
    * step of wsi_win32_retire_blit_waits, played through.
    */
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 1, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 2);
   CHECK(wsi_win32_route_retire_wait_value(2, 1) == 0); /* nothing left to wait for */

   /* The LATE LIVE presenter: the wait expired, and the device is alive. The value is still the
    * presenter's to signal, and a host signal would call a frame presented that never was. Refused
    * whatever the semaphore reads.
    */
   CHECK(wsi_win32_route_retire_action(live, owed, 2, 1, &value) == WSI_WIN32_RETIRE_REFUSE);
   CHECK(wsi_win32_route_retire_action(live, owed, 2, 0, &value) == WSI_WIN32_RETIRE_REFUSE);
   /* And the UNPROVEN presenter, which is not a synonym for either: also refused. */
   CHECK(wsi_win32_route_retire_action(unproven, owed, 2, 1, &value) == WSI_WIN32_RETIRE_REFUSE);
   CHECK(wsi_win32_route_retire_action(unproven, owed, 2, 0, &value) == WSI_WIN32_RETIRE_REFUSE);

   /* Signalled already: the same image asks for nothing a second time, because vkSignalSemaphore may
    * only raise a timeline semaphore and 2 -> 2 is not a raise. True for every presenter state: an
    * image with nothing outstanding is nothing outstanding.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 2, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(value == 0);
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 3, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(live, owed, 2, 2, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(unproven, owed, 2, 9, &value) == WSI_WIN32_RETIRE_NOTHING);

   /* An image that was never presented has timeline value 0 and no wait of ours behind it. */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, owed, 0, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(value == 0);
   CHECK(wsi_win32_route_retire_action(removed, owed, 0, 7, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_wait_value(0, 0) == 0);

   /* Both images of the lab's two-image chain, with the readings arm A2f printed (want 2 on image 0
    * and a queued present of image 1 at value 2), the presenter proved removed, and the application's
    * own signals completed: every image with an outstanding value is released, not only the one
    * whose acquire expired.
    */
   const uint64_t want[2] = {2, 2};
   uint64_t have[2] = {1, 1};
   unsigned released = 0, refused = 0;
   for (unsigned i = 0; i < 2; i++) {
      switch (wsi_win32_route_retire_action(removed, owed, want[i], have[i], &value)) {
      case WSI_WIN32_RETIRE_SIGNAL: have[i] = value; released++; break;
      case WSI_WIN32_RETIRE_REFUSE: refused++; break;
      default: break;
      }
   }
   CHECK(released == 2 && refused == 0 && have[0] == 2 && have[1] == 2);
   /* And the chain's teardown, which runs the same release again, finds nothing left to do. */
   for (unsigned i = 0; i < 2; i++)
      CHECK(wsi_win32_route_retire_action(removed, owed, want[i], have[i], &value) ==
            WSI_WIN32_RETIRE_NOTHING);

   /* The same chain with a LIVE presenter: nothing is released and both images are reported
    * outstanding, which is what the caller turns into an answer instead of a forced signal.
    */
   uint64_t live_have[2] = {1, 1};
   released = 0; refused = 0;
   for (unsigned i = 0; i < 2; i++) {
      switch (wsi_win32_route_retire_action(live, owed, want[i], live_have[i], &value)) {
      case WSI_WIN32_RETIRE_SIGNAL: live_have[i] = value; released++; break;
      case WSI_WIN32_RETIRE_REFUSE: refused++; break;
      default: break;
      }
   }
   CHECK(released == 0 && refused == 2 && live_have[0] == 1 && live_have[1] == 1);

   /* The deadline rules are unchanged by the release: a route a wait retired stays retired, and the
    * release is not a proof of anything. An expiry releases the waits and keeps the route dead.
    */
   struct wsi_win32_route_state dead = {0};
   CHECK(wsi_win32_route_wait_expired(&dead));
   CHECK(!wsi_win32_route_usable(&dead));
   CHECK(wsi_win32_route_retire_action(removed, owed, 2, 1, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(!wsi_win32_route_note_acquired(&dead, 2, 2));
   CHECK(!wsi_win32_route_presented(&dead) && !wsi_win32_route_usable(&dead));

   /* A live route must not be released from the CPU: there the value is the presenter's to signal,
    * and the rule is only ever reached on a path that has already given the route up. The rule
    * itself still answers for the state, which is what the caller's gate rests on.
    */
   struct wsi_win32_route_state alive = {WSI_WIN32_ROUTE_PRESENTED};
   CHECK(wsi_win32_route_usable(&alive));

   CHECK(!strcmp(wsi_win32_retire_action_name(WSI_WIN32_RETIRE_SIGNAL), "signal"));
   CHECK(!strcmp(wsi_win32_retire_action_name(WSI_WIN32_RETIRE_REFUSE), "refuse"));
   CHECK(!strcmp(wsi_win32_retire_action_name(WSI_WIN32_RETIRE_NOTHING), "nothing"));
}

/* V1, the third half, which the review of round 4b sent back: the value an image carries is the
 * APPLICATION's own pending signal whenever wsi_dxgi_blit returned a failure, and no arithmetic on
 * the timeline tells it from the presenter's value. Every case here has an image_present_value that
 * the presenter never promised, and the old rule - which asked only whether the semaphore read one
 * below it - answered SIGNAL for the first of them.
 *
 * The second review of the same round sent back the FORM of the extra input: a bit saying "the
 * presenter owes one more value" is a fact about a value it does not name, and the timeline entry it
 * is read against moves without it. The last part of this case is that reading - a debt recorded for
 * W read against an entry of W+1 - and it must answer NOTHING.
 */
static void
test_retire_debt(void)
{
   uint64_t value = 0;
   const enum wsi_win32_presenter_state removed = WSI_WIN32_PRESENTER_REMOVED;
   const enum wsi_win32_presenter_state live = WSI_WIN32_PRESENTER_LIVE;
   const enum wsi_win32_presenter_state unproven = WSI_WIN32_PRESENTER_UNPROVEN;

   /* THE FAILURE SCENARIO of the review, on the two-image chain the lab measured. Image 0 is
    * presented; the application's own submission signalling V = 1 has not retired, so the semaphore
    * reads 0; wsi_dxgi_blit fails (a refused queue Wait, a device removed by ExecuteCommandLists or
    * a refused Signal) and returns VK_ERROR_DEVICE_LOST, so timeline_values[0] keeps 1 - the
    * application's value, not the presenter's 2. A later acquire expires, the route is retired and
    * the presenter reads REMOVED, exactly as round 3 measured. The reading is then "present value 1,
    * semaphore 0", which is 'one below' and which round 4b would have host-signalled to 1 while the
    * application's own signal of 1 was pending: VUID-VkSemaphoreSignalInfo-value-03259 requires the
    * host value to be LESS than any pending signal, and 1 < 1 is false.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, false, 1, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(value == 0); /* nothing is handed back, so nothing is signalled */
   /* And with the debt - the same numbers for a cycle whose Signal WAS accepted (V = 0 can never
    * happen; this is the shape, kept apart from the case above only by the debt).
    */
   CHECK(wsi_win32_route_retire_action(removed, true, 1, 0, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 1);

   /* The same state after one good cycle, which is the second reading the review names: V = 3 with
    * the semaphore at 2. Without the debt it is the application's pending 3 and must not be passed;
    * with it, it is the presenter's.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, false, 3, 2, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(value == 0);
   CHECK(wsi_win32_route_retire_action(removed, true, 3, 2, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 3);

   /* NOTHING and not REFUSE, for a reason read from wsi_common.c:2831-2832 and 2887-2888: a blit
    * that does not return VK_SUCCESS skips the second submission and the present, so the only
    * submission behind the image is the application's own first one, which waits on the
    * application's own semaphores on a device that is alive. Reporting that as outstanding would end
    * the client with VK_ERROR_DEVICE_LOST over a wait that does not exist, so the report for every
    * presenter state here is out-of-date and the client falls back to CPU images.
    */
   CHECK(wsi_win32_route_retire_action(live, false, 1, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(unproven, false, 1, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(removed, false, 2, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(removed, false, 9, 4, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_report(wsi_win32_route_retire_action(removed, false, 1, 0, &value) ==
                                WSI_WIN32_RETIRE_REFUSE) == WSI_WIN32_REPORT_OUT_OF_DATE);
   /* An image with nothing outstanding stays nothing outstanding whatever the debt says, and so does
    * an image that was never presented: the debt is a precondition of a signal, not a trigger.
    */
   CHECK(wsi_win32_route_retire_action(removed, true, 2, 2, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(removed, false, 2, 2, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(removed, true, 0, 0, &value) == WSI_WIN32_RETIRE_NOTHING);
   CHECK(wsi_win32_route_retire_action(removed, false, 0, 0, &value) == WSI_WIN32_RETIRE_NOTHING);

   /* The two-image chain of the failure scenario, played through as the retirement loop runs it:
    * image 0's blit failed (debt false, the application's own value 1 outstanding on our device) and
    * image 1 completed a cycle whose presenter value is 2. Only image 1 is released, and nothing is
    * reported outstanding, so the acquire answers VK_ERROR_OUT_OF_DATE_KHR and the client recreates.
    */
   const uint64_t want[2] = {1, 2};
   const bool debt[2] = {false, true};
   uint64_t have[2] = {0, 1};
   unsigned released = 0, refused = 0, nothing = 0;
   for (unsigned i = 0; i < 2; i++) {
      switch (wsi_win32_route_retire_action(removed, debt[i], want[i], have[i], &value)) {
      case WSI_WIN32_RETIRE_SIGNAL: have[i] = value; released++; break;
      case WSI_WIN32_RETIRE_REFUSE: refused++; break;
      default: nothing++; break;
      }
   }
   CHECK(released == 1 && refused == 0 && nothing == 1);
   CHECK(have[0] == 0 && have[1] == 2); /* image 0's timeline is left to the application */
   CHECK(wsi_win32_route_report(refused != 0) == WSI_WIN32_REPORT_OUT_OF_DATE);

   /* The record the production path carries this fact in: one per image, zero-initialised with the
    * chain (vk_zalloc), set only where the presenter's accepted Signal raised the value, cleared
    * when the blit is entered again for that image. It carries the VALUE it was recorded against,
    * and every question asked of it names the value the timeline entry holds now.
    */
   struct wsi_win32_image_debt word = {0};
   CHECK(!wsi_win32_image_debt_owed(&word, 0)); /* a zeroed image owes nothing, for any value */
   CHECK(!wsi_win32_image_debt_owed(&word, 2));
   wsi_win32_image_debt_note_signalled(&word, 2);
   CHECK(wsi_win32_image_debt_owed(&word, 2));
   wsi_win32_image_debt_note_signalled(&word, 2); /* idempotent: one accepted Signal per cycle */
   CHECK(wsi_win32_image_debt_owed(&word, 2));

   /* THE READING THE REVIEW OF ROUND 4b SENT BACK, and the reason the record is not a bit. The
    * timeline entry moves without the debt: wsi_common.c:2768 pre-increments it to W+1 for the
    * application's next submission, and the clear of the debt lives inside wsi_dxgi_blit, which
    * wsi_common.c:2820-2821 skips when that submission fails. A debt recorded for 2 is then read
    * against an entry of 3 - and a bit would have answered "owed", which is a host signal of a
    * value nobody promised.
    */
   CHECK(!wsi_win32_image_debt_owed(&word, 3));
   CHECK(!wsi_win32_image_debt_owed(&word, 1));
   CHECK(!wsi_win32_image_debt_owed(&word, 0));
   /* The same through the rule: want = 3, have = 2, presenter REMOVED is the exact SIGNAL shape,
    * and it answers NOTHING because the 3 is nobody's promise.
    */
   value = 0;
   CHECK(wsi_win32_route_retire_action(removed, wsi_win32_image_debt_owed(&word, 3), 3, 2, &value) ==
         WSI_WIN32_RETIRE_NOTHING);
   CHECK(value == 0); /* nothing handed back, so nothing signalled */
   /* And the same numbers once the next cycle's Signal really was accepted for 3. */
   wsi_win32_image_debt_note_signalled(&word, 3);
   CHECK(wsi_win32_route_retire_action(removed, wsi_win32_image_debt_owed(&word, 3), 3, 2, &value) ==
         WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 3);
   wsi_win32_image_debt_reset(&word);
   CHECK(!wsi_win32_image_debt_owed(&word, 3));
   CHECK(!wsi_win32_image_debt_owed(&word, 0));
   wsi_win32_image_debt_reset(&word); /* idempotent */
   CHECK(!wsi_win32_image_debt_owed(&word, 3));

   /* The order the blit writes them in, as the state a reader may see: the value first and the debt
    * after it, so a reader that sees the debt always sees the presenter's value with it. The reverse
    * order is the defect: debt set against the application's value is the 03259 state.
    */
   uint64_t timeline = 1; /* the application's own signal of V = 1 */
   wsi_win32_image_debt_reset(&word);
   CHECK(wsi_win32_route_retire_action(removed, wsi_win32_image_debt_owed(&word, timeline), timeline,
                                       0, &value) == WSI_WIN32_RETIRE_NOTHING);
   timeline = 2; /* the presenter's Signal of V + 1 was accepted, then the value was raised */
   wsi_win32_image_debt_note_signalled(&word, timeline);
   CHECK(wsi_win32_route_retire_action(removed, wsi_win32_image_debt_owed(&word, timeline), timeline,
                                       1, &value) == WSI_WIN32_RETIRE_SIGNAL);
   CHECK(value == 2);

   /* The whole chain of the review's scenario, in the order the production path runs it: the cycle
    * above completed for 2, then the next vkQueuePresentKHR pre-increments the entry to 3 and its
    * own submit fails, so neither the blit nor the clear of the debt ever runs. A retirement on that
    * state - an acquire that expired, or the teardown - reads want = 3 against a debt for 2.
    */
   timeline = 3;
   CHECK(!wsi_win32_image_debt_owed(&word, timeline));
   CHECK(wsi_win32_route_retire_action(removed, wsi_win32_image_debt_owed(&word, timeline), timeline,
                                       2, &value) == WSI_WIN32_RETIRE_NOTHING);
   /* So nothing of ours is reported outstanding and the client recreates instead of being ended. */
   CHECK(wsi_win32_route_report(false) == WSI_WIN32_REPORT_OUT_OF_DATE);
}

/* What the route ANSWERS when it could not retire what it queued. Reporting out-of-date there is
 * what froze lab round 3: the application recreated the swapchain and its own vkDeviceWaitIdle then
 * waited for a submission nothing could retire, with no timeout in the API to end it.
 */
static void
test_route_report(void)
{
   CHECK(wsi_win32_route_report(false) == WSI_WIN32_REPORT_OUT_OF_DATE);
   CHECK(wsi_win32_route_report(true) == WSI_WIN32_REPORT_DEVICE_LOST);
   CHECK((int)WSI_WIN32_REPORT_OUT_OF_DATE == 0); /* the answer for a chain with nothing left */

   /* Played through the three presenter states of one presented image whose copy never ran. Removed
    * with the application's signal complete is the only one the host may release, so it is the only
    * one that answers out-of-date and lets the client fall back to CPU images.
    */
   const uint64_t want = 2;
   const struct {
      enum wsi_win32_presenter_state presenter;
      bool owes;
      uint64_t have;
      enum wsi_win32_route_report report;
   } cases[] = {
      {WSI_WIN32_PRESENTER_REMOVED, true, 1, WSI_WIN32_REPORT_OUT_OF_DATE},
      {WSI_WIN32_PRESENTER_REMOVED, true, 0, WSI_WIN32_REPORT_DEVICE_LOST},
      {WSI_WIN32_PRESENTER_LIVE, true, 1, WSI_WIN32_REPORT_DEVICE_LOST},
      {WSI_WIN32_PRESENTER_UNPROVEN, true, 1, WSI_WIN32_REPORT_DEVICE_LOST},
      {WSI_WIN32_PRESENTER_REMOVED, true, 2, WSI_WIN32_REPORT_OUT_OF_DATE}, /* nothing outstanding */
      /* The blit of this image failed, so the value is the application's own and the second
       * submission was never made: out-of-date, and no client ended over a wait of no one's.
       */
      {WSI_WIN32_PRESENTER_REMOVED, false, 1, WSI_WIN32_REPORT_OUT_OF_DATE},
      {WSI_WIN32_PRESENTER_LIVE, false, 1, WSI_WIN32_REPORT_OUT_OF_DATE},
      {WSI_WIN32_PRESENTER_UNPROVEN, false, 0, WSI_WIN32_REPORT_OUT_OF_DATE},
   };
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      uint64_t value = 0;
      const enum wsi_win32_retire_action action =
         wsi_win32_route_retire_action(cases[i].presenter, cases[i].owes, want, cases[i].have, &value);
      const bool outstanding = action == WSI_WIN32_RETIRE_REFUSE;
      CHECK(wsi_win32_route_report(outstanding) == cases[i].report);
   }
}

/* V5. The lifetime rule of the bounded queue drain, with a reference-count model of exactly the
 * objects wsi_win32_swapchain_destroy and wsi_win32_take_old_dxgi release: the back buffer, the copy
 * command list, its allocator, the D3D12 blit resource behind the imported Vulkan memory, the shared
 * blit fence and the DXGI swap chain. Round 4's flush returned void and its callers released all of
 * them whatever happened, including after a drain that timed out.
 *
 * This is a model of the caller's rule, not of the caller: what it proves is that
 * wsi_win32_flush_releases answers the three states correctly and that a release driven by it leaves
 * nothing dropped in the unproven one. That the production path really asks it is a source gate.
 */
struct model_chain {
   int back_buffer, cmd_list, cmd_alloc, blit_res, blit_fence, swap_chain, imported_memory;
   bool resized;
};

static void
model_chain_init(struct model_chain *chain)
{
   chain->back_buffer = chain->cmd_list = chain->cmd_alloc = 1;
   chain->blit_res = chain->blit_fence = chain->swap_chain = chain->imported_memory = 1;
   chain->resized = false;
}

static int
model_chain_live(const struct model_chain *chain)
{
   return chain->back_buffer + chain->cmd_list + chain->cmd_alloc + chain->blit_res +
          chain->blit_fence + chain->swap_chain + chain->imported_memory;
}

/* The teardown, gated the way the production one is. */
static void
model_destroy(struct model_chain *chain, enum wsi_win32_flush_result flushed)
{
   if (!wsi_win32_flush_releases(flushed))
      return;
   chain->back_buffer = chain->cmd_list = chain->cmd_alloc = 0;
   chain->blit_res = chain->blit_fence = chain->swap_chain = chain->imported_memory = 0;
}

/* The steal, which releases the back buffers AND resizes the swap chain under them. */
static void
model_steal(struct model_chain *chain, enum wsi_win32_flush_result flushed, bool *handed_over)
{
   *handed_over = false;
   if (!wsi_win32_flush_releases(flushed))
      return;
   chain->back_buffer = chain->cmd_list = chain->cmd_alloc = 0;
   chain->resized = true;
   *handed_over = true;
}

static void
test_flush_lifetime(void)
{
   struct model_chain chain;

   CHECK(wsi_win32_flush_releases(WSI_WIN32_FLUSH_DRAINED));
   CHECK(wsi_win32_flush_releases(WSI_WIN32_FLUSH_REMOVED));
   CHECK(!wsi_win32_flush_releases(WSI_WIN32_FLUSH_UNPROVEN));
   CHECK((int)WSI_WIN32_FLUSH_UNPROVEN == 0); /* a zeroed result releases nothing */
   CHECK(!strcmp(wsi_win32_flush_result_name(WSI_WIN32_FLUSH_DRAINED), "drained"));
   CHECK(!strcmp(wsi_win32_flush_result_name(WSI_WIN32_FLUSH_REMOVED), "removed"));
   CHECK(!strcmp(wsi_win32_flush_result_name(WSI_WIN32_FLUSH_UNPROVEN), "unproven"));

   /* The drain completed: everything submitted before the drain fence retired, so every reference
    * goes. This is the ordinary teardown and it must stay ordinary.
    */
   model_chain_init(&chain);
   CHECK(model_chain_live(&chain) == 7);
   model_destroy(&chain, WSI_WIN32_FLUSH_DRAINED);
   CHECK(model_chain_live(&chain) == 0);

   /* The presenter's device is proved removed: its queues execute nothing further, which is the
    * other releasable state (and the one assumption this round names out loud).
    */
   model_chain_init(&chain);
   model_destroy(&chain, WSI_WIN32_FLUSH_REMOVED);
   CHECK(model_chain_live(&chain) == 0);

   /* The three unproven shapes the audit named - the wait expired, the drain fence could not be
    * created, the drain Signal failed - are one state here, and in it NOTHING is dropped. Not the
    * back buffer the queue may be writing, not the command list, not the memory the imported image
    * aliases, not the fence, not the swap chain.
    */
   model_chain_init(&chain);
   model_destroy(&chain, WSI_WIN32_FLUSH_UNPROVEN);
   CHECK(model_chain_live(&chain) == 7);
   CHECK(chain.back_buffer == 1 && chain.cmd_list == 1 && chain.cmd_alloc == 1);
   CHECK(chain.blit_res == 1 && chain.blit_fence == 1 && chain.swap_chain == 1);
   CHECK(chain.imported_memory == 1);
   /* Running the teardown again changes nothing: an expiry is not a countdown to a release. */
   model_destroy(&chain, WSI_WIN32_FLUSH_UNPROVEN);
   CHECK(model_chain_live(&chain) == 7);

   /* The steal path. A proved drain hands the swap chain over and resizes it; an unproven one hands
    * nothing over and resizes nothing, so the new chain cannot take the route and the application
    * gets CPU images.
    */
   bool handed_over = true;
   model_chain_init(&chain);
   model_steal(&chain, WSI_WIN32_FLUSH_DRAINED, &handed_over);
   CHECK(handed_over && chain.resized && chain.back_buffer == 0);

   model_chain_init(&chain);
   model_steal(&chain, WSI_WIN32_FLUSH_UNPROVEN, &handed_over);
   CHECK(!handed_over && !chain.resized && chain.back_buffer == 1 && chain.cmd_list == 1);
   CHECK(model_chain_live(&chain) == 7);

   /* A route that was retired is not a drain. The flags and the flush are separate answers, and the
    * audit's finding is exactly that round 4 read the first one as the second.
    */
   struct wsi_win32_route_state retired = {0};
   CHECK(wsi_win32_route_wait_expired(&retired));
   CHECK(!wsi_win32_route_usable(&retired));
   model_chain_init(&chain);
   model_destroy(&chain, WSI_WIN32_FLUSH_UNPROVEN);
   CHECK(model_chain_live(&chain) == 7);
}

/* V6. The NT handle CreateSharedHandle makes for the swapchain's own D3D12 resource belongs to us:
 * ref/Vulkan-Docs/chapters/memory.adoc:2466-2472 says importing it transfers no ownership and the
 * application must close it. RADV keeps the payload without consuming the handle, so nothing closed
 * it, on either outcome of the import, and every swapchain recreation leaked one per image.
 *
 * The model is a handle table: open counts up, close counts down, a double close is an error, and a
 * handle still open at the end is a leak. Both outcomes of the import are driven.
 */
struct model_handles {
   int open, closed, double_closed;
};

static int
model_handle_create(struct model_handles *table)
{
   table->open++;
   return table->open;  /* a non-zero handle */
}

static void
model_handle_close(struct model_handles *table, int handle)
{
   if (!handle)
      return;
   if (table->closed >= table->open)
      table->double_closed++;
   table->closed++;
}

/* The import, with the close placed where the production path places it: after AllocateMemory, on
 * both outcomes, and once.
 */
static bool
model_import(struct model_handles *table, bool allocate_succeeds)
{
   int handle = model_handle_create(table);
   const bool imported = allocate_succeeds;
   model_handle_close(table, handle);
   handle = 0;
   return imported;
}

static void
test_handle_ownership(void)
{
   struct model_handles table = {0};

   /* A successful import: the memory keeps the resource alive through its own reference, so the
    * handle is closed and nothing is lost.
    */
   CHECK(model_import(&table, true));
   CHECK(table.open == 1 && table.closed == 1 && table.double_closed == 0);

   /* A failed import: there is nothing to keep the handle for, and round 4 returned
    * AllocateMemory's result straight out of the function with the handle still open.
    */
   CHECK(!model_import(&table, false));
   CHECK(table.open == 2 && table.closed == 2 && table.double_closed == 0);

   /* Four images of two swapchain generations, the shape a resizing game walks: no handle survives
    * its import, whichever way the import went.
    */
   struct model_handles chain_handles = {0};
   for (unsigned generation = 0; generation < 2; generation++)
      for (unsigned image = 0; image < 2; image++)
         (void)model_import(&chain_handles, generation == 0);
   CHECK(chain_handles.open == 4 && chain_handles.closed == 4);
   CHECK(chain_handles.open - chain_handles.closed == 0);
   CHECK(chain_handles.double_closed == 0);

   /* The exported SEMAPHORE handle is a different one, closed where it is opened, and the model says
    * so: closing a handle that was never created must not count as a close of ours.
    */
   struct model_handles semaphore_handles = {0};
   const int exported = model_handle_create(&semaphore_handles);
   model_handle_close(&semaphore_handles, exported);
   model_handle_close(&semaphore_handles, 0);  /* no handle: nothing happens */
   CHECK(semaphore_handles.open == 1 && semaphore_handles.closed == 1);
   CHECK(semaphore_handles.double_closed == 0);
}

/* V2. The first D3D12 call of the route that failed, kept once and logged once. Round 4 dropped the
 * HRESULT of Close, of the queue's Wait and of the queue's Signal, returned VK_SUCCESS, and left the
 * first diagnostic to a timeout two seconds later that could name no call at all.
 */
static void
test_route_error(void)
{
   struct wsi_win32_route_error error = {0};

   CHECK(!wsi_win32_route_error_taken(&error));
   /* The first failure is recorded, and its recorder is the one caller that logs it. */
   CHECK(wsi_win32_route_note_error(&error, "ID3D12CommandQueue::Wait", 0x887A0005u, 0x887A0005u, 1));
   CHECK(wsi_win32_route_error_taken(&error));
   CHECK(!strcmp(error.call, "ID3D12CommandQueue::Wait"));
   CHECK(error.hr == 0x887A0005u && error.removed_reason == 0x887A0005u && error.image == 1);

   /* The cascade after it writes nothing: the later refusals are consequences of the first, and the
    * first is the one that answers the question.
    */
   CHECK(!wsi_win32_route_note_error(&error, "IDXGISwapChain3::Present1", 0x887A0006u, 0, 0));
   CHECK(!wsi_win32_route_note_error(&error, "ID3D12CommandQueue::Signal", 0x80004005u, 0, 1));
   CHECK(!strcmp(error.call, "ID3D12CommandQueue::Wait"));
   CHECK(error.hr == 0x887A0005u && error.image == 1);

   /* A call with no image of its own says so, and it says it with a value no image index can be. */
   struct wsi_win32_route_error create = {0};
   CHECK(wsi_win32_route_note_error(&create, "ID3D12GraphicsCommandList::Close", 0x80070057u, 0,
                                    WSI_WIN32_ROUTE_ERROR_NO_IMAGE));
   CHECK(create.image == WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
   CHECK(create.image > 16); /* no swapchain of this route has that many images */
   CHECK(create.removed_reason == 0); /* the reason could not be read: 0 is not "alive" here */

   /* A zeroed ledger is an empty one, which is what vk_zalloc gives the instance. */
   struct wsi_win32_route_error fresh = {0};
   CHECK(!wsi_win32_route_error_taken(&fresh));
   CHECK(fresh.taken == 0);
}

/* V3. The route flags from two threads at once, on two independent swapchains, which is the case the
 * audit found missing and the reason the flags are one atomic word now. Round 4 had two plain bools
 * with a comment that called the race benign.
 *
 * What is driven: many threads, each acting for a swapchain of its own, racing note_acquired against
 * wait_expired on the SAME instance state. Two invariants must hold however the interleaving falls.
 *   1. note_acquired returns true to AT MOST ONE caller, ever. It is what writes the "deadlines off"
 *      line, and two of those lines would mean two threads lifted the same deadline.
 *   2. PRESENTED and DEAD are never both set. They are read together - the expiry rule asks about
 *      PRESENTED before it sets DEAD - and that is exactly the dependency the old comment said to
 *      make atomic before anything relied on it.
 */
#define MODEL_THREADS 8
#define MODEL_ROUNDS 2000

struct concurrent_case {
   struct wsi_win32_route_state state;
   uint32_t claimed;     /* the next thread identity, claimed atomically */
   uint32_t lifted;      /* how many threads got true from note_acquired */
   uint32_t both_flags;  /* how many times both flags were seen set */
   uint32_t done;        /* threads that finished */
   uint32_t mix;         /* which thread acquires and which one expires */
};

/* An atomic increment out of the two operations the header offers, so the counters of this case are
 * not themselves a data race.
 */
static uint32_t
model_atomic_inc(uint32_t *word)
{
   for (;;) {
      const uint32_t have = wsi_win32_atomic_load32(word);
      if (wsi_win32_atomic_cas32(word, have, have + 1) == have)
         return have;
   }
}

static void
concurrent_worker(void *arg)
{
   struct concurrent_case *c = (struct concurrent_case *)arg;
   const uint32_t id = model_atomic_inc(&c->claimed);
   for (uint32_t round = 0; round < MODEL_ROUNDS; round++) {
      /* Each thread drives its own swapchain's values: the acquired image's present value and the
       * fence reading that proves that present completed.
       */
      if ((id + round) % c->mix == 0) {
         if (wsi_win32_route_note_acquired(&c->state, 2, 2))
            (void)model_atomic_inc(&c->lifted);
      } else {
         (void)wsi_win32_route_wait_expired(&c->state);
      }
      const uint32_t flags = wsi_win32_route_flags(&c->state);
      if ((flags & WSI_WIN32_ROUTE_PRESENTED) && (flags & WSI_WIN32_ROUTE_DEAD))
         (void)model_atomic_inc(&c->both_flags);
   }
   (void)model_atomic_inc(&c->done);
}

static void
test_route_flags_concurrent(void)
{
   /* Two mixes, so both orders are driven: one where the acquire usually wins and one where the
    * expiry usually does.
    */
   for (uint32_t mix = 2; mix <= 3; mix++) {
      struct concurrent_case c = {{0}, 0, 0, 0, 0, mix};
      unsigned started = 0;
      for (unsigned t = 0; t < MODEL_THREADS; t++)
         if (_beginthread(concurrent_worker, 0, &c) != (uintptr_t)-1)
            started++;
      CHECK(started > 0);
      /* Joined on the state this header already provides: the worker counts itself out atomically,
       * so no Windows wait object is needed to see them all finish.
       */
      while (wsi_win32_atomic_load32(&c.done) < started) {
         /* spin: the workers do a fixed number of rounds and then count themselves out */
      }
      const uint32_t flags = wsi_win32_route_flags(&c.state);
      /* Exactly one of the two transitions won, and the loser never happened. */
      CHECK((flags & (WSI_WIN32_ROUTE_PRESENTED | WSI_WIN32_ROUTE_DEAD)) != 0);
      CHECK((flags & WSI_WIN32_ROUTE_PRESENTED) == 0 || (flags & WSI_WIN32_ROUTE_DEAD) == 0);
      CHECK(c.both_flags == 0);
      /* The "deadlines off" line is written at most once in the life of the process. */
      CHECK(c.lifted <= 1);
      CHECK(c.lifted == ((flags & WSI_WIN32_ROUTE_PRESENTED) ? 1u : 0u));
      /* And the derived answers agree with the word. */
      CHECK(wsi_win32_route_presented(&c.state) ==
            ((flags & WSI_WIN32_ROUTE_PRESENTED) != 0));
      CHECK(wsi_win32_route_usable(&c.state) == ((flags & WSI_WIN32_ROUTE_DEAD) == 0));
   }

   /* The two accessors are atomic operations and nothing else, which is what the LLVM memory model
    * asks for (Atomics.rst:148-165). A compare-and-swap that does not match leaves the word alone
    * and answers what was there.
    */
   uint32_t word = 5;
   CHECK(wsi_win32_atomic_load32(&word) == 5);
   CHECK(wsi_win32_atomic_cas32(&word, 4, 9) == 5 && word == 5);
   CHECK(wsi_win32_atomic_cas32(&word, 5, 9) == 5 && word == 9);
   CHECK(wsi_win32_atomic_load32(&word) == 9);
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
   struct wsi_win32_route_state route = {0};
   struct wsi_win32_route_state healthy = {WSI_WIN32_ROUTE_PRESENTED};
   const uint64_t forever = UINT64_MAX;

   CHECK(wsi_win32_route_usable(&route)); /* a fresh instance may take the route */
   CHECK(wsi_win32_acquire_timeout_ns(forever, wsi_win32_route_presented(&route)) ==
         WSI_WIN32_ROUTE_DEADLINE_NS);
   CHECK(wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&route)));
   /* The wait expired before any present: the route is retired for the process, and the swapchain
    * is reported out of date so that the application creates the next one on CPU images.
    */
   CHECK(wsi_win32_route_wait_expired(&route));
   CHECK(!wsi_win32_route_usable(&route));
   /* It stays retired; nothing re-arms it inside the process. */
   CHECK(wsi_win32_route_wait_expired(&route));
   CHECK(!wsi_win32_route_usable(&route));

   /* A route that has shown a frame keeps the application's own timeout, and one late wait after
    * that is a slow frame, not a broken route.
    */
   CHECK(wsi_win32_acquire_timeout_ns(forever, wsi_win32_route_presented(&healthy)) == forever);
   CHECK(!wsi_win32_acquire_timeout_capped(forever, wsi_win32_route_presented(&healthy)));
   CHECK(!wsi_win32_route_wait_expired(&healthy));
   CHECK(wsi_win32_route_usable(&healthy));
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
   {"presenter_state", test_presenter_state},
   {"retire_action", test_retire_action},
   {"retire_debt", test_retire_debt},
   {"route_report", test_route_report},
   {"flush_lifetime", test_flush_lifetime},
   {"handle_ownership", test_handle_ownership},
   {"route_error", test_route_error},
   {"route_flags_concurrent", test_route_flags_concurrent},
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
