/* SPDX-License-Identifier: MIT */

/* Lowers hit objects (VK_EXT_ray_tracing_invocation_reorder) to plain variables.
 *
 * A hit object becomes a struct variable with the ray, the hit and the attributes. A hit object trace becomes a
 * trace_ray with the internal RADV_RAY_FLAG_HIT_OBJECT_RECORD flag: the traversal shader then writes the hit or
 * the miss to the hit attribute storage instead of calling a closest-hit or miss shader, and this pass reads it
 * back after the call. Executing a hit object becomes execute_hit_object_amd, which the function-call lowering
 * turns into a call of the closest-hit or the miss shader.
 *
 * The reorder instructions are removed. The implementation reports
 * VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_EXT.
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"

#include "util/hash_table.h"

#include "nir/radv_nir.h"
#include "nir/radv_nir_rt_common.h"
#include "nir/radv_nir_rt_stage_common.h"
#include "radv_constants.h"
#include "radv_shader.h"

enum radv_hit_object_field {
   radv_hit_object_kind,
   radv_hit_object_sbt_index,
   radv_hit_object_flags,
   radv_hit_object_cull_mask,
   radv_hit_object_origin,
   radv_hit_object_tmin,
   radv_hit_object_direction,
   radv_hit_object_tmax,
   radv_hit_object_primitive_addr,
   radv_hit_object_primitive_id,
   radv_hit_object_instance_addr,
   radv_hit_object_geometry_id_and_flags,
   radv_hit_object_hit_kind,
   radv_hit_object_attribs,
   radv_hit_object_field_count,
};

static const glsl_type *
radv_get_hit_object_type(void)
{
   glsl_struct_field fields[radv_hit_object_field_count];

#define FIELD(field_name, field_type)                                                                                  \
   fields[radv_hit_object_##field_name] = (glsl_struct_field){                                                         \
      .type = field_type,                                                                                              \
      .name = #field_name,                                                                                             \
   }

   FIELD(kind, glsl_uint_type());
   FIELD(sbt_index, glsl_uint_type());
   FIELD(flags, glsl_uint_type());
   FIELD(cull_mask, glsl_uint_type());
   FIELD(origin, glsl_vec_type(3));
   FIELD(tmin, glsl_float_type());
   FIELD(direction, glsl_vec_type(3));
   FIELD(tmax, glsl_float_type());
   FIELD(primitive_addr, glsl_uint64_t_type());
   FIELD(primitive_id, glsl_uint_type());
   FIELD(instance_addr, glsl_uint64_t_type());
   FIELD(geometry_id_and_flags, glsl_uint_type());
   FIELD(hit_kind, glsl_uint_type());
   FIELD(attribs, glsl_vector_type(GLSL_TYPE_UINT, RADV_MAX_HIT_ATTRIB_DWORDS));

#undef FIELD

   return glsl_struct_type(fields, radv_hit_object_field_count, "hit_object", false);
}

#define ho_deref(b, deref, field) nir_build_deref_struct(b, deref, radv_hit_object_##field)
#define ho_load(b, deref, field)  nir_load_deref(b, ho_deref(b, deref, field))
#define ho_store(b, deref, field, value)                                                                               \
   nir_store_deref(b, ho_deref(b, deref, field), value, BITFIELD_MASK((value)->num_components))

struct lower_hit_objects_state {
   const struct radv_compiler_info *compiler_info;
   struct hash_table *vars;
};

static void
store_empty(nir_builder *b, nir_deref_instr *ho)
{
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *fzero = nir_imm_float(b, 0.0f);
   nir_def *vzero = nir_imm_vec3(b, 0.0f, 0.0f, 0.0f);

   ho_store(b, ho, kind, nir_imm_int(b, RADV_HIT_OBJECT_KIND_EMPTY));
   ho_store(b, ho, sbt_index, zero);
   ho_store(b, ho, flags, zero);
   ho_store(b, ho, cull_mask, zero);
   ho_store(b, ho, origin, vzero);
   ho_store(b, ho, tmin, fzero);
   ho_store(b, ho, direction, vzero);
   ho_store(b, ho, tmax, fzero);
}

/* A hit object that was never written is empty, as a default-initialized D3D12 HitObject is a NOP. */
static void
init_hit_objects(nir_builder *b, nir_deref_instr *deref)
{
   if (glsl_type_is_array(deref->type)) {
      for (unsigned i = 0; i < glsl_get_length(deref->type); i++)
         init_hit_objects(b, nir_build_deref_array_imm(b, deref, i));
      return;
   }

   store_empty(b, deref);
}

static nir_variable *
create_hit_object_var(nir_shader *shader, nir_variable *opaque, struct lower_hit_objects_state *state)
{
   struct hash_entry *entry = _mesa_hash_table_search(state->vars, opaque);
   if (entry)
      return entry->data;

   const glsl_type *type = glsl_type_wrap_in_arrays(radv_get_hit_object_type(), opaque->type);
   nir_variable *var =
      nir_variable_create(shader, nir_var_shader_temp, type, opaque->name ? opaque->name : "hit_object");
   _mesa_hash_table_insert(state->vars, opaque, var);

   nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(shader)));
   init_hit_objects(&b, nir_build_deref_var(&b, var));

   return var;
}

static nir_deref_instr *
rebuild_deref(nir_builder *b, nir_deref_instr *opaque_deref, nir_variable *var)
{
   if (opaque_deref->deref_type == nir_deref_type_var)
      return nir_build_deref_var(b, var);

   assert(opaque_deref->deref_type == nir_deref_type_array);
   nir_deref_instr *parent = rebuild_deref(b, nir_deref_instr_parent(opaque_deref), var);
   return nir_build_deref_array(b, parent, opaque_deref->arr.index.ssa);
}

static nir_def *
load_record(nir_builder *b, enum radv_hit_object_record_dword dword)
{
   return nir_load_hit_attrib_amd(b, .base = dword);
}

static void
lower_trace_ray(nir_builder *b, nir_intrinsic_instr *intr, nir_deref_instr *ho)
{
   nir_def *flags = intr->src[2].ssa;
   nir_def *cull_mask = intr->src[3].ssa;
   nir_def *miss_index = intr->src[6].ssa;
   nir_def *origin = intr->src[7].ssa;
   nir_def *tmin = intr->src[8].ssa;
   nir_def *direction = intr->src[9].ssa;
   nir_def *tmax = intr->src[10].ssa;

   nir_trace_ray(b, intr->src[1].ssa, nir_ior_imm(b, flags, RADV_RAY_FLAG_HIT_OBJECT_RECORD), cull_mask,
                 intr->src[4].ssa, intr->src[5].ssa, miss_index, origin, tmin, direction, tmax, intr->src[11].ssa);

   /* Read the result back at once: the next shader call can overwrite the hit attribute storage. */
   nir_def *kind = load_record(b, RADV_HIT_OBJECT_RECORD_KIND);
   nir_def *is_hit = nir_ieq_imm(b, kind, RADV_HIT_OBJECT_KIND_HIT);
   nir_def *sbt_index = load_record(b, RADV_HIT_OBJECT_RECORD_SBT_INDEX);
   nir_def *hit_tmax = load_record(b, RADV_HIT_OBJECT_RECORD_TMAX);
   nir_def *primitive_addr = nir_pack_64_2x32_split(b, load_record(b, RADV_HIT_OBJECT_RECORD_PRIMITIVE_ADDR_LO),
                                                    load_record(b, RADV_HIT_OBJECT_RECORD_PRIMITIVE_ADDR_HI));
   nir_def *primitive_id = load_record(b, RADV_HIT_OBJECT_RECORD_PRIMITIVE_ID);
   nir_def *instance_addr = nir_pack_64_2x32_split(b, load_record(b, RADV_HIT_OBJECT_RECORD_INSTANCE_ADDR_LO),
                                                   load_record(b, RADV_HIT_OBJECT_RECORD_INSTANCE_ADDR_HI));
   nir_def *geometry_id_and_flags = load_record(b, RADV_HIT_OBJECT_RECORD_GEOMETRY_ID_AND_FLAGS);
   nir_def *hit_kind = load_record(b, RADV_HIT_OBJECT_RECORD_HIT_KIND);

   nir_def *attribs[RADV_MAX_HIT_ATTRIB_DWORDS];
   for (unsigned i = 0; i < RADV_MAX_HIT_ATTRIB_DWORDS; i++)
      attribs[i] = nir_load_hit_attrib_amd(b, .base = i);

   ho_store(b, ho, kind, nir_bcsel(b, is_hit, nir_imm_int(b, RADV_HIT_OBJECT_KIND_HIT),
                                   nir_imm_int(b, RADV_HIT_OBJECT_KIND_MISS)));
   /* The SPIR-V extension uses only the 16 least-significant bits of the miss index. */
   ho_store(b, ho, sbt_index, nir_bcsel(b, is_hit, sbt_index, nir_iand_imm(b, miss_index, 0xffff)));
   ho_store(b, ho, flags, flags);
   ho_store(b, ho, cull_mask, nir_iand_imm(b, cull_mask, 0xff));
   ho_store(b, ho, origin, origin);
   ho_store(b, ho, tmin, tmin);
   ho_store(b, ho, direction, direction);
   ho_store(b, ho, tmax, nir_bcsel(b, is_hit, hit_tmax, tmax));
   ho_store(b, ho, primitive_addr, primitive_addr);
   ho_store(b, ho, primitive_id, primitive_id);
   ho_store(b, ho, instance_addr, instance_addr);
   ho_store(b, ho, geometry_id_and_flags, geometry_id_and_flags);
   ho_store(b, ho, hit_kind, hit_kind);
   ho_store(b, ho, attribs, nir_vec(b, attribs, RADV_MAX_HIT_ATTRIB_DWORDS));
}

static void
lower_execute_shader(nir_builder *b, nir_intrinsic_instr *intr, nir_deref_instr *ho)
{
   /* A closest-hit shader reads its attributes from the hit attribute storage. */
   nir_def *attribs = ho_load(b, ho, attribs);
   for (unsigned i = 0; i < RADV_MAX_HIT_ATTRIB_DWORDS; i++)
      nir_store_hit_attrib_amd(b, nir_channel(b, attribs, i), .base = i);

   nir_def *cull_mask_and_flags =
      nir_ior(b, nir_ishl_imm(b, ho_load(b, ho, cull_mask), 24),
              nir_iand_imm(b, ho_load(b, ho, flags), RADV_RAY_FLAGS_API_MASK));

   nir_execute_hit_object_amd(b, ho_load(b, ho, kind), ho_load(b, ho, sbt_index), cull_mask_and_flags,
                              ho_load(b, ho, origin), ho_load(b, ho, tmin), ho_load(b, ho, direction),
                              ho_load(b, ho, tmax), ho_load(b, ho, primitive_addr), ho_load(b, ho, primitive_id),
                              ho_load(b, ho, instance_addr), ho_load(b, ho, geometry_id_and_flags),
                              ho_load(b, ho, hit_kind), intr->src[1].ssa);
}

static void
lower_record_miss(nir_builder *b, nir_intrinsic_instr *intr, nir_deref_instr *ho)
{
   ho_store(b, ho, kind, nir_imm_int(b, RADV_HIT_OBJECT_KIND_MISS));
   /* D3D12 uses only the 16 least-significant bits of the miss shader index. */
   ho_store(b, ho, sbt_index, nir_iand_imm(b, intr->src[2].ssa, 0xffff));
   ho_store(b, ho, flags, intr->src[1].ssa);
   ho_store(b, ho, cull_mask, nir_imm_int(b, 0));
   ho_store(b, ho, origin, intr->src[3].ssa);
   ho_store(b, ho, tmin, intr->src[4].ssa);
   ho_store(b, ho, direction, intr->src[5].ssa);
   ho_store(b, ho, tmax, intr->src[6].ssa);
}

static nir_def *
rq_value(nir_builder *b, nir_def *rq, unsigned num_components, unsigned bit_size, nir_ray_query_value value,
         bool committed)
{
   return nir_rq_load(b, num_components, bit_size, rq, .ray_query_value = value, .committed = committed);
}

static void
lower_record_from_query(nir_builder *b, nir_intrinsic_instr *intr, nir_deref_instr *ho)
{
   nir_def *rq = intr->src[1].ssa;

   /* 0 is no committed intersection, 1 a triangle and 2 an AABB (RayQueryCommittedIntersectionTypeKHR). */
   nir_def *type = rq_value(b, rq, 1, 32, nir_ray_query_value_intersection_type, true);
   nir_def *is_triangle = nir_ieq_imm(b, type, 1);

   nir_def *front_face = rq_value(b, rq, 1, 1, nir_ray_query_value_intersection_front_face, true);
   nir_def *triangle_hit_kind = nir_bcsel(b, front_face, nir_imm_int(b, 0xfe), nir_imm_int(b, 0xff));

   nir_def *barycentrics = rq_value(b, rq, 2, 32, nir_ray_query_value_intersection_barycentrics, true);
   nir_def *triangle_attribs[RADV_MAX_HIT_ATTRIB_DWORDS];
   for (unsigned i = 0; i < RADV_MAX_HIT_ATTRIB_DWORDS; i++)
      triangle_attribs[i] = i < 2 ? nir_channel(b, barycentrics, i) : nir_imm_int(b, 0);

   /* Without a committed intersection the hit object is empty, as in D3D12 HitObject::FromRayQuery. */
   ho_store(b, ho, kind,
            nir_bcsel(b, nir_ieq_imm(b, type, 0), nir_imm_int(b, RADV_HIT_OBJECT_KIND_EMPTY),
                      nir_imm_int(b, RADV_HIT_OBJECT_KIND_HIT)));
   ho_store(b, ho, sbt_index, intr->src[2].ssa);
   ho_store(b, ho, flags, rq_value(b, rq, 1, 32, nir_ray_query_value_flags, false));
   ho_store(b, ho, cull_mask, rq_value(b, rq, 1, 32, nir_ray_query_value_cull_mask, false));
   ho_store(b, ho, origin, rq_value(b, rq, 3, 32, nir_ray_query_value_world_ray_origin, false));
   ho_store(b, ho, tmin, rq_value(b, rq, 1, 32, nir_ray_query_value_tmin, false));
   ho_store(b, ho, direction, rq_value(b, rq, 3, 32, nir_ray_query_value_world_ray_direction, false));
   ho_store(b, ho, tmax, rq_value(b, rq, 1, 32, nir_ray_query_value_intersection_t, true));
   ho_store(b, ho, primitive_addr, rq_value(b, rq, 1, 64, nir_ray_query_value_intersection_primitive_addr_amd, true));
   ho_store(b, ho, primitive_id, rq_value(b, rq, 1, 32, nir_ray_query_value_intersection_primitive_index, true));
   ho_store(b, ho, instance_addr, rq_value(b, rq, 1, 64, nir_ray_query_value_intersection_instance_addr_amd, true));
   ho_store(b, ho, geometry_id_and_flags,
            rq_value(b, rq, 1, 32, nir_ray_query_value_intersection_geometry_index, true));
   ho_store(b, ho, hit_kind, nir_bcsel(b, is_triangle, triangle_hit_kind, intr->src[4].ssa));
   ho_store(b, ho, attribs,
            nir_bcsel(b, is_triangle, nir_vec(b, triangle_attribs, RADV_MAX_HIT_ATTRIB_DWORDS), intr->src[3].ssa));
}

static void
lower_set_sbt_record_index(nir_builder *b, nir_intrinsic_instr *intr, nir_deref_instr *ho)
{
   /* The index of an empty hit object stays 0, as D3D12 ignores the value for a NOP HitObject. */
   nir_def *is_empty = nir_ieq_imm(b, ho_load(b, ho, kind), RADV_HIT_OBJECT_KIND_EMPTY);
   ho_store(b, ho, sbt_index, nir_bcsel(b, is_empty, nir_imm_int(b, 0), intr->src[1].ssa));
}

/* Values that only a hit has. They are 0 for a miss and for an empty hit object. The memory loads that they need
 * run only for a hit, because a miss has no instance or primitive address.
 */
static nir_def *
load_hit_value(nir_builder *b, const struct radv_compiler_info *compiler_info, nir_intrinsic_instr *intr,
               nir_deref_instr *ho)
{
   const unsigned num_components = intr->def.num_components;
   const unsigned bit_size = intr->def.bit_size;
   const unsigned column = nir_intrinsic_column(intr);
   nir_def *is_hit = nir_ieq_imm(b, ho_load(b, ho, kind), RADV_HIT_OBJECT_KIND_HIT);
   /* The else value of the phi is defined before the if: after nir_pop_if it would land between the if and the phi. */
   nir_def *zero = nir_imm_zero(b, num_components, bit_size);

   nir_def *value;
   nir_push_if(b, is_hit);
   {
      nir_def *instance_addr = ho_load(b, ho, instance_addr);

      switch (nir_intrinsic_hit_object_value(intr)) {
      case nir_hit_object_value_object_ray_origin: {
         nir_def *wto_matrix[3];
         radv_load_wto_matrix(compiler_info, b, instance_addr, wto_matrix);
         value = nir_build_vec3_mat_mult(b, ho_load(b, ho, origin), wto_matrix, true);
         break;
      }
      case nir_hit_object_value_object_ray_direction: {
         nir_def *wto_matrix[3];
         radv_load_wto_matrix(compiler_info, b, instance_addr, wto_matrix);
         value = nir_build_vec3_mat_mult(b, ho_load(b, ho, direction), wto_matrix, false);
         break;
      }
      case nir_hit_object_value_object_to_world: {
         nir_def *otw_matrix[3];
         radv_load_otw_matrix(compiler_info, b, instance_addr, otw_matrix);
         value = nir_vec3(b, nir_channel(b, otw_matrix[0], column), nir_channel(b, otw_matrix[1], column),
                          nir_channel(b, otw_matrix[2], column));
         break;
      }
      case nir_hit_object_value_world_to_object: {
         nir_def *wto_matrix[3];
         radv_load_wto_matrix(compiler_info, b, instance_addr, wto_matrix);
         value = nir_vec3(b, nir_channel(b, wto_matrix[0], column), nir_channel(b, wto_matrix[1], column),
                          nir_channel(b, wto_matrix[2], column));
         break;
      }
      case nir_hit_object_value_instance_id:
         value = radv_load_instance_id(compiler_info, b, instance_addr);
         break;
      case nir_hit_object_value_instance_custom_index:
         value = radv_load_custom_instance(compiler_info, b, instance_addr);
         break;
      case nir_hit_object_value_geometry_index:
         value = nir_iand_imm(b, ho_load(b, ho, geometry_id_and_flags), 0xfffffff);
         break;
      case nir_hit_object_value_primitive_index:
         value = ho_load(b, ho, primitive_id);
         break;
      case nir_hit_object_value_hit_kind:
         value = ho_load(b, ho, hit_kind);
         break;
      case nir_hit_object_value_triangle_vertex_positions:
         value = radv_load_vertex_position(compiler_info, b, ho_load(b, ho, primitive_addr), column);
         break;
      case nir_hit_object_value_attributes:
         value = ho_load(b, ho, attribs);
         break;
      default:
         UNREACHABLE("Not a hit value");
      }
   }
   nir_pop_if(b, NULL);

   assert(value->num_components == num_components && value->bit_size == bit_size);
   return nir_if_phi(b, value, zero);
}

static nir_def *
load_shader_record_buffer_handle(nir_builder *b, nir_deref_instr *ho)
{
   nir_def *kind = ho_load(b, ho, kind);
   nir_def *sbt_index = ho_load(b, ho, sbt_index);

   nir_def *hit_ptr, *miss_ptr;
   nir_def *null_ptr = nir_imm_int64(b, 0);
   nir_push_if(b, nir_ieq_imm(b, kind, RADV_HIT_OBJECT_KIND_HIT));
   {
      hit_ptr = radv_nir_load_sbt_entry(b, nir_load_sbt_base_amd(b), sbt_index, SBT_HIT, SBT_RECURSIVE_PTR)
                   .shader_record_ptr;
   }
   nir_push_else(b, NULL);
   {
      nir_push_if(b, nir_ieq_imm(b, kind, RADV_HIT_OBJECT_KIND_MISS));
      {
         miss_ptr = radv_nir_load_sbt_entry(b, nir_load_sbt_base_amd(b), sbt_index, SBT_MISS, SBT_RECURSIVE_PTR)
                       .shader_record_ptr;
      }
      nir_pop_if(b, NULL);
      miss_ptr = nir_if_phi(b, miss_ptr, null_ptr);
   }
   nir_pop_if(b, NULL);

   return nir_unpack_64_2x32(b, nir_if_phi(b, hit_ptr, miss_ptr));
}

static nir_def *
lower_load(nir_builder *b, const struct radv_compiler_info *compiler_info, nir_intrinsic_instr *intr,
           nir_deref_instr *ho)
{
   switch (nir_intrinsic_hit_object_value(intr)) {
   case nir_hit_object_value_is_empty:
      return nir_ieq_imm(b, ho_load(b, ho, kind), RADV_HIT_OBJECT_KIND_EMPTY);
   case nir_hit_object_value_is_hit:
      return nir_ieq_imm(b, ho_load(b, ho, kind), RADV_HIT_OBJECT_KIND_HIT);
   case nir_hit_object_value_is_miss:
      return nir_ieq_imm(b, ho_load(b, ho, kind), RADV_HIT_OBJECT_KIND_MISS);
   case nir_hit_object_value_ray_flags:
      return ho_load(b, ho, flags);
   case nir_hit_object_value_ray_t_min:
      return ho_load(b, ho, tmin);
   case nir_hit_object_value_ray_t_max:
      return ho_load(b, ho, tmax);
   case nir_hit_object_value_world_ray_origin:
      return ho_load(b, ho, origin);
   case nir_hit_object_value_world_ray_direction:
      return ho_load(b, ho, direction);
   case nir_hit_object_value_sbt_record_index:
      return ho_load(b, ho, sbt_index);
   case nir_hit_object_value_shader_record_buffer_handle:
      return load_shader_record_buffer_handle(b, ho);
   default:
      return load_hit_value(b, compiler_info, intr, ho);
   }
}

static bool
lower_hit_objects_impl(nir_function_impl *impl, struct lower_hit_objects_state *state)
{
   bool progress = false;
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic == nir_intrinsic_reorder_thread) {
            nir_instr_remove(instr);
            progress = true;
            continue;
         }

         if (!nir_intrinsic_is_hit_object(intr->intrinsic))
            continue;

         nir_deref_instr *opaque_deref = nir_src_as_deref(intr->src[0]);
         nir_variable *var = create_hit_object_var(impl->function->shader, nir_deref_instr_get_variable(opaque_deref),
                                                   state);

         b.cursor = nir_before_instr(instr);
         nir_deref_instr *ho = rebuild_deref(&b, opaque_deref, var);

         nir_def *def = NULL;
         switch (intr->intrinsic) {
         case nir_intrinsic_hit_object_trace_ray:
            lower_trace_ray(&b, intr, ho);
            break;
         case nir_intrinsic_hit_object_execute_shader:
            lower_execute_shader(&b, intr, ho);
            break;
         case nir_intrinsic_hit_object_record_empty:
            store_empty(&b, ho);
            break;
         case nir_intrinsic_hit_object_record_miss:
            lower_record_miss(&b, intr, ho);
            break;
         case nir_intrinsic_hit_object_record_from_query:
            lower_record_from_query(&b, intr, ho);
            break;
         case nir_intrinsic_hit_object_set_sbt_record_index:
            lower_set_sbt_record_index(&b, intr, ho);
            break;
         case nir_intrinsic_hit_object_reorder_thread:
            break;
         case nir_intrinsic_load_hit_object:
            def = lower_load(&b, state->compiler_info, intr, ho);
            break;
         default:
            UNREACHABLE("Unsupported hit object intrinsic");
         }

         if (def)
            nir_def_rewrite_uses(&intr->def, def);

         nir_instr_remove(instr);
         progress = true;
      }
   }

   return nir_progress(progress, impl, nir_metadata_none);
}

bool
radv_nir_lower_hit_objects(nir_shader *shader, const struct radv_compiler_info *compiler_info)
{
   if (!mesa_shader_stage_is_rt(shader->info.stage))
      return false;

   struct lower_hit_objects_state state = {
      .compiler_info = compiler_info,
      .vars = _mesa_pointer_hash_table_create(NULL),
   };

   bool progress = false;
   nir_foreach_function_impl (impl, shader)
      progress |= lower_hit_objects_impl(impl, &state);

   _mesa_hash_table_destroy(state.vars, NULL);

   if (progress) {
      NIR_PASS(_, shader, nir_remove_dead_derefs);
      NIR_PASS(_, shader, nir_remove_dead_variables, nir_var_shader_temp | nir_var_function_temp, NULL);
      NIR_PASS(_, shader, nir_split_struct_vars, nir_var_shader_temp);
      NIR_PASS(_, shader, nir_lower_global_vars_to_local);
      NIR_PASS(_, shader, nir_lower_vars_to_ssa);
   }

   return progress;
}
