/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_mem_precheck.h (C70, BD-096): the two rules that refuse a device-local
 * allocation which no eviction can satisfy, and the switch that turns them off. No GPU, no Vulkan,
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

/* Unit A, 2026-10-08 (scratch/bd096/lab/results/watch-20261008T033845Z). The two published per-heap
 * budgets are 7295.9 and 3647.9 MiB, and they add up to the 10943.8 MiB total of that run. The local
 * segment is the one of facts M65 (0x1FD736000 bytes, 8151.2 MiB) and the host-visible heap the same
 * adapter reports is 4008.7 MiB, so the two segments together are about 12159.9 MiB. */
static const uint64_t LAB_BUDGET_LOCAL = 7295ull * MIB + 922ull * 1024ull;    /* 7295.900 MiB */
static const uint64_t LAB_BUDGET_NONLOCAL = 3647ull * MIB + 921ull * 1024ull; /* 3647.899 MiB */
static const uint64_t LAB_SEGMENT_LOCAL = 0x1FD736000ull;                     /* 8151.2 MiB */
static const uint64_t LAB_SEGMENTS = 0x1FD736000ull + 4008ull * MIB + 734ull * 1024ull;
static const uint64_t CHUNK = 256ull * MIB;

static struct radv_wddm2_mem_limits
lab_limits(void)
{
   struct radv_wddm2_mem_limits limits = {LAB_BUDGET_LOCAL, LAB_BUDGET_NONLOCAL, LAB_SEGMENT_LOCAL,
                                          LAB_SEGMENTS};
   return limits;
}

static void
test_threshold(void)
{
   struct radv_wddm2_mem_limits limits = lab_limits();
   const uint64_t budget = LAB_BUDGET_LOCAL + LAB_BUDGET_NONLOCAL;

   /* The two published budgets add up to the total of that run, 10943.8 MiB. */
   CHECK(radv_wddm2_mem_budget_total(limits) == budget);
   CHECK(radv_wddm2_mem_budget_total(limits) / MIB == 10943);

   /* The threshold is the published total plus 1/32 of it. */
   CHECK(radv_wddm2_mem_threshold(limits) == budget + budget / 32);
   /* On unit A that is 11285.8 MiB: over the 11264 MiB the lab admitted, under the 11397.6 MiB the
    * kernel named as the maximum, and under the 11520 MiB request that took 86 s and failed. */
   CHECK(radv_wddm2_mem_threshold(limits) / MIB == 11285);
   CHECK(radv_wddm2_mem_threshold(limits) > 11264ull * MIB);
   CHECK(radv_wddm2_mem_threshold(limits) < 11397ull * MIB);

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

   /* The two segments together are the ceiling of the threshold. */
   limits.budget_local = 8192ull * MIB;
   limits.budget_nonlocal = 4096ull * MIB;
   limits.segment_total = 12288ull * MIB;
   CHECK(radv_wddm2_mem_threshold(limits) == 12288ull * MIB);

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

   /* Every chunk the lab admitted is admitted: the last one takes the total to 11264 MiB. Neither
    * rule refuses one of them, although the local budget is passed from 7296 MiB on. */
   for (uint64_t device_local = 0; device_local < 7680ull * MIB; device_local += CHUNK) {
      CHECK(radv_wddm2_mem_check(limits, true, device_local, host_visible, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
      CHECK(!radv_wddm2_mem_no_eviction_room(limits, device_local, host_visible, CHUNK));
   }

   /* The chunk that took 86 s in the kernel and then failed is refused. It is over the maximum
    * budget, and the second rule refuses it as well: it passes the local budget by more than the
    * overshoot, this process holds more than 7/8 of the local segment, and its share of system
    * memory is full, so nothing can move out. */
   CHECK(radv_wddm2_mem_check(limits, true, 7680ull * MIB, host_visible, CHUNK) ==
         RADV_WDDM2_MEM_REFUSE_OVER_MAX);
   CHECK(radv_wddm2_mem_no_eviction_room(limits, 7680ull * MIB, host_visible, CHUNK));
   CHECK(!strcmp(radv_wddm2_mem_verdict_name(RADV_WDDM2_MEM_REFUSE_OVER_MAX), "over-max-budget"));
   CHECK(!strcmp(radv_wddm2_mem_verdict_name(RADV_WDDM2_MEM_REFUSE_NO_ROOM), "no-eviction-room"));

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

   for (uint64_t device_local = 0; device_local < 9216ull * MIB; device_local += CHUNK) {
      CHECK(radv_wddm2_mem_check(limits, true, device_local, host_visible, CHUNK) ==
            RADV_WDDM2_MEM_ADMIT);
   }
   /* The 1024 MiB device-local arm of the same script is far from both rules. */
   CHECK(radv_wddm2_mem_check(limits, true, 768ull * MIB, 3584ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
}

/* The second rule on its own. */
static void
test_no_eviction_room(void)
{
   struct radv_wddm2_mem_limits limits = lab_limits();

   /* Over the local budget and the tenant of the segment, but system memory has room: admitted,
    * because that is the device-local-first case the lab served. */
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 7680ull * MIB, 0, CHUNK));
   CHECK(radv_wddm2_mem_check(limits, true, 7680ull * MIB, 0, CHUNK) == RADV_WDDM2_MEM_ADMIT);

   /* Under the local budget: nothing has to move out, so the rule says nothing. */
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 1024ull * MIB, 3584ull * MIB, CHUNK));

   /* The bound on the local side sits between the overshoot the lab served, 7680 MiB against a
    * 7295.9 MiB local budget, and the 7936 MiB request that failed. */
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 7424ull * MIB, 3584ull * MIB, CHUNK));
   CHECK(radv_wddm2_mem_no_eviction_room(limits, 7680ull * MIB, 3584ull * MIB, CHUNK));
   CHECK(LAB_BUDGET_LOCAL + (LAB_BUDGET_LOCAL >> RADV_WDDM2_MEM_LOCAL_OVERSHOOT_SHIFT) >
         7680ull * MIB);
   CHECK(LAB_BUDGET_LOCAL + (LAB_BUDGET_LOCAL >> RADV_WDDM2_MEM_LOCAL_OVERSHOOT_SHIFT) <
         7936ull * MIB);

   /* Another process holds the local segment, so this process is over a small budget while it holds
    * a quarter of the segment. Trimming the other process may fit the request: admitted. */
   limits.budget_local = 2048ull * MIB;
   limits.budget_nonlocal = 512ull * MIB;
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 2048ull * MIB, 512ull * MIB, CHUNK));
   CHECK(radv_wddm2_mem_check(limits, true, 2048ull * MIB, 512ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
   /* The same process once it holds 7/8 of the segment, with its system memory share full. */
   CHECK(radv_wddm2_mem_no_eviction_room(limits, 7168ull * MIB, 512ull * MIB, CHUNK));

   /* An unknown number never refuses. */
   limits = lab_limits();
   limits.segment_local = 0;
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 7424ull * MIB, 3584ull * MIB, CHUNK));
   CHECK(radv_wddm2_mem_check(limits, true, 7424ull * MIB, 3584ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
   limits = lab_limits();
   limits.budget_nonlocal = 0;
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 7424ull * MIB, 3584ull * MIB, CHUNK));
   limits = lab_limits();
   limits.budget_local = 0;
   CHECK(!radv_wddm2_mem_no_eviction_room(limits, 7424ull * MIB, 3584ull * MIB, CHUNK));

   /* A host-visible request is not this rule's business. */
   limits = lab_limits();
   CHECK(radv_wddm2_mem_check(limits, false, 7424ull * MIB, 3584ull * MIB, CHUNK) ==
         RADV_WDDM2_MEM_ADMIT);
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
   {"no_eviction_room", test_no_eviction_room},
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
