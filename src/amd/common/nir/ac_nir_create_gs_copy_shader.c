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

/* Gives the position export of a capture copy shader the Layer, ViewportIndex,
 * ClipDistance and CullDistance values of the rasterized stream. Streams 1-3
 * keep these builtins in private slots (ac_nir_lower_gs_stream_positions).
 * When one stream writes Layer or ViewportIndex, every stream exports it, with
 * zero when the stream does not write it. When a stream 1-3 writes clip or cull
 * distances, every stream exports the largest clip count of all streams, then
 * the largest cull count, with zero for a distance that the stream does not
 * write: radv_nir_shader_info_pass programs the same layout. Returns the slots
 * that the position export must read. */
static uint64_t
ac_nir_gs_alias_raster_builtins(nir_builder *b, const nir_shader *gs_nir, unsigned stream,
                                ac_nir_prerast_out *out)
{
   const struct shader_info *info = &gs_nir->info;
   uint64_t slots = 0;

   const struct {
      unsigned slot, component, mask;
   } misc[] = {
      {VARYING_SLOT_LAYER, 0, info->gs.stream_layer_mask},
      {VARYING_SLOT_VIEWPORT, 1, info->gs.stream_viewport_mask},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(misc); i++) {
      if (!(info->outputs_written & BITFIELD64_BIT(misc[i].slot)) && !misc[i].mask)
         continue;
      nir_def *value = out->outputs[misc[i].slot][0];
      if (stream) {
         value = (misc[i].mask & BITFIELD_BIT(stream)) ?
            out->outputs[AC_NIR_GS_STREAM_MISC_SLOT(stream)][misc[i].component] : NULL;
      }
      out->outputs[misc[i].slot][0] = value ? value : nir_imm_int(b, 0);
      out->infos[misc[i].slot].as_sysval_mask |= 1;
      slots |= BITFIELD64_BIT(misc[i].slot);
   }

   const uint8_t *sizes = info->gs.stream_clip_cull;
   if (!sizes[1] && !sizes[2] && !sizes[3])
      return slots;
   unsigned max_clip = 0, max_cull = 0;
   for (unsigned s = 0; s < 4; s++) {
      max_clip = MAX2(max_clip, sizes[s] & 15);
      max_cull = MAX2(max_cull, sizes[s] >> 4);
   }
   const unsigned clip = sizes[stream] & 15, cull = sizes[stream] >> 4;
   const unsigned source = stream ? AC_NIR_GS_STREAM_CLIP_SLOT(stream) : VARYING_SLOT_CLIP_DIST0;
   nir_def *values[8] = {0};
   for (unsigned i = 0; i < max_clip + max_cull; i++) {
      const bool is_clip = i < max_clip;
      const unsigned index = is_clip ? i : clip + i - max_clip;
      const bool written = is_clip ? i < clip : i - max_clip < cull;
      nir_def *value = written ? out->outputs[source + index / 4][index % 4] : NULL;
      values[i] = value ? value : nir_imm_float(b, 0);
   }
   for (unsigned i = 0; i < 8; i++) {
      out->outputs[VARYING_SLOT_CLIP_DIST0 + i / 4][i % 4] = values[i];
      if (values[i]) {
         out->infos[VARYING_SLOT_CLIP_DIST0 + i / 4].as_sysval_mask |= BITFIELD_BIT(i % 4);
         slots |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST0 + i / 4);
      }
   }
   return slots;
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
         /* Without a rasterized stream VGT_STRMOUT_CONFIG.RAST_STREAM stays 0 and the rasterizer discards
          * the primitives, but the vertices of stream 0 still need their position export, as in the copy
          * shader without the capture. A copy shader that exported no position hung the GPU (TDR). */
         nir_def *raster_stream = nir_bcsel(&b, nir_ult_imm(&b, selected, 4), selected, zero);
         nir_push_if(&b, nir_ieq_imm(&b, raster_stream, stream));
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
         static const unsigned aliased_slots[] = {
            VARYING_SLOT_LAYER, VARYING_SLOT_VIEWPORT, VARYING_SLOT_CLIP_DIST0, VARYING_SLOT_CLIP_DIST1,
         };
         nir_def *saved_outputs[ARRAY_SIZE(aliased_slots)][4];
         ac_nir_prerast_per_output_info saved_infos[ARRAY_SIZE(aliased_slots)];
         uint64_t raster_outputs = stream_outputs | VARYING_BIT_POS;
         if (b.shader->info.bc250_pgp_so_capture) {
            for (unsigned i = 0; i < ARRAY_SIZE(aliased_slots); i++) {
               memcpy(saved_outputs[i], out->outputs[aliased_slots[i]], sizeof(saved_outputs[i]));
               saved_infos[i] = out->infos[aliased_slots[i]];
            }
            raster_outputs |= ac_nir_gs_alias_raster_builtins(&b, gs_nir, stream, out);
         }
         unsigned clip_mask = 0;
         for (unsigned c = 0; c < 8; c++) {
            if (out->outputs[VARYING_SLOT_CLIP_DIST0 + c / 4][c % 4])
               clip_mask |= BITFIELD_BIT(c);
         }
         ac_nir_export_position(&b, options->gfx_level, options->export_clipdist_mask & clip_mask, false,
                                options->write_pos_to_clipvertex,
                                !(options->has_param_exports || b.shader->info.bc250_pgp_so_capture),
                                options->force_vrs, raster_outputs,
                                out, NULL);

         memcpy(out->outputs[VARYING_SLOT_POS], saved_position, sizeof(saved_position));
         if (b.shader->info.bc250_pgp_so_capture) {
            for (unsigned i = 0; i < ARRAY_SIZE(aliased_slots); i++) {
               memcpy(out->outputs[aliased_slots[i]], saved_outputs[i], sizeof(saved_outputs[i]));
               out->infos[aliased_slots[i]] = saved_infos[i];
            }
         }

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
