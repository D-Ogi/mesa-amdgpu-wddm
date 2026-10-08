/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_mem_precheck.h (C70, BD-096): the rule that refuses a device-local
 * allocation which no eviction can satisfy, and the switch that turns it off. No GPU, no Vulkan,
 * no Windows header.
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

/* Unit A, 2026-10-08 (scratch/bd096/lab/results/watch-20261008T033845Z). The run measured the sum of
 * the two segment group budgets, 10943.8 MiB. It did NOT measure how the OS splits that sum: the
 * 7295.9 and 3647.9 MiB the probe prints are RADV's 2 : 1 redistribution of the sum over the APU
 * fake heaps. So the tests below use that sum and also prove, in test_split_does_not_matter, that no
 * verdict moves when the split changes. The local segment is the one of facts M65 (0x1FD736000
 * bytes, 8151.2 MiB) and the host-visible heap the same adapter reports is 4008.7 MiB, so the two
 * segments together are about 12159.9 MiB. That last number is an estimate, and only its order
 * matters: the floor it sets, 9120 MiB, is far below the threshold the budget sets, so it does not
 * bind, and the segment total no longer caps the threshold. The ICD reads the real number and prints
 * it in its first line. */
static const uint64_t LAB_BUDGET_TOTAL = 10943ull * MIB + 819ull * 1024ull; /* 10943.799 MiB */
static const uint64_t LAB_SEGMENT_LOCAL = 0x1FD736000ull;                   /* 8151.2 MiB */
static const uint64_t LAB_SEGMENTS = 0x1FD736000ull + 4008ull * MIB + 734ull * 1024ull;
static const uint64_t CHUNK = 256ull * MIB;

/* The lab split, 2 : 1. Only the sum matters, and test_split_does_not_matter holds it to that. */
static struct radv_wddm2_mem_limits
lab_limits(void)
{
   struct radv_wddm2_mem_limits limits = {LAB_BUDGET_TOTAL - LAB_BUDGET_TOTAL / 3,
                                          LAB_BUDGET_TOTAL / 3, LAB_SEGMENT_LOCAL, LAB_SEGMENTS};
   return limits;
}

/* The same total, split any way the OS likes. */
static struct radv_wddm2_mem_limits
split_limits(uint64_t local)
{
   struct radv_wddm2_mem_limits limits = {local, LAB_BUDGET_TOTAL - local, LAB_SEGMENT_LOCAL,
                                          LAB_SEGMENTS};
   return limits;
}

static void
test_threshold(void)
{
   struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t budget = LAB_BUDGET_TOTAL;

   /* The two reads add up to the total of that run, 10943.8 MiB. */
   CHECK(radv_wddm2_mem_budget_total(limits) == budget);
   CHECK(radv_wddm2_mem_budget_total(limits) / MIB == 10943);

   /* The threshold is the published total plus 1/32 of it. */
   CHECK(radv_wddm2_mem_threshold(limits) == budget + budget / 32);
   /* On unit A that is 11285.8 MiB: over the 11264 MiB the lab admitted, and under the 11520 MiB
    * request that took 86 s and failed. */
   CHECK(radv_wddm2_mem_threshold(limits) / MIB == 11285);
   CHECK(radv_wddm2_mem_threshold(limits) > 11264ull * MIB);
   CHECK(radv_wddm2_mem_threshold(limits) < 11520ull * MIB);

   /* One budget missing is no budget: the OS did not answer, so there is no threshold. */
   limits.budget_local = 0;
   CHECK(radv_wddm2_mem_budget_total(limits) == 0);
   CHECK(radv_wddm2_mem_threshold(limits) == 0);
   limits = lab_limits();
   limits.budget_nonlocal = 0;
   CHECK(radv_wddm2_mem_threshold(limits) == 0);

   /* No segment total: the budget alone sets the threshold. */
   limits = lab_limits();
   limits.segment_total = 0;
   CHECK(radv_wddm2_mem_threshold(limits) == budget + budget / 32);

   /* The floor of 3/4 of the two segments lifts a squeezed budget. */
   limits.budget_local = 1024ull * MIB;
   limits.budget_nonlocal = 1024ull * MIB;
   limits.segment_total = 12288ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 9216ull * MIB);

   /* The segment total never caps the threshold. A part whose segments sit just above its budget
    * total keeps the measured overshoot: a cap there would refuse inside it. */
   limits.budget_local = 8192ull * MIB;
   limits.budget_nonlocal = 4096ull * MIB;
   limits.segment_total = 12288ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 12288ull * MIB + 384ull * MIB);
   limits.segment_total = 12289ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 12288ull * MIB + 384ull * MIB);
   CHECK(radv_wddm2_mem_check(limits, true, 12288ull * MIB, 0, CHUNK) == RADV_WDDM2_MEM_ADMIT);

   /* A segment total under the budget is not a capacity, so it is dropped and does not cap. */
   limits.budget_local = 7168ull * MIB;
   limits.budget_nonlocal = 1024ull * MIB;
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
   for (uint64_t device_local = 0; device_local < 7680ull * MIB; device_local += CHUNK)
      CHECK(radv_wddm2_mem_check(limits, true, device_local, host_visible, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);

   /* The chunk that took 86 s in the kernel and then failed is refused. */
   CHECK(radv_wddm2_mem_check(limits, true, 7680ull * MIB, host_visible, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(!strcmp(radv_wddm2_mem_verdict_name(RADV_WDDM2_MEM_REFUSE_OVER_MAX), "over-max-budget"));

   /* The host-visible phase of the same run is never refused here, at any total. */
   for (uint64_t held = 0; held < 12288ull * MIB; held += CHUNK)
      CHECK(radv_wddm2_mem_check(limits, false, held, host_visible, CHUNK) == RADV_WDDM2_MEM_ADMIT);
}

/* The device-local-first case of the same probe: 9216 MiB device-local over an 8017.5 MiB heap with
 * 256 MiB host-visible. The OS fits it by paging our own memory out, so nothing may be refused. */
static void
test_lab_device_local_first(void)
{
   const struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t host_visible = 256ull * MIB;

   for (uint64_t device_local = 0; device_local < 9216ull * MIB; device_local += CHUNK)
      CHECK(radv_wddm2_mem_check(limits, true, device_local, host_visible, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
   /* The 1024 MiB device-local arm of the same script is far from the rule. */
   CHECK(radv_wddm2_mem_check(limits, true, 768ull * MIB, 3584ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
}

/* The lab measured the sum of the two segment group budgets, never the split. So the split may not
 * change one verdict. This case walks every split of the measured total in 64 MiB steps and holds
 * the threshold and every verdict equal to the verdict of the 2 : 1 split, over a grid of held bytes
 * that covers the three measured probe arms and the band just under the threshold.
 *
 * The grid is chosen to catch a rule that reads one budget alone. The dropped second rule of C70
 * (review of 2026-10-08) refused held_local 6912 MiB plus held_nonlocal 4096 MiB plus a 256 MiB
 * request, which is 11264 MiB in all and exactly what the lab served, as soon as the split put
 * budget_local near 6656 MiB. The split sweep below passes through that value. */
static void
test_split_does_not_matter(void)
{
   const struct radv_wddm2_mem_limits reference = lab_limits();
   /* MiB: the probe arms, the band just under the threshold, and the band just over it. */
   static const uint64_t held_local_mib[] = {0,    1024, 3584, 6656,  6912,  7168, 7424,
                                             7680, 7936, 8960, 9216,  10752, 11264};
   static const uint64_t held_nonlocal_mib[] = {0, 256, 1024, 3328, 3584, 4096, 4352, 4608};

   for (uint64_t local = 64ull * MIB; local < LAB_BUDGET_TOTAL; local += 64ull * MIB) {
      const struct radv_wddm2_mem_limits limits = split_limits(local);
      CHECK(radv_wddm2_mem_budget_total(limits) == LAB_BUDGET_TOTAL);
      CHECK(radv_wddm2_mem_threshold(limits) == radv_wddm2_mem_threshold(reference));
      for (unsigned i = 0; i < sizeof(held_local_mib) / sizeof(held_local_mib[0]); i++) {
         for (unsigned j = 0; j < sizeof(held_nonlocal_mib) / sizeof(held_nonlocal_mib[0]); j++) {
            const uint64_t hl = held_local_mib[i] * MIB, hn = held_nonlocal_mib[j] * MIB;
            CHECK(radv_wddm2_mem_check(limits, true, hl, hn, CHUNK) ==
                  radv_wddm2_mem_check(reference, true, hl, hn, CHUNK));
         }
      }
   }
}

/* The hard rule of C70: a request that keeps the process inside the threshold is admitted, however
 * the held bytes sit between the two domains, and whatever the budget split says. The lab proved
 * the OS serves 11264 MiB of this process, and it served it as 7680 local plus 3584 non-local and
 * as 9216 local plus 256 non-local. */
static void
test_never_refuses_inside_the_threshold(void)
{
   const struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t threshold = radv_wddm2_mem_threshold(limits);

   for (uint64_t held_local = 0; held_local + CHUNK <= threshold; held_local += 64ull * MIB) {
      const uint64_t room = threshold - held_local - CHUNK;
      const uint64_t held_nonlocal = room < 4608ull * MIB ? room : 4608ull * MIB;
      CHECK(radv_wddm2_mem_check(limits, true, held_local, held_nonlocal, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
   }
   /* One byte past the threshold is the first refusal, and not one byte before it. */
   CHECK(radv_wddm2_mem_check(limits, true, threshold - CHUNK, 0, CHUNK) == RADV_WDDM2_MEM_ADMIT);
   CHECK(radv_wddm2_mem_check(limits, true, threshold - CHUNK + 1, 0, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   /* A device-local request is weighed against the total, so held non-local bytes count. */
   CHECK(radv_wddm2_mem_check(limits, true, 0, threshold - CHUNK, CHUNK) == RADV_WDDM2_MEM_ADMIT);
   CHECK(radv_wddm2_mem_check(limits, true, 0, threshold - CHUNK + 1, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
}

static void
test_no_information(void)
{
   struct radv_wddm2_mem_limits limits = {0, 0, 0, 0};

   /* Without a budget nothing is refused, however large the request. */
   CHECK(radv_wddm2_mem_check(limits, true, 0, 0, 64ull * 1024 * MIB) == RADV_WDDM2_MEM_ADMIT);
   limits.segment_total = LAB_SEGMENTS;
   limits.segment_local = LAB_SEGMENT_LOCAL;
   CHECK(radv_wddm2_mem_check(limits, true, LAB_SEGMENTS, 0, CHUNK) == RADV_WDDM2_MEM_ADMIT);

   /* A request larger than everything is refused once the budget is known. */
   limits = lab_limits();
   CHECK(radv_wddm2_mem_check(limits, true, 0, 0, 64ull * 1024 * MIB) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   /* The sum of the held bytes and the request cannot wrap into an admission. */
   CHECK(radv_wddm2_mem_check(limits, true, UINT64_MAX - CHUNK + 1, 0, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(radv_wddm2_mem_check(limits, true, UINT64_MAX, UINT64_MAX, UINT64_MAX) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(radv_wddm2_mem_check(limits, true, UINT64_MAX, 1, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   /* A request of no bytes is never refused, whatever the process holds. */
   CHECK(radv_wddm2_mem_check(limits, true, 7680ull * MIB, 3584ull * MIB, 0) ==
         RADV_WDDM2_MEM_ADMIT);
   CHECK(radv_wddm2_mem_check(limits, true, LAB_SEGMENTS, LAB_SEGMENTS, 0) ==
         RADV_WDDM2_MEM_ADMIT);
   /* A host-visible request is never refused, whatever the process holds. */
   CHECK(radv_wddm2_mem_check(limits, false, LAB_SEGMENTS, LAB_SEGMENTS, 64ull * 1024 * MIB) ==
         RADV_WDDM2_MEM_ADMIT);
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
   {"split_does_not_matter", test_split_does_not_matter},
   {"never_refuses_inside_the_threshold", test_never_refuses_inside_the_threshold},
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
