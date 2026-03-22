/*
 * Copyright © 2016 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "vtn_private.h"

static nir_def *
vtn_build_subgroup_intrinsic(struct vtn_builder *b,
                             nir_intrinsic_op nir_op,
                             const struct glsl_type *dest_type,
                             nir_def *src0,
                             nir_def *src1,
                             nir_def *index,
                             unsigned const_idx0,
                             unsigned const_idx1)
{
   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->nb.shader, nir_op);
   nir_def_init_for_type(&intrin->instr, &intrin->def, dest_type);
   intrin->num_components = intrin->def.num_components;
   intrin->src[0] = nir_src_for_ssa(src0);
   if (src1)
      intrin->src[1] = nir_src_for_ssa(src1);
   if (index)
      intrin->src[src1 ? 2 : 1] = nir_src_for_ssa(index);
   intrin->const_index[0] = const_idx0;
   intrin->const_index[1] = const_idx1;
   nir_builder_instr_insert(&b->nb, &intrin->instr);

   return &intrin->def;
}

static struct vtn_ssa_value *
vtn_build_subgroup_instr(struct vtn_builder *b,
                         nir_intrinsic_op nir_op,
                         struct vtn_ssa_value *src0,
                         struct vtn_ssa_value *src1,
                         nir_def *index,
                         unsigned const_idx0,
                         unsigned const_idx1)
{
   /* Some of the subgroup operations take an index.  SPIR-V allows this to be
    * any integer type.  To make things simpler for drivers, we only support
    * 32-bit indices.
    */
   if (index && index->bit_size != 32)
      index = nir_u2u32(&b->nb, index);

   const bool has_long_vector =
      src0->type->base_type == vtn_base_type_long_vector ||
      (src1 && src1->type->base_type == vtn_base_type_long_vector);
   if (has_long_vector) {
      vtn_fail_if(src0->type->base_type != vtn_base_type_long_vector ||
                  (src1 && src1->type->base_type != vtn_base_type_long_vector),
                  "Long-vector subgroup ops currently require all vector "
                  "operands to be long vectors");
      vtn_fail_if(src1 && src0->type->length != src1->type->length,
                  "Long-vector subgroup operands must have matching lengths");
   }

   if (src1)
      vtn_assert(src1->type->type == src0->type->type);

   if (has_long_vector) {
      struct vtn_ssa_value *dst = vtn_create_ssa_value(b, src0->type);
      nir_variable *idx_var =
         nir_local_variable_create(b->nb.impl, glsl_uint_type(),
                                   "long_vector_subgroup_idx");
      nir_store_var(&b->nb, idx_var, nir_imm_int(&b->nb, 0), 1);

      vtn_fail_if(!b->nb.impl->structured,
                  "Long-vector loops require structured control flow");
      nir_loop *loop = nir_push_loop(&b->nb);
      nir_def *idx = nir_load_var(&b->nb, idx_var);
      nir_break_if(&b->nb, nir_uge_imm(&b->nb, idx, src0->type->length));

      nir_def *lane0 = nir_load_array_var(&b->nb, src0->var, idx);
      nir_def *lane1 = src1 ? nir_load_array_var(&b->nb, src1->var, idx) : NULL;

      nir_def *lane =
         vtn_build_subgroup_intrinsic(b, nir_op,
                                      glsl_get_scalar_type(src0->type->type),
                                      lane0, lane1, index,
                                      const_idx0, const_idx1);

      nir_store_array_var(&b->nb, dst->var, idx, lane, 1);
      nir_store_var(&b->nb, idx_var, nir_iadd_imm(&b->nb, idx, 1), 1);
      nir_pop_loop(&b->nb, loop);

      return dst;
   }

   struct vtn_ssa_value *dst = vtn_create_ssa_value(b, src0->type);

   vtn_assert(dst->type->type == src0->type->type);
   vtn_assert(glsl_type_is_vector_or_scalar(dst->type->type));
   dst->def =
      vtn_build_subgroup_intrinsic(b, nir_op, dst->type->type,
                                   src0->def, src1 ? src1->def : NULL, index,
                                   const_idx0, const_idx1);

   return dst;
}

static void
vtn_handle_subgroup_vote(struct vtn_builder *b, SpvOp opcode,
                         const uint32_t *w, unsigned count)
{
   const unsigned expected_count =
      opcode == SpvOpSubgroupAllKHR ||
      opcode == SpvOpSubgroupAnyKHR ||
      opcode == SpvOpSubgroupAllEqualKHR ||
      opcode == SpvOpGroupNonUniformQuadAllKHR ||
      opcode == SpvOpGroupNonUniformQuadAnyKHR ? 4 : 5;
   vtn_assert(count == expected_count);

   struct vtn_type *dest_type = vtn_get_type(b, w[1]);
   vtn_fail_if(dest_type->type != glsl_bool_type(),
               "OpGroupNonUniform(All|Any|AllEqual) must return a bool");

   struct vtn_ssa_value *vtn_src[1] = { NULL, };
   vtn_collect_ssa_sources(b, &w[count - 1], vtn_src, ARRAY_SIZE(vtn_src));

   if (vtn_src[0]->type->base_type == vtn_base_type_long_vector) {
      nir_intrinsic_op lane_op;
      bool accum_all;

      switch (opcode) {
      case SpvOpGroupNonUniformAll:
      case SpvOpGroupAll:
      case SpvOpSubgroupAllKHR:
         lane_op = nir_intrinsic_vote_all;
         accum_all = true;
         break;
      case SpvOpGroupNonUniformAny:
      case SpvOpGroupAny:
      case SpvOpSubgroupAnyKHR:
         lane_op = nir_intrinsic_vote_any;
         accum_all = false;
         break;
      case SpvOpSubgroupAllEqualKHR:
         lane_op = nir_intrinsic_vote_ieq;
         accum_all = true;
         break;
      case SpvOpGroupNonUniformAllEqual:
         switch (glsl_get_base_type(
                   glsl_get_scalar_type(vtn_src[0]->type->type))) {
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_FLOAT16:
         case GLSL_TYPE_DOUBLE:
            lane_op = nir_intrinsic_vote_feq;
            break;
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_INT:
         case GLSL_TYPE_UINT8:
         case GLSL_TYPE_INT8:
         case GLSL_TYPE_UINT16:
         case GLSL_TYPE_INT16:
         case GLSL_TYPE_UINT64:
         case GLSL_TYPE_INT64:
         case GLSL_TYPE_BOOL:
            lane_op = nir_intrinsic_vote_ieq;
            break;
         default:
            UNREACHABLE("Unhandled type");
         }
         accum_all = true;
         break;
      default:
         UNREACHABLE("invalid subgroup vote opcode");
      }

      struct vtn_reduction_ctx r = {0};
      vtn_reduction_prepare(b, &r, vtn_src, ARRAY_SIZE(vtn_src));
      vtn_reduction_start(b, &r, glsl_bool_type(),
                          nir_imm_bool(&b->nb, accum_all),
                          "long_vector_subgroup_vote_acc");

      nir_intrinsic_instr *intrin =
         nir_intrinsic_instr_create(b->nb.shader, lane_op);
      nir_def *lane = r.srcs[0]->def;
      if (nir_intrinsic_infos[lane_op].src_components[0] == 0)
         intrin->num_components = lane->num_components;
      intrin->src[0] = nir_src_for_ssa(lane);
      nir_def_init_for_type(&intrin->instr, &intrin->def, glsl_bool_type());
      nir_builder_instr_insert(&b->nb, &intrin->instr);

      nir_def *acc = vtn_reduction_accumulator(b, &r);
      vtn_reduction_update(b, &r,
                           accum_all ?
                              nir_iand(&b->nb, acc, &intrin->def) :
                              nir_ior(&b->nb, acc, &intrin->def));

      nir_def *def = vtn_reduction_finish(b, &r);
      vtn_push_ssa_value(b, w[2],
                         vtn_reduction_result(b, &r, dest_type, def));
      return;
   }

   nir_def *src0 = vtn_src[0]->def;

   if (opcode == SpvOpGroupNonUniformQuadAllKHR ||
       opcode == SpvOpGroupNonUniformQuadAnyKHR) {
      nir_def *dest = opcode == SpvOpGroupNonUniformQuadAllKHR ?
         nir_quad_vote_all(&b->nb, 1, src0) :
         nir_quad_vote_any(&b->nb, 1, src0);
      vtn_push_nir_ssa(b, w[2], dest);
      return;
   }

   nir_intrinsic_op op;
   switch (opcode) {
   case SpvOpGroupNonUniformAll:
   case SpvOpGroupAll:
   case SpvOpSubgroupAllKHR:
      op = nir_intrinsic_vote_all;
      break;
   case SpvOpGroupNonUniformAny:
   case SpvOpGroupAny:
   case SpvOpSubgroupAnyKHR:
      op = nir_intrinsic_vote_any;
      break;
   case SpvOpSubgroupAllEqualKHR:
      op = nir_intrinsic_vote_ieq;
      break;
   case SpvOpGroupNonUniformAllEqual:
      switch (glsl_get_base_type(vtn_src[0]->type->type)) {
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_FLOAT16:
      case GLSL_TYPE_DOUBLE:
         op = nir_intrinsic_vote_feq;
         break;
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT8:
      case GLSL_TYPE_INT8:
      case GLSL_TYPE_UINT16:
      case GLSL_TYPE_INT16:
      case GLSL_TYPE_UINT64:
      case GLSL_TYPE_INT64:
      case GLSL_TYPE_BOOL:
         op = nir_intrinsic_vote_ieq;
         break;
      default:
         UNREACHABLE("Unhandled type");
      }
      break;
   default:
      UNREACHABLE("invalid subgroup vote opcode");
   }

   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->nb.shader, op);
   if (nir_intrinsic_infos[op].src_components[0] == 0)
      intrin->num_components = src0->num_components;
   intrin->src[0] = nir_src_for_ssa(src0);
   nir_def_init_for_type(&intrin->instr, &intrin->def, dest_type->type);
   nir_builder_instr_insert(&b->nb, &intrin->instr);

   vtn_push_nir_ssa(b, w[2], &intrin->def);
}

void
vtn_handle_subgroup(struct vtn_builder *b, SpvOp opcode,
                    const uint32_t *w, unsigned count)
{
   struct vtn_type *dest_type = vtn_get_type(b, w[1]);

   switch (opcode) {
   case SpvOpGroupNonUniformElect: {
      vtn_fail_if(dest_type->type != glsl_bool_type(),
                  "OpGroupNonUniformElect must return a Bool");
      nir_intrinsic_instr *elect =
         nir_intrinsic_instr_create(b->nb.shader, nir_intrinsic_elect);
      nir_def_init_for_type(&elect->instr, &elect->def, dest_type->type);
      nir_builder_instr_insert(&b->nb, &elect->instr);
      vtn_push_nir_ssa(b, w[2], &elect->def);
      break;
   }

   case SpvOpGroupNonUniformBallot:
   case SpvOpSubgroupBallotKHR: {
      bool has_scope = (opcode != SpvOpSubgroupBallotKHR);
      vtn_fail_if(dest_type->type != glsl_vector_type(GLSL_TYPE_UINT, 4),
                  "OpGroupNonUniformBallot must return a uvec4");
      nir_intrinsic_instr *ballot =
         nir_intrinsic_instr_create(b->nb.shader, nir_intrinsic_ballot);
      ballot->src[0] = nir_src_for_ssa(vtn_get_nir_ssa(b, w[3 + has_scope]));
      nir_def_init(&ballot->instr, &ballot->def, 4, 32);
      ballot->num_components = 4;
      nir_builder_instr_insert(&b->nb, &ballot->instr);
      vtn_push_nir_ssa(b, w[2], &ballot->def);
      break;
   }

   case SpvOpGroupNonUniformInverseBallot: {
      nir_def *dest = nir_inverse_ballot(&b->nb, vtn_get_nir_ssa(b, w[4]));
      vtn_push_nir_ssa(b, w[2], dest);
      break;
   }

   case SpvOpGroupNonUniformBallotBitExtract: {
      nir_def *src0 = vtn_get_nir_ssa(b, w[4]);
      nir_def *src1 = vtn_get_nir_ssa(b, w[5]);
      nir_def *dest = nir_ballot_bitfield_extract(&b->nb, src0, src1);
      vtn_push_nir_ssa(b, w[2], dest);
      break;
   }

   case SpvOpGroupNonUniformBallotBitCount:
   case SpvOpGroupNonUniformBallotFindLSB:
   case SpvOpGroupNonUniformBallotFindMSB: {
      nir_def *dest;
      switch (opcode) {
      case SpvOpGroupNonUniformBallotBitCount: {
         nir_def *src = vtn_get_nir_ssa(b, w[5]);
         switch ((SpvGroupOperation)w[4]) {
         case SpvGroupOperationReduce:
            dest = nir_ballot_bit_count_reduce(&b->nb, src);
            break;
         case SpvGroupOperationInclusiveScan:
            dest = nir_ballot_bit_count_inclusive(&b->nb, src);
            break;
         case SpvGroupOperationExclusiveScan:
            dest = nir_ballot_bit_count_exclusive(&b->nb, src);
            break;
         default:
            UNREACHABLE("Invalid group operation");
         }
         break;
      }
      default: {
         nir_def *src = vtn_get_nir_ssa(b, w[4]);
         switch (opcode) {
         case SpvOpGroupNonUniformBallotFindLSB:
            dest = nir_ballot_find_lsb(&b->nb, src);
            break;
         case SpvOpGroupNonUniformBallotFindMSB:
            dest = nir_ballot_find_msb(&b->nb, src);
            break;
         default:
            UNREACHABLE("Unhandled opcode");
         }
         break;
      }
      }

      dest = nir_i2iN(&b->nb, dest, glsl_get_bit_size(dest_type->type));
      vtn_push_nir_ssa(b, w[2], dest);
      break;
   }

   case SpvOpGroupNonUniformBroadcastFirst:
   case SpvOpSubgroupFirstInvocationKHR: {
      bool has_scope = (opcode != SpvOpSubgroupFirstInvocationKHR);
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, nir_intrinsic_read_first_invocation,
                                  vtn_ssa_value(b, w[3 + has_scope]),
                                  NULL, NULL, 0, 0));
      break;
   }

   case SpvOpGroupNonUniformBroadcast:
   case SpvOpGroupBroadcast:
   case SpvOpSubgroupReadInvocationKHR: {
      bool has_scope = (opcode != SpvOpSubgroupReadInvocationKHR);
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, nir_intrinsic_read_invocation,
                                  vtn_ssa_value(b, w[3 + has_scope]),
                                  NULL,
                                  vtn_get_nir_ssa(b, w[4 + has_scope]), 0, 0));
      break;
   }

   case SpvOpGroupNonUniformAll:
   case SpvOpGroupNonUniformAny:
   case SpvOpGroupNonUniformAllEqual:
   case SpvOpGroupAll:
   case SpvOpGroupAny:
   case SpvOpSubgroupAllKHR:
   case SpvOpSubgroupAnyKHR:
   case SpvOpSubgroupAllEqualKHR:
      vtn_handle_subgroup_vote(b, opcode, w, count);
      break;

   case SpvOpGroupNonUniformShuffle:
   case SpvOpGroupNonUniformShuffleXor:
   case SpvOpGroupNonUniformShuffleUp:
   case SpvOpGroupNonUniformShuffleDown: {
      nir_intrinsic_op op;
      switch (opcode) {
      case SpvOpGroupNonUniformShuffle:
         op = nir_intrinsic_shuffle;
         break;
      case SpvOpGroupNonUniformShuffleXor:
         op = nir_intrinsic_shuffle_xor;
         break;
      case SpvOpGroupNonUniformShuffleUp:
         op = nir_intrinsic_shuffle_up;
         break;
      case SpvOpGroupNonUniformShuffleDown:
         op = nir_intrinsic_shuffle_down;
         break;
      default:
         UNREACHABLE("Invalid opcode");
      }
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, op, vtn_ssa_value(b, w[4]),
                                  NULL,
                                  vtn_get_nir_ssa(b, w[5]), 0, 0));
      break;
   }

   case SpvOpSubgroupShuffleINTEL:
   case SpvOpSubgroupShuffleXorINTEL: {
      nir_intrinsic_op op = opcode == SpvOpSubgroupShuffleINTEL ?
         nir_intrinsic_shuffle : nir_intrinsic_shuffle_xor;
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, op, vtn_ssa_value(b, w[3]),
                                  NULL,
                                  vtn_get_nir_ssa(b, w[4]), 0, 0));
      break;
   }

   case SpvOpSubgroupShuffleUpINTEL:
   case SpvOpSubgroupShuffleDownINTEL: {
      nir_intrinsic_op op = opcode == SpvOpSubgroupShuffleUpINTEL ?
         nir_intrinsic_shuffle_up_intel : nir_intrinsic_shuffle_down_intel;
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, op,
                                  vtn_ssa_value(b, w[3]),
                                  vtn_ssa_value(b, w[4]),
                                  vtn_get_nir_ssa(b, w[5]),
                                  0, 0));
      break;
   }

   case SpvOpGroupNonUniformRotateKHR: {
      const uint32_t cluster_size = count > 6 ? vtn_constant_uint(b, w[6]) : 0;
      vtn_fail_if(cluster_size && !IS_POT(cluster_size),
                  "Behavior is undefined unless ClusterSize is at least 1 and a power of 2.");

      struct vtn_ssa_value *value = vtn_ssa_value(b, w[4]);
      struct vtn_ssa_value *delta = vtn_ssa_value(b, w[5]);
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, nir_intrinsic_rotate,
                                  value, NULL, delta->def, cluster_size, 0));
      break;
   }

   case SpvOpGroupNonUniformQuadBroadcast:
      /* From the Vulkan spec 1.3.269:
       *
       * 9.27. Quad Group Operations:
       * "Fragment shaders that statically execute quad group operations
       * must launch sufficient invocations to ensure their correct operation;"
       */
      if (b->shader->info.stage == MESA_SHADER_FRAGMENT)
         b->shader->info.fs.require_full_quads = true;

      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, nir_intrinsic_quad_broadcast,
                                  vtn_ssa_value(b, w[4]),
                                  NULL,
                                  vtn_get_nir_ssa(b, w[5]), 0, 0));
      break;

   case SpvOpGroupNonUniformQuadSwap: {
      if (b->shader->info.stage == MESA_SHADER_FRAGMENT)
         b->shader->info.fs.require_full_quads = true;

      unsigned direction = vtn_constant_uint(b, w[5]);
      nir_intrinsic_op op;
      switch (direction) {
      case 0:
         op = nir_intrinsic_quad_swap_horizontal;
         break;
      case 1:
         op = nir_intrinsic_quad_swap_vertical;
         break;
      case 2:
         op = nir_intrinsic_quad_swap_diagonal;
         break;
      default:
         vtn_fail("Invalid constant value in OpGroupNonUniformQuadSwap");
      }
      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, op, vtn_ssa_value(b, w[4]), NULL, NULL,
                                  0, 0));
      break;
   }

   case SpvOpGroupNonUniformQuadAllKHR:
   case SpvOpGroupNonUniformQuadAnyKHR:
      vtn_handle_subgroup_vote(b, opcode, w, count);
      break;

   case SpvOpGroupNonUniformIAdd:
   case SpvOpGroupNonUniformFAdd:
   case SpvOpGroupNonUniformIMul:
   case SpvOpGroupNonUniformFMul:
   case SpvOpGroupNonUniformSMin:
   case SpvOpGroupNonUniformUMin:
   case SpvOpGroupNonUniformFMin:
   case SpvOpGroupNonUniformSMax:
   case SpvOpGroupNonUniformUMax:
   case SpvOpGroupNonUniformFMax:
   case SpvOpGroupNonUniformBitwiseAnd:
   case SpvOpGroupNonUniformBitwiseOr:
   case SpvOpGroupNonUniformBitwiseXor:
   case SpvOpGroupNonUniformLogicalAnd:
   case SpvOpGroupNonUniformLogicalOr:
   case SpvOpGroupNonUniformLogicalXor:
   case SpvOpGroupIAdd:
   case SpvOpGroupFAdd:
   case SpvOpGroupFMin:
   case SpvOpGroupUMin:
   case SpvOpGroupSMin:
   case SpvOpGroupFMax:
   case SpvOpGroupUMax:
   case SpvOpGroupSMax:
   case SpvOpGroupIMulKHR:
   case SpvOpGroupFMulKHR:
   case SpvOpGroupBitwiseAndKHR:
   case SpvOpGroupBitwiseOrKHR:
   case SpvOpGroupBitwiseXorKHR:
   case SpvOpGroupLogicalAndKHR:
   case SpvOpGroupLogicalOrKHR:
   case SpvOpGroupLogicalXorKHR:
   case SpvOpGroupIAddNonUniformAMD:
   case SpvOpGroupFAddNonUniformAMD:
   case SpvOpGroupFMinNonUniformAMD:
   case SpvOpGroupUMinNonUniformAMD:
   case SpvOpGroupSMinNonUniformAMD:
   case SpvOpGroupFMaxNonUniformAMD:
   case SpvOpGroupUMaxNonUniformAMD:
   case SpvOpGroupSMaxNonUniformAMD: {
      nir_op reduction_op;
      switch (opcode) {
      case SpvOpGroupNonUniformIAdd:
      case SpvOpGroupIAdd:
      case SpvOpGroupIAddNonUniformAMD:
         reduction_op = nir_op_iadd;
         break;
      case SpvOpGroupNonUniformFAdd:
      case SpvOpGroupFAdd:
      case SpvOpGroupFAddNonUniformAMD:
         reduction_op = nir_op_fadd;
         break;
      case SpvOpGroupIMulKHR:
      case SpvOpGroupNonUniformIMul:
         reduction_op = nir_op_imul;
         break;
      case SpvOpGroupFMulKHR:
      case SpvOpGroupNonUniformFMul:
         reduction_op = nir_op_fmul;
         break;
      case SpvOpGroupNonUniformSMin:
      case SpvOpGroupSMin:
      case SpvOpGroupSMinNonUniformAMD:
         reduction_op = nir_op_imin;
         break;
      case SpvOpGroupNonUniformUMin:
      case SpvOpGroupUMin:
      case SpvOpGroupUMinNonUniformAMD:
         reduction_op = nir_op_umin;
         break;
      case SpvOpGroupNonUniformFMin:
      case SpvOpGroupFMin:
      case SpvOpGroupFMinNonUniformAMD:
         reduction_op = nir_op_fmin;
         break;
      case SpvOpGroupNonUniformSMax:
      case SpvOpGroupSMax:
      case SpvOpGroupSMaxNonUniformAMD:
         reduction_op = nir_op_imax;
         break;
      case SpvOpGroupNonUniformUMax:
      case SpvOpGroupUMax:
      case SpvOpGroupUMaxNonUniformAMD:
         reduction_op = nir_op_umax;
         break;
      case SpvOpGroupNonUniformFMax:
      case SpvOpGroupFMax:
      case SpvOpGroupFMaxNonUniformAMD:
         reduction_op = nir_op_fmax;
         break;
      case SpvOpGroupBitwiseAndKHR:
      case SpvOpGroupLogicalAndKHR:
      case SpvOpGroupNonUniformBitwiseAnd:
      case SpvOpGroupNonUniformLogicalAnd:
         reduction_op = nir_op_iand;
         break;
      case SpvOpGroupBitwiseOrKHR:
      case SpvOpGroupLogicalOrKHR:
      case SpvOpGroupNonUniformBitwiseOr:
      case SpvOpGroupNonUniformLogicalOr:
         reduction_op = nir_op_ior;
         break;
      case SpvOpGroupBitwiseXorKHR:
      case SpvOpGroupLogicalXorKHR:
      case SpvOpGroupNonUniformBitwiseXor:
      case SpvOpGroupNonUniformLogicalXor:
         reduction_op = nir_op_ixor;
         break;
      default:
         UNREACHABLE("Invalid reduction operation");
      }

      nir_intrinsic_op op;
      unsigned cluster_size = 0;
      switch ((SpvGroupOperation)w[4]) {
      case SpvGroupOperationReduce:
         op = nir_intrinsic_reduce;
         break;
      case SpvGroupOperationInclusiveScan:
         op = nir_intrinsic_inclusive_scan;
         break;
      case SpvGroupOperationExclusiveScan:
         op = nir_intrinsic_exclusive_scan;
         break;
      case SpvGroupOperationClusteredReduce:
         op = nir_intrinsic_reduce;
         assert(count == 7);
         cluster_size = vtn_constant_uint(b, w[6]);
         break;
      default:
         UNREACHABLE("Invalid group operation");
      }

      vtn_push_ssa_value(b, w[2],
         vtn_build_subgroup_instr(b, op, vtn_ssa_value(b, w[5]), NULL, NULL,
                                  reduction_op, cluster_size));
      break;
   }

   default:
      UNREACHABLE("Invalid SPIR-V opcode");
   }
}
