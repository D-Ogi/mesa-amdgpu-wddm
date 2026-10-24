/* SPDX-License-Identifier: MIT */

#include "nir.h"
#include "nir_builder.h"
#include "amdgfxregs.h"
#include "radv_nir.h"

/* GFX10's TRUNC_COORD also truncates linear-filter coordinates. Mixed image
 * filters therefore need two uniform sampler variants and a result select.
 * The per-lane LOD must never enter a sampler handle: ACO expects that handle
 * to be uniform after descriptor waterfalls.
 *
 * This experimental pass covers explicit LOD/gradients and implicit/bias modes
 * in fragment shaders or shaders with declared derivative groups.
 * It does not authorize
 * PointSamplingAddressesNeverRoundUp until all sample modes are implemented
 * and verified on the target. Pure point descriptors use the existing sampler
 * path; linear-only and anisotropic descriptors are left intact.
 */

static nir_def *
field(nir_builder *b, nir_def *desc, unsigned index, uint32_t mask)
{
   return nir_ubfe_imm(b, nir_channel(b, desc, index), ffs(mask) - 1, util_bitcount(mask));
}

static nir_def *
fixed8(nir_builder *b, nir_def *value)
{
   return nir_fmul_imm(b, nir_u2f32(b, value), 1.0f / 256.0f);
}

static nir_def *
query_raw_lod(nir_builder *b, nir_tex_instr *original)
{
   nir_tex_instr *query = nir_tex_instr_create(b->shader, original->num_srcs);
   unsigned src_count = 0;
   query->op = nir_texop_lod;
   query->sampler_dim = original->sampler_dim;
   query->is_array = original->is_array;
   query->array_is_lowered_cube = original->array_is_lowered_cube;
   query->coord_components = original->coord_components;
   query->dest_type = nir_type_float32;
   query->texture_non_uniform = original->texture_non_uniform;
   query->sampler_non_uniform = original->sampler_non_uniform;
   query->texture_index = original->texture_index;
   query->sampler_index = original->sampler_index;
   for (unsigned i = 0; i < original->num_srcs; i++) {
      switch (original->src[i].src_type) {
      case nir_tex_src_backend1: {
         nir_def *coord = original->src[i].src.ssa;
         nir_intrinsic_instr *wqm = nir_instr_as_intrinsic(nir_def_instr(coord));
         assert(wqm->intrinsic == nir_intrinsic_strict_wqm_coord_amd);
         if (nir_intrinsic_base(wqm)) {
            /* backend1 is a linear VGPR allocation, including reserved words
             * for offset/bias/comparator before the coordinates. QueryLod has
             * none of those operands, so it needs a zero-prefix allocation.
             * Use the original quad-valued input at its original dominating
             * WQM point, never recreate it inside the descriptor waterfall.
             * Leave the sample's allocation and operand packing untouched.
             */
            nir_builder top = nir_builder_at(nir_before_instr(&wqm->instr));
            coord = nir_strict_wqm_coord_amd(&top, wqm->src[0].ssa, 0);
         }
         query->src[src_count++] = nir_tex_src_for_ssa(nir_tex_src_backend1, coord);
         break;
      }
      case nir_tex_src_coord:
      case nir_tex_src_texture_handle:
      case nir_tex_src_sampler_handle:
         query->src[src_count++] = nir_tex_src_for_ssa(original->src[i].src_type, original->src[i].src.ssa);
         break;
      default:
         break;
      }
   }
   query->num_srcs = src_count;
   nir_def_init(&query->instr, &query->def, 2, 32);
   nir_builder_instr_insert(b, &query->instr);
   /* The x component can be rounded to a mip index. The y component retains
    * continuous LOD and includes sampler bias, but not the sample's Bias operand.
    * Carry original quad values into the query without recomputing them.
    */
   return nir_channel(b, &query->def, 1);
}

static nir_def *
gradient_lod(nir_builder *b, nir_tex_instr *tex, nir_def *sampler, nir_def *image)
{
   nir_def *dx = nir_f2f32(b, nir_get_tex_src(tex, nir_tex_src_ddx));
   nir_def *dy = nir_f2f32(b, nir_get_tex_src(tex, nir_tex_src_ddy));
   const unsigned dimensions = dx->num_components;
   nir_def *width = nir_iadd(b, field(b, image, 1, ~C_00A004_WIDTH_LO),
                             nir_ishl_imm(b, field(b, image, 2, ~C_00A008_WIDTH_HI), 2));
   nir_def *height = field(b, image, 2, ~C_00A008_HEIGHT);
   nir_def *depth = field(b, image, 4, ~C_00A010_DEPTH_GFX10);
   nir_def *base = field(b, image, 3, ~C_00A00C_BASE_LEVEL);
   nir_def *size[3] = {tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ? height : width, height, depth};
   nir_def *unnormalized = nir_test_mask(b, nir_channel(b, sampler, 0), S_008F30_FORCE_UNNORMALIZED(1));
   for (unsigned i = 0; i < dimensions; i++) {
      size[i] = nir_umax_imm(b, nir_ushr(b, nir_iadd_imm(b, size[i], 1), base), 1);
      size[i] = nir_bcsel(b, unnormalized, nir_imm_float(b, 1.0f), nir_u2f32(b, size[i]));
   }
   nir_def *scale = nir_vec(b, size, dimensions);
   dx = nir_fmul(b, dx, scale);
   dy = nir_fmul(b, dy, scale);

   /* The max-gradient-length approximation follows nir_lower_tex's TXD LOD
    * formula and the allowed per-axis scale factor bounds in Vulkan's texture
    * chapter. Normalization avoids squaring large gradients.
    * For cubes ac_nir_lower_tex_coords has already projected both gradients
    * into face space, including the factor of 1/2.
    *
    * This is ONLY the filter-selection predicate. Both samples retain their
    * original explicit gradients and hardware mip/LOD calculations. Matching
    * this predicate to GFX10's min/mag decision near zero remains a target
    * validation gate; the feature cap must not be enabled on these IR tests.
    */
   nir_def *magnitude = nir_imm_float(b, 0.0f);
   for (unsigned i = 0; i < dimensions; i++) {
      magnitude = nir_fmax(b, magnitude, nir_fabs(b, nir_channel(b, dx, i)));
      magnitude = nir_fmax(b, magnitude, nir_fabs(b, nir_channel(b, dy, i)));
   }
   nir_def *divisor = nir_bcsel(b, nir_feq_imm(b, magnitude, 0.0f), nir_imm_float(b, 1.0f), magnitude);
   dx = nir_fdiv(b, dx, divisor);
   dy = nir_fdiv(b, dy, divisor);
   nir_def *a = nir_fdot(b, dx, dx);
   nir_def *c = nir_fdot(b, dy, dy);
   nir_def *major_squared = nir_fmax(b, a, c);
   nir_def *lod = nir_fadd(b, nir_flog2(b, magnitude), nir_fmul_imm(b, nir_flog2(b, major_squared), 0.5f));
   return nir_bcsel(b, nir_feq_imm(b, magnitude, INFINITY), nir_imm_float(b, INFINITY), lod);
}

static nir_def *
effective_lod(nir_builder *b, nir_tex_instr *tex, nir_def *sampler, nir_def *image)
{
   nir_def *lod;
   if (tex->op == nir_texop_txl || tex->op == nir_texop_txd) {
      lod = tex->op == nir_texop_txd ? gradient_lod(b, tex, sampler, image) :
                                    nir_f2f32(b, nir_get_tex_src(tex, nir_tex_src_lod));
      nir_def *bias = nir_ibfe_imm(b, nir_channel(b, sampler, 2),
                                  ffs(~C_008F38_LOD_BIAS) - 1,
                                  util_bitcount(~C_008F38_LOD_BIAS));
      lod = nir_fadd(b, lod, nir_fmul_imm(b, nir_i2f32(b, bias), 1.0f / 256.0f));
   } else {
      lod = query_raw_lod(b, tex);
      nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
      if (bias)
         lod = nir_fadd(b, lod, nir_f2f32(b, bias));
   }

   nir_def *min_lod = fixed8(b, field(b, sampler, 1, ~C_008F34_MIN_LOD_GFX6));
   nir_def *max_lod = fixed8(b, field(b, sampler, 1, ~C_008F34_MAX_LOD_GFX6));
   nir_def *instruction_min = nir_get_tex_src(tex, nir_tex_src_min_lod);
   if (instruction_min)
      min_lod = nir_fmax(b, min_lod, nir_f2f32(b, instruction_min));

   /* D3D gives MinLOD precedence if the clamps cross. An image-view MinLOD is
    * absolute, whereas sampler and instruction clamps are relative to BASE_LEVEL.
    * LAST_LEVEL does not decide minification: a one-mip view can still minify.
    */
   lod = nir_fmax(b, min_lod, nir_fmin(b, max_lod, lod));
   nir_def *image_min = fixed8(b, field(b, image, 1, ~C_00A004_MIN_LOD));
   nir_def *base_level = nir_u2f32(b, field(b, image, 3, ~C_00A00C_BASE_LEVEL));
   return nir_fmax(b, lod, nir_fsub(b, image_min, base_level));
}

static nir_def *
sampler_variant(nir_builder *b, nir_def *sampler, bool point)
{
   nir_def *word0 = nir_iand_imm(b, nir_channel(b, sampler, 0), C_008F30_TRUNC_COORD);
   if (point)
      word0 = nir_ior_imm(b, word0, S_008F30_TRUNC_COORD(1));
   const unsigned filter = point ? V_008F38_SQ_TEX_XY_FILTER_POINT : V_008F38_SQ_TEX_XY_FILTER_BILINEAR;
   nir_def *word2 = nir_iand_imm(b, nir_channel(b, sampler, 2),
                               C_008F38_XY_MIN_FILTER & C_008F38_XY_MAG_FILTER);
   word2 = nir_ior_imm(b, word2, S_008F38_XY_MIN_FILTER(filter) | S_008F38_XY_MAG_FILTER(filter));
   return nir_vec4(b, word0, nir_channel(b, sampler, 1), word2, nir_channel(b, sampler, 3));
}

static nir_def *
emit_sample(nir_builder *b, const nir_tex_instr *original, nir_def *sampler)
{
   nir_tex_instr *tex = nir_instr_as_tex(nir_instr_clone(b->shader, &original->instr));
   nir_builder_instr_insert(b, &tex->instr);
   nir_src_rewrite(&tex->src[nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle)].src, sampler);
   return &tex->def;
}

static bool
lower_point_rounding(nir_builder *b, nir_tex_instr *tex, void *unused)
{
   if (tex->op == nir_texop_txl) {
      if (!nir_get_tex_src(tex, nir_tex_src_lod))
         return false;
   } else if (tex->op == nir_texop_txd) {
      nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
      nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx);
      nir_def *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
      if (!coord || !dx || !dy || dx->num_components != dy->num_components ||
          dx->num_components > 3)
         return false;
   } else if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb) ||
              (b->shader->info.stage != MESA_SHADER_FRAGMENT &&
               b->shader->info.derivative_group == DERIVATIVE_GROUP_NONE)) {
      return false;
   }

   nir_def *sampler = nir_get_tex_src(tex, nir_tex_src_sampler_handle);
   nir_def *image = nir_get_tex_src(tex, nir_tex_src_texture_handle);
   if (!sampler || !image || sampler->num_components != 4 || image->num_components != 8)
      return false;

   b->cursor = nir_before_instr(&tex->instr);
   nir_def *min_filter = field(b, sampler, 2, ~C_008F38_XY_MIN_FILTER);
   nir_def *mag_filter = field(b, sampler, 2, ~C_008F38_XY_MAG_FILTER);
   nir_def *min_point = nir_ieq_imm(b, min_filter, V_008F38_SQ_TEX_XY_FILTER_POINT);
   nir_def *mag_point = nir_ieq_imm(b, mag_filter, V_008F38_SQ_TEX_XY_FILTER_POINT);
   nir_def *min_linear = nir_ieq_imm(b, min_filter, V_008F38_SQ_TEX_XY_FILTER_BILINEAR);
   nir_def *mag_linear = nir_ieq_imm(b, mag_filter, V_008F38_SQ_TEX_XY_FILTER_BILINEAR);
   nir_def *mixed = nir_ior(b, nir_iand(b, min_point, mag_linear), nir_iand(b, min_linear, mag_point));

   /* The branch depends only on already-waterfalled sampler state. Coordinates,
    * LOD and the point/linear result selection can vary independently per lane.
    */
   nir_push_if(b, mixed);
   nir_def *lod = effective_lod(b, tex, sampler, image);
   nir_def *is_minifying = nir_flt(b, nir_imm_float(b, 0.0f), lod);
   nir_def *want_point = nir_bcsel(b, is_minifying, min_point, mag_point);
   nir_def *point = emit_sample(b, tex, sampler_variant(b, sampler, true));
   nir_def *linear = emit_sample(b, tex, sampler_variant(b, sampler, false));
   /* Select every component, including sparse residency, from the same sample. */
   nir_def *mixed_result = nir_bcsel(b, want_point, point, linear);
   nir_push_else(b, NULL);
   nir_def *unchanged = emit_sample(b, tex, sampler);
   nir_pop_if(b, NULL);
   nir_def *result = nir_if_phi(b, mixed_result, unchanged);
   nir_def_replace(&tex->def, result);
   return true;
}

bool
radv_nir_lower_point_rounding(nir_shader *shader, enum amd_gfx_level gfx_level)
{
   if (gfx_level != GFX10 && gfx_level != GFX10_3)
      return false;
   return nir_shader_tex_pass(shader, lower_point_rounding, nir_metadata_none, NULL);
}
