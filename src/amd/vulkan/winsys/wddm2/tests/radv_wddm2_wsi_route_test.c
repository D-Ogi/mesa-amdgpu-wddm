/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_wsi_route.h: the present route switch, the module gate and the LB7A
 * checks of the shared-resource import. No GPU, no Vulkan, no Windows header. Run through
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

   /* Nothing set: the default, which stays GDI until the lab passes the DXGI route. */
   c = radv_wddm2_wsi_route_choose(NULL, NULL);
   CHECK(c.route == RADV_WDDM2_WSI_ROUTE_DEFAULT && c.source == RADV_WDDM2_WSI_SOURCE_DEFAULT &&
         !c.invalid);
   CHECK(RADV_WDDM2_WSI_ROUTE_DEFAULT == RADV_WDDM2_WSI_ROUTE_GDI);

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

static void
test_module_gate(void)
{
   const wchar_t *sys = L"C:\\Windows\\System32";
   CHECK(radv_wddm2_wsi_module_gate(sys, NULL, NULL, NULL) == RADV_WDDM2_WSI_GATE_OK);
   CHECK(radv_wddm2_wsi_module_gate(sys, L"C:\\Windows\\System32\\dxgi.dll",
                                    L"C:\\Windows\\System32\\d3d12.dll",
                                    L"C:\\Windows\\System32\\D3D12Core.dll") ==
         RADV_WDDM2_WSI_GATE_OK);
   /* DXVK next to the game (facts M792). */
   CHECK(radv_wddm2_wsi_module_gate(sys, L"D:\\Games\\Foo\\dxgi.dll", NULL, NULL) ==
         RADV_WDDM2_WSI_GATE_FOREIGN_DXGI);
   /* vkd3d-proton next to the game (facts M794). */
   CHECK(radv_wddm2_wsi_module_gate(sys, L"C:\\Windows\\System32\\dxgi.dll",
                                    L"D:\\Games\\Foo\\d3d12.dll", NULL) ==
         RADV_WDDM2_WSI_GATE_FOREIGN_D3D12);
   CHECK(radv_wddm2_wsi_module_gate(sys, NULL, NULL, L"D:\\Games\\Foo\\d3d12core.dll") ==
         RADV_WDDM2_WSI_GATE_FOREIGN_D3D12CORE);
   CHECK(radv_wddm2_wsi_module_gate(L"", NULL, NULL, NULL) == RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR);
   CHECK(radv_wddm2_wsi_module_gate(NULL, NULL, NULL, NULL) == RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR);
   CHECK(!strcmp(radv_wddm2_wsi_gate_name(RADV_WDDM2_WSI_GATE_FOREIGN_DXGI), "foreign-dxgi"));
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
   {"module_gate", test_module_gate},
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
