/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"
#include "radv_pipeline.h"
#include "radv_shader.h"

static const nir_shader_compiler_options options = {0};
static unsigned checks, failures;
#define CHECK(c)                                                                                                       \
   do {                                                                                                                \
      checks++;                                                                                                        \
      if (!(c)) {                                                                                                      \
         failures++;                                                                                                   \
         if (failures < 12)                                                                                            \
            fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #c);                                                       \
      }                                                                                                                \
   } while (0)

/* Evaluate the actual pass output over a 2x2 quad. The three input planes are
 * affine (I/W, J/W, 1/W); the oracle independently evaluates the planes at the
 * requested position and divides there. Unsupported IR aborts rather than
 * silently treating it as a model of the pass.
 */
struct quad {
   float plane[3][3]; /* constant, dx, dy */
   float offset[4][2];
   unsigned samples;
};
static const int sample8[8][2] = {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
static float eval(nir_def *d, unsigned component, unsigned lane, const struct quad *q);

static float
plane(const struct quad *q, unsigned c, double x, double y)
{
   return q->plane[c][0] + q->plane[c][1] * x + q->plane[c][2] * y;
}

static float
src(nir_alu_instr *a, unsigned s, unsigned c, unsigned lane, const struct quad *q)
{
   return eval(a->src[s].src.ssa, a->src[s].swizzle[c], lane, q);
}

static float
eval(nir_def *d, unsigned c, unsigned lane, const struct quad *q)
{
   nir_instr *i = nir_def_instr(d);
   if (i->type == nir_instr_type_load_const)
      return nir_instr_as_load_const(i)->value[c].f32;
   if (i->type == nir_instr_type_alu) {
      nir_alu_instr *a = nir_instr_as_alu(i);
      switch (a->op) {
      case nir_op_mov:
         return src(a, 0, c, lane, q);
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         return src(a, c, 0, lane, q);
      case nir_op_fadd:
         return src(a, 0, c, lane, q) + src(a, 1, c, lane, q);
      case nir_op_fmul:
         return src(a, 0, c, lane, q) * src(a, 1, c, lane, q);
      case nir_op_frcp:
         return 1.0f / src(a, 0, c, lane, q);
      default:
         fprintf(stderr, "unsupported ALU %s\n", nir_op_infos[a->op].name);
         abort();
      }
   }
   if (i->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(i);
      switch (in->intrinsic) {
      case nir_intrinsic_ddx_fine:
         return eval(in->src[0].ssa, c, lane | 1, q) - eval(in->src[0].ssa, c, lane & ~1, q);
      case nir_intrinsic_ddy_fine:
         return eval(in->src[0].ssa, c, lane | 2, q) - eval(in->src[0].ssa, c, lane & ~2, q);
      case nir_intrinsic_load_input:
         return q->offset[lane][c];
      case nir_intrinsic_load_barycentric_model:
         return plane(q, c, lane & 1, lane >> 1);
      case nir_intrinsic_load_sample_positions_amd: {
         unsigned sample = nir_src_as_uint(in->src[0]);
         return 0.5f + sample8[sample][c] / 16.0f;
      }
      case nir_intrinsic_load_barycentric_pixel:
         return plane(q, c, lane & 1, lane >> 1) / plane(q, 2, lane & 1, lane >> 1);
      case nir_intrinsic_load_barycentric_at_offset: {
         /* Unchanged affine path, and exact old smooth coarse-quad algorithm
          * for the negative control built from the unpatched production pass. */
         bool linear = nir_intrinsic_interp_mode(in) == INTERP_MODE_NOPERSPECTIVE;
         float v[4];
         for (unsigned l = 0; l < 4; l++)
            v[l] = plane(q, c, l & 1, l >> 1) / (linear ? 1.0f : plane(q, 2, l & 1, l >> 1));
         return v[lane] + (v[1] - v[0]) * eval(in->src[0].ssa, 0, lane, q) +
                (v[2] - v[0]) * eval(in->src[0].ssa, 1, lane, q);
      }
      default:
         fprintf(stderr, "unsupported intrinsic %s\n", nir_intrinsic_infos[in->intrinsic].name);
         abort();
      }
   }
   abort();
}

static unsigned
count(nir_shader *s, nir_intrinsic_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl (impl, s)
      nir_foreach_block (b, impl)
         nir_foreach_instr (i, b)
            if (i->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(i)->intrinsic == op)
               n++;
   return n;
}

static nir_intrinsic_instr *
make_shader(nir_builder *b, enum glsl_interp_mode mode, bool at_sample, unsigned sample, bool divergent)
{
   *b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "perspective-offset");
   nir_def *offset = nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = 0, .range = 1, .dest_type = nir_type_float32);
   if (divergent)
      nir_push_if(b, nir_flt_imm(b, nir_channel(b, offset, 0), 0.0f));
   nir_def *ij = at_sample ? nir_load_barycentric_at_sample(b, 32, nir_imm_int(b, sample), .interp_mode = mode)
                           : nir_load_barycentric_at_offset(b, 32, offset, .interp_mode = mode);
   nir_intrinsic_instr *store =
      nir_store_ssbo(b, ij, nir_imm_int(b, 0), nir_imm_int(b, 0), .write_mask = 3, .align_mul = 4);
   if (divergent)
      nir_pop_if(b, NULL);
   return store;
}

static void
numeric(enum glsl_interp_mode mode, bool at_sample, unsigned sample, bool equal_w)
{
   unsigned start_checks = checks, start_failures = failures;
   nir_builder b;
   nir_intrinsic_instr *store = make_shader(&b, mode, at_sample, sample, false);
   struct radv_graphics_state_key key = {0};
   key.ms.rasterization_samples = 8;
   bool changed = radv_nir_lower_fs_barycentric(b.shader, &key, 3, false);
   bool smooth = mode != INTERP_MODE_NOPERSPECTIVE;
   CHECK(changed == smooth);
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_model) == (smooth ? 1 : 0));
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_at_offset) == (smooth || at_sample ? 0 : 1));
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   CHECK(BITSET_TEST(b.shader->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL) == smooth);
   CHECK(b.shader->info.fs.needs_coarse_quad_helper_invocations == smooth);
   struct quad q = {.plane = {{0.2f, 0.11f, -0.07f}, {0.3f, -0.03f, 0.13f}, {1.0f, 0.19f, -0.12f}}, .samples = 8};
   if (equal_w)
      q.plane[2][1] = q.plane[2][2] = 0;
   /* Every snapped offset in the D3D range, all quad parities, lane-varying
    * offsets. The sample case tests the native8 own-offset audit invariant. */
   for (int ox = -8; ox < 8; ox++)
      for (int oy = -8; oy < 8; oy++) {
         for (unsigned l = 0; l < 4; l++) {
            q.offset[l][0] = ((ox + (int)l + 8) % 16 - 8) / 16.0f;
            q.offset[l][1] = ((oy + 2 * (int)l + 8) % 16 - 8) / 16.0f;
         }
         for (unsigned l = 0; l < 4; l++)
            for (unsigned c = 0; c < 2; c++) {
               double x = (l & 1) + (at_sample ? sample8[sample][0] / 16.0 : q.offset[l][0]);
               double y = (l >> 1) + (at_sample ? sample8[sample][1] / 16.0 : q.offset[l][1]);
               double numerator = q.plane[c][0] + q.plane[c][1] * x + q.plane[c][2] * y;
               double denominator = smooth ? q.plane[2][0] + q.plane[2][1] * x + q.plane[2][2] * y : 1.0;
               CHECK(fabs(eval(store->src[0].ssa, c, l, &q) - numerator / denominator) < 1e-5);
            }
      }
   printf("numeric mode=%u sample=%d equal_w=%u: %u checks, %u failures\n", mode, at_sample ? (int)sample : -1, equal_w,
          checks - start_checks, failures - start_failures);
   ralloc_free(b.shader);
}

static void
gradients_at_entry(nir_shader *shader)
{
   nir_block *entry = nir_start_block(nir_shader_get_entrypoint(shader));
   unsigned gradients = 0;
   nir_foreach_function_impl (impl, shader)
      nir_foreach_block (block, impl)
         nir_foreach_instr (i, block) {
            if (i->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(i);
            if (in->intrinsic == nir_intrinsic_ddx_fine || in->intrinsic == nir_intrinsic_ddy_fine) {
               gradients++;
               CHECK(block == entry);
            }
         }
   CHECK(gradients == 6);
}

static void
io_pipeline(bool at_sample, bool divergent, bool terminate_first)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "interp-deref-offset");
   nir_variable *var = nir_variable_create(b.shader, nir_var_shader_in, glsl_vec2_type(), "varying");
   var->data.location = VARYING_SLOT_VAR0;
   var->data.interpolation = INTERP_MODE_SMOOTH;
   nir_def *condition = nir_load_front_face(&b, 1);
   if (terminate_first)
      nir_terminate_if(&b, condition);
   if (divergent)
      nir_push_if(&b, condition);
   nir_def *deref = &nir_build_deref_var(&b, var)->def;
   nir_def *v = at_sample ? nir_interp_deref_at_sample(&b, 2, 32, deref, nir_imm_int(&b, 3))
                          : nir_interp_deref_at_offset(&b, 2, 32, deref, nir_imm_vec2(&b, -0.375f, 0.25f));
   nir_store_ssbo(&b, v, nir_imm_int(&b, 0), nir_imm_int(&b, 0), .write_mask = 3, .align_mul = 4);
   if (divergent)
      nir_pop_if(&b, NULL);
   struct radv_graphics_state_key key = {0};
   key.ms.rasterization_samples = 8;
   /* The real ordering: explicit barycentrics, IO lowering, ordinary barycentrics. */
   CHECK(!radv_nir_lower_fs_barycentric(b.shader, &key, 3, false));
   radv_nir_lower_io(b.shader);
   CHECK(count(b.shader,
               at_sample ? nir_intrinsic_load_barycentric_at_sample : nir_intrinsic_load_barycentric_at_offset) == 1);
   CHECK(radv_nir_lower_fs_barycentric(b.shader, &key, 3, false));
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_at_offset) == 0);
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_at_sample) == 0);
   gradients_at_entry(b.shader);
   if (terminate_first) {
      unsigned gradients = 0;
      nir_foreach_instr (i, nir_start_block(nir_shader_get_entrypoint(b.shader))) {
         if (i->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_op op = nir_instr_as_intrinsic(i)->intrinsic;
         gradients += op == nir_intrinsic_ddx_fine || op == nir_intrinsic_ddy_fine;
         if (op == nir_intrinsic_terminate_if)
            CHECK(gradients == 6);
      }
   }
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   CHECK(BITSET_TEST(b.shader->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL));
   CHECK(b.shader->info.fs.needs_coarse_quad_helper_invocations);
   CHECK(b.shader->info.fs.needs_full_quad_helper_invocations);
   nir_validate_shader(b.shader, "interp deref pipeline");
   ralloc_free(b.shader);
}

static void
structure(bool dynamic, unsigned samples, bool divergent, unsigned vertices)
{
   nir_builder b;
   make_shader(&b, INTERP_MODE_SMOOTH, true, 3, divergent);
   struct radv_graphics_state_key key = {0};
   key.dynamic_rasterization_samples = dynamic;
   key.ms.rasterization_samples = samples;
   CHECK(radv_nir_lower_fs_barycentric(b.shader, &key, vertices, false));
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_at_sample) == 0);
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_at_offset) == 0);
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_model) == (dynamic || samples ? 1 : 0));
   CHECK(count(b.shader, nir_intrinsic_load_barycentric_pixel) == (dynamic || !samples ? 1 : 0));
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   CHECK(BITSET_TEST(b.shader->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL) == (dynamic || samples));
   if (dynamic || samples)
      gradients_at_entry(b.shader);
   nir_validate_shader(b.shader, "perspective offset structural test");
   ralloc_free(b.shader);
}

int
main(int argc, char **argv)
{
   glsl_type_singleton_init_or_ref();
   for (unsigned equal = 0; equal < 2; equal++) {
      numeric(INTERP_MODE_SMOOTH, false, 0, equal);
      numeric(INTERP_MODE_NONE, false, 0, equal);
      numeric(INTERP_MODE_NOPERSPECTIVE, false, 0, equal);
      if (argc == 1)
         for (unsigned s = 0; s < 8; s++)
            numeric(INTERP_MODE_SMOOTH, true, s, equal);
   }
   for (unsigned v = 0; argc == 1 && v < 4; v++)
      for (unsigned d = 0; d < 2; d++) {
         structure(false, 0, d, v);
         structure(false, 8, d, v);
         structure(true, 0, d, v);
      }
   if (argc == 1)
      for (unsigned s = 0; s < 2; s++)
         for (unsigned d = 0; d < 2; d++)
            for (unsigned t = 0; t < 2; t++)
               io_pipeline(s, d, t);
   glsl_type_singleton_decref();
   printf("perspective offset: %u checks, %u failures\n", checks, failures);
   return failures != 0;
}
