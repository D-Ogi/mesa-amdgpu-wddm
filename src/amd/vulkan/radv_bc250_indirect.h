/* SPDX-License-Identifier: MIT
 *
 * BC-250 mesh indirect draws: the bound on the number of records (BD-256).
 *
 * With native mesh or hybrid Task, the BC-250 mesh paths record each indirect record (Task) or rewrite each
 * record in a setup dispatch (split). Both are sized for at most RADV_BC250_MAX_DRAW_INDIRECT_COUNT records, and
 * VkPhysicalDeviceLimits::maxDrawIndirectCount reports that value.
 *
 * Vulkan bounds drawCount of vkCmdDrawMeshTasksIndirectEXT by maxDrawIndirectCount, so a larger drawCount is
 * invalid use and the draw is refused. The count variants are different: maxDrawCount has no such bound, only the
 * value in the count buffer has. vkd3d-proton passes the MaxCommandCount of a D3D12 command signature as
 * maxDrawCount, so a valid ExecuteIndirect can have a maxDrawCount above the limit. The device then draws
 * min(count, maxDrawCount) records, and count is at most the limit, so a maxDrawCount clamped to the limit draws
 * the same records.
 */
#ifndef RADV_BC250_INDIRECT_H
#define RADV_BC250_INDIRECT_H

#include <stdbool.h>
#include <stdint.h>

#define RADV_BC250_MAX_DRAW_INDIRECT_COUNT 4096u

/* The number of records the BC-250 mesh path records for one indirect draw. has_count: the draw is a count
 * variant (draw_count is its maxDrawCount). Returns false when the draw must be refused. */
static inline bool
radv_bc250_indirect_records(uint32_t draw_count, bool has_count, uint32_t *records)
{
   if (draw_count <= RADV_BC250_MAX_DRAW_INDIRECT_COUNT) {
      *records = draw_count;
      return true;
   }
   if (has_count) {
      *records = RADV_BC250_MAX_DRAW_INDIRECT_COUNT;
      return true;
   }
   *records = 0;
   return false;
}

#endif
