/* SPDX-License-Identifier: MIT
 * Fragment shader lowering of 16-sample draws without attachments
 * (radv_no16.h: the case, the pipeline and draw decisions, the lab kill switch).
 */
#ifndef RADV_NIR_NO16_H
#define RADV_NIR_NO16_H

#include "nir.h"
#include "radv_no16.h"

struct radv_shader_args;

/* SPLIT16, after radv_nir_lower_io: a sample mask output keeps the bits of
 * the 8 logical samples of the current pass. Uses the logical sample index,
 * so it is correct after radv_nir_no16_lower_split_late. */
bool radv_nir_no16_lower_split_early(nir_shader *fs);

/* SPLIT16, immediately before ac_nir_lower_intrinsics_to_args: every sample
 * index is the hardware index plus the pass base from the PS state SGPR. */
bool radv_nir_no16_lower_split_late(nir_shader *fs, const struct radv_shader_args *args);

/* SPLIT16, NGG vertex shader before radv_lower_ngg: every memory write runs
 * only when NGG_STATE_NO16_PASS is 0, so pass 1 does not repeat it. */
bool radv_nir_no16_guard_vs_memory(nir_shader *vs, const struct radv_shader_args *args);

/* True when a memory write of the shader returns a value that the shader uses. */
bool radv_nir_no16_memory_result_used(nir_shader *s);

#endif
