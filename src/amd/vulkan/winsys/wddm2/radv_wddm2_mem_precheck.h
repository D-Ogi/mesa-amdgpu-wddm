/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * C70 (BD-096): the rule that refuses a device-local allocation which no eviction can satisfy.
 *
 * The default memory overflow policy makes every allocation resident with CantTrimFurther, so the
 * video memory manager may page other memory out to fit it. That is what lets a program hold more
 * device-local memory than the process budget, and games rely on it. Past one point the paging
 * cannot help any more, and the kernel call does not fail at once: it evicts the process first. On
 * unit A one MakeResident of 256 MiB stayed in the kernel for 86 s, evicted the whole process, read
 * pages from disk, and then failed with STATUS_NO_MEMORY all the same (K245, K247).
 *
 * This header holds the rule that tells the hopeless request from the admissible one. It is plain C
 * with no Windows header, so the host test can run every case (tests/radv_wddm2_mem_precheck_test.c).
 *
 * WHAT THE LAB MEASURED, AND WHAT IT DID NOT
 * The numbers come from the lab run of 2026-10-08 (scratch/bd096/lab/results/watch-20261008T033845Z,
 * default policy, 7912 MiB of system memory, 256 MiB chunks, ICD F8958BAB):
 *   published budget total   10943.8 MiB  the sum of the Budget of the two segment groups
 *   admitted and resident    11264.0 MiB  3584 host-visible plus 7680 device-local, +2.93 %
 *   the refused request      11520.0 MiB  the next 256 MiB chunk, +5.26 %, 86 s then STATUS_NO_MEMORY
 * The 10943.8 MiB total is a sound read of budget_local + budget_nonlocal. The probe prints the two
 * Vulkan heap budgets, 7295.9 and 3647.9 MiB, and those are NOT the two segment group budgets: RADV
 * redistributes the capped total over the APU fake heaps at 2/3 and 1/3 in
 * radv_get_memory_budget_properties, and 7295.9 : 3647.9 is exactly that 2 : 1 split. The run
 * therefore says nothing about how the OS splits the total between the local and the non-local
 * group. So the rule below uses the sum alone, and nothing in this file may depend on the split.
 *
 * THE RULE
 * Refuse a device-local request when
 *   held_local + held_nonlocal + size > budget_total + budget_total / 32
 * The admitted overshoot of 1/32 (+3.125 %, 11285.8 MiB on unit A) sits above every total the lab
 * saw admitted and below the total the lab saw fail. The same run admits 9216 MiB of device-local
 * memory when little host-visible memory is held (total 9472 MiB), and the rule leaves that case
 * alone. Host-visible requests are never refused here. They already fail in milliseconds at the
 * non-local budget (3584 MiB in the same run), and their behaviour stays as it is.
 *
 * Two guards keep the rule away from memory that trimming can still free:
 *   - the floor: the rule refuses nothing while the process holds less than 3/4 of the two segments
 *     of the adapter together. The OS lowers the budget of a process when another process asks for
 *     memory, and trimming that other process can give the memory back. Below a floor the budget is
 *     a share, not a wall.
 *   - unknown numbers never refuse. Without a budget there is no threshold.
 *
 * WHAT WAS TRIED AND DROPPED (review of 2026-10-08)
 * A second rule refused a request when it passed the local budget, the process held more than 7/8 of
 * the local segment, and held_nonlocal + size passed the non-local budget, on the reading that the
 * evicted bytes need room in our share of system memory. That rule is gone. Its model is wrong: an
 * evicted allocation leaves the segment for its own backing store, and K245 watched those pages go
 * to and from disk, not into the non-local budget of this process. It was also calibrated on the
 * 7295.9 and 3647.9 MiB heap budgets above, which are not the numbers the rule read. With a real
 * split it refused requests the lab served, for example the admitted chunk at 7680 MiB of
 * device-local memory, and requests that fit inside the local segment with room to spare. The rule
 * above refuses the one chunk the lab measured as hopeless, so nothing is lost.
 */
#ifndef RADV_WDDM2_MEM_PRECHECK_H
#define RADV_WDDM2_MEM_PRECHECK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The overshoot over the published budget that the rule admits: 1/32, so +3.125 %. */
#define RADV_WDDM2_MEM_OVERSHOOT_SHIFT 5u
/* The floor, as a fraction of the two segment sizes: 3/4. */
#define RADV_WDDM2_MEM_FLOOR_NUM 3u
#define RADV_WDDM2_MEM_FLOOR_DEN 4u
/* The switch that turns the rule off for one process. */
#define RADV_WDDM2_MEM_PRECHECK_ENV "AMDGPU_WDDM_VK_MEM_PRECHECK"

/* What the OS says about this process and this adapter. Zero means "not known".
 *
 * budget_local and budget_nonlocal are two reads of the same call, and only their sum steers the
 * rule. The lab never measured how the OS splits the total, so no rule here may read one of them
 * alone. segment_local is reported in the log line and is not part of any rule. */
struct radv_wddm2_mem_limits {
   /* D3DKMT_QUERYVIDEOMEMORYINFO.Budget of each segment group. */
   uint64_t budget_local;
   uint64_t budget_nonlocal;
   /* D3DKMT_SEGMENTSIZEINFO: DedicatedVideoMemorySize, and the three sizes of that struct added up. */
   uint64_t segment_local;
   uint64_t segment_total;
};

/* The budget of the two groups together, which is what the process may hold in all. */
static inline uint64_t
radv_wddm2_mem_budget_total(struct radv_wddm2_mem_limits limits)
{
   if (!limits.budget_local || !limits.budget_nonlocal)
      return 0;
   return limits.budget_local + limits.budget_nonlocal;
}

enum radv_wddm2_mem_verdict {
   RADV_WDDM2_MEM_ADMIT = 0,
   /* The request puts the process over the maximum budget. No eviction can fit it. */
   RADV_WDDM2_MEM_REFUSE_OVER_MAX,
};

static inline const char *
radv_wddm2_mem_verdict_name(enum radv_wddm2_mem_verdict verdict)
{
   switch (verdict) {
   case RADV_WDDM2_MEM_ADMIT:
      return "admit";
   case RADV_WDDM2_MEM_REFUSE_OVER_MAX:
      return "over-max-budget";
   default:
      return "?";
   }
}

/* The segment total is only used when it is at least the budget total. A segment total below the
 * budget the OS gives this process is not a capacity, so it is dropped. */
static inline uint64_t
radv_wddm2_mem_usable_segment_total(struct radv_wddm2_mem_limits limits)
{
   if (limits.segment_total < radv_wddm2_mem_budget_total(limits))
      return 0;
   return limits.segment_total;
}

/* The total this process may hold across both segment groups before the rule refuses more
 * device-local memory. Zero means "no threshold", so nothing is refused.
 *
 * The segment total only ever raises the threshold, through the floor. It never caps it. A cap at the
 * reported segments would refuse inside the overshoot on a part whose segment total sits within
 * 3.125 % of the budget total, and the lab has no measurement that says a process cannot pass the
 * reported segment total. Only the budget and its measured overshoot may refuse. */
static inline uint64_t
radv_wddm2_mem_threshold(struct radv_wddm2_mem_limits limits)
{
   const uint64_t budget_total = radv_wddm2_mem_budget_total(limits);
   if (!budget_total)
      return 0;

   uint64_t threshold = budget_total + (budget_total >> RADV_WDDM2_MEM_OVERSHOOT_SHIFT);
   const uint64_t segments = radv_wddm2_mem_usable_segment_total(limits);
   if (segments) {
      const uint64_t floor_bytes = segments / RADV_WDDM2_MEM_FLOOR_DEN * RADV_WDDM2_MEM_FLOOR_NUM;
      if (threshold < floor_bytes)
         threshold = floor_bytes;
   }
   return threshold;
}

/* held_local and held_nonlocal: the bytes this process already holds in each domain. size: the
 * request, which is a device-local request when device_local is set. */
static inline enum radv_wddm2_mem_verdict
radv_wddm2_mem_check(struct radv_wddm2_mem_limits limits, bool device_local, uint64_t held_local,
                     uint64_t held_nonlocal, uint64_t size)
{
   /* A host-visible request is not refused here, and a request of no bytes needs no memory. */
   if (!device_local || !size)
      return RADV_WDDM2_MEM_ADMIT;

   const uint64_t threshold = radv_wddm2_mem_threshold(limits);
   if (!threshold)
      return RADV_WDDM2_MEM_ADMIT;

   if (held_local > UINT64_MAX - held_nonlocal)
      return RADV_WDDM2_MEM_REFUSE_OVER_MAX;
   const uint64_t held = held_local + held_nonlocal;
   if (size > UINT64_MAX - held || held + size > threshold)
      return RADV_WDDM2_MEM_REFUSE_OVER_MAX;
   return RADV_WDDM2_MEM_ADMIT;
}

struct radv_wddm2_mem_precheck_switch {
   bool enabled;
   bool invalid; /* the value was there but is not one of ours, so the default is used */
};

/* Equal, with no regard for the case of a letter. No locale and no Windows header. */
static inline bool
radv_wddm2_mem_streq_ci(const char *a, const char *b)
{
   if (!a || !b)
      return false;
   for (; *a && *b; a++, b++) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z')
         ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z')
         cb = (char)(cb - 'A' + 'a');
      if (ca != cb)
         return false;
   }
   return !*a && !*b;
}

/* AMDGPU_WDDM_VK_MEM_PRECHECK: "off" or "0" turns the rule off, "on" or "1" turns it on. Absent,
 * empty or unknown gives the default, which is on. Case does not matter. */
static inline struct radv_wddm2_mem_precheck_switch
radv_wddm2_mem_precheck_parse(const char *value)
{
   struct radv_wddm2_mem_precheck_switch result = {true, false};
   if (!value || !value[0])
      return result;
   if (radv_wddm2_mem_streq_ci(value, "0") || radv_wddm2_mem_streq_ci(value, "off")) {
      result.enabled = false;
      return result;
   }
   if (radv_wddm2_mem_streq_ci(value, "1") || radv_wddm2_mem_streq_ci(value, "on"))
      return result;
   result.invalid = true;
   return result;
}

#endif /* RADV_WDDM2_MEM_PRECHECK_H */
