/* SPDX-License-Identifier: MIT
 *
 * BD-288. Which push words of a device-generated-commands layout the BC-250 mesh DGC route
 * (radv_bc250_dgc.c) can serve.
 *
 * The route records one ordinary indirect mesh draw per sequence into a capture and patches the
 * push words of each sequence into ONE application constant table, which every graphics stage of the
 * captured draw reads (bc250_dgc_capture sets push_constant_stages to all stages). That is exact only
 * when the token updates the word for every stage that can read it:
 *
 *   - A PUSH_CONSTANT or SEQUENCE_INDEX token updates updateRange.stageFlags only. A word that the
 *     pipeline layout also declares for another graphics stage must keep its old value there, which
 *     one shared table cannot do. So the token's stages must cover every graphics stage that the
 *     pipeline layout's push ranges declare for the word, and without a pipeline layout nothing can
 *     be proved.
 *   - A PUSH_DATA or PUSH_DATA_SEQUENCE_INDEX token (VK_EXT_descriptor_heap) updates push data, and
 *     push data has no per-stage ranges: vkCmdPushDataEXT writes one space that every stage reads
 *     (radv_CmdPushDataEXT stores it in cmd_buffer->push_constants and marks every stage). Such a
 *     layout has no pipeline layout to read (the D3D12 engine passes VK_NULL_HANDLE on its
 *     descriptor-heap path: vkd3d-proton libs/vkd3d/command.c:28946-28947 with the root signature
 *     layouts of state.c:1952-1962), and needs none: the shared table is the push data space.
 *
 * Before BD-288 the route required a pipeline layout for every token, so every D3D12 mesh command
 * signature that changes a root argument (root constant, root CBV/SRV/UAV, incrementing constant)
 * was refused at creation with VK_ERROR_OUT_OF_DEVICE_MEMORY on the engine's descriptor-heap path.
 *
 * A layout that mixes the two token families is refused: the rule above has no proof for it, and
 * no client of this route makes one.
 *
 * Pure functions of their arguments; nothing from Mesa is included, so a host test can run the
 * shipped code.
 */
#ifndef RADV_BC250_DGC_PUSH_H
#define RADV_BC250_DGC_PUSH_H

#include <stdbool.h>
#include <stdint.h>

enum radv_bc250_dgc_push_kind {
   RADV_BC250_DGC_PUSH_NONE = 0,     /* no push token */
   RADV_BC250_DGC_PUSH_CONSTANT = 1, /* PUSH_CONSTANT and SEQUENCE_INDEX tokens only */
   RADV_BC250_DGC_PUSH_DATA = 2,     /* PUSH_DATA and PUSH_DATA_SEQUENCE_INDEX tokens only */
   RADV_BC250_DGC_PUSH_MIXED = 3,    /* both families */
};

/* Adds one token to the kind of a layout. push_constant: the token is PUSH_CONSTANT_EXT or
 * SEQUENCE_INDEX_EXT. push_data: PUSH_DATA_EXT or PUSH_DATA_SEQUENCE_INDEX_EXT. Any other token
 * passes false for both and leaves the kind as it is. */
static inline enum radv_bc250_dgc_push_kind
radv_bc250_dgc_push_kind_add(enum radv_bc250_dgc_push_kind kind, bool push_constant, bool push_data)
{
   return (enum radv_bc250_dgc_push_kind)((unsigned)kind | (push_constant ? RADV_BC250_DGC_PUSH_CONSTANT : 0u) |
                                          (push_data ? RADV_BC250_DGC_PUSH_DATA : 0u));
}

/* May the route serve one push word that a push token of the layout writes?
 * has_layout: the layout names a pipeline layout. token_stages: the token's updateRange.stageFlags.
 * layout_stages: the stages the pipeline layout's push ranges declare for this word (0 without a
 * layout). graphics: the stages of the captured draw (MESH | TASK | FRAGMENT). */
static inline bool
radv_bc250_dgc_push_word_valid(enum radv_bc250_dgc_push_kind kind, bool has_layout, uint32_t token_stages,
                               uint32_t layout_stages, uint32_t graphics)
{
   switch (kind) {
   case RADV_BC250_DGC_PUSH_DATA:
      return true;
   case RADV_BC250_DGC_PUSH_CONSTANT:
      return has_layout && (token_stages & layout_stages & graphics) == (layout_stages & graphics);
   default:
      return false;
   }
}

#endif
