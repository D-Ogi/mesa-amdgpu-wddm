/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "ac_nir_helpers.h"
#include "ac_pgp_so.h"
#include "amdgfxregs.h"

#include "nir_builder.h"
#include "nir_xfb_info.h"

static void
ac_nir_export_gs_raster_parameters(nir_builder *b, unsigned stream,
                                    const nir_xfb_info *info, ac_nir_prerast_out *out)
{
   nir_def *table = nir_load_streamout_buffer_amd(b, .base = 4);
   nir_def *zero = nir_imm_int(b, 0);
   for (unsigned reg = 0; reg < 32; reg++) {
      nir_def *values[4];
      for (unsigned component = 0; component < 4; component++) {
         nir_def *source = nir_load_buffer_amd(b, 1, 32, table, zero, zero, zero,
            .base = offsetof(struct bc250_pgp_so_table, raster_source) + 4 * (reg * 4 + component),
            .access = ACCESS_CAN_REORDER | ACCESS_NON_WRITEABLE, .align_mul = 4);
         nir_def *value = zero;
         for (unsigned i = 0; i < info->output_count; i++) {
            const nir_xfb_output_info *o = &info->outputs[i];
            if (info->buffer_to_stream[o->buffer] != stream)
               continue;
            u_foreach_bit(c, o->component_mask) {
               nir_def *candidate = out->outputs[o->location][c];
               if (!candidate)
                  continue;
               if (o->data_is_16bit) {
                  candidate = o->high_16bits ? nir_unpack_32_2x16_split_y(b, candidate)
                                            : nir_unpack_32_2x16_split_x(b, candidate);
                  if (o->mediump)
                     candidate = nir_convert_to_bit_size(b, candidate, o->mediump_upconvert_type, 32);
               }
               unsigned index = o->buffer * 128 + o->offset / 4 + c - o->component_offset;
               value = nir_bcsel(b, nir_ieq_imm(b, source, index), candidate, value);
            }
         }
         values[component] = value;
      }
      nir_export_amd(b, nir_vec(b, values, 4), .target = V_008DFC_SQ_EXP_PARAM + reg,
                     .enabled_channels = 15);
   }
}

nir_shader *
ac_nir_create_gs_copy_shader(const nir_shader *gs_nir, ac_nir_lower_legacy_gs_options *options,
                             ac_nir_prerast_out *out)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, gs_nir->options, "gs_copy");

   b.shader->info.api_subgroup_size = 64;
   b.shader->info.max_subgroup_size = 64;
   b.shader->info.min_subgroup_size = 64;
   b.shader->info.outputs_written = gs_nir->info.outputs_written;
   b.shader->info.bc250_pgp_so_capture = gs_nir->info.bc250_pgp_so_capture;
   b.shader->info.outputs_written_16bit = gs_nir->info.outputs_written_16bit;

   nir_def *gsvs_ring = nir_load_ring_gsvs_amd(&b);

   nir_xfb_info *info = ac_nir_get_sorted_xfb_info(gs_nir);
   nir_def *stream_id = NULL;
   if (!options->disable_streamout && info)
      stream_id = nir_ubfe_imm(&b, nir_load_streamout_config_amd(&b), 24, 2);

   nir_def *vtx_offset = nir_imul_imm(&b, nir_load_vertex_id_zero_base(&b), 4);
   nir_def *zero = nir_imm_zero(&b, 1, 32);

   for (unsigned stream = 0; stream < 4; stream++) {
      if (stream > 0 && (!stream_id || !(info->streams_written & BITFIELD_BIT(stream))))
         continue;

      if (stream_id)
         nir_push_if(&b, nir_ieq_imm(&b, stream_id, stream));

      unsigned offset = 0;
      uint64_t stream_outputs = 0;
      uint16_t stream_outputs_16bit = 0;
      /* Each stream is a separate control-flow branch. Do not let an SSA value
       * from an earlier stream escape its defining branch through exports. */
      memset(out->outputs, 0, sizeof(out->outputs));

      u_foreach_bit64_two_masks(i, gs_nir->info.outputs_written,
                                VARYING_SLOT_VAR0_16BIT, gs_nir->info.outputs_written_16bit) {
         u_foreach_bit (j, out->infos[i].components_mask) {
            if (((out->infos[i].stream >> (j * 2)) & 0x3) != stream)
               continue;

            if (i >= VARYING_SLOT_VAR0_16BIT)
               stream_outputs_16bit |= BITFIELD_BIT(i - VARYING_SLOT_VAR0_16BIT);
            else
               stream_outputs |= BITFIELD64_BIT(i);

            if (ac_nir_is_const_output(out, i, j)) {
               out->outputs[i][j] = ac_nir_get_const_output(&b, out, i, j);
               continue;
            }

            unsigned base = offset * gs_nir->info.gs.vertices_out * 16;
            out->outputs[i][j] =
               nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                   .base = base,
                                   .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL |
                                             ACCESS_CAN_REORDER | ACCESS_CAN_SPECULATE);
            offset += 4;
         }
      }

      if (stream_id)
         ac_nir_emit_legacy_streamout(&b, stream, info, out);

      /* This should be after streamout and before exports. */
      ac_nir_clamp_vertex_color_outputs(&b, out);

      if (b.shader->info.bc250_pgp_so_capture) {
         nir_def *table = nir_load_streamout_buffer_amd(&b, .base = 4);
         nir_def *selected = nir_load_buffer_amd(&b, 1, 32, table, zero, zero, zero,
            .base = offsetof(struct bc250_pgp_so_table, rasterized_stream),
            .access = ACCESS_CAN_REORDER | ACCESS_NON_WRITEABLE, .align_mul = 4);
         nir_push_if(&b, nir_ieq_imm(&b, selected, stream));
      }
      if (stream == 0 || b.shader->info.bc250_pgp_so_capture) {
         nir_def *saved_position[4];
         memcpy(saved_position, out->outputs[VARYING_SLOT_POS], sizeof(saved_position));
         const unsigned position_slot = VARYING_SLOT_TEX4 + stream;
         if (stream && (gs_nir->info.outputs_written & BITFIELD64_BIT(position_slot))) {
            /* Streamout has already consumed the original locations. Only the
             * raster position export aliases the selected stream's position. */
            memcpy(out->outputs[VARYING_SLOT_POS], out->outputs[position_slot], sizeof(saved_position));
         }
         unsigned clip_mask = 0;
         for (unsigned c = 0; c < 8; c++) {
            if (out->outputs[VARYING_SLOT_CLIP_DIST0 + c / 4][c % 4])
               clip_mask |= BITFIELD_BIT(c);
         }
         ac_nir_export_position(&b, options->gfx_level, options->export_clipdist_mask & clip_mask, false,
                                options->write_pos_to_clipvertex,
                                !(options->has_param_exports || b.shader->info.bc250_pgp_so_capture),
                                options->force_vrs, stream_outputs | VARYING_BIT_POS,
                                out, NULL);

         memcpy(out->outputs[VARYING_SLOT_POS], saved_position, sizeof(saved_position));

         if (b.shader->info.bc250_pgp_so_capture && info) {
            ac_nir_export_gs_raster_parameters(&b, stream, info, out);
         } else if (options->has_param_exports) {
            ac_nir_export_parameters(&b, options->param_offsets,
                                     stream_outputs,
                                     stream_outputs_16bit,
                                     out);
         }
      }

      if (b.shader->info.bc250_pgp_so_capture)
         nir_pop_if(&b, NULL);

      if (stream_id)
         nir_push_else(&b, NULL);
   }

   b.shader->info.clip_distance_array_size = gs_nir->info.clip_distance_array_size;
   b.shader->info.cull_distance_array_size = gs_nir->info.cull_distance_array_size;

   /* The copy shader goes directly to ABI lowering and native compilation. */
   if (b.shader->info.bc250_pgp_so_capture) {
      nir_lower_vars_to_ssa(b.shader);
      nir_opt_dce(b.shader);
   }

   return b.shader;
}
