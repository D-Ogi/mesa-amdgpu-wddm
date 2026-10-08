/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * C70 (BD-096): the rules that refuse a device-local allocation which no eviction can satisfy.
 *
 * The default memory overflow policy makes every allocation resident with CantTrimFurther, so the
 * video memory manager may page other memory out to fit it. That is what lets a program hold more
 * device-local memory than the process budget, and games rely on it. Past one point the paging
 * cannot help any more, and the kernel call does not fail at once: it evicts the process first. On
 * unit A one MakeResident of 256 MiB stayed in the kernel for 86 s, evicted the whole process, read
 * pages from disk, and then failed with STATUS_NO_MEMORY all the same (K245, K247).
 *
 * This header holds the two rules that tell the hopeless request from the admissible one. It is
 * plain C with no Windows header, so the host test can run every case (tests/radv_wddm2_mem_precheck_test.c).
 *
 * The numbers come from the lab run of 2026-10-08 (scratch/bd096/lab/results/watch-20261008T033845Z,
 * default policy, 7912 MiB of system memory, 256 MiB chunks):
 *   published budget total   10943.8 MiB  Budget of the local plus the non-local segment group
 *   admitted and resident    11264.0 MiB  3584 host-visible plus 7680 device-local, +2.93 %
 *   the refused request      11520.0 MiB  the next 256 MiB chunk, +5.26 %, 86 s then STATUS_NO_MEMORY
 *   the kernel's own answer    122.4 MiB  NumBytesToTrim of that failure, so the hard maximum of
 *                                         the process was about 11397.6 MiB, +4.15 %
 * The admitted overshoot of 1/32 (+3.125 %, 11285.8 MiB on unit A) therefore sits above every total
 * the lab saw admitted and below the total the lab saw fail. The same run admits 9216 MiB of
 * device-local memory when little host-visible memory is held (total 9472 MiB), and these rules leave
 * that case alone.
 *
 * The same run calibrates a second rule from the other side. The content that must leave the local
 * segment has to go to system memory, and the OS gives this process a share of it, the non-local
 * budget. In the failing case that share was full: the host-visible phase had just stopped at it,
 * at 3584 MiB of 3647.9 MiB. A device-local allocation is therefore refused when all three hold:
 *   - it passes the local budget by more than 1/16 of that budget, so the OS has to evict,
 *   - this process alone holds more than 7/8 of the local segment, so trimming another process
 *     cannot free enough,
 *   - the non-local share has no room for the bytes that must move out.
 * In the device-local-first case the non-local share is nearly empty, 256 MiB of 3647.9 MiB, so the
 * third test fails and all 9216 MiB are admitted. The 1/16 overshoot on the local budget keeps this
 * rule above what the kernel served: the lab held 7680 MiB of device-local memory against a 7295.9 MiB
 * local budget, which is +5.26 %, while system memory was full, and it failed at 7936 MiB, +8.78 %.
 * The bound sits between the two, at +6.25 %, so the rule admits every chunk the lab admitted and
 * refuses the one that stayed in the kernel for 86 s. It reads the local budget, the local segment and
 * the non-local budget apart from each other, so it answers for a reason the first rule cannot see:
 * where the evicted bytes would go.
 *
 * Two guards keep both rules away from memory that trimming can still free:
 *   - the floor: the first rule refuses nothing while the process holds less than 3/4 of the two
 *     segments of the adapter together, and the second rule refuses nothing while the process holds
 *     less than 7/8 of the local segment. The OS lowers the budget of a process when another process
 *     asks for memory, and trimming that other process can give the memory back. Below a floor the
 *     budget is a share, not a wall.
 *   - unknown numbers never refuse. Without a budget there is no threshold.
 * Host-visible allocations are never refused here. They already fail in milliseconds at the
 * non-local budget (3584 MiB with NumBytesToTrim 81.6 MiB in the same run), and their behaviour
 * stays as it is.
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
/* The share of the local segment above which this process is the tenant of it: 7/8. */
#define RADV_WDDM2_MEM_TENANT_NUM 7u
#define RADV_WDDM2_MEM_TENANT_DEN 8u
/* The overshoot over the local budget that the second rule admits: 1/16, so +6.25 %. */
#define RADV_WDDM2_MEM_LOCAL_OVERSHOOT_SHIFT 4u
/* The switch that turns the rule off for one process. */
#define RADV_WDDM2_MEM_PRECHECK_ENV "AMDGPU_WDDM_VK_MEM_PRECHECK"

/* What the OS says about this process and this adapter. Zero means "not known". */
struct radv_wddm2_mem_limits {
   /* D3DKMT_QUERYVIDEOMEMORYINFO.Budget of each segment group. */
   uint64_t budget_local;
   uint64_t budget_nonlocal;
   /* D3DKMT_SEGMENTSIZEINFO: DedicatedVideoMemorySize, and it plus SharedSystemMemorySize. */
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
   /* The request needs an eviction, and what it evicts has nowhere to go. */
   RADV_WDDM2_MEM_REFUSE_NO_ROOM,
};

static inline const char *
radv_wddm2_mem_verdict_name(enum radv_wddm2_mem_verdict verdict)
{
   switch (verdict) {
   case RADV_WDDM2_MEM_ADMIT:
      return "admit";
   case RADV_WDDM2_MEM_REFUSE_OVER_MAX:
      return "over-max-budget";
   case RADV_WDDM2_MEM_REFUSE_NO_ROOM:
      return "no-eviction-room";
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
 * device-local memory. Zero means "no threshold", so nothing is refused. */
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
      /* Nothing can hold more than the two segments together. */
      if (threshold > segments)
         threshold = segments;
   }
   return threshold;
}

/* The second rule: the request needs an eviction, this process is the tenant of the local segment,
 * and its share of system memory cannot take what must move out. Without all three numbers the
 * answer is false, because an unknown limit is never a reason to refuse. */
static inline bool
radv_wddm2_mem_no_eviction_room(struct radv_wddm2_mem_limits limits, uint64_t held_local,
                                uint64_t held_nonlocal, uint64_t size)
{
   if (!limits.budget_local || !limits.budget_nonlocal || !limits.segment_local)
      return false;
   if (size > UINT64_MAX - held_local || size > UINT64_MAX - held_nonlocal)
      return true;

   const uint64_t want_local = held_local + size;
   const uint64_t local_max =
      limits.budget_local + (limits.budget_local >> RADV_WDDM2_MEM_LOCAL_OVERSHOOT_SHIFT);
   if (want_local <= local_max)
      return false; /* the local budget and its overshoot have room, so nothing has to move out */
   if (want_local <= limits.segment_local / RADV_WDDM2_MEM_TENANT_DEN * RADV_WDDM2_MEM_TENANT_NUM)
      return false; /* another process holds the segment, and trimming it may fit this request */
   /* What leaves the local segment needs that many bytes of this process's system memory share. */
   return held_nonlocal + size > limits.budget_nonlocal;
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
   if (threshold) {
      if (held_local > UINT64_MAX - held_nonlocal)
         return RADV_WDDM2_MEM_REFUSE_OVER_MAX;
      const uint64_t held = held_local + held_nonlocal;
      if (size > UINT64_MAX - held || held + size > threshold)
         return RADV_WDDM2_MEM_REFUSE_OVER_MAX;
   }
   if (radv_wddm2_mem_no_eviction_room(limits, held_local, held_nonlocal, size))
      return RADV_WDDM2_MEM_REFUSE_NO_ROOM;
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
