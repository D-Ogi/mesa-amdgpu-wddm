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
