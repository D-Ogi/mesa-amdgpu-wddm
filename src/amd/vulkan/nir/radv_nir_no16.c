/* SPDX-License-Identifier: MIT
 * Lab-only automatic no-output16 transform for the frozen Stage B VS/FS pair.
 * Ported from the host prototype scratch/bd232 R3 (no-output16-r3-body-prototype.c)
 * with these changes: RADV names, the negative lab modes, and admission of
 * load_front_face_select_amd, which radv_nir_opt_fs_builtins adds before
 * radv_nir_lower_io. PROVENANCE: Mesa upstream MIT; uses existing NIR
 * cloning, inlining and helper lowering.
 *
 * Contract: the wrapper runs once per physical sample ID p of an 8-sample
 * pipeline with 16 coverage samples. It runs the original body for logical
 * IDs p and p+8, each with its own coverage bit, helper state, standard16
 * sample location, logical fragment position and interpolation offset.
 * Only observable stores and atomics are guarded by the logical helper
 * state; demote and terminate end one logical phase, never the second one.
 * Ownership: every allocation below has the shader or a nir_function
 * (ralloc) as its parent; nir_variable and instructions are GC objects
 * (nir.c nir_variable_create, nir_instr_create) and are never ralloc parents.
 */
#include "radv_nir_no16.h"
#include "nir_builder.h"
#include "util/mesa-blake3.h"
#include "util/u_call_once.h"

const uint32_t radv_no16_stageb_ps_blake3[8] = {0x0e8ba7fe, 0xc140abe9, 0x8ca99153, 0xb00eb11b,
                                                0x4e11f0a7, 0x7c4f9762, 0xdce51af0, 0x111d8d51};
const uint32_t radv_no16_stageb_vs_blake3[8] = {0x15459962, 0x346f6ae5, 0xcaa484c0, 0x0965e5ac,
                                                0xb7b65bec, 0xaf35daf1, 0x725cc11c, 0x31a10df2};

static enum radv_no16_auto_mode radv_no16_latched_mode;

static void
radv_no16_auto_latch(void)
{
   const char *value = getenv("RADV_NO16_AUTO");
   radv_no16_latched_mode = radv_no16_auto_parse(value, getenv("RADV_S16F8_PROBE_MODE"));
   if (value && *value)
      fprintf(stderr, "RADV_NO16_AUTO mode=%s requested=%s\n", radv_no16_auto_mode_name(radv_no16_latched_mode),
              value);
}

enum radv_no16_auto_mode
radv_no16_auto_mode(void)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;
   util_call_once(&once, radv_no16_auto_latch);
   return radv_no16_latched_mode;
}

const char *
radv_no16_result_name(enum radv_no16_result r)
{
   static const char *names[] = {"ok", "stage", "functions", "globals", "output", "instruction", "intrinsic",
                                 "helper-access", "exit-with-quad", "control-with-quad", "loop", "fixture",
                                 "identity", "forced-refusal", "mode"};
   return (unsigned)r < ARRAY_SIZE(names) ? names[r] : "invalid";
}

void
radv_no16_format_blake3(const uint32_t blake3[8], char out[65])
{
   unsigned char bytes[32];
   memcpy(bytes, blake3, sizeof(bytes));
   _mesa_blake3_format(out, bytes);
}

static bool
is_quad(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy_coarse:
      return true;
   default:
      return false;
   }
}

struct fixture_state {
   enum radv_no16_auto_mode mode;
   nir_intrinsic_instr *zw;
};

/* Vulkan standard16 locations in sample-index order, in 1/16 pixel units
 * (ref/Vulkan-Docs primsrast.adoc standard-sample-locations; the same table as
 * radv_device.c sample_locs_16x_probe). */
static nir_def *
standard16_position(nir_builder *b, nir_def *id)
{
   static const unsigned pos[16][2] = {{9, 9}, {7, 5},  {5, 10}, {12, 7}, {3, 6},  {10, 13}, {13, 11}, {11, 3},
                                       {6, 14}, {8, 1}, {4, 2},  {2, 12}, {0, 8},  {15, 4},  {14, 15}, {1, 0}};
   nir_def *v = nir_undef(b, 2, 32);
   for (unsigned i = 0; i < 16; i++)
      v = nir_bcsel(b, nir_ieq_imm(b, id, i), nir_imm_vec2(b, pos[i][0] / 16.f, pos[i][1] / 16.f), v);
   return v;
}

static bool
admitted(nir_intrinsic_instr *in)
{
   if (is_quad(in->intrinsic))
      return true;
   switch (in->intrinsic) {
   case nir_intrinsic_load_sample_id:
   case nir_intrinsic_load_sample_mask_in:
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_is_helper_invocation:
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_load_global:
   case nir_intrinsic_store_global:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_push_constant:
   case nir_intrinsic_load_frag_coord:
   case nir_intrinsic_load_frag_coord_xy:
   case nir_intrinsic_load_frag_coord_z:
   case nir_intrinsic_load_frag_coord_w:
   case nir_intrinsic_load_frag_coord_w_rcp:
   case nir_intrinsic_load_front_face:
   /* Per-draw user SGPR field (radv_nir_lower_abi.c), the same for both logical phases. */
   case nir_intrinsic_load_front_face_select_amd:
   case nir_intrinsic_load_primitive_id:
   case nir_intrinsic_load_sample_pos:
   case nir_intrinsic_load_sample_pos_or_center:
   case nir_intrinsic_load_rasterization_samples_amd:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_at_offset:
      return true;
   case nir_intrinsic_load_deref:
   case nir_intrinsic_store_deref:
      return nir_deref_mode_is(nir_src_as_deref(in->src[0]), nir_var_function_temp);
   default:
      return false;
   }
}

static enum radv_no16_result
preflight(nir_shader *s, const nir_shader *vs, struct fixture_state *f)
{
   unsigned input_mask = 0;
   if (f->mode == RADV_NO16_AUTO_OFF)
      return RADV_NO16_MODE;
   if (s->info.stage != MESA_SHADER_FRAGMENT)
      return RADV_NO16_STAGE;
   /* Exact frozen host-engine SPIR-V pair, including its packed IO. The caller
    * supplies the actual linked VS NIR, not a boolean assertion. */
   if (!vs || vs->info.stage != MESA_SHADER_VERTEX ||
       memcmp(s->info.source_blake3, radv_no16_stageb_ps_blake3, sizeof(s->info.source_blake3)) ||
       memcmp(vs->info.source_blake3, radv_no16_stageb_vs_blake3, sizeof(vs->info.source_blake3)))
      return RADV_NO16_IDENTITY;
   if (f->mode == RADV_NO16_AUTO_NEG_REFUSE)
      return RADV_NO16_FORCED_REFUSAL;
   unsigned functions = 0;
   nir_foreach_function_impl (impl, s) {
      functions++;
   }
   if (functions != 1)
      return RADV_NO16_FUNCTIONS;
   nir_foreach_variable_with_modes (v, s, nir_var_shader_temp | nir_var_mem_shared) {
      (void)v;
      return RADV_NO16_GLOBALS;
   }
   if (s->info.outputs_written)
      return RADV_NO16_OUTPUT;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   if (impl->function->num_params)
      return RADV_NO16_FUNCTIONS;
   bool exits = false, quad = false, cf = false;
   nir_foreach_block (block, impl) {
      if (block->cf_node.parent->type != nir_cf_node_function)
         cf = true;
      nir_foreach_instr (i, block) {
         switch (i->type) {
         case nir_instr_type_alu:
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
         case nir_instr_type_phi:
            break;
         case nir_instr_type_deref:
            if (!nir_deref_mode_is(nir_instr_as_deref(i), nir_var_function_temp))
               return RADV_NO16_GLOBALS;
            break;
         case nir_instr_type_jump: {
            nir_jump_instr *j = nir_instr_as_jump(i);
            if (j->type == nir_jump_return) {
               exits |= block != nir_impl_last_block(impl);
            } else if (j->type != nir_jump_break && j->type != nir_jump_continue) {
               return RADV_NO16_INSTRUCTION;
            }
            break;
         }
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(i);
            if (nir_intrinsic_has_access(in) && (nir_intrinsic_access(in) & ACCESS_INCLUDE_HELPERS))
               return RADV_NO16_HELPER_ACCESS;
            if (!admitted(in))
               return RADV_NO16_INTRINSIC;
            if (in->intrinsic == nir_intrinsic_load_input &&
                (nir_intrinsic_io_semantics(in).location != VARYING_SLOT_PRIMITIVE_ID || in->def.num_components != 1 ||
                 in->def.bit_size != 32 || nir_intrinsic_component(in) != 0 || !nir_src_is_const(in->src[0]) ||
                 nir_src_as_uint(in->src[0]) != 0))
               return RADV_NO16_FIXTURE;
            if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
               const nir_io_semantics sem = nir_intrinsic_io_semantics(in);
               unsigned component = nir_intrinsic_component(in), slot;
               if (sem.location == VARYING_SLOT_VAR1 && component == 0)
                  slot = 0;
               else if (sem.location == VARYING_SLOT_VAR2 && component == 0)
                  slot = 1;
               else if (sem.location == VARYING_SLOT_VAR1 && component == 2)
                  slot = 2;
               else
                  return RADV_NO16_FIXTURE;
               if (in->def.bit_size != 32 || in->def.num_components != 2 || !nir_src_is_const(in->src[1]) ||
                   nir_src_as_uint(in->src[1]) != 0)
                  return RADV_NO16_FIXTURE;
               input_mask |= 1u << slot;
               if (nir_def_instr(in->src[0].ssa)->type != nir_instr_type_intrinsic)
                  return RADV_NO16_FIXTURE;
               nir_intrinsic_instr *bary = nir_instr_as_intrinsic(nir_def_instr(in->src[0].ssa));
               if (bary->intrinsic != nir_intrinsic_load_barycentric_sample &&
                   bary->intrinsic != nir_intrinsic_load_barycentric_pixel &&
                   bary->intrinsic != nir_intrinsic_load_barycentric_at_sample &&
                   bary->intrinsic != nir_intrinsic_load_barycentric_at_offset)
                  return RADV_NO16_FIXTURE;
               enum glsl_interp_mode mode = nir_intrinsic_interp_mode(bary);
               if (slot == 1 ? (mode != INTERP_MODE_SMOOTH && mode != INTERP_MODE_NONE)
                             : mode != INTERP_MODE_NOPERSPECTIVE)
                  return RADV_NO16_FIXTURE;
               if (slot == 2) {
                  if (f->zw && nir_intrinsic_base(f->zw) != nir_intrinsic_base(in))
                     return RADV_NO16_FIXTURE;
                  f->zw = in;
               }
            }
            quad |= is_quad(in->intrinsic);
            exits |= in->intrinsic == nir_intrinsic_terminate || in->intrinsic == nir_intrinsic_terminate_if;
            break;
         }
         default:
            return RADV_NO16_INSTRUCTION;
         }
      }
   }
   if (quad && exits)
      return RADV_NO16_EXIT_QUAD;
   if (quad && cf)
      return RADV_NO16_CONTROL_QUAD;
   if (input_mask != 7)
      return RADV_NO16_FIXTURE;
   return RADV_NO16_OK;
}

static bool
phase_intrinsic(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   (void)data;
   unsigned param;
   switch (in->intrinsic) {
   case nir_intrinsic_load_sample_id:
      param = 0;
      break;
   case nir_intrinsic_load_helper_invocation:
      param = 1;
      break;
   case nir_intrinsic_load_sample_mask_in:
      param = 2;
      break;
   case nir_intrinsic_load_frag_coord:
      param = 3;
      break;
   case nir_intrinsic_load_frag_coord_xy:
   case nir_intrinsic_load_frag_coord_z:
   case nir_intrinsic_load_frag_coord_w:
   case nir_intrinsic_load_frag_coord_w_rcp: {
      b->cursor = nir_before_instr(&in->instr);
      nir_def *p = nir_load_param(b, 3), *v;
      if (in->intrinsic == nir_intrinsic_load_frag_coord_xy) {
         v = nir_channels(b, p, 3);
      } else {
         v = nir_channel(b, p, in->intrinsic == nir_intrinsic_load_frag_coord_z ? 2 : 3);
         if (in->intrinsic == nir_intrinsic_load_frag_coord_w_rcp)
            v = nir_frcp(b, v);
      }
      nir_def_replace(&in->def, v);
      return true;
   }
   case nir_intrinsic_load_sample_pos:
   case nir_intrinsic_load_sample_pos_or_center:
      param = 4;
      break;
   case nir_intrinsic_load_rasterization_samples_amd:
      b->cursor = nir_before_instr(&in->instr);
      nir_def_replace(&in->def, nir_imm_int(b, 16));
      return true;
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_at_sample: {
      b->cursor = nir_before_instr(&in->instr);
      nir_def *offset = in->intrinsic == nir_intrinsic_load_barycentric_sample
                           ? nir_load_param(b, 5)
                           : nir_fadd_imm(b, standard16_position(b, in->src[0].ssa), -.5f);
      nir_def_replace(&in->def,
                      nir_load_barycentric_at_offset(b, 32, offset, .interp_mode = nir_intrinsic_interp_mode(in)));
      return true;
   }
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
      nir_instr_remove(&in->instr);
      return true;
   default:
      return false;
   }
   b->cursor = nir_before_instr(&in->instr);
   nir_def *value = nir_load_param(b, param);
   if (in->def.bit_size != value->bit_size)
      value = param == 1 ? nir_b2iN(b, value, in->def.bit_size) : nir_u2uN(b, value, in->def.bit_size);
   nir_def_replace(&in->def, value);
   return true;
}

enum radv_no16_result
radv_nir_no16_lower_stageb(nir_shader *s, const nir_shader *linked_vs, enum radv_no16_auto_mode mode)
{
   struct fixture_state f = {.mode = mode};
   enum radv_no16_result result = preflight(s, linked_vs, &f);
   if (result != RADV_NO16_OK)
      return result; /* No mutation on refusal. */
   nir_function_impl *body = nir_shader_get_entrypoint(s);
   nir_validate_shader(s, "no16 original");
   nir_lower_variable_initializers(s, nir_var_function_temp);

   /* All halts here originate from a real terminate admitted above. Convert
    * shader termination to this template's local return before inlining. */
   nir_lower_terminate_to_demote(s);
   nir_foreach_block (block, body) {
      nir_foreach_instr (i, block) {
         if (i->type == nir_instr_type_jump && nir_instr_as_jump(i)->type == nir_jump_halt)
            nir_instr_as_jump(i)->type = nir_jump_return;
      }
   }
   nir_lower_returns(s);
   nir_lower_helper_writes(s, true);
   nir_lower_is_helper_invocation(s);

   body->function->is_entrypoint = false;
   nir_function *main = nir_function_create(s, "no16_wrapper");
   main->is_entrypoint = true;
   nir_function_impl *wrapper = nir_function_impl_create(main);
   body->function->num_params = 6;
   body->function->params = ralloc_array(body->function, nir_parameter, body->function->num_params);
   body->function->params[0] = (nir_parameter){.num_components = 1, .bit_size = 32};
   body->function->params[1] = (nir_parameter){.num_components = 1, .bit_size = 1};
   body->function->params[2] = (nir_parameter){.num_components = 1, .bit_size = 32};
   body->function->params[3] = (nir_parameter){.num_components = 4, .bit_size = 32};
   body->function->params[4] = (nir_parameter){.num_components = 2, .bit_size = 32};
   body->function->params[5] = (nir_parameter){.num_components = 2, .bit_size = 32};
   nir_shader_intrinsics_pass(s, phase_intrinsic, nir_metadata_none, &f);
   nir_validate_shader(s, "no16 phase template");

   nir_builder b = nir_builder_at(nir_before_impl(wrapper));
   nir_def *physical_id = nir_load_sample_id(&b);
   nir_def *coverage = nir_load_sample_mask_in(&b);
   nir_def *physical_helper = nir_load_helper_invocation(&b, 1);
   nir_def *physical_pos = nir_load_frag_coord_xy(&b);
   const unsigned phases = mode == RADV_NO16_AUTO_NEG_UPPER ? 1 : 2;
   for (unsigned phase = 0; phase < phases; phase++) {
      nir_def *id = nir_iadd_imm(&b, physical_id, phase * 8);
      nir_def *bit = nir_ishl(&b, nir_imm_int(&b, 1), id);
      nir_def *covered = nir_ine_imm(&b, nir_iand(&b, coverage, bit), 0);
      nir_def *helper = nir_ior(&b, physical_helper, nir_inot(&b, covered));
      nir_def *api_coverage = nir_bcsel(&b, helper, nir_imm_int(&b, 0), bit);
      nir_def *params[6] = {id, helper, api_coverage};
      /* The position negative keeps the logical ID and coverage bit of phase 1
       * but gives it the sample location of phase 0. */
      nir_def *location_id = mode == RADV_NO16_AUTO_NEG_POSITION ? physical_id : id;
      nir_def *position = standard16_position(&b, location_id);
      nir_def *offset = nir_fadd_imm(&b, position, -.5f);
      nir_def *bary = nir_load_barycentric_at_offset(&b, 32, offset, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
      nir_def *zw = nir_load_interpolated_input(&b, 2, 32, bary, nir_imm_int(&b, 0), .base = nir_intrinsic_base(f.zw),
                                                .component = nir_intrinsic_component(f.zw),
                                                .io_semantics = nir_intrinsic_io_semantics(f.zw),
                                                .dest_type = nir_type_float32);
      params[3] = nir_vec4(&b, nir_fadd(&b, nir_ffloor(&b, nir_channel(&b, physical_pos, 0)), nir_channel(&b, position, 0)),
                           nir_fadd(&b, nir_ffloor(&b, nir_channel(&b, physical_pos, 1)), nir_channel(&b, position, 1)),
                           nir_channel(&b, zw, 0), nir_channel(&b, zw, 1));
      params[4] = position;
      params[5] = offset;
      nir_inline_function_impl(&b, body, params, NULL);
      /* The generic inline helper inserts cloned definitions without assigning
       * caller-wide SSA indices. Reindex before adding the next phase or
       * running passes which use those indices as bitset keys. */
      nir_index_ssa_defs(wrapper);
      nir_progress(true, wrapper, nir_metadata_none);
   }
   nir_remove_non_entrypoints(s);
   nir_lower_vars_to_ssa(s);
   nir_opt_deref(s);
   nir_remove_dead_derefs(s);
   nir_shader_gather_info(s, wrapper);
   nir_validate_shader(s, "no16 body wrapper");
   return RADV_NO16_OK;
}
