/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_mem_precheck.h (C70, BD-096): the threshold that refuses a device-local
 * allocation which no eviction can satisfy, and the switch that turns it off. No GPU, no Vulkan, no
 * Windows header.
 *
 * Usage: radv_wddm2_mem_precheck_test [--negative-control]
 * The negative control runs every case with each expectation inverted. It must fail every case.
 */
#include "../radv_wddm2_mem_precheck.h"

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

#define MIB (1024ull * 1024ull)

/* Unit A, 2026-10-08 (scratch/bd096/lab/results/watch-20261008T033845Z). The budget total is the
 * sum of the two published per-heap budgets, 7295.9 and 3647.9 MiB. The segment total is the local
 * segment of facts M65 (0x1FD736000 bytes, 8151.2 MiB) plus the 4008.7 MiB host-visible heap the
 * same adapter reports, so about 12159.9 MiB. Only its order matters here: the floor stays under the
 * budget arm and the ceiling stays over it. */
static const uint64_t LAB_BUDGET = 10943ull * MIB + 819ull * 1024ull; /* 10943.8 MiB */
static const uint64_t LAB_SEGMENTS = 0x1FD736000ull + 4008ull * MIB + 734ull * 1024ull;
static const uint64_t CHUNK = 256ull * MIB;

static struct radv_wddm2_mem_limits
lab_limits(void)
{
   struct radv_wddm2_mem_limits limits = {LAB_BUDGET, LAB_SEGMENTS};
   return limits;
}

static void
test_threshold(void)
{
   struct radv_wddm2_mem_limits limits = lab_limits();

   /* The threshold is the published budget plus 1/32 of it. */
   CHECK(radv_wddm2_mem_threshold(limits) == LAB_BUDGET + LAB_BUDGET / 32);
   /* On unit A that is 11285.8 MiB: over the 11264 MiB the lab admitted, under the 11397.6 MiB the
    * kernel named as the maximum, and under the 11520 MiB request that took 86 s and failed. */
   CHECK(radv_wddm2_mem_threshold(limits) / MIB == 11285);
   CHECK(radv_wddm2_mem_threshold(limits) > 11264ull * MIB);
   CHECK(radv_wddm2_mem_threshold(limits) < 11397ull * MIB);

   /* No budget, no threshold. */
   limits.budget_total = 0;
   CHECK(radv_wddm2_mem_threshold(limits) == 0);

   /* No segment total: the budget alone sets the threshold. */
   limits.budget_total = LAB_BUDGET;
   limits.segment_total = 0;
   CHECK(radv_wddm2_mem_threshold(limits) == LAB_BUDGET + LAB_BUDGET / 32);

   /* The floor of 3/4 of the two segments lifts a squeezed budget. */
   limits.budget_total = 2048ull * MIB;
   limits.segment_total = 12288ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 9216ull * MIB);

   /* The two segments together are the ceiling of the threshold. */
   limits.budget_total = 12288ull * MIB;
   limits.segment_total = 12288ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 12288ull * MIB);

   /* A segment total under the budget is not a capacity, so it is dropped and does not cap. */
   limits.budget_total = 8192ull * MIB;
   limits.segment_total = 512ull * MIB;
   CHECK(radv_wddm2_mem_usable_segment_total(limits) == 0);
   CHECK(radv_wddm2_mem_threshold(limits) == 8192ull * MIB + 256ull * MIB);
}

/* The measured case: 3584 MiB host-visible resident, then 256 MiB device-local chunks. */
static void
test_lab_default_sizes(void)
{
   const struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t host_visible = 3584ull * MIB;

   /* Every chunk the lab admitted is admitted: the last one takes the total to 11264 MiB. */
   for (uint64_t device_local = 0; device_local < 7680ull * MIB; device_local += CHUNK) {
      CHECK(radv_wddm2_mem_check(limits, true, host_visible + device_local, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
   }
   /* The chunk that took 86 s in the kernel and then failed is refused. */
   CHECK(radv_wddm2_mem_check(limits, true, host_visible + 7680ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(!strcmp(radv_wddm2_mem_verdict_name(RADV_WDDM2_MEM_REFUSE_OVER_MAX), "over-max-budget"));

   /* The host-visible phase of the same run is never refused here, at any total. */
   for (uint64_t held = 0; held < 12288ull * MIB; held += CHUNK)
      CHECK(radv_wddm2_mem_check(limits, false, held, CHUNK) == RADV_WDDM2_MEM_ADMIT);
}

/* The device-local-first case of the same probe: 9216 MiB device-local over an 8017.5 MiB heap with
 * 256 MiB host-visible. The OS fits it by paging our own memory out, so nothing may be refused. */
static void
test_lab_device_local_first(void)
{
   const struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t host_visible = 256ull * MIB;

   for (uint64_t device_local = 0; device_local < 9216ull * MIB; device_local += CHUNK) {
      CHECK(radv_wddm2_mem_check(limits, true, host_visible + device_local, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
   }
   /* The 1024 MiB device-local arm of the same script is far from the threshold. */
   CHECK(radv_wddm2_mem_check(limits, true, 3584ull * MIB + 768ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
}

static void
test_no_information(void)
{
   struct radv_wddm2_mem_limits limits = {0, 0};

   /* Without a budget nothing is refused, however large the request. */
   CHECK(radv_wddm2_mem_check(limits, true, 0, 64ull * 1024 * MIB) == RADV_WDDM2_MEM_ADMIT);
   limits.segment_total = LAB_SEGMENTS;
   CHECK(radv_wddm2_mem_check(limits, true, LAB_SEGMENTS, CHUNK) == RADV_WDDM2_MEM_ADMIT);

   /* A request larger than everything is refused once the budget is known. */
   limits = lab_limits();
   CHECK(radv_wddm2_mem_check(limits, true, 0, 64ull * 1024 * MIB) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   /* The sum of the held bytes and the request cannot wrap into an admission. */
   CHECK(radv_wddm2_mem_check(limits, true, UINT64_MAX - CHUNK + 1, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(radv_wddm2_mem_check(limits, true, UINT64_MAX, UINT64_MAX) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   /* A zero-byte request is never refused. */
   CHECK(radv_wddm2_mem_check(limits, true, LAB_BUDGET, 0) == RADV_WDDM2_MEM_ADMIT);
}

static void
test_switch(void)
{
   struct radv_wddm2_mem_precheck_switch s;

   s = radv_wddm2_mem_precheck_parse(NULL);
   CHECK(s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("");
   CHECK(s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("off");
   CHECK(!s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("OFF");
   CHECK(!s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("0");
   CHECK(!s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("On");
   CHECK(s.enabled && !s.invalid);
   s = radv_wddm2_mem_precheck_parse("1");
   CHECK(s.enabled && !s.invalid);
   /* An unknown value gives the default and says that it was not understood. */
   s = radv_wddm2_mem_precheck_parse("strict");
   CHECK(s.enabled && s.invalid);
   s = radv_wddm2_mem_precheck_parse("off ");
   CHECK(s.enabled && s.invalid);
   CHECK(!strcmp(RADV_WDDM2_MEM_PRECHECK_ENV, "AMDGPU_WDDM_VK_MEM_PRECHECK"));
   CHECK(!strcmp(radv_wddm2_mem_verdict_name(RADV_WDDM2_MEM_ADMIT), "admit"));
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"threshold", test_threshold},
   {"lab_default_sizes", test_lab_default_sizes},
   {"lab_device_local_first", test_lab_device_local_first},
   {"no_information", test_no_information},
   {"switch", test_switch},
};

int
main(int argc, char **argv)
{
   int failed_tests = 0;
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      const int before = failures;
      tests[i].fn();
      if (failures != before) {
         failed_tests++;
         printf("FAIL %s\n", tests[i].name);
      } else {
         printf("PASS %s\n", tests[i].name);
      }
   }
   printf("mem precheck: %u tests, %d checks, %d failed checks%s\n",
          (unsigned)(sizeof(tests) / sizeof(tests[0])), checks, failures,
          invert ? " (negative control)" : "");
   return failed_tests ? 1 : 0;
}
