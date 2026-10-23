/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_cs_placement.h (BD-226): when the command buffers go to the local segment.
 * No GPU, no Vulkan, no Windows header.
 *
 * Usage: radv_wddm2_cs_placement_test [--negative-control]
 * The negative control runs every case with each expectation inverted; it must fail every case.
 */
#include "../radv_wddm2_cs_placement.h"

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

#define MIB (1ull << 20)
#define GIB (1ull << 30)

static void
test_reserve(void)
{
   /* The floor holds below 4 GiB of budget, one eighth above it. */
   CHECK(radv_wddm2_cs_vram_reserve(0) == 512 * MIB);
   CHECK(radv_wddm2_cs_vram_reserve(2 * GIB) == 512 * MIB);
   CHECK(radv_wddm2_cs_vram_reserve(4 * GIB) == 512 * MIB);
   CHECK(radv_wddm2_cs_vram_reserve(8 * GIB) == GIB);
}

static void
test_fits(void)
{
   /* The lab's game: budget 7.18 GiB, usage 4.82 GiB. */
   CHECK(radv_wddm2_cs_vram_fits(true, true, 7706443776ull, 5175951360ull));
   /* Exactly the reserve left fits; one byte less does not. */
   CHECK(radv_wddm2_cs_vram_fits(true, true, 8 * GIB, 7 * GIB));
   CHECK(!radv_wddm2_cs_vram_fits(true, true, 8 * GIB, 7 * GIB + 1));
   CHECK(radv_wddm2_cs_vram_fits(true, true, 2 * GIB, 2 * GIB - 512 * MIB));
   CHECK(!radv_wddm2_cs_vram_fits(true, true, 2 * GIB, 2 * GIB - 512 * MIB + 1));
   /* Usage above the budget (VidMm serves it) keeps the command buffers in GTT. */
   CHECK(!radv_wddm2_cs_vram_fits(true, true, 4 * GIB, 5 * GIB));
   /* A small budget below the reserve never fits. */
   CHECK(!radv_wddm2_cs_vram_fits(true, true, 256 * MIB, 0));
}

static void
test_refusals(void)
{
   /* Part of the VRAM invisible to the CPU: GTT, whatever the budget. */
   CHECK(!radv_wddm2_cs_vram_fits(false, true, 8 * GIB, 0));
   /* A failed query, or one that answers no budget: GTT. */
   CHECK(!radv_wddm2_cs_vram_fits(true, false, 8 * GIB, 0));
   CHECK(!radv_wddm2_cs_vram_fits(true, true, 0, 0));
}

int
main(int argc, char **argv)
{
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   test_reserve();
   test_fits();
   test_refusals();
   printf("radv_wddm2_cs_placement_test%s: checks=%d, failures=%d\n", invert ? " (negative control)" : "", checks,
          failures);
   if (invert)
      return failures == checks ? 0 : 1;
   return failures ? 1 : 0;
}
