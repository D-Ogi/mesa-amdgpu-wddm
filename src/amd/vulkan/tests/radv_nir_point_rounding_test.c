/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"
#include "amdgfxregs.h"

static const nir_shader_compiler_options options = {0};
static unsigned checks;

#define CHECK(condition) do { \
   checks++; \
   if (!(condition)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #condition); exit(1); } \
} while (0)

struct sample_case {
   const char *name;
   unsigned min_filter, mag_filter;
   float lod;
   int bias256;
   unsigned min256, max256, image_min256, base_level;
   unsigned expected_filter;
   bool expected_trunc;
};

static nir_tex_instr *
create_sample(nir_builder *b, const struct sample_case *c, bool varying, bool sparse)
{
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 4);
   tex->op = nir_texop_txl;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_float32;
   tex->coord_components = 2;
   tex->is_sparse = sparse;
   const unsigned trunc = c->min_filter == 0 && c->mag_filter == 0;
   nir_def *sampler = nir_imm_ivec4(b, S_008F30_TRUNC_COORD(trunc) | S_008F30_CLAMP_X(2),
                                   S_008F34_MIN_LOD_GFX6(c->min256) | S_008F34_MAX_LOD_GFX6(c->max256),
                                   S_008F38_XY_MIN_FILTER(c->min_filter) |
                                   S_008F38_XY_MAG_FILTER(c->mag_filter) |
                                   S_008F38_LOD_BIAS(c->bias256) | S_008F38_MIP_FILTER(2),
                                   S_008F3C_BORDER_COLOR_TYPE(1));
   nir_def *imgwords[8];
   for (unsigned i = 0; i < 8; i++)
      imgwords[i] = nir_imm_int(b, i == 1 ? S_00A004_MIN_LOD(c->image_min256) | 1 :
                                    i == 3 ? S_00A00C_BASE_LEVEL(c->base_level) |
                                             S_00A00C_LAST_LEVEL_GFX10(c->base_level) : 0);
   nir_def *image = nir_vec(b, imgwords, 8);
   nir_def *lod = varying ? nir_u2f32(b, nir_load_local_invocation_index(b)) : nir_imm_float(b, c->lod);
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_vec2(b, 0.49993896484375f, 0.5f));
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_sampler_handle, sampler);
   tex->src[3] = nir_tex_src_for_ssa(nir_tex_src_texture_handle, image);
   nir_def_init(&tex->instr, &tex->def, sparse ? 5 : 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   nir_def *value = sparse ? nir_vec2(b, nir_channel(b, &tex->def, 0), nir_channel(b, &tex->def, 4)) :
                            nir_channel(b, &tex->def, 0);
   nir_store_ssbo(b, value, nir_imm_int(b, 0), nir_imm_int(b, 0),
                  .align_mul = 4, .write_mask = sparse ? 3 : 1);
   return tex;
}

static void
optimize(nir_shader *shader)
{
   bool progress;
   do {
      progress = nir_opt_constant_folding(shader);
      progress |= nir_opt_algebraic(shader);
      progress |= nir_opt_copy_prop(shader);
      progress |= nir_opt_dead_cf(shader);
      progress |= nir_opt_dce(shader);
   } while (progress);
   nir_validate_shader(shader, "point rounding test after optimization");
}

static void
test_constant(const struct sample_case *c, bool sparse)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "%s", c->name);
   create_sample(&b, c, false, sparse);
   CHECK(radv_nir_lower_point_rounding(b.shader, GFX10));
   nir_validate_shader(b.shader, "point rounding test before optimization");
   optimize(b.shader);
   unsigned count = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            count++;
            nir_src sampler = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_sampler_handle));
            CHECK(nir_src_is_const(sampler));
            if (G_008F38_XY_MIN_FILTER(nir_src_comp_as_uint(sampler, 2)) != c->expected_filter)
               nir_print_shader(b.shader, stderr);
            CHECK(G_008F38_XY_MIN_FILTER(nir_src_comp_as_uint(sampler, 2)) == c->expected_filter);
            CHECK(G_008F38_XY_MAG_FILTER(nir_src_comp_as_uint(sampler, 2)) == c->expected_filter);
            CHECK(G_008F30_TRUNC_COORD(nir_src_comp_as_uint(sampler, 0)) == c->expected_trunc);
            CHECK(G_008F30_CLAMP_X(nir_src_comp_as_uint(sampler, 0)) == 2);
            CHECK(G_008F38_MIP_FILTER(nir_src_comp_as_uint(sampler, 2)) == 2);
            CHECK(nir_src_comp_as_uint(sampler, 3) == S_008F3C_BORDER_COLOR_TYPE(1));
            CHECK(tex->is_sparse == sparse);
            nir_src coord = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_coord));
            CHECK(nir_src_comp_as_float(coord, 0) == 0.49993896484375f);
            CHECK(nir_src_comp_as_float(coord, 1) == 0.5f);
            nir_src lod = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_lod));
            CHECK(nir_src_as_float(lod) == c->lod);
         }
      }
   }
   CHECK(count == 1);
   ralloc_free(b.shader);
}

static void
test_varying(void)
{
   const struct sample_case c = {"varying LOD", 0, 1, 0, 0, 0, 4095, 0, 0, 0, true};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "varying LOD");
   nir_tex_instr *original = create_sample(&b, &c, true, true);
   nir_def *original_lod = nir_get_tex_src(original, nir_tex_src_lod);
   CHECK(!radv_nir_lower_point_rounding(b.shader, GFX11));
   CHECK(radv_nir_lower_point_rounding(b.shader, GFX10));
   optimize(b.shader);
   nir_divergence_analysis(b.shader);
   unsigned count = 0, point = 0, linear = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            count++;
            nir_src sampler = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_sampler_handle));
            CHECK(nir_src_is_const(sampler));
            CHECK(!sampler.ssa->divergent);
            CHECK(nir_get_tex_src(tex, nir_tex_src_lod) == original_lod);
            CHECK(original_lod->divergent);
            CHECK(tex->is_sparse && tex->def.num_components == 5);
            unsigned filter = G_008F38_XY_MIN_FILTER(nir_src_comp_as_uint(sampler, 2));
            point += filter == 0;
            linear += filter == 1;
            CHECK(G_008F30_TRUNC_COORD(nir_src_comp_as_uint(sampler, 0)) == (filter == 0));
         }
      }
   }
   CHECK(count == 2 && point == 1 && linear == 1);
   ralloc_free(b.shader);
}

static void
test_implicit(bool biased, bool wqm, float raw_lod, float shader_bias, unsigned expected_filter, bool compute)
{
   /* Sampler bias is already included in QueryLod.y. A nonzero descriptor bias
    * makes the negative-raw-LOD control detect accidentally applying it twice.
    */
   const struct sample_case c = {"implicit LOD", 0, 1, 0, 512, 0, 4095, 0, 0, 0, true};
   nir_builder b = nir_builder_init_simple_shader(compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT,
                                                  &options, "implicit LOD");
   if (compute) {
      b.shader->info.derivative_group = DERIVATIVE_GROUP_LINEAR;
      b.shader->info.workgroup_size[0] = 4;
   }
   nir_tex_instr *original = create_sample(&b, &c, false, false);
   original->op = biased ? nir_texop_txb : nir_texop_tex;
   nir_tex_instr_remove_src(original, nir_tex_instr_src_index(original, nir_tex_src_lod));
   b.cursor = nir_before_instr(&original->instr);
   if (biased)
      nir_tex_instr_add_src(original, nir_tex_src_bias, nir_imm_float(&b, shader_bias));
   nir_def *coord = nir_get_tex_src(original, nir_tex_src_coord);
   if (wqm) {
      coord = nir_strict_wqm_coord_amd(&b, coord, biased ? 4 : 0);
      nir_tex_instr_remove_src(original, nir_tex_instr_src_index(original, nir_tex_src_coord));
      original->coord_components = 0;
      nir_tex_instr_add_src(original, nir_tex_src_backend1, coord);
   }
   CHECK(radv_nir_lower_point_rounding(b.shader, GFX10));
   nir_validate_shader(b.shader, "implicit point rounding");
   unsigned queries = 0, samples = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->op == nir_texop_lod) {
               queries++;
               nir_def *query_coord = nir_get_tex_src(tex, wqm ? nir_tex_src_backend1 : nir_tex_src_coord);
               if (wqm) {
                  nir_intrinsic_instr *query_wqm = nir_instr_as_intrinsic(nir_def_instr(query_coord));
                  nir_intrinsic_instr *sample_wqm = nir_instr_as_intrinsic(nir_def_instr(coord));
                  CHECK(nir_intrinsic_base(query_wqm) == 0);
                  CHECK(query_wqm->src[0].ssa == sample_wqm->src[0].ssa);
               } else {
                  CHECK(query_coord == coord);
               }
               CHECK(!nir_get_tex_src(tex, nir_tex_src_bias));
               CHECK(!tex->is_shadow && !tex->is_sparse);
               b.cursor = nir_before_instr(instr);
               nir_def_replace(&tex->def, nir_imm_vec2(&b, 0.0f, raw_lod));
            } else {
               samples++;
               CHECK(nir_get_tex_src(tex, wqm ? nir_tex_src_backend1 : nir_tex_src_coord) == coord);
               CHECK(tex->op == (biased ? nir_texop_txb : nir_texop_tex));
               if (biased) {
                  nir_src bias = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_bias));
                  CHECK(nir_src_as_float(bias) == shader_bias);
               }
            }
         }
      }
   }
   CHECK(queries == 1 && samples == 3);
   optimize(b.shader);
   samples = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            samples++;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            nir_src sampler = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_sampler_handle));
            CHECK(nir_src_is_const(sampler));
            CHECK(G_008F38_XY_MIN_FILTER(nir_src_comp_as_uint(sampler, 2)) == expected_filter);
            CHECK(G_008F30_TRUNC_COORD(nir_src_comp_as_uint(sampler, 0)) == (expected_filter == 0));
         }
      }
   }
   CHECK(samples == 1);
   ralloc_free(b.shader);
}

static void
test_wqm_prefix(unsigned operands)
{
   const struct sample_case c = {"WQM prefix", 0, 1, 0, 0, 0, 4095, 0, 0, 0, true};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "WQM prefix");
   nir_def *quad_coord = nir_load_frag_coord_xy(&b);
   const unsigned prefix = util_bitcount(operands) * 4;
   nir_def *sample_coord = nir_strict_wqm_coord_amd(&b, quad_coord, prefix);
   nir_instr *producer = nir_def_instr(sample_coord);
   /* Model a sample nested inside descriptor/control-flow lowering. The query
    * allocation must remain at the original dominating quad-valued producer.
    * This test retains the real query; it does not substitute a constant LOD.
    */
   nir_push_if(&b, nir_flt_imm(&b, nir_channel(&b, quad_coord, 0), 8.0f));
   nir_tex_instr *original = create_sample(&b, &c, false, false);
   original->op = operands & 1 ? nir_texop_txb : nir_texop_tex;
   nir_tex_instr_remove_src(original, nir_tex_instr_src_index(original, nir_tex_src_lod));
   nir_tex_instr_remove_src(original, nir_tex_instr_src_index(original, nir_tex_src_coord));
   original->coord_components = 0;
   nir_tex_instr_add_src(original, nir_tex_src_backend1, sample_coord);
   b.cursor = nir_before_instr(&original->instr);
   nir_def *bias = nir_imm_float(&b, 0.75f);
   nir_def *compare = nir_imm_float(&b, 0.375f);
   nir_def *offset = nir_imm_ivec2(&b, 1, -1);
   if (operands & 1)
      nir_tex_instr_add_src(original, nir_tex_src_bias, bias);
   if (operands & 2) {
      original->is_shadow = true;
      nir_tex_instr_add_src(original, nir_tex_src_comparator, compare);
   }
   if (operands & 4)
      nir_tex_instr_add_src(original, nir_tex_src_backend2, offset);
   nir_pop_if(&b, NULL);
   CHECK(radv_nir_lower_point_rounding(b.shader, GFX10));
   nir_validate_shader(b.shader, "WQM prefixes 0/4/8/12 retained query");
   nir_metadata_require(nir_shader_get_entrypoint(b.shader), nir_metadata_dominance);
   unsigned queries = 0, samples = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            nir_def *coord = nir_get_tex_src(tex, nir_tex_src_backend1);
            nir_intrinsic_instr *wqm = nir_instr_as_intrinsic(nir_def_instr(coord));
            CHECK(wqm->src[0].ssa == quad_coord);
            if (tex->op == nir_texop_lod) {
               queries++;
               CHECK(nir_intrinsic_base(wqm) == 0);
               CHECK(wqm->instr.block == producer->block);
               CHECK(nir_block_dominates(wqm->instr.block, tex->instr.block));
               CHECK(wqm->instr.block != tex->instr.block);
               CHECK(!nir_get_tex_src(tex, nir_tex_src_bias));
               CHECK(!nir_get_tex_src(tex, nir_tex_src_comparator));
               CHECK(!nir_get_tex_src(tex, nir_tex_src_backend2));
               CHECK(!tex->is_shadow);
               CHECK((coord == sample_coord) == (prefix == 0));
            } else {
               samples++;
               CHECK(coord == sample_coord);
               CHECK(nir_intrinsic_base(wqm) == prefix);
               CHECK(nir_get_tex_src(tex, nir_tex_src_bias) == (operands & 1 ? bias : NULL));
               CHECK(nir_get_tex_src(tex, nir_tex_src_comparator) == (operands & 2 ? compare : NULL));
               CHECK(nir_get_tex_src(tex, nir_tex_src_backend2) == (operands & 4 ? offset : NULL));
               CHECK(tex->is_shadow == !!(operands & 2));
            }
         }
      }
   }
   CHECK(queries == 1 && samples == 3);
   ralloc_free(b.shader);
}

static void
test_gradient(float dx0, float dx1, float dy0, float dy1, int bias256,
              unsigned expected_filter, bool mixed, bool half)
{
   const struct sample_case c = {"gradient LOD", mixed ? 0 : 1, 1, 0,
                                 bias256, 0, 4095, 0, 0, 0, true};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "gradient LOD");
   nir_tex_instr *original = create_sample(&b, &c, false, true);
   original->op = nir_texop_txd;
   nir_tex_instr_remove_src(original, nir_tex_instr_src_index(original, nir_tex_src_lod));
   b.cursor = nir_before_instr(&original->instr);
   nir_def *dx = nir_imm_vec2(&b, dx0, dx1);
   nir_def *dy = nir_imm_vec2(&b, dy0, dy1);
   if (half) {
      dx = nir_f2f16(&b, dx);
      dy = nir_f2f16(&b, dy);
      nir_def *coord = nir_f2f16(&b, nir_get_tex_src(original, nir_tex_src_coord));
      nir_src_rewrite(&original->src[nir_tex_instr_src_index(original, nir_tex_src_coord)].src, coord);
   }
   nir_tex_instr_add_src(original, nir_tex_src_ddx, dx);
   nir_tex_instr_add_src(original, nir_tex_src_ddy, dy);
   nir_def *words[8];
   for (unsigned i = 0; i < 8; i++)
      words[i] = nir_imm_int(&b, i == 1 ? S_00A004_WIDTH_LO(3) | 1 :
                                i == 2 ? S_00A008_HEIGHT(7) : 0);
   nir_src_rewrite(&original->src[nir_tex_instr_src_index(original, nir_tex_src_texture_handle)].src,
                   nir_vec(&b, words, 8));
   CHECK(radv_nir_lower_point_rounding(b.shader, GFX10));
   nir_validate_shader(b.shader, "gradient point rounding");
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               CHECK(tex->op == nir_texop_txd);
               CHECK(nir_get_tex_src(tex, nir_tex_src_ddx) == dx);
               CHECK(nir_get_tex_src(tex, nir_tex_src_ddy) == dy);
               CHECK(nir_get_tex_src(tex, nir_tex_src_coord)->bit_size == (half ? 16 : 32));
            }
         }
      }
   }
   optimize(b.shader);
   unsigned samples = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            samples++;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            nir_src sampler = nir_src_for_ssa(nir_get_tex_src(tex, nir_tex_src_sampler_handle));
            CHECK(nir_src_is_const(sampler));
            CHECK(G_008F38_XY_MIN_FILTER(nir_src_comp_as_uint(sampler, 2)) == expected_filter);
            CHECK(G_008F30_TRUNC_COORD(nir_src_comp_as_uint(sampler, 0)) == (expected_filter == 0));
            CHECK(tex->is_sparse && tex->def.num_components == 5);
            /* NIR's standard constant folding may turn all-zero gradients
             * into an explicit zero LOD, independently of this pass.
             */
            if (dx0 == 0 && dx1 == 0 && dy0 == 0 && dy1 == 0) {
               CHECK(tex->op == nir_texop_txl);
            } else {
               CHECK(tex->op == nir_texop_txd);
               if (!half) {
                  CHECK(nir_get_tex_src(tex, nir_tex_src_ddx) == dx);
                  CHECK(nir_get_tex_src(tex, nir_tex_src_ddy) == dy);
               } else {
                  CHECK(nir_get_tex_src(tex, nir_tex_src_ddx)->bit_size == 16);
                  CHECK(nir_get_tex_src(tex, nir_tex_src_ddy)->bit_size == 16);
               }
            }
            CHECK(G_008F38_LOD_BIAS(nir_src_comp_as_uint(sampler, 2)) == (unsigned)bias256);
         }
      }
   }
   CHECK(samples == 1);
   ralloc_free(b.shader);
}

int main(void)
{
   glsl_type_singleton_init_or_ref();
   const struct sample_case cases[] = {
      {"min point", 0, 1, 1, 0, 0, 4095, 0, 0, 0, true},
      {"mag linear", 0, 1, -1, 0, 0, 4095, 0, 0, 1, false},
      {"mag point", 1, 0, -1, 0, 0, 4095, 0, 0, 0, true},
      {"min linear", 1, 0, 1, 0, 0, 4095, 0, 0, 1, false},
      {"zero mag", 0, 1, 0, 0, 0, 4095, 0, 0, 1, false},
      {"positive bias", 0, 1, -1, 512, 0, 4095, 0, 0, 0, true},
      {"negative bias", 0, 1, 1, -512, 0, 4095, 0, 0, 1, false},
      {"sampler min", 0, 1, -1, 0, 256, 4095, 0, 0, 0, true},
      {"sampler max", 0, 1, 1, 0, 0, 0, 0, 0, 1, false},
      {"min overrides max", 0, 1, 1, 0, 256, 0, 0, 0, 0, true},
      {"image min relative", 0, 1, -1, 0, 0, 4095, 768, 2, 0, true},
      {"image min below base", 0, 1, -1, 0, 0, 4095, 256, 2, 1, false},
      {"point unchanged", 0, 0, 1, 0, 0, 4095, 0, 0, 0, true},
      {"linear unchanged", 1, 1, 1, 0, 0, 4095, 0, 0, 1, false},
      {"aniso unchanged", 3, 3, 1, 0, 0, 4095, 0, 0, 3, false},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      test_constant(&cases[i], false);
      test_constant(&cases[i], true);
   }
   test_varying();
   for (unsigned operands = 0; operands < 8; operands++)
      test_wqm_prefix(operands);
   for (unsigned wqm = 0; wqm < 2; wqm++) {
      test_implicit(false, wqm, 0.25f, 0.0f, 0, false);
      test_implicit(false, wqm, -1.0f, 0.0f, 1, false);
      test_implicit(true, wqm, -1.0f, 2.0f, 0, false);
      test_implicit(true, wqm, 1.0f, -2.0f, 1, false);
   }
   test_implicit(false, false, 0.25f, 0.0f, 0, true);
   test_implicit(true, false, -1.0f, 2.0f, 0, true);
   test_gradient(0.5f, 0, 0, 0.125f, 0, 0, true, false);
   test_gradient(0.125f, 0, 0, 0.0625f, 0, 1, true, false);
   test_gradient(0, 0, 0, 0, 0, 1, true, false);
   test_gradient(0.25f, 0, 0.25f, 0, 0, 1, true, false);
   test_gradient(0.125f, 0, 0, 0.0625f, 512, 0, true, false);
   test_gradient(0.5f, 0, 0, 0.125f, 512, 1, false, false);
   test_gradient(0.5f, 0, 0, 0.125f, 0, 0, true, true);
   glsl_type_singleton_decref();
   printf("PASS point-rounding NIR: %u checks, %u constant cases, varying sparse LOD, implicit/bias WQM/compute, gradients32/16\n", checks,
          (unsigned)ARRAY_SIZE(cases) * 2);
   return 0;
}
