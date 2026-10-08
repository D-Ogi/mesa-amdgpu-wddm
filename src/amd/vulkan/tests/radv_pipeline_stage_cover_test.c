/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_pipeline_stage_cover.h (session 486): does a set of compiled shader slots cover
 * the stages of a graphics pipeline? No GPU, no Vulkan, no Windows header, so it builds and runs on
 * the development machine.
 *
 * The test pins the numbers of the two mirrored enums as well as the rule. radv_pipeline_graphics.c
 * asserts the same numbers against VkShaderStageFlagBits and mesa_shader_stage, so a change on either
 * side breaks the build or this test, and the two cannot drift apart silently.
 *
 * Usage: radv_pipeline_stage_cover_test [--negative-control]
 * The negative control runs every case with each expectation inverted; it must fail every case.
 */
#include "../radv_pipeline_stage_cover.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
static bool invert;

#define CHECK(cond)                                                                                \
   do {                                                                                            \
      checks++;                                                                                     \
      if (!(cond) != invert) {                                                                      \
         failures++;                                                                                \
         printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                     \
      }                                                                                             \
   } while (0)

/* Short names for the cases below. */
#define S_VS RADV_STAGE_COVER_VERTEX
#define S_TCS RADV_STAGE_COVER_TESS_CTRL
#define S_TES RADV_STAGE_COVER_TESS_EVAL
#define S_GS RADV_STAGE_COVER_GEOMETRY
#define S_FS RADV_STAGE_COVER_FRAGMENT
#define S_TASK RADV_STAGE_COVER_TASK
#define S_MESH RADV_STAGE_COVER_MESH

#define L_VS RADV_STAGE_COVER_SLOT_VERTEX
#define L_TCS RADV_STAGE_COVER_SLOT_TESS_CTRL
#define L_TES RADV_STAGE_COVER_SLOT_TESS_EVAL
#define L_GS RADV_STAGE_COVER_SLOT_GEOMETRY
#define L_FS RADV_STAGE_COVER_SLOT_FRAGMENT
#define L_TASK RADV_STAGE_COVER_SLOT_TASK
#define L_MESH RADV_STAGE_COVER_SLOT_MESH

/* The values of VkShaderStageFlagBits and of 1u << mesa_shader_stage, written out. */
static void
test_values(void)
{
   CHECK(S_VS == 0x00000001u);
   CHECK(S_TCS == 0x00000002u);
   CHECK(S_TES == 0x00000004u);
   CHECK(S_GS == 0x00000008u);
   CHECK(S_FS == 0x00000010u);
   CHECK(S_TASK == 0x00000040u);
   CHECK(S_MESH == 0x00000080u);

   CHECK(L_VS == 1u << 0);
   CHECK(L_TCS == 1u << 1);
   CHECK(L_TES == 1u << 2);
   CHECK(L_GS == 1u << 3);
   CHECK(L_FS == 1u << 4);
   /* Slot 5 is the compute stage, which no graphics pipeline holds. */
   CHECK(L_TASK == 1u << 6);
   CHECK(L_MESH == 1u << 7);
}

/* radv_get_shader(shaders, MESA_SHADER_VERTEX) walks three slots. */
static void
test_vertex_slot(void)
{
   CHECK(!radv_stage_cover_has_vertex(0));
   CHECK(radv_stage_cover_has_vertex(L_VS));
   CHECK(radv_stage_cover_has_vertex(L_TCS));
   CHECK(radv_stage_cover_has_vertex(L_GS));
   CHECK(radv_stage_cover_has_vertex(L_VS | L_FS));
   /* Neither the evaluation nor the fragment slot ever holds a vertex shader. */
   CHECK(!radv_stage_cover_has_vertex(L_TES));
   CHECK(!radv_stage_cover_has_vertex(L_FS));
   CHECK(!radv_stage_cover_has_vertex(L_TES | L_FS | L_MESH | L_TASK));
}

/* radv_get_shader(shaders, MESA_SHADER_TESS_EVAL) answers nothing without a control shader. */
static void
test_tess_eval_slot(void)
{
   CHECK(!radv_stage_cover_has_tess_eval(0));
   CHECK(!radv_stage_cover_has_tess_eval(L_TES));
   CHECK(!radv_stage_cover_has_tess_eval(L_TES | L_GS));
   CHECK(!radv_stage_cover_has_tess_eval(L_TCS));
   CHECK(radv_stage_cover_has_tess_eval(L_TCS | L_TES));
   CHECK(radv_stage_cover_has_tess_eval(L_TCS | L_GS));
   CHECK(radv_stage_cover_has_tess_eval(L_TCS | L_TES | L_GS));
}

/* Pipelines that the compiler produces, and the broken shapes of sessions 486 to 488. */
static void
test_pipelines(void)
{
   /* An empty pipeline asks for nothing. */
   CHECK(radv_pipeline_stages_covered(0, 0));

   /* Vertex and fragment, unmerged. */
   CHECK(radv_pipeline_stages_covered(S_VS | S_FS, L_VS | L_FS));
   /* Session 486: the pipeline reached the state setup with no shader at all. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_FS, 0));
   /* A pipeline that lost one of the two. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_FS, L_FS));
   CHECK(!radv_pipeline_stages_covered(S_VS | S_FS, L_VS));

   /* GFX9 and later merge the vertex shader into the control slot and the evaluation shader into the
    * geometry slot, so these three slot sets are complete pipelines. */
   CHECK(radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_FS, L_TCS | L_TES | L_FS));
   CHECK(radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_GS | S_FS, L_TCS | L_GS | L_FS));
   CHECK(radv_pipeline_stages_covered(S_VS | S_GS | S_FS, L_GS | L_FS));

   /* Before GFX9 every stage keeps its own slot. */
   CHECK(radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_GS | S_FS, L_VS | L_TCS | L_TES | L_GS | L_FS));

   /* A control shader without its evaluation shader: radv_compute_ia_multi_vgt_param reads
    * radv_get_shader(TESS_EVAL) without a null check. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_FS, L_TCS | L_FS));
   /* A missing fragment shader would draw with no pixel shader. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_GS | S_FS, L_TCS | L_GS));
   CHECK(!radv_pipeline_stages_covered(S_VS | S_FS, L_VS | L_GS));
   /* A missing geometry shader. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_GS | S_FS, L_VS | L_FS));
   /* A missing control shader, with the evaluation slot filled. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_TCS | S_TES | S_FS, L_VS | L_TES | L_FS));

   /* No fragment stage at all: rasterizer discard, or a pre-raster library. The rule asks for
    * nothing in the fragment slot, which is what radv_pipeline_needs_noop_fs() relies on. */
   CHECK(radv_pipeline_stages_covered(S_VS, L_VS));
   CHECK(radv_pipeline_stages_covered(S_VS | S_TCS | S_TES, L_TCS | L_TES));
   /* A library that holds only the fragment stage. */
   CHECK(radv_pipeline_stages_covered(S_FS, L_FS));
   CHECK(!radv_pipeline_stages_covered(S_FS, 0));

   /* A slot the pipeline did not ask for does not make it invalid. */
   CHECK(radv_pipeline_stages_covered(S_VS, L_VS | L_FS | L_GS));

   /* Mesh and task shading. */
   CHECK(radv_pipeline_stages_covered(S_MESH | S_FS, L_MESH | L_FS));
   CHECK(!radv_pipeline_stages_covered(S_MESH | S_FS, L_FS));
   CHECK(radv_pipeline_stages_covered(S_TASK | S_MESH | S_FS, L_TASK | L_MESH | L_FS));
   CHECK(!radv_pipeline_stages_covered(S_TASK | S_MESH | S_FS, L_MESH | L_FS));
   CHECK(!radv_pipeline_stages_covered(S_TASK | S_MESH | S_FS, L_TASK | L_MESH));
   /* The vertex rule never answers for a mesh pipeline. */
   CHECK(!radv_pipeline_stages_covered(S_VS | S_MESH, L_MESH));
}

/* An evaluation stage without a control stage is not a legal Vulkan pipeline, but the rule must still
 * mirror radv_get_shader() rather than guess, so that a cache entry of that shape is refused. */
static void
test_tess_eval_without_control(void)
{
   CHECK(!radv_pipeline_stages_covered(S_TES, L_TES));
   CHECK(!radv_pipeline_stages_covered(S_TES, L_GS));
   CHECK(radv_pipeline_stages_covered(S_TES, L_TCS | L_TES));
}

int
main(int argc, char **argv)
{
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");

   static const struct {
      const char *name;
      void (*fn)(void);
   } tests[] = {
      {"values", test_values},
      {"vertex_slot", test_vertex_slot},
      {"tess_eval_slot", test_tess_eval_slot},
      {"pipelines", test_pipelines},
      {"tess_eval_without_control", test_tess_eval_without_control},
   };

   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      const int before = failures;

      tests[i].fn();
      printf("%s %s\n", failures == before ? "PASS" : "FAIL", tests[i].name);
   }

   printf("stage cover: %u tests, %d checks, %d failed checks%s\n", (unsigned)(sizeof(tests) / sizeof(tests[0])),
          checks, failures, invert ? " (negative control)" : "");

   return failures ? 1 : 0;
}
