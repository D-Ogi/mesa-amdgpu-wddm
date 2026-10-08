/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * radv_pipeline_stage_cover.h - do the compiled shaders of a graphics pipeline cover the stages the
 * application asked for? Plain C with no Vulkan and no Mesa header, so a host test drives the
 * production rule (tests/radv_pipeline_stage_cover_test.c). radv_pipeline_graphics.c asserts every
 * constant below against the enumerator it mirrors, so the two cannot drift apart.
 *
 * Two inputs, both bit masks:
 *
 *   stages  what the application asked for: radv_graphics_pipeline::active_stages, which holds
 *           VkShaderStageFlagBits values.
 *   slots   one bit per non-null slot of radv_pipeline::shaders, bit (1u << mesa_shader_stage).
 *
 * The slots are not the stages. On GFX9 and later the hardware runs merged shaders, and the compiler
 * writes the merged pair into the slot of the later stage: the vertex shader lives in the tessellation
 * control slot (LS+HS) or in the geometry slot (ES+GS), and the tessellation evaluation shader lives in
 * the geometry slot (ES+GS). radv_get_shader() resolves this at run time; the rule below is the same
 * walk over the masks, and it must stay the same walk. Every other stage keeps its own slot.
 *
 * Session 486: a pipeline that reaches the state setup without a shader for an active stage faults.
 * A pipeline with no shader at all faulted in radv_pipeline_init_vertex_input_state; a tessellation
 * control shader without its evaluation shader faults in radv_compute_ia_multi_vgt_param, which reads
 * radv_get_shader(TESS_EVAL) without a null check; a pipeline that lost its fragment shader draws with
 * no pixel shader. The rule holds for a freshly compiled pipeline and for one read back from the
 * shader cache, which is where sessions 487 and 488 took their crash from.
 */

#ifndef RADV_PIPELINE_STAGE_COVER_H
#define RADV_PIPELINE_STAGE_COVER_H

#include <stdbool.h>
#include <stdint.h>

/* VkShaderStageFlagBits values. */
enum radv_stage_cover_stage {
   RADV_STAGE_COVER_VERTEX = 0x00000001,
   RADV_STAGE_COVER_TESS_CTRL = 0x00000002,
   RADV_STAGE_COVER_TESS_EVAL = 0x00000004,
   RADV_STAGE_COVER_GEOMETRY = 0x00000008,
   RADV_STAGE_COVER_FRAGMENT = 0x00000010,
   RADV_STAGE_COVER_TASK = 0x00000040,
   RADV_STAGE_COVER_MESH = 0x00000080,
};

/* 1u << mesa_shader_stage, the index of radv_pipeline::shaders. */
enum radv_stage_cover_slot {
   RADV_STAGE_COVER_SLOT_VERTEX = 1u << 0,
   RADV_STAGE_COVER_SLOT_TESS_CTRL = 1u << 1,
   RADV_STAGE_COVER_SLOT_TESS_EVAL = 1u << 2,
   RADV_STAGE_COVER_SLOT_GEOMETRY = 1u << 3,
   RADV_STAGE_COVER_SLOT_FRAGMENT = 1u << 4,
   RADV_STAGE_COVER_SLOT_TASK = 1u << 6,
   RADV_STAGE_COVER_SLOT_MESH = 1u << 7,
};

/* radv_get_shader(shaders, MESA_SHADER_VERTEX): the vertex slot, else the tessellation control slot,
 * else the geometry slot. */
static inline bool
radv_stage_cover_has_vertex(uint32_t slots)
{
   return (slots & (RADV_STAGE_COVER_SLOT_VERTEX | RADV_STAGE_COVER_SLOT_TESS_CTRL |
                    RADV_STAGE_COVER_SLOT_GEOMETRY)) != 0;
}

/* radv_get_shader(shaders, MESA_SHADER_TESS_EVAL): nothing at all without a tessellation control
 * shader, then the evaluation slot, else the geometry slot. */
static inline bool
radv_stage_cover_has_tess_eval(uint32_t slots)
{
   if (!(slots & RADV_STAGE_COVER_SLOT_TESS_CTRL))
      return false;

   return (slots & (RADV_STAGE_COVER_SLOT_TESS_EVAL | RADV_STAGE_COVER_SLOT_GEOMETRY)) != 0;
}

/* True when every stage in "stages" resolves to a shader in "slots". */
static inline bool
radv_pipeline_stages_covered(uint32_t stages, uint32_t slots)
{
   if ((stages & RADV_STAGE_COVER_VERTEX) && !radv_stage_cover_has_vertex(slots))
      return false;

   if ((stages & RADV_STAGE_COVER_TESS_EVAL) && !radv_stage_cover_has_tess_eval(slots))
      return false;

   if ((stages & RADV_STAGE_COVER_TESS_CTRL) && !(slots & RADV_STAGE_COVER_SLOT_TESS_CTRL))
      return false;

   if ((stages & RADV_STAGE_COVER_GEOMETRY) && !(slots & RADV_STAGE_COVER_SLOT_GEOMETRY))
      return false;

   if ((stages & RADV_STAGE_COVER_FRAGMENT) && !(slots & RADV_STAGE_COVER_SLOT_FRAGMENT))
      return false;

   if ((stages & RADV_STAGE_COVER_TASK) && !(slots & RADV_STAGE_COVER_SLOT_TASK))
      return false;

   if ((stages & RADV_STAGE_COVER_MESH) && !(slots & RADV_STAGE_COVER_SLOT_MESH))
      return false;

   return true;
}

#endif /* RADV_PIPELINE_STAGE_COVER_H */
