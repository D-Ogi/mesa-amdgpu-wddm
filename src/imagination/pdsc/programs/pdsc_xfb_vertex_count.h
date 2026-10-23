/*
 * Copyright © 2026 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "common/pvr_iface.h"
#include "pdsc/pdsc.h"
#include "pdsc/pdsc_internal.h"

static inline pdsc_program *
pdsc_xfb_vertex_count_gen(void *mem_ctx, enum pdsc_feature features)
{
   pdsc_program *p = pdsc_program_create(mem_ctx, features, "xfb_vertex_count");

   ALLOC_CONST64(p,
                 counter_offset,
                 PVR_XFB_VERTEX_COUNT_PATCH_ID_NEGATIVE_COUNTER_OFFSET);
   /* Reciprocal of vertex_stride patched as ceil(2^32 / stride) by the CPU.
    * Used in the MAD-based multiply-high division below.
    */
   ALLOC_CONST32(p,
                 vertex_stride_recip,
                 PVR_XFB_VERTEX_COUNT_PATCH_ID_VERTEX_STRIDE_RECIP);
   ALLOC_CONST32(p,
                 instance_count,
                 PVR_XFB_VERTEX_COUNT_PATCH_ID_INSTANCE_COUNT);
   ALLOC_CONST32(p,
                 first_instance,
                 PVR_XFB_VERTEX_COUNT_PATCH_ID_FIRST_INSTANCE);

   ALLOC_TEMP64(p, tzero);
   pdsc_limm(p, pdsc_ref_lo32(tzero), pdsc_ref_imm(0));
   pdsc_limm(p, pdsc_ref_hi32(tzero), pdsc_ref_imm(0));

   ALLOC_TEMP64_ARRAY(p, counter_buffer_value, 1);
   ALLOC_TEMP32_ARRAY(p, draw_params, 4);
   ALLOC_TEMP64(p, working_temp);

   pdsc_ld(p,
           counter_buffer_value,
           PVR_XFB_VERTEX_COUNT_REF_ID_COUNTER_BUFFER_LD_DEST,
           PVR_XFB_VERTEX_COUNT_PATCH_ID_COUNTER_BUFFER_LD_SRC0,
           false);
   pdsc_wdf(p);

   pdsc_cmp(p, tzero, counter_offset, .cop = PDSC_COP_NE);
   pdsc_setc(p, PDSC_PRED_P0);
   pdsc_add64(p,
              counter_buffer_value,
              counter_buffer_value,
              counter_offset,
              .cc = true,
              .sna = true);

   pdsc_mov32(p,
              pdsc_ref_elem32(draw_params, 0),
              pdsc_ref_lo32(counter_buffer_value));

   /* Divide draw_params[0] (byte count) by vertex_stride using magic-number
    * multiply: floor(n / stride) = hi32(n * ceil(2^32 / stride)).
    * pdsc_idiv is not a real HW instruction; use MAD + hi32 instead.
    * (same pattern as pdsc_xfb_end.h counter-stride multiplication)
    */
   pdsc_mad(p,
            working_temp,
            pdsc_ref_elem32(draw_params, 0),
            vertex_stride_recip,
            tzero);
   pdsc_mov32(p, pdsc_ref_elem32(draw_params, 0), pdsc_ref_hi32(working_temp));

   pdsc_mov32(p, pdsc_ref_elem32(draw_params, 1), instance_count);
   pdsc_limm(p, pdsc_ref_elem32(draw_params, 2), pdsc_ref_imm(0));
   pdsc_mov32(p, pdsc_ref_elem32(draw_params, 3), first_instance);

   pdsc_st(p,
           pdsc_ref_elem32(draw_params, 0),
           PVR_XFB_VERTEX_COUNT_REF_ID_OUTPUT_COUNT_BUFFER_ST_SRC0,
           PVR_XFB_VERTEX_COUNT_PATCH_ID_OUTPUT_COUNT_BUFFER_ST_SRC0,
           false);
   pdsc_wdf(p);

   pdsc_halt(p);

   return p;
}
