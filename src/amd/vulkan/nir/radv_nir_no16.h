/* SPDX-License-Identifier: MIT
 * Automatic no-output16 transform for the frozen Stage B VS/FS pair
 * (radv_no16.h: the case, the pipeline and draw decisions, the lab kill switch).
 */
#ifndef RADV_NIR_NO16_H
#define RADV_NIR_NO16_H

#include "nir.h"
#include "radv_no16.h"

enum radv_no16_result {
   RADV_NO16_OK,
   RADV_NO16_STAGE,
   RADV_NO16_FUNCTIONS,
   RADV_NO16_GLOBALS,
   RADV_NO16_OUTPUT,
   RADV_NO16_INSTRUCTION,
   RADV_NO16_INTRINSIC,
   RADV_NO16_HELPER_ACCESS,
   RADV_NO16_EXIT_QUAD,
   RADV_NO16_CONTROL_QUAD,
   RADV_NO16_LOOP,
   RADV_NO16_FIXTURE,
   RADV_NO16_IDENTITY,
   RADV_NO16_FORCED_REFUSAL,
   RADV_NO16_MODE,
};

/* Exact source identities (blake3 of the SPIR-V words, spirv_to_nir.c) of the
 * frozen Stage B pair. Captured on the host engine in scratch/bd232 R3. */
extern const uint32_t radv_no16_stageb_ps_blake3[8];
extern const uint32_t radv_no16_stageb_vs_blake3[8];

/* Transforms the lowered-IO fragment shader of the frozen Stage B pair into a
 * physical-8 wrapper that runs the body for logical sample IDs p and p+8.
 * Call after radv_nir_lower_io and before the second barycentric lowering.
 * On any result other than RADV_NO16_OK the shader is not changed.
 */
enum radv_no16_result radv_nir_no16_lower_stageb(nir_shader *fs, const nir_shader *linked_vs,
                                                 enum radv_no16_auto_mode mode);

const char *radv_no16_result_name(enum radv_no16_result result);

#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
/* Lab markers: writes 64 hex digits and a terminating zero. */
void radv_no16_format_blake3(const uint32_t blake3[8], char out[65]);
#endif

#endif
