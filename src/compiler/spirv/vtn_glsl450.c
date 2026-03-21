/*
 * Copyright © 2015 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <math.h>

#include "nir/nir_builtin_builder.h"

#include "vtn_private.h"
#include "GLSL.std.450.h"

#ifndef M_PIf
#define M_PIf   ((float) M_PI)
#endif
#ifndef M_PI_2f
#define M_PI_2f ((float) M_PI_2)
#endif
#ifndef M_PI_4f
#define M_PI_4f ((float) M_PI_4)
#endif

static nir_def *build_det(nir_builder *b, nir_def **col, unsigned cols);

/* Computes the determinate of the submatrix given by taking src and
 * removing the specified row and column.
 */
static nir_def *
build_mat_subdet(struct nir_builder *b, struct nir_def **src,
                 unsigned size, unsigned row, unsigned col)
{
   assert(row < size && col < size);
   if (size == 2) {
      return nir_channel(b, src[1 - col], 1 - row);
   } else {
      /* Swizzle to get all but the specified row */
      unsigned swiz[NIR_MAX_VEC_COMPONENTS] = {0};
      for (unsigned j = 0; j < 3; j++)
         swiz[j] = j + (j >= row);

      /* Grab all but the specified column */
      nir_def *subcol[3];
      for (unsigned j = 0; j < size; j++) {
         if (j != col) {
            subcol[j - (j > col)] = nir_swizzle(b, src[j], swiz, size - 1);
         }
      }

      return build_det(b, subcol, size - 1);
   }
}

static nir_def *
build_det(nir_builder *b, nir_def **col, unsigned size)
{
   assert(size <= 4);
   nir_def *subdet[4];
   for (unsigned i = 0; i < size; i++)
      subdet[i] = build_mat_subdet(b, col, size, i, 0);

   nir_def *prod = nir_fmul(b, col[0], nir_vec(b, subdet, size));

   nir_def *result = NULL;
   for (unsigned i = 0; i < size; i += 2) {
      nir_def *term;
      if (i + 1 < size) {
         term = nir_fsub(b, nir_channel(b, prod, i),
                            nir_channel(b, prod, i + 1));
      } else {
         term = nir_channel(b, prod, i);
      }

      result = result ? nir_fadd(b, result, term) : term;
   }

   return result;
}

static nir_def *
build_mat_det(struct vtn_builder *b, struct vtn_ssa_value *src)
{
   unsigned size = vtn_type_get_matrix_columns(src->type);

   nir_def *cols[4];
   for (unsigned i = 0; i < size; i++)
      cols[i] = src->elems[i]->def;

   return build_det(&b->nb, cols, size);
}

static struct vtn_ssa_value *
matrix_inverse(struct vtn_builder *b, struct vtn_ssa_value *src)
{
   nir_def *adj_col[4];
   unsigned size = vtn_type_get_matrix_columns(src->type);

   nir_def *cols[4];
   for (unsigned i = 0; i < size; i++)
      cols[i] = src->elems[i]->def;

   /* Build up an adjugate matrix */
   for (unsigned c = 0; c < size; c++) {
      nir_def *elem[4];
      for (unsigned r = 0; r < size; r++) {
         elem[r] = build_mat_subdet(&b->nb, cols, size, c, r);

         if ((r + c) % 2)
            elem[r] = nir_fneg(&b->nb, elem[r]);
      }

      adj_col[c] = nir_vec(&b->nb, elem, size);
   }

   nir_def *det_inv = nir_frcp(&b->nb, build_det(&b->nb, cols, size));

   struct vtn_ssa_value *val = vtn_create_ssa_value(b, src->type);
   for (unsigned i = 0; i < size; i++)
      val->elems[i]->def = nir_fmul(&b->nb, adj_col[i], det_inv);

   return val;
}

static nir_op
vtn_nir_alu_op_for_spirv_glsl_opcode(struct vtn_builder *b,
                                     enum GLSLstd450 opcode,
                                     unsigned execution_mode,
                                     unsigned *extra_fp_math_ctrl)
{
   *extra_fp_math_ctrl = nir_fp_fast_math;
   switch (opcode) {
   case GLSLstd450NMin:
   case GLSLstd450NMax:
      *extra_fp_math_ctrl |= nir_fp_preserve_nan;
      FALLTHROUGH;
   case GLSLstd450FMax:
   case GLSLstd450FMin: {
      /* We don't have to preserve infinities according to the VK spec,
       * but games break without it. Both Unity and Unreal Engine
       * are affected.
       */
      *extra_fp_math_ctrl |= nir_fp_preserve_inf;
      if (b->options->workarounds.force_nan_preserve_min_max)
         *extra_fp_math_ctrl |= nir_fp_preserve_nan;
      switch (opcode) {
      case GLSLstd450FMin:
      case GLSLstd450NMin: return nir_op_fmin;
      case GLSLstd450FMax:
      case GLSLstd450NMax: return nir_op_fmax;
      default: UNREACHABLE("unhandled");
      }
   }
   case GLSLstd450Fma:
      if (b->options->workarounds.force_exact_glsl_fma)
         *extra_fp_math_ctrl |= nir_fp_exact;
      return nir_op_ffma_weak;
   case GLSLstd450Round:         return nir_op_fround_even;
   case GLSLstd450RoundEven:     return nir_op_fround_even;
   case GLSLstd450Trunc:         return nir_op_ftrunc;
   case GLSLstd450FAbs:          return nir_op_fabs;
   case GLSLstd450SAbs:          return nir_op_iabs;
   case GLSLstd450FSign:         return nir_op_fsign;
   case GLSLstd450SSign:         return nir_op_isign;
   case GLSLstd450Floor:         return nir_op_ffloor;
   case GLSLstd450Ceil:          return nir_op_fceil;
   case GLSLstd450Fract:         return nir_op_ffract;
   case GLSLstd450Sin:           return nir_op_fsin;
   case GLSLstd450Cos:           return nir_op_fcos;
   case GLSLstd450Pow:           return nir_op_fpow;
   case GLSLstd450Exp2:          return nir_op_fexp2;
   case GLSLstd450Log2:          return nir_op_flog2;
   case GLSLstd450Sqrt:          return nir_op_fsqrt;
   case GLSLstd450InverseSqrt:   return nir_op_frsq;
   case GLSLstd450UMin:          return nir_op_umin;
   case GLSLstd450SMin:          return nir_op_imin;
   case GLSLstd450UMax:          return nir_op_umax;
   case GLSLstd450SMax:          return nir_op_imax;
   case GLSLstd450FMix:          return nir_op_flrp;
   case GLSLstd450FindILsb:      return nir_op_find_lsb;
   case GLSLstd450FindSMsb:      return nir_op_ifind_msb;
   case GLSLstd450FindUMsb:      return nir_op_ufind_msb;

   default:
      vtn_fail("No NIR equivalent");
   }
}

#define NIR_IMM_FP(n, v) (nir_imm_floatN_t(n, v, src[0]->bit_size))

static void
handle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,
                   const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   struct vtn_handler_ctx h = {0};

   const bool mediump_16bit =
      vtn_value_needs_mediump_16bit(b, vtn_untyped_value(b, w[2]));

   unsigned num_inputs = count - 5;
   vtn_handler_prepare(b, &h, w[1], w[2], &w[5], num_inputs);
   h.mediump_16bit = mediump_16bit;
   vtn_handler_start(b, &h);

   nir_def *src[3] = { NULL, };
   vtn_fail_if(num_inputs > ARRAY_SIZE(src),
               "GLSL.std.450 instructions can have at most %zu operands",
               ARRAY_SIZE(src));
   for (unsigned i = 0; i < num_inputs; i++)
      src[i] = h.srcs[i]->def;

   nir_def *def = NULL;

   switch (entrypoint) {
   case GLSLstd450Radians:
      def = nir_radians(nb, src[0]);
      break;
   case GLSLstd450Degrees:
      def = nir_degrees(nb, src[0]);
      break;
   case GLSLstd450Tan:
      def = nir_ftan(nb, src[0]);
      break;

   case GLSLstd450Step: {
      /* The SPIR-V Extended Instructions for GLSL spec says:
       *
       *    Result is 0.0 if x < edge; otherwise result is 1.0.
       *
       * Here src[1] is x, and src[0] is edge.  The direct implementation is
       *
       *    bcsel(src[1] < src[0], 0.0, 1.0)
       *
       * This is effectively b2f(!(src1 < src0)).  Previously this was
       * implemented using sge(src1, src0), but that produces incorrect
       * results for NaN.  Instead, we use the identity b2f(!x) = 1 - b2f(x).
       */
      const unsigned save_math_ctrl = nb->fp_math_ctrl;
      nb->fp_math_ctrl |= nir_fp_preserve_nan | nir_fp_preserve_inf;

      nir_def *cmp = nir_slt(nb, src[1], src[0]);

      nb->fp_math_ctrl = save_math_ctrl;
      def = nir_fsub_imm(nb, 1.0f, cmp);
      break;
   }

   case GLSLstd450Exp:
      def = nir_fexp(nb, src[0]);
      break;

   case GLSLstd450Log:
      def = nir_flog(nb, src[0]);
      break;

   case GLSLstd450FClamp: {
      /* We don't have to preserve infinities according to the VK spec,
       * but games break without it. Both Unity and Unreal Engine
       * are affected.
       */
      const unsigned save_math_ctrl = nb->fp_math_ctrl;
      b->nb.fp_math_ctrl = nir_fp_preserve_inf;
      if (b->options->workarounds.force_nan_preserve_min_max)
         b->nb.fp_math_ctrl |= nir_fp_preserve_nan;

      def = nir_fclamp(nb, src[0], src[1], src[2]);

      nb->fp_math_ctrl = save_math_ctrl;
      break;
   }
   case GLSLstd450NClamp: {
      const unsigned save_math_ctrl = nb->fp_math_ctrl;
      nb->fp_math_ctrl |= nir_fp_preserve_nan | nir_fp_preserve_inf;

      def = nir_fclamp(nb, src[0], src[1], src[2]);

      nb->fp_math_ctrl = save_math_ctrl;
      break;
   }
   case GLSLstd450UClamp:
      def = nir_uclamp(nb, src[0], src[1], src[2]);
      break;
   case GLSLstd450SClamp:
      def = nir_iclamp(nb, src[0], src[1], src[2]);
      break;

   case GLSLstd450SmoothStep: {
      def = nir_smoothstep(nb, src[0], src[1], src[2]);
      break;
   }

   case GLSLstd450Sinh:
      /* 0.5 * (e^x - e^(-x)) */
      def =
         nir_fmul_imm(nb, nir_fsub(nb, nir_fexp(nb, src[0]),
                                       nir_fexp(nb, nir_fneg(nb, src[0]))),
                          0.5f);
      break;

   case GLSLstd450Cosh:
      /* 0.5 * (e^x + e^(-x)) */
      def =
         nir_fmul_imm(nb, nir_fadd(nb, nir_fexp(nb, src[0]),
                                       nir_fexp(nb, nir_fneg(nb, src[0]))),
                          0.5f);
      break;

   case GLSLstd450Tanh: {
      def = b->shader->options->has_tanh ?
               nir_ftanh(nb, src[0]) :
               nir_tanh_emulated(nb, src[0]);
      break;
   }

   case GLSLstd450Asinh:
      def = nir_fmul(nb, nir_fsign(nb, src[0]),
         nir_flog(nb, nir_fadd(nb, nir_fabs(nb, src[0]),
                      nir_fsqrt(nb, nir_ffma_weak_imm2(nb, src[0], src[0], 1.0f)))));
      break;
   case GLSLstd450Acosh:
      def = nir_flog(nb, nir_fadd(nb, src[0],
         nir_fsqrt(nb, nir_ffma_weak_imm2(nb, src[0], src[0], -1.0f))));
      break;
   case GLSLstd450Atanh: {
      def =
         nir_fmul_imm(nb, nir_flog(nb, nir_fdiv(nb, nir_fadd_imm(nb, src[0], 1.0),
                                       nir_fsub_imm(nb, 1.0, src[0]))),
                          0.5f);
      break;
   }

   case GLSLstd450Asin:
      def = nir_asin(nb, src[0]);
      break;

   case GLSLstd450Acos:
      def = nir_acos(nb, src[0]);
      break;

   case GLSLstd450Atan:
      def = nir_atan(nb, src[0]);
      break;

   case GLSLstd450Atan2:
      def = nir_atan2(nb, src[0], src[1]);
      break;

   case GLSLstd450Ldexp: {
      nir_def *exp = src[1];

      if (exp->bit_size == 64) {
         exp = nir_iclamp(nb, exp, nir_imm_intN_t(nb, INT32_MIN, 64),
                                   nir_imm_intN_t(nb, INT32_MAX, 64));
      }

      def = nir_ldexp(nb, src[0], nir_i2i32(nb, exp));
      break;
   }

   default: {
      unsigned execution_mode =
         b->shader->info.float_controls_execution_mode;
      unsigned extra_fp_math_ctrl;
      nir_op op = vtn_nir_alu_op_for_spirv_glsl_opcode(b, entrypoint, execution_mode, &extra_fp_math_ctrl);
      b->nb.fp_math_ctrl |= extra_fp_math_ctrl;
      def = nir_build_alu(&b->nb, op, src[0], src[1], src[2], NULL);
      break;
   }
   }

   h.dest->def = def;
   vtn_push_ssa_value(b, w[2], vtn_handler_finish_value(b, &h));
}

static void
handle_glsl450_interpolation(struct vtn_builder *b, enum GLSLstd450 opcode,
                             const uint32_t *w, unsigned count)
{
   nir_intrinsic_op op;
   switch (opcode) {
   case GLSLstd450InterpolateAtCentroid:
      op = nir_intrinsic_interp_deref_at_centroid;
      break;
   case GLSLstd450InterpolateAtSample:
      op = nir_intrinsic_interp_deref_at_sample;
      break;
   case GLSLstd450InterpolateAtOffset:
      op = nir_intrinsic_interp_deref_at_offset;
      break;
   default:
      vtn_fail("Invalid opcode");
   }

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);

   struct vtn_pointer *ptr =
      vtn_value(b, w[5], vtn_value_type_pointer)->pointer;
   nir_deref_instr *deref = vtn_pointer_to_deref(b, ptr);

   /* If the value we are interpolating has an index into a vector then
    * interpolate the vector and index the result of that instead. This is
    * necessary because the index will get generated as a series of nir_bcsel
    * instructions so it would no longer be an input variable.
    */
   const bool vec_array_deref = deref->deref_type == nir_deref_type_array &&
      glsl_type_is_vector(nir_deref_instr_parent(deref)->type);

   nir_deref_instr *vec_deref = NULL;
   if (vec_array_deref) {
      vec_deref = deref;
      deref = nir_deref_instr_parent(deref);
   }
   intrin->src[0] = nir_src_for_ssa(&deref->def);

   switch (opcode) {
   case GLSLstd450InterpolateAtCentroid:
      break;
   case GLSLstd450InterpolateAtSample:
   case GLSLstd450InterpolateAtOffset:
      intrin->src[1] = nir_src_for_ssa(vtn_get_nir_ssa(b, w[6]));
      break;
   default:
      vtn_fail("Invalid opcode");
   }

   intrin->num_components = glsl_get_vector_elements(deref->type);
   nir_def_init(&intrin->instr, &intrin->def,
                glsl_get_vector_elements(deref->type),
                glsl_get_bit_size(deref->type));

   nir_builder_instr_insert(&b->nb, &intrin->instr);

   nir_def *def = &intrin->def;
   if (vec_array_deref)
      def = nir_vector_extract(&b->nb, def, vec_deref->arr.index.ssa);

   vtn_push_nir_ssa(b, w[2], def);
}

static void
vtn_glsl450_store_component(struct vtn_builder *b,
                            struct vtn_pointer *ptr,
                            nir_def *idx,
                            nir_def *value)
{
   nir_deref_instr *deref = vtn_pointer_to_deref(b, ptr);
   idx = nir_u2uN(&b->nb, idx, deref->def.bit_size);
   nir_deref_instr *elem = nir_build_deref_array(&b->nb, deref, idx);

   nir_store_deref_with_access(&b->nb, elem, value, 0x1,
                               ptr->access | ptr->type->pointed->access);
}

static nir_def *
vtn_glsl450_convert_frexp_exp(struct vtn_builder *b, nir_def *exp,
                              const struct vtn_type *type)
{
   return nir_i2iN(&b->nb, exp,
                   glsl_get_bit_size(glsl_get_scalar_type(type->type)));
}

static void
vtn_glsl450_modf_parts(struct vtn_builder *b, nir_def *src,
                       nir_def **frac_out, nir_def **whole_out)
{
   nir_def *inf = nir_imm_floatN_t(&b->nb, INFINITY, src->bit_size);
   nir_def *sign_bit =
      nir_imm_intN_t(&b->nb, (uint64_t)1 << (src->bit_size - 1),
                     src->bit_size);
   nir_def *signed_zero = nir_iand(&b->nb, src, sign_bit);
   nir_def *abs = nir_fabs(&b->nb, src);

   /* NaN input should produce a NaN results, and ±Inf input should provide
    * ±0 result.  The fmul(sign(x), ffract(x)) calculation will already
    * produce the expected NaN.  To get ±0, directly compare for equality
    * with Inf instead of using fisfinite (which is false for NaN).
    */
   *frac_out = nir_bcsel(&b->nb,
                         nir_ieq(&b->nb, abs, inf),
                         signed_zero,
                         nir_ior(&b->nb, signed_zero,
                                 nir_ffract(&b->nb, abs)));
   *whole_out = nir_ior(&b->nb, signed_zero, nir_ffloor(&b->nb, abs));
}

static void
handle_glsl450_modf_frexp(struct vtn_builder *b, enum GLSLstd450 opcode,
                          const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   const struct glsl_type *dest_type = vtn_get_type(b, w[1])->type;

   /* TODO: Mediump currently not used, not sure how to detect the ->elems[i]
    * destinations on these in vtn_upconvert_value().
    */

   struct vtn_handler_ctx h = {0};
   vtn_handler_prepare(b, &h, w[1], w[2], &w[5], 1);
   vtn_handler_start(b, &h);

   nir_def *src = h.srcs[0]->def;

   if (opcode == GLSLstd450Modf || opcode == GLSLstd450Frexp) {
      nir_def *def;
      nir_def *secondary;

      switch (opcode) {
      case GLSLstd450Modf:
         vtn_glsl450_modf_parts(b, src, &def, &secondary);
         break;

      case GLSLstd450Frexp:
         def = nir_frexp_sig(nb, src);
         secondary = nir_frexp_exp(nb, src);
         break;

      default:
         UNREACHABLE("not modf or frexp");
      }

      struct vtn_pointer *i_ptr = vtn_value(b, w[6], vtn_value_type_pointer)->pointer;
      if (opcode == GLSLstd450Frexp) {
         secondary = vtn_glsl450_convert_frexp_exp(b, secondary,
                                                   i_ptr->type->pointed);
      }

      if (h.lv.length > 0) {
         vtn_fail_if(i_ptr->type->pointed->base_type != vtn_base_type_long_vector ||
                     i_ptr->type->pointed->length != h.lv.length,
                     "The pointer operand of Modf/Frexp must point to the "
                     "same type as the first operand");
         vtn_glsl450_store_component(b, i_ptr, h.lv.idx, secondary);
      } else {
         struct vtn_ssa_value *out = vtn_create_ssa_value(b, i_ptr->type->pointed);
         out->def = secondary;
         vtn_variable_store(b, out, i_ptr, 0);
      }

      h.dest->def = def;
      vtn_push_ssa_value(b, w[2], vtn_handler_finish_value(b, &h));
      return;
   }

   struct vtn_ssa_value *dest = h.dest;

   switch (opcode) {
   case GLSLstd450ModfStruct: {
      nir_def *frac, *whole;
      vtn_assert(glsl_type_is_struct_or_ifc(dest_type));

      vtn_glsl450_modf_parts(b, src, &frac, &whole);

      if (dest->elems[0]->type->base_type == vtn_base_type_long_vector)
         nir_store_array_var(&b->nb, dest->elems[0]->var, h.lv.idx, frac, 1);
      else
         dest->elems[0]->def = frac;

      if (dest->elems[1]->type->base_type == vtn_base_type_long_vector)
         nir_store_array_var(&b->nb, dest->elems[1]->var, h.lv.idx, whole, 1);
      else
         dest->elems[1]->def = whole;
      break;
   }

   case GLSLstd450FrexpStruct: {
      vtn_assert(glsl_type_is_struct_or_ifc(dest_type));

      nir_def *sig = nir_frexp_sig(nb, src);
      nir_def *exp = nir_frexp_exp(nb, src);
      exp = vtn_glsl450_convert_frexp_exp(b, exp, dest->elems[1]->type);

      if (dest->elems[0]->type->base_type == vtn_base_type_long_vector)
         nir_store_array_var(&b->nb, dest->elems[0]->var, h.lv.idx, sig, 1);
      else
         dest->elems[0]->def = sig;

      if (dest->elems[1]->type->base_type == vtn_base_type_long_vector)
         nir_store_array_var(&b->nb, dest->elems[1]->var, h.lv.idx, exp, 1);
      else
         dest->elems[1]->def = exp;
      break;
   }

   default:
      UNREACHABLE("not modf or frexp");
   }

   vtn_push_ssa_value(b, w[2], vtn_handler_finish_value(b, &h));
}

static void
handle_glsl450_packing_ops(struct vtn_builder *b, enum GLSLstd450 opcode,
                           const uint32_t *w, unsigned count)
{
   const struct vtn_type *dest_type = vtn_get_type(b, w[1]);

   /* Asking for relaxed precision snorm 4x8 pack results (for example)
    * doesn't even make sense.  The NIR opcodes have a fixed output size, so
    * no trying to reduce precision.
    */

   struct vtn_ssa_value *vtn_src = {0};
   vtn_collect_ssa_sources(b, &w[5], &vtn_src, 1);

   nir_op op;
   switch (opcode) {
   case GLSLstd450PackSnorm4x8:     op = nir_op_pack_snorm_4x8;    break;
   case GLSLstd450PackUnorm4x8:     op = nir_op_pack_unorm_4x8;    break;
   case GLSLstd450PackSnorm2x16:    op = nir_op_pack_snorm_2x16;   break;
   case GLSLstd450PackUnorm2x16:    op = nir_op_pack_unorm_2x16;   break;
   case GLSLstd450PackHalf2x16:     op = nir_op_pack_half_2x16;    break;
   case GLSLstd450PackDouble2x32:   op = nir_op_pack_64_2x32;      break;
   case GLSLstd450UnpackSnorm4x8:   op = nir_op_unpack_snorm_4x8;  break;
   case GLSLstd450UnpackUnorm4x8:   op = nir_op_unpack_unorm_4x8;  break;
   case GLSLstd450UnpackSnorm2x16:  op = nir_op_unpack_snorm_2x16; break;
   case GLSLstd450UnpackUnorm2x16:  op = nir_op_unpack_unorm_2x16; break;
   case GLSLstd450UnpackHalf2x16:   op = nir_op_unpack_half_2x16;  break;
   case GLSLstd450UnpackDouble2x32: op = nir_op_unpack_64_2x32;    break;
   default:
      UNREACHABLE("invalid opcode for packing op");
   }

   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);
   dest->def = nir_build_alu1(&b->nb, op, vtn_src->def);

   vtn_push_ssa_value(b, w[2], dest);
}

static void
handle_glsl450_vector_ops(struct vtn_builder *b, enum GLSLstd450 opcode,
                          const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   const struct vtn_type *dest_type = vtn_get_type(b, w[1]);

   unsigned num_inputs = count - 5;
   struct vtn_ssa_value *vtn_src[3] = {0};
   vtn_collect_ssa_sources(b, &w[5], vtn_src, num_inputs);

   const bool mediump_16bit =
      vtn_value_needs_mediump_16bit(b, vtn_untyped_value(b, w[2]));
   if (mediump_16bit)
      vtn_downconvert_ssa_sources(b, vtn_src, num_inputs);

   nir_def *src[3] = { NULL, };
   for (unsigned i = 0; i < num_inputs; i++)
      src[i] = vtn_src[i]->def;

   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);

   switch (opcode) {
   case GLSLstd450Length:
      dest->def = nir_fast_length(nb, src[0]);
      break;

   case GLSLstd450Distance:
      dest->def = nir_fast_distance(nb, src[0], src[1]);
      break;

   case GLSLstd450Normalize:
      dest->def = nir_fast_normalize(nb, src[0]);
      break;

   case GLSLstd450Cross:
      dest->def = nir_cross3(nb, src[0], src[1]);
      break;

   case GLSLstd450FaceForward:
      dest->def =
         nir_bcsel(nb, nir_flt(nb, nir_fdot(nb, src[2], src[1]),
                                   NIR_IMM_FP(nb, 0.0)),
                       src[0], nir_fneg(nb, src[0]));
      break;

   case GLSLstd450Reflect:
      /* I - 2 * dot(N, I) * N */
      dest->def =
         nir_a_minus_bc(nb, src[0],
                            src[1],
                            nir_fmul(nb, nir_fdot(nb, src[0], src[1]),
                                         NIR_IMM_FP(nb, 2.0)));
      break;

   case GLSLstd450Refract: {
      nir_def *I = src[0];
      nir_def *N = src[1];
      nir_def *eta = src[2];
      nir_def *n_dot_i = nir_fdot(nb, N, I);
      nir_def *one = NIR_IMM_FP(nb, 1.0);
      nir_def *zero = NIR_IMM_FP(nb, 0.0);
      /* According to the SPIR-V and GLSL specs, eta is always a float
       * regardless of the type of the other operands. However in practice it
       * seems that if you try to pass it a float then glslang will just
       * promote it to a double and generate invalid SPIR-V. In order to
       * support a hypothetical fixed version of glslang we’ll promote eta to
       * double if the other operands are double also.
       */
      if (I->bit_size != eta->bit_size) {
         eta = nir_type_convert(nb, eta, nir_type_float,
                                nir_type_float | I->bit_size,
                                nir_rounding_mode_undef);
      }
      /* k = 1.0 - eta * eta * (1.0 - dot(N, I) * dot(N, I)) */
      nir_def *k =
         nir_a_minus_bc(nb, one, eta,
                            nir_fmul(nb, eta, nir_a_minus_bc(nb, one, n_dot_i, n_dot_i)));
      nir_def *result =
         nir_a_minus_bc(nb, nir_fmul(nb, eta, I),
                            nir_ffma_weak(nb, eta, n_dot_i, nir_fsqrt(nb, k)),
                            N);
      /* XXX: bcsel, or if statement? */
      dest->def = nir_bcsel(nb, nir_flt(nb, k, zero), zero, result);
      break;
   }

   default:
      UNREACHABLE("invalid vector opcode");
   }

   if (mediump_16bit)
      dest = vtn_mediump_upconvert_value(b, dest);

   vtn_push_ssa_value(b, w[2], dest);
}

bool
vtn_handle_glsl450_instruction(struct vtn_builder *b, SpvOp ext_opcode,
                               const uint32_t *w, unsigned count)
{
   vtn_handle_fp_fast_math(b, vtn_untyped_value(b, w[2]), vtn_untyped_value(b, w[5]));
   switch ((enum GLSLstd450)ext_opcode) {
   case GLSLstd450Determinant: {
      vtn_push_nir_ssa(b, w[2], build_mat_det(b, vtn_ssa_value(b, w[5])));
      break;
   }

   case GLSLstd450MatrixInverse: {
      vtn_push_ssa_value(b, w[2], matrix_inverse(b, vtn_ssa_value(b, w[5])));
      break;
   }

   case GLSLstd450InterpolateAtCentroid:
   case GLSLstd450InterpolateAtSample:
   case GLSLstd450InterpolateAtOffset:
      handle_glsl450_interpolation(b, (enum GLSLstd450)ext_opcode, w, count);
      break;

   case GLSLstd450Modf:
   case GLSLstd450Frexp:
   case GLSLstd450ModfStruct:
   case GLSLstd450FrexpStruct:
      handle_glsl450_modf_frexp(b, (enum GLSLstd450)ext_opcode, w, count);
      break;

   case GLSLstd450PackSnorm4x8:
   case GLSLstd450PackUnorm4x8:
   case GLSLstd450PackSnorm2x16:
   case GLSLstd450PackUnorm2x16:
   case GLSLstd450PackHalf2x16:
   case GLSLstd450PackDouble2x32:
   case GLSLstd450UnpackSnorm4x8:
   case GLSLstd450UnpackUnorm4x8:
   case GLSLstd450UnpackSnorm2x16:
   case GLSLstd450UnpackUnorm2x16:
   case GLSLstd450UnpackHalf2x16:
   case GLSLstd450UnpackDouble2x32:
      handle_glsl450_packing_ops(b, (enum GLSLstd450)ext_opcode, w, count);
      break;

   case GLSLstd450Length:
   case GLSLstd450Distance:
   case GLSLstd450Normalize:
   case GLSLstd450Cross:
   case GLSLstd450FaceForward:
   case GLSLstd450Reflect:
   case GLSLstd450Refract:
      handle_glsl450_vector_ops(b, (enum GLSLstd450)ext_opcode, w, count);
      break;

   default:
      handle_glsl450_alu(b, (enum GLSLstd450)ext_opcode, w, count);
      break;
   }

   b->nb.fp_math_ctrl = nir_fp_fast_math;
   return true;
}
