/* SPDX-License-Identifier: MIT
 *
 * BD-288: the push-word rule of the BC-250 mesh DGC route (radv_bc250_dgc_push.h), run as shipped.
 *
 * The first case is the layout that vkd3d-proton makes on its descriptor-heap path for the D3D12
 * command signature of test_mesh_shader_execute_indirect_state (CONSTANT on root parameter 0,
 * UNORDERED_ACCESS_VIEW on root parameter 1, DISPATCH_MESH): two PUSH_DATA tokens, one draw mesh
 * token and no pipeline layout. Before BD-288 the route refused it, and CreateCommandSignature
 * returned E_OUTOFMEMORY. The other cases keep the push-constant rule of BD-119 as it was.
 */
#include <stdio.h>
#include <string.h>

#include "radv_bc250_dgc_push.h"

/* VkShaderStageFlagBits values (vulkan_core.h). */
#define STAGE_FRAGMENT 0x00000010u
#define STAGE_COMPUTE  0x00000020u
#define STAGE_TASK     0x00000040u
#define STAGE_MESH     0x00000080u
#define STAGE_ALL      0x7FFFFFFFu
#define GRAPHICS       (STAGE_MESH | STAGE_TASK | STAGE_FRAGMENT)

/* The token kinds this test needs, as the layout loop of radv_dgc.c classifies them. */
enum token { TOKEN_PUSH_CONSTANT, TOKEN_SEQUENCE_INDEX, TOKEN_PUSH_DATA, TOKEN_PUSH_DATA_SEQUENCE_INDEX,
             TOKEN_DRAW_MESH_TASKS };

struct push_token {
   enum token type;
   unsigned offset_B, size_B; /* updateRange.offset, updateRange.size */
   uint32_t stages;           /* updateRange.stageFlags */
};

static int checks, failures;

static void
check(int ok, const char *what)
{
   checks++;
   if (!ok) {
      failures++;
      printf("FAIL %s\n", what);
   }
}

/* The loop of radv_CreateIndirectCommandsLayoutEXT (radv_dgc.c) over this layout: the kind of every
 * token first, then every word of every push token. layout_stages == NULL means no pipeline layout. */
static int
layout_valid(const struct push_token *t, unsigned n, const uint32_t *layout_stages)
{
   enum radv_bc250_dgc_push_kind kind = RADV_BC250_DGC_PUSH_NONE;
   for (unsigned i = 0; i < n; i++)
      kind = radv_bc250_dgc_push_kind_add(kind,
         t[i].type == TOKEN_PUSH_CONSTANT || t[i].type == TOKEN_SEQUENCE_INDEX,
         t[i].type == TOKEN_PUSH_DATA || t[i].type == TOKEN_PUSH_DATA_SEQUENCE_INDEX);
   int valid = 1;
   for (unsigned i = 0; i < n; i++) {
      if (t[i].type == TOKEN_DRAW_MESH_TASKS)
         continue;
      for (unsigned word = t[i].offset_B / 4; word < (t[i].offset_B + t[i].size_B) / 4; word++)
         if (!radv_bc250_dgc_push_word_valid(kind, layout_stages != NULL, t[i].stages,
                                             layout_stages ? layout_stages[word] : 0, GRAPHICS))
            valid = 0;
   }
   return valid;
}

int
main(void)
{
   /* 1. BD-288, the vkd3d-proton descriptor-heap layout of the _execute_indirect_state signature:
    * the root constant and the root UAV address as push data (vk_push_stages is VK_SHADER_STAGE_ALL on
    * that path), then the mesh draw. No pipeline layout. */
   const struct push_token heap[] = {
      {TOKEN_PUSH_DATA, 16, 4, STAGE_ALL},
      {TOKEN_PUSH_DATA, 0, 8, STAGE_ALL},
      {TOKEN_DRAW_MESH_TASKS, 0, 0, 0},
   };
   check(layout_valid(heap, 3, NULL) == 1, "BD-288 heap layout without a pipeline layout is served");

   /* 2. The same signature with an incrementing constant: PUSH_DATA_SEQUENCE_INDEX is push data too. */
   const struct push_token heap_si[] = {
      {TOKEN_PUSH_DATA_SEQUENCE_INDEX, 20, 4, STAGE_ALL},
      {TOKEN_PUSH_DATA, 0, 8, STAGE_ALL},
      {TOKEN_DRAW_MESH_TASKS, 0, 0, 0},
   };
   check(layout_valid(heap_si, 3, NULL) == 1, "push-data sequence index is served");

   /* 3. Push data needs no pipeline layout, and one that is given does not change the answer, even
    * when the token names fewer stages (push data has no per-stage ranges). */
   uint32_t ranges[64];
   for (unsigned i = 0; i < 64; i++)
      ranges[i] = STAGE_MESH | STAGE_FRAGMENT;
   const struct push_token heap_mesh_only[] = {{TOKEN_PUSH_DATA, 0, 4, STAGE_MESH}};
   check(layout_valid(heap_mesh_only, 1, ranges) == 1, "push data with a pipeline layout is served");
   check(layout_valid(heap_mesh_only, 1, NULL) == 1, "push data naming one stage, no layout, is served");

   /* 4. BD-119 push constants, unchanged: covered words pass, a partial-stage update and a missing
    * pipeline layout are refused. */
   const struct push_token pc_all[] = {{TOKEN_PUSH_CONSTANT, 0, 8, STAGE_MESH | STAGE_FRAGMENT}};
   check(layout_valid(pc_all, 1, ranges) == 1, "push constants covering the layout's stages are served");
   const struct push_token pc_wide[] = {{TOKEN_PUSH_CONSTANT, 0, 8, STAGE_ALL}};
   check(layout_valid(pc_wide, 1, ranges) == 1, "push constants naming more stages than declared are served");
   const struct push_token pc_part[] = {{TOKEN_PUSH_CONSTANT, 0, 8, STAGE_MESH}};
   check(layout_valid(pc_part, 1, ranges) == 0, "a push constant for MESH of a MESH|FRAGMENT word is refused");
   check(layout_valid(pc_all, 1, NULL) == 0, "push constants without a pipeline layout are refused");
   const struct push_token si_part[] = {{TOKEN_SEQUENCE_INDEX, 4, 4, STAGE_FRAGMENT}};
   check(layout_valid(si_part, 1, ranges) == 0, "a sequence index for FRAGMENT of a MESH|FRAGMENT word is refused");
   /* A non-graphics stage in the layout does not count: the captured draw has none. */
   uint32_t with_compute[64];
   for (unsigned i = 0; i < 64; i++)
      with_compute[i] = STAGE_MESH | STAGE_COMPUTE;
   const struct push_token pc_mesh[] = {{TOKEN_PUSH_CONSTANT, 0, 4, STAGE_MESH}};
   check(layout_valid(pc_mesh, 1, with_compute) == 1, "a compute stage in the layout is ignored");

   /* 5. Both families in one layout: no proof, refused with or without a pipeline layout. */
   const struct push_token mixed[] = {{TOKEN_PUSH_DATA, 0, 4, STAGE_ALL}, {TOKEN_PUSH_CONSTANT, 4, 4, STAGE_ALL}};
   check(layout_valid(mixed, 2, ranges) == 0, "a mixed layout with a pipeline layout is refused");
   check(layout_valid(mixed, 2, NULL) == 0, "a mixed layout without a pipeline layout is refused");

   /* 6. The kind arithmetic itself. */
   enum radv_bc250_dgc_push_kind k = RADV_BC250_DGC_PUSH_NONE;
   check(radv_bc250_dgc_push_kind_add(k, false, false) == RADV_BC250_DGC_PUSH_NONE, "a draw token adds nothing");
   k = radv_bc250_dgc_push_kind_add(k, false, true);
   check(k == RADV_BC250_DGC_PUSH_DATA, "push data");
   check(radv_bc250_dgc_push_kind_add(k, false, true) == RADV_BC250_DGC_PUSH_DATA, "push data twice");
   check(radv_bc250_dgc_push_kind_add(k, true, false) == RADV_BC250_DGC_PUSH_MIXED, "push data then a push constant");
   check(radv_bc250_dgc_push_kind_add(RADV_BC250_DGC_PUSH_NONE, true, false) == RADV_BC250_DGC_PUSH_CONSTANT,
         "push constant");
   check(!radv_bc250_dgc_push_word_valid(RADV_BC250_DGC_PUSH_NONE, true, STAGE_ALL, GRAPHICS, GRAPHICS),
         "no push kind is never served");
   check(!radv_bc250_dgc_push_word_valid(RADV_BC250_DGC_PUSH_MIXED, true, STAGE_ALL, GRAPHICS, GRAPHICS),
         "a mixed word is never served");

   printf("radv_bc250_dgc_push_test: checks=%d, failures=%d\n", checks, failures);
   return failures ? 1 : 0;
}
