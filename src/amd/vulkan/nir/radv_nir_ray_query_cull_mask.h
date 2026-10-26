/* SPDX-License-Identifier: MIT */
#ifndef RADV_NIR_RAY_QUERY_CULL_MASK_H
#define RADV_NIR_RAY_QUERY_CULL_MASK_H

#include "nir/nir.h"
#include "util/hash_table.h"
#include "util/set.h"
#include "util/u_dynarray.h"

struct radv_rq_cull_mask_info {
   bool all_full_mask;
   struct util_dynarray initializers;
};

/* Analyze the original CFG, before ray-query lowering inserts traversal control
 * flow and deletes the initializers. The returned set contains only proceed
 * instruction identities; lowering must not inspect an initializer again.
 */
static inline struct set *
radv_nir_ray_query_full_mask_proceeds(nir_shader *shader)
{
   struct set *result = _mesa_pointer_set_create(NULL);
   struct hash_table *queries = _mesa_pointer_hash_table_create(NULL);

   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_rq_initialize)
               continue;
            nir_deref_instr *deref = nir_def_as_deref(intr->src[0].ssa);
            if (deref->deref_type != nir_deref_type_var || glsl_type_is_array(deref->var->type))
               continue;

            struct hash_entry *entry = _mesa_hash_table_search(queries, deref->var);
            struct radv_rq_cull_mask_info *info;
            if (!entry) {
               info = rzalloc(queries, struct radv_rq_cull_mask_info);
               info->all_full_mask = true;
               util_dynarray_init(&info->initializers, queries);
               _mesa_hash_table_insert(queries, deref->var, info);
            } else {
               info = (struct radv_rq_cull_mask_info *)entry->data;
            }
            info->all_full_mask &= nir_src_is_const(intr->src[3]) && nir_src_as_uint(intr->src[3]) == 0xff;
            util_dynarray_append(&info->initializers, instr);
         }
      }
   }

   nir_foreach_function_impl (impl, shader) {
      nir_metadata_require(impl, nir_metadata_dominance);
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_rq_proceed)
               continue;
            nir_deref_instr *deref = nir_def_as_deref(intr->src[0].ssa);
            if (deref->deref_type != nir_deref_type_var || glsl_type_is_array(deref->var->type))
               continue;
            struct hash_entry *entry = _mesa_hash_table_search(queries, deref->var);
            if (!entry)
               continue;
            struct radv_rq_cull_mask_info *info = (struct radv_rq_cull_mask_info *)entry->data;
            if (!info->all_full_mask)
               continue;

            util_dynarray_foreach (&info->initializers, nir_instr *, init) {
               if ((*init)->block->impl != impl)
                  continue;
               const bool dominates = (*init)->block == block ? nir_instr_is_before(*init, instr)
                                                              : nir_block_dominates((*init)->block, block);
               if (dominates) {
                  _mesa_set_add(result, instr);
                  break;
               }
            }
         }
      }
   }
   ralloc_free(queries);
   return result;
}

#endif
