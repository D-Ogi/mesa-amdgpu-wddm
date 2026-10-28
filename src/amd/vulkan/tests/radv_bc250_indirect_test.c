/* SPDX-License-Identifier: MIT
 *
 * radv_bc250_indirect_records (BD-256): a direct indirect draw above the limit is refused, a count variant
 * above the limit is clamped to the limit, and every draw at or below the limit keeps its draw count.
 */
#include <stdio.h>

#include "radv_bc250_indirect.h"

static unsigned failures;

static void
check(uint32_t draw_count, bool has_count, bool want_ok, uint32_t want_records)
{
   uint32_t records = 12345;
   const bool ok = radv_bc250_indirect_records(draw_count, has_count, &records);
   if (ok != want_ok || records != want_records) {
      fprintf(stderr, "FAIL draw_count=%u has_count=%d: ok=%d records=%u, want ok=%d records=%u\n", draw_count,
              has_count, ok, records, want_ok, want_records);
      failures++;
   }
}

int
main(void)
{
   const uint32_t limit = RADV_BC250_MAX_DRAW_INDIRECT_COUNT;
   for (int c = 0; c < 2; c++) {
      const bool has_count = c != 0;
      check(0, has_count, true, 0);
      check(1, has_count, true, 1);
      check(limit - 1, has_count, true, limit - 1);
      check(limit, has_count, true, limit);
   }
   /* Direct draws: drawCount above maxDrawIndirectCount is invalid use. */
   check(limit + 1, false, false, 0);
   check(65535, false, false, 0);
   check(UINT32_MAX, false, false, 0);
   /* Count variants: only the count buffer value is bounded, so maxDrawCount is clamped. */
   check(limit + 1, true, true, limit);
   check(65535, true, true, limit);
   check(1u << 20, true, true, limit);
   check(UINT32_MAX, true, true, limit);
   printf("radv_bc250_indirect_test: %u failure(s)\n", failures);
   return failures ? 1 : 0;
}
