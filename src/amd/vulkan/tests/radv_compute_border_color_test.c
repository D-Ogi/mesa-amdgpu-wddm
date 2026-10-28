/* SPDX-License-Identifier: MIT */
/* The custom border color on the BC-250 compute node (WDDM node 2, one MEC queue).
 *
 * 1. radv_custom_border_on_compute_queue: the BC-250 compute node keeps the custom border color in compute
 *    shaders on the compute queue, as the drirc option does; any other device keeps the upstream replacement
 *    by transparent black.
 * 2. ac_init_compute_preamble_state: with cs_idle_before_border_color, the preamble waits for the earlier
 *    dispatches (CS_PARTIAL_FLUSH) before it writes TA_CS_BC_BASE_ADDR. Without it, the stream has no wait,
 *    which is the stream of every other device and the control of the check.
 *
 * No GPU is used. The PM4 stream is read packet by packet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_cmdbuf.h"
#include "ac_gpu_info.h"
#include "ac_pm4.h"
#include "radv_physical_device.h"
#include "sid.h"

static unsigned checks, failures;

#define CHECK(condition)                                                                                           \
   do {                                                                                                            \
      checks++;                                                                                                    \
      if (!(condition)) {                                                                                          \
         failures++;                                                                                               \
         fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #condition);                                              \
      }                                                                                                            \
   } while (0)

static void
test_decision(void)
{
   struct radv_physical_device *pdev = calloc(1, sizeof(*pdev));
   if (!pdev) {
      CHECK(!"calloc");
      return;
   }

   CHECK(!radv_custom_border_on_compute_queue(pdev));

   pdev->info.has_bc250_compute_node = true;
   CHECK(radv_custom_border_on_compute_queue(pdev));

   pdev->info.has_bc250_compute_node = false;
   pdev->drirc.features.enable_custom_border_on_compute_queue = true;
   CHECK(radv_custom_border_on_compute_queue(pdev));

   free(pdev);
}

struct preamble_scan {
   int cs_partial_flush; /* dword index of the first CS_PARTIAL_FLUSH event, -1 when there is none */
   int border_color_lo;  /* dword index of the TA_CS_BC_BASE_ADDR value, -1 when it is not written */
   uint32_t border_color_lo_value;
   bool malformed;
};

static struct preamble_scan
scan(const struct ac_pm4_state *pm4)
{
   struct preamble_scan s = {-1, -1, 0, false};
   unsigned i = 0;

   while (i < pm4->ndw) {
      const uint32_t header = pm4->pm4[i];
      if ((header >> 30) != 3) {
         s.malformed = true;
         return s;
      }
      const unsigned opcode = (header >> 8) & 0xff;
      const unsigned body = ((header >> 16) & 0x3fff) + 1;
      if (i + 1 + body > pm4->ndw) {
         s.malformed = true;
         return s;
      }

      if (opcode == PKT3_EVENT_WRITE && (pm4->pm4[i + 1] & 0x3f) == V_028A90_CS_PARTIAL_FLUSH &&
          s.cs_partial_flush < 0)
         s.cs_partial_flush = i;

      if (opcode == PKT3_SET_UCONFIG_REG || opcode == PKT3_SET_UCONFIG_REG_INDEX) {
         const unsigned first = CIK_UCONFIG_REG_OFFSET + (pm4->pm4[i + 1] & 0xffff) * 4;
         for (unsigned v = 0; v + 1 < body; v++) {
            if (first + v * 4 == R_030E00_TA_CS_BC_BASE_ADDR && s.border_color_lo < 0) {
               s.border_color_lo = i + 2 + v;
               s.border_color_lo_value = pm4->pm4[i + 2 + v];
            }
         }
      }

      i += 1 + body;
   }
   return s;
}

static struct preamble_scan
build(const struct radeon_info *info, bool wait, uint64_t va)
{
   struct preamble_scan s = {-1, -1, 0, true};
   struct ac_pm4_state *pm4 = ac_pm4_create_sized(info, false, 64, true);
   if (!pm4)
      return s;

   const struct ac_preamble_state state = {
      .border_color_va = va,
      .cs_idle_before_border_color = wait,
      .gfx11 = {.compute_dispatch_interleave = 64},
   };
   ac_init_compute_preamble_state(&state, pm4);
   ac_pm4_finalize(pm4);
   s = scan(pm4);
   ac_pm4_free_state(pm4);
   return s;
}

static void
test_preamble(void)
{
   struct radeon_info info;
   memset(&info, 0, sizeof(info));
   info.gfx_level = GFX10;
   info.family = CHIP_GFX1013;
   info.has_graphics = true;
   info.max_se = 1;
   info.spi_cu_en = ~0u;

   const uint64_t va = 0x123456789a00ull;

   const struct preamble_scan with = build(&info, true, va);
   CHECK(!with.malformed);
   CHECK(with.border_color_lo >= 0);
   CHECK(with.border_color_lo_value == (uint32_t)(va >> 8));
   CHECK(with.cs_partial_flush >= 0);
   CHECK(with.cs_partial_flush < with.border_color_lo);

   const struct preamble_scan without = build(&info, false, va);
   CHECK(!without.malformed);
   CHECK(without.border_color_lo >= 0);
   CHECK(without.border_color_lo_value == (uint32_t)(va >> 8));
   CHECK(without.cs_partial_flush < 0);
}

int
main(void)
{
   test_decision();
   test_preamble();
   printf("compute border color: checks=%u, failures=%u\n", checks, failures);
   return failures ? 1 : 0;
}
