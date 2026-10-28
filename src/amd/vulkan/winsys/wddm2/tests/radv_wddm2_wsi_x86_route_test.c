/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_wsi_x86_route.h: the route switch of the 32-bit system Vulkan ICD, which reads the names
 * and the precedence of the x64 switch and always presents through GDI. No GPU, no Vulkan, no Windows header.
 *
 * Usage: radv_wddm2_wsi_x86_route_test [--negative-control]
 * The negative control runs every case with each expectation inverted; it must fail every case.
 */
#include "../radv_wddm2_wsi_x86_route.h"

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

static bool
is(struct radv_wddm2_wsi_x86_route r, enum radv_wddm2_wsi_x86_asked asked, const char *source, bool invalid,
   const char *reason)
{
   return r.asked == asked && !strcmp(r.source, source) && r.invalid == invalid && !strcmp(r.reason, reason);
}

/* Nothing set: the x64 default (dxgi) is what was asked, and this ICD has no such route. */
static void
test_default(void)
{
   CHECK(is(radv_wddm2_wsi_x86_route_choose(NULL, NULL), RADV_WDDM2_WSI_X86_ASKED_DXGI, "default", false,
            "x86-gdi-only"));
   CHECK(is(radv_wddm2_wsi_x86_route_choose("", ""), RADV_WDDM2_WSI_X86_ASKED_DXGI, "default", false,
            "x86-gdi-only"));
}

/* The environment wins over the registry; the names do not depend on case. */
static void
test_precedence(void)
{
   CHECK(is(radv_wddm2_wsi_x86_route_choose("gdi", "dxgi"), RADV_WDDM2_WSI_X86_ASKED_GDI, "env", false, "asked"));
   CHECK(is(radv_wddm2_wsi_x86_route_choose("DXGI", "gdi"), RADV_WDDM2_WSI_X86_ASKED_DXGI, "env", false,
            "x86-gdi-only"));
   CHECK(is(radv_wddm2_wsi_x86_route_choose(NULL, "Gdi"), RADV_WDDM2_WSI_X86_ASKED_GDI, "registry", false, "asked"));
   CHECK(is(radv_wddm2_wsi_x86_route_choose("", "dxgi-composition"), RADV_WDDM2_WSI_X86_ASKED_DXGI_COMPOSITION,
            "registry", false, "x86-gdi-only"));
}

/* A value that is not a route, and a registry value that is not a short REG_SZ, are invalid. */
static void
test_invalid(void)
{
   CHECK(is(radv_wddm2_wsi_x86_route_choose("dxgi ", NULL), RADV_WDDM2_WSI_X86_ASKED_GDI, "env", true,
            "invalid-value"));
   CHECK(is(radv_wddm2_wsi_x86_route_choose(NULL, "invalid"), RADV_WDDM2_WSI_X86_ASKED_GDI, "registry", true,
            "invalid-value"));
}

static void
test_names(void)
{
   CHECK(!strcmp(radv_wddm2_wsi_x86_asked_name(RADV_WDDM2_WSI_X86_ASKED_GDI), "gdi"));
   CHECK(!strcmp(radv_wddm2_wsi_x86_asked_name(RADV_WDDM2_WSI_X86_ASKED_DXGI), "dxgi"));
   CHECK(!strcmp(radv_wddm2_wsi_x86_asked_name(RADV_WDDM2_WSI_X86_ASKED_DXGI_COMPOSITION), "dxgi-composition"));
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"default", test_default},
   {"precedence", test_precedence},
   {"invalid", test_invalid},
   {"names", test_names},
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
   printf("wsi x86 route: %u tests, %d checks, %d failed checks%s\n", (unsigned)(sizeof(tests) / sizeof(tests[0])),
          checks, failures, invert ? " (negative control)" : "");
   return failed_tests ? 1 : 0;
}
