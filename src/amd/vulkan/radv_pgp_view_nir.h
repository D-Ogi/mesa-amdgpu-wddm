/* SPDX-License-Identifier: MIT */
#ifndef RADV_PGP_VIEW_NIR_H
#define RADV_PGP_VIEW_NIR_H
#include "radv_pgp_view.h"

struct radv_pgp_view_lower_state { uint32_t address32_hi; bool valid; };

static bool
radv_pgp_view_load(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct radv_pgp_view_lower_state *s = data;
   if (in->intrinsic != nir_intrinsic_load_deref) return false;
   nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   nir_deref_instr *parent = nir_deref_instr_parent(deref);
   bool private = var && var->data.mode == nir_var_mem_ubo &&
       var->data.descriptor_set == BC250_PGP_VIEW_DESCRIPTOR_SET &&
       var->data.binding == BC250_PGP_VIEW_DESCRIPTOR_BINDING;
   if (!var && parent && parent->deref_type == nir_deref_type_cast && (parent->modes & nir_var_mem_ubo)) {
      nir_binding binding = nir_chase_binding(parent->parent);
      private = binding.success && binding.desc_set == BC250_PGP_VIEW_DESCRIPTOR_SET &&
                binding.binding == BC250_PGP_VIEW_DESCRIPTOR_BINDING;
      if (private && (binding.num_indices != 1 || !nir_src_is_const(binding.indices[0]) ||
                      nir_src_as_uint(binding.indices[0]) != 0)) {
         s->valid = false;
         return false;
      }
   }
   if (!private)
      return false;
   if (deref->deref_type != nir_deref_type_struct || deref->strct.index >= 5 ||
       !parent || (parent->deref_type != nir_deref_type_var && parent->deref_type != nir_deref_type_cast) ||
       in->def.num_components != 1 || in->def.bit_size != 32) {
      s->valid = false;
      return false;
   }
   if ((!glsl_type_is_struct(parent->type) && !glsl_type_is_interface(parent->type)) ||
       glsl_get_length(parent->type) != 5) {
      s->valid = false;
      return false;
   }
   for (unsigned i = 0; i < 5; i++) {
      const struct glsl_type *type = glsl_get_struct_field(parent->type, i);
      if (!glsl_type_is_scalar(type) || glsl_get_base_type(type) != GLSL_TYPE_UINT ||
          glsl_get_struct_field_offset(parent->type, i) != (int)(4 * i)) {
         s->valid = false;
         return false;
      }
   }
   b->cursor = nir_before_instr(&in->instr);
   nir_def *address = nir_pack_64_2x32_split(b, nir_load_view_index(b), nir_imm_int(b, s->address32_hi));
   address = nir_iadd_imm(b, address, 4 * deref->strct.index);
   nir_def *value = nir_load_global_constant(b, 1, 32, address, .align_mul = 4);
   nir_def_replace(&in->def, value);
   b->shader->info.bc250_pgp_view_data = true;
   return true;
}

static bool
radv_lower_pgp_view_data(nir_shader *nir, uint32_t address32_hi)
{
   struct radv_pgp_view_lower_state state = {address32_hi, true};
   unsigned declarations = 0;
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo) {
      if (var->data.descriptor_set != BC250_PGP_VIEW_DESCRIPTOR_SET ||
          var->data.binding != BC250_PGP_VIEW_DESCRIPTOR_BINDING) continue;
      if (++declarations != 1 ||
          (!glsl_type_is_struct(var->type) && !glsl_type_is_interface(var->type)) ||
          glsl_get_length(var->type) != 5)
         return false;
      for (unsigned i = 0; i < 5; i++) {
         const struct glsl_type *type = glsl_get_struct_field(var->type, i);
         if (!glsl_type_is_scalar(type) || glsl_get_base_type(type) != GLSL_TYPE_UINT ||
             glsl_get_struct_field_offset(var->type, i) != (int)(4 * i))
            return false;
      }
   }
   if (!declarations) return true;
   /* The shared userdata slot cannot simultaneously mean native ViewIndex. */
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block)
      if (instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_view_index) return false;
   nir_shader_intrinsics_pass(nir, radv_pgp_view_load, nir_metadata_control_flow, &state);
   if (!state.valid) return false;
   /* The descriptor is wholly private and must not reach application layouts. */
   nir_opt_dce(nir);
   nir_remove_dead_derefs(nir);
   nir_opt_dce(nir);
   nir_remove_dead_variables(nir, nir_var_mem_ubo, NULL);
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo)
      if (var->data.descriptor_set == BC250_PGP_VIEW_DESCRIPTOR_SET &&
          var->data.binding == BC250_PGP_VIEW_DESCRIPTOR_BINDING) return false;
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_intrinsic) continue;
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      if (in->intrinsic == nir_intrinsic_vulkan_resource_index &&
          nir_intrinsic_desc_set(in) == BC250_PGP_VIEW_DESCRIPTOR_SET &&
          nir_intrinsic_binding(in) == BC250_PGP_VIEW_DESCRIPTOR_BINDING) return false;
   }
   return true;
}
#endif
