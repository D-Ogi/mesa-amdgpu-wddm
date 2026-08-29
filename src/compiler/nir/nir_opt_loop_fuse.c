/*
 * Copyright © 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_control_flow.h"
#include "nir_deref.h"

/* Fuse adjacent loops.  For now it recognizes very simple loops in the form
 *
 *    for (uint i = 0; i < limit; i++)
 *       body(i);
 *
 * with one basic-block body and no loop-carried value other than the index.
 *
 * Cross-loop memory dependencies must either be proven non-aliasing or access
 * the same element in both iterations.
 *
 * This pass should be sufficient to handle NIR loops from array copy
 * lowering, and per-component loops used in long_vector lowering without
 * requiring a more comprehensive analysis.
 */

struct fusion_candidate_loop {
   nir_loop *loop;
   nir_block *body;
   nir_phi_instr *index_phi;
   nir_alu_instr *increment;
   nir_alu_instr *compare;
   nir_if *condition_if;
   nir_def *limit;
};

static bool
is_const_uint(nir_def *def, uint64_t value)
{
   if (!nir_def_is_const(def) || def->num_components != 1)
      return false;
   nir_load_const_instr *load = nir_def_as_load_const(def);
   return nir_const_value_as_uint(load->value[0], def->bit_size) == value;
}

static bool
same_positive_constant_limit(nir_def *a, nir_def *b)
{
   if (!nir_def_is_const(a) || !nir_def_is_const(b) ||
       a->bit_size != b->bit_size || a->num_components != 1 ||
       b->num_components != 1)
      return false;

   nir_load_const_instr *a_load = nir_def_as_load_const(a);
   nir_load_const_instr *b_load = nir_def_as_load_const(b);
   uint64_t a_value =
      nir_const_value_as_uint(a_load->value[0], a->bit_size);
   uint64_t b_value =
      nir_const_value_as_uint(b_load->value[0], b->bit_size);
   return a_value > 0 && a_value == b_value;
}

static bool
same_iteration_deref(nir_deref_instr *a, nir_def *a_index,
                     nir_deref_instr *b, nir_def *b_index)
{
   nir_deref_path a_path, b_path;
   nir_deref_path_init(&a_path, a, NULL);
   nir_deref_path_init(&b_path, b, NULL);

   bool same = true;
   bool iteration_indexed = false;
   unsigned i;
   for (i = 0; a_path.path[i] && b_path.path[i]; i++) {
      nir_deref_instr *ad = a_path.path[i];
      nir_deref_instr *bd = b_path.path[i];
      if (ad->deref_type != bd->deref_type) {
         same = false;
         break;
      }

      switch (ad->deref_type) {
      case nir_deref_type_var:
         same = ad->var == bd->var;
         break;
      case nir_deref_type_array:
         if (ad->arr.index.ssa == a_index && bd->arr.index.ssa == b_index) {
            iteration_indexed = true;
            break;
         }
         same = nir_srcs_equal(ad->arr.index, bd->arr.index);
         break;
      case nir_deref_type_ptr_as_array:
         /* Proving different iterations disjoint requires pointer-stride
          * analysis which this pass deliberately does not perform.
          */
         same = false;
         break;
      case nir_deref_type_struct:
         same = ad->strct.index == bd->strct.index;
         break;
      default:
         /* Cast and wildcard derefs need better analysis. */
         same = ad == bd;
         break;
      }

      if (!same)
         break;
   }
   same = same && a_path.path[i] == NULL && b_path.path[i] == NULL;

   nir_deref_path_finish(&a_path);
   nir_deref_path_finish(&b_path);
   return same && iteration_indexed;
}

static bool
get_memory_access(nir_instr *instr, nir_deref_instr **deref,
                  bool *write)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_deref:
      *deref = nir_src_as_deref(intrin->src[0]);
      *write = false;
      if (nir_intrinsic_access(intrin) & (ACCESS_COHERENT | ACCESS_VOLATILE))
         *deref = NULL;
      return true;
   case nir_intrinsic_store_deref:
      *deref = nir_src_as_deref(intrin->src[0]);
      *write = true;
      if (nir_intrinsic_access(intrin) & (ACCESS_COHERENT | ACCESS_VOLATILE))
         *deref = NULL;
      return true;
   default:
      return false;
   }
}

static bool
body_is_supported(struct fusion_candidate_loop *loop)
{
   nir_foreach_instr(instr, loop->body) {
      if (instr == &loop->increment->instr)
         continue;

      switch (instr->type) {
      case nir_instr_type_alu:
      case nir_instr_type_load_const:
      case nir_instr_type_deref:
         break;
      case nir_instr_type_intrinsic: {
         nir_deref_instr *deref = NULL;
         bool write;
         if (!get_memory_access(instr, &deref, &write) || !deref)
            return false;
         break;
      }
      default:
         return false;
      }

      nir_def *def = nir_instr_def(instr);
      if (!def)
         continue;

      nir_foreach_use_including_if(use, def) {
         if (nir_src_is_if(use) ||
             nir_src_use_instr(use)->block != loop->body)
            return false;
      }
   }

   return true;
}

static bool
memory_dependencies_allow_fusion(struct fusion_candidate_loop *first,
                                 struct fusion_candidate_loop *second)
{
   bool has_temporary_raw_dependency = false;

   nir_foreach_instr(a_instr, first->body) {
      nir_deref_instr *a;
      bool a_write;
      if (!get_memory_access(a_instr, &a, &a_write))
         continue;
      if (!a)
         return false;

      nir_foreach_instr(b_instr, second->body) {
         nir_deref_instr *b;
         bool b_write;
         if (!get_memory_access(b_instr, &b, &b_write))
            continue;
         if (!b)
            return false;
         if (!a_write && !b_write)
            continue;

         nir_deref_compare_result cmp = nir_compare_derefs(a, b);
         if (!(cmp & nir_derefs_may_alias_bit))
            continue;

         if (!same_iteration_deref(a, &first->index_phi->def,
                                   b, &second->index_phi->def))
            return false;

         if (a_write && !b_write) {
            nir_variable *a_var = nir_deref_instr_get_variable(a);
            nir_variable *b_var = nir_deref_instr_get_variable(b);
            if (a_var && a_var == b_var &&
                (a_var->data.mode &
                 (nir_var_function_temp | nir_var_shader_temp)))
               has_temporary_raw_dependency = true;
         }
      }
   }

   return has_temporary_raw_dependency;
}

static bool
block_is_inside_loop(nir_block *block, nir_loop *loop)
{
   for (nir_cf_node *node = block->cf_node.parent; node; node = node->parent) {
      if (node == &loop->cf_node)
         return true;
   }
   return false;
}

static bool
control_uses_are_supported(struct fusion_candidate_loop *loop)
{
   nir_foreach_use_including_if(use, &loop->index_phi->def) {
      if (nir_src_is_if(use) ||
          !block_is_inside_loop(nir_src_use_instr(use)->block, loop->loop))
         return false;
   }

   nir_foreach_use_including_if(use, &loop->compare->def) {
      if (use != &loop->condition_if->condition)
         return false;
   }

   nir_foreach_use_including_if(use, &loop->increment->def) {
      if (nir_src_is_if(use))
         return false;

      if (nir_src_use_instr(use) != &loop->index_phi->instr)
         return false;
   }

   return true;
}

static bool
header_is_supported(struct fusion_candidate_loop *loop)
{
   nir_block *header = nir_loop_first_block(loop->loop);
   nir_foreach_instr(instr, header) {
      if (instr == &loop->index_phi->instr ||
          instr == &loop->compare->instr)
         continue;

      switch (instr->type) {
      case nir_instr_type_alu:
      case nir_instr_type_load_const:
      case nir_instr_type_deref:
         break;
      default:
         return false;
      }

      nir_def *def = nir_instr_def(instr);
      if (!def)
         return false;

      nir_foreach_use_including_if(use, def) {
         if (nir_src_is_if(use) ||
             !block_is_inside_loop(nir_src_use_instr(use)->block, loop->loop))
            return false;
      }
   }

   return true;
}

static bool
match_fusion_candidate_loop(nir_loop *loop, struct fusion_candidate_loop *match)
{
   if (nir_loop_has_continue_construct(loop) ||
       loop->control == nir_loop_control_unroll)
      return false;

   nir_cf_node *preheader_node = nir_cf_node_prev(&loop->cf_node);
   if (!preheader_node || preheader_node->type != nir_cf_node_block)
      return false;

   nir_block *header = nir_loop_first_block(loop);
   nir_cf_node *if_node = nir_cf_node_next(&header->cf_node);
   if (!if_node || if_node->type != nir_cf_node_if)
      return false;

   nir_cf_node *tail_node = nir_cf_node_next(if_node);
   if (!tail_node || tail_node->type != nir_cf_node_block ||
       !nir_cf_node_is_last(tail_node))
      return false;

   nir_if *nif = nir_cf_node_as_if(if_node);
   nir_block *then_block = nir_if_first_then_block(nif);
   nir_block *else_block = nir_if_first_else_block(nif);
   if (then_block != nir_if_last_then_block(nif) ||
       else_block != nir_if_last_else_block(nif) ||
       !nir_block_ends_in_break(then_block) ||
       nir_block_first_instr(then_block) != nir_block_last_instr(then_block) ||
       !exec_list_is_empty(&else_block->instr_list))
      return false;

   nir_alu_instr *compare = nir_src_as_alu(nif->condition);
   if (!compare || compare->op != nir_op_uge ||
       nir_block_last_instr(header) != &compare->instr ||
       !nir_alu_src_is_trivial_ssa(&compare->src[0], 1) ||
       !nir_alu_src_is_trivial_ssa(&compare->src[1], 1))
      return false;

   nir_def *index = compare->src[0].src.ssa;
   nir_phi_instr *phi = nir_def_as_phi_or_null(index);
   if (!phi || phi->instr.block != header)
      return false;

   nir_block *body = nir_cf_node_as_block(tail_node);
   nir_instr *increment_instr = nir_block_last_instr(body);
   if (!increment_instr || increment_instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *increment = nir_instr_as_alu(increment_instr);

   if (increment->op != nir_op_iadd ||
       !nir_alu_src_is_trivial_ssa(&increment->src[0], 1) ||
       !nir_alu_src_is_trivial_ssa(&increment->src[1], 1))
      return false;

   if (is_const_uint(increment->src[0].src.ssa, 1)) {
      if (increment->src[1].src.ssa != index)
         return false;
   } else if (is_const_uint(increment->src[1].src.ssa, 1)) {
      if (increment->src[0].src.ssa != index)
         return false;
   } else {
      return false;
   }

   if (nir_block_num_preds(header) != 2)
      return false;

   nir_phi_src *init =
      nir_phi_get_src_from_block(phi, nir_cf_node_as_block(preheader_node));
   nir_phi_src *backedge = nir_phi_get_src_from_block(phi, body);
   if (!is_const_uint(init->src.ssa, 0) ||
       backedge->src.ssa != &increment->def)
      return false;

   unsigned header_phis = 0;
   nir_foreach_phi(other, header)
      header_phis++;
   if (header_phis != 1)
      return false;

   *match = (struct fusion_candidate_loop){
      .loop = loop,
      .body = body,
      .index_phi = phi,
      .increment = increment,
      .compare = compare,
      .condition_if = nif,
      .limit = compare->src[1].src.ssa,
   };
   return control_uses_are_supported(match) &&
          header_is_supported(match) && body_is_supported(match);
}

static bool
src_is_defined_outside_loop(nir_src *src, void *data)
{
   nir_loop *loop = data;
   nir_block *block = nir_def_block(src->ssa);
   return !block || !block_is_inside_loop(block, loop);
}

static bool
block_is_movable_before_loop(nir_block *block, nir_loop *loop)
{
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
      case nir_instr_type_load_const:
      case nir_instr_type_deref:
         break;
      case nir_instr_type_intrinsic:
         if (!nir_intrinsic_can_reorder(nir_instr_as_intrinsic(instr)))
            return false;
         break;
      default:
         return false;
      }

      if (!nir_foreach_src(instr, src_is_defined_outside_loop, loop))
         return false;
   }

   return true;
}

static bool
can_fuse_loops(nir_loop *first_loop, nir_block *between,
               nir_loop *second_loop, struct fusion_candidate_loop *first,
               struct fusion_candidate_loop *second)
{
   return match_fusion_candidate_loop(first_loop, first) &&
          match_fusion_candidate_loop(second_loop, second) &&
          same_positive_constant_limit(first->limit, second->limit) &&
          memory_dependencies_allow_fusion(first, second) &&
          block_is_movable_before_loop(between, first_loop);
}

static void
fuse_loops(struct fusion_candidate_loop *first, nir_block *between,
           struct fusion_candidate_loop *second)
{
   nir_foreach_instr_safe(instr, between)
      nir_instr_move(nir_before_cf_node(&first->loop->cf_node), instr);

   nir_def_rewrite_uses(&second->index_phi->def, &first->index_phi->def);

   nir_block *header = nir_loop_first_block(second->loop);
   nir_foreach_instr_safe(instr, header) {
      if (instr == &second->index_phi->instr ||
          instr == &second->compare->instr)
         continue;
      nir_instr_move(nir_before_instr(&first->increment->instr), instr);
   }

   nir_foreach_instr_safe(instr, second->body) {
      if (instr == &second->increment->instr)
         continue;
      nir_instr_move(nir_before_instr(&first->increment->instr), instr);
   }

   if (first->loop->control == nir_loop_control_dont_unroll ||
       second->loop->control == nir_loop_control_dont_unroll)
      first->loop->control = nir_loop_control_dont_unroll;

   nir_cf_node_remove(&second->loop->cf_node);
}

static bool
fuse_in_list(struct exec_list *list)
{
   bool progress = false;

   foreach_list_typed_safe(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= fuse_in_list(&nif->then_list);
         progress |= fuse_in_list(&nif->else_list);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         progress |= fuse_in_list(&loop->body);
         if (nir_loop_has_continue_construct(loop))
            progress |= fuse_in_list(&loop->continue_list);

         nir_cf_node *between_node = nir_cf_node_next(node);
         if (!between_node || between_node->type != nir_cf_node_block)
            break;
         nir_cf_node *second_node = nir_cf_node_next(between_node);
         if (!second_node || second_node->type != nir_cf_node_loop)
            break;

         nir_block *between = nir_cf_node_as_block(between_node);
         struct fusion_candidate_loop first, second;
         if (can_fuse_loops(loop, between,
                            nir_cf_node_as_loop(second_node), &first, &second)) {
            fuse_loops(&first, between, &second);
            progress = true;
         }
         break;
      }
      default:
         break;
      }
   }

   return progress;
}

bool
nir_opt_loop_fuse(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      progress |= nir_progress(fuse_in_list(&impl->body),
                               impl, nir_metadata_none);
   }

   return progress;
}
