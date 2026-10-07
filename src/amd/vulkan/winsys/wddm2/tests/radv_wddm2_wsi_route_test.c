/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_wsi_route.h: the present route switch, the report of application-local
 * runtime modules, the D3D12 implementation check and the LB7A checks of the shared-resource
 * import. No GPU, no Vulkan, no Windows header. Run through
 * bc250-win tools/build/build-radv-wsi-route-test.ps1.
 *
 * Usage: radv_wddm2_wsi_route_test [--negative-control]
 * The negative control runs every case with one expectation inverted; it must fail, which shows
 * that a broken rule is reported.
 */
#include "../radv_wddm2_wsi_route.h"

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

   /* Nothing set: the default is the DXGI route (owner decision 2026-10-07). */
   c = radv_wddm2_wsi_route_choose(NULL, NULL);
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT &&
         !c.invalid);
   CHECK(RADV_WDDM2_WSI_ROUTE_DEFAULT == RADV_WDDM2_WSI_ROUTE_DXGI);
   c = radv_wddm2_wsi_route_choose("", "");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT);

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

   /* The per-application WsiRoute (Applications\<exe>) sits between the environment and the global value. */
   c = radv_wddm2_wsi_route_choose_app(NULL, "gdi", "dxgi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_APP_REGISTRY && !c.invalid);
   c = radv_wddm2_wsi_route_choose_app("dxgi", "gdi", "gdi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_ENV);
   c = radv_wddm2_wsi_route_choose_app(NULL, "", "gdi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.source == RADV_WDDM2_WSI_SOURCE_REGISTRY);
   c = radv_wddm2_wsi_route_choose_app(NULL, NULL, NULL);
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DXGI && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT);
   c = radv_wddm2_wsi_route_choose_app(NULL, "invalid", "dxgi");
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_GDI && c.invalid && c.source == RADV_WDDM2_WSI_SOURCE_APP_REGISTRY);
   CHECK(!strcmp(radv_wddm2_wsi_route_source_name(RADV_WDDM2_WSI_SOURCE_APP_REGISTRY), "registry-app"));
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
