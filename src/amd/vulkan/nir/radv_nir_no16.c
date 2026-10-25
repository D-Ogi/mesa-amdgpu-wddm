/* SPDX-License-Identifier: MIT
 * Fragment shader lowering of 16-sample draws without attachments (radv_no16.h).
 *
 * NATIVE16 needs no shader change: the 16-sample position table of the ring
 * (radv_nir_lower_abi.c) serves sample positions and at-sample interpolation.
 * SPLIT16 records each draw two times with 8 hardware samples. The shader is
 * compiled once; the PS state SGPR gives the pass base (0 or 8), which this
 * file adds to the sample index. The sample positions, the frag coord and the
 * per-sample barycentrics come from the programmed locations of the pass.
 * The vertex shader runs in both passes; its memory writes run in pass 0 only.
 */
#include "radv_nir_no16.h"
#include "nir_builder.h"
#include "ac_nir.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "util/u_dynarray.h"
#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
#include <stdio.h>
#include <stdlib.h>
#include "util/u_call_once.h"
#endif

#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
static enum radv_no16_auto_mode radv_no16_latched_mode;

/* Lab kill switch (radv_no16.h). A set variable prints one line. */
static void
radv_no16_auto_latch(void)
{
   const char *value = getenv("RADV_NO16_AUTO");
   radv_no16_latched_mode = radv_no16_auto_parse(value, getenv("RADV_S16F8_PROBE_MODE"));
   if (value && *value)
      fprintf(stderr, "RADV_NO16_AUTO mode=%s requested=%s\n", radv_no16_auto_mode_name(radv_no16_latched_mode),
              value);
}

static util_once_flag radv_no16_latch_once = UTIL_ONCE_FLAG_INIT;

enum radv_no16_auto_mode
radv_no16_auto_mode(void)
{
   util_call_once(&radv_no16_latch_once, radv_no16_auto_latch);
   return radv_no16_latched_mode;
}
#endif

static bool
split_early(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   (void)data;
   if (in->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(in).location != FRAG_RESULT_SAMPLE_MASK)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   /* The logical index of pass p is in [8p, 8p + 7], so its bit 3 is the base 8p. */
   nir_def *base = nir_iand_imm(b, nir_load_sample_id(b), 8);
   nir_src_rewrite(&in->src[0], nir_ushr(b, in->src[0].ssa, base));
   return true;
}

bool
radv_nir_no16_lower_split_early(nir_shader *fs)
{
   return nir_shader_intrinsics_pass(fs, split_early, nir_metadata_control_flow, NULL);
}

bool
radv_nir_no16_lower_split_late(nir_shader *fs, const struct radv_shader_args *args)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(fs);
   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   /* The base is read once at the start; every hardware index of the
    * original shader is rewritten, the new loads are not. */
   b.cursor = nir_before_impl(impl);
   nir_def *state = ac_nir_load_arg(&b, &args->ac, args->ps_state);
   nir_def *base = nir_ubfe_imm(&b, state, PS_STATE_NO16_BASE__SHIFT, util_bitcount(PS_STATE_NO16_BASE__MASK));

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_load_sample_id)
            continue;
         b.cursor = nir_after_instr(instr);
         nir_def *logical = nir_iadd(&b, &in->def, base);
         nir_def_rewrite_uses_after(&in->def, logical);
         progress = true;
      }
   }
   return nir_progress(progress, impl, nir_metadata_control_flow);
}

bool
radv_nir_no16_memory_result_used(nir_shader *s)
{
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (nir_intrinsic_writes_external_memory(in) && nir_intrinsic_infos[in->intrinsic].has_dest &&
                !nir_def_is_unused(&in->def))
               return true;
         }
      }
   }
   return false;
}

bool
radv_nir_no16_guard_vs_memory(nir_shader *vs, const struct radv_shader_args *args)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(vs);
   struct util_dynarray writes;
   util_dynarray_init(&writes, NULL);
   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             nir_intrinsic_writes_external_memory(nir_instr_as_intrinsic(instr)))
            util_dynarray_append_typed(&writes, nir_instr *, instr);
      }
   }
   if (!util_dynarray_num_elements(&writes, nir_instr *)) {
      util_dynarray_fini(&writes);
      return nir_no_progress(impl);
   }

   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_impl(impl);
   nir_def *state = ac_nir_load_arg(&b, &args->ac, args->ngg_state);
   nir_def *first_pass =
      nir_ieq_imm(&b, nir_ubfe_imm(&b, state, NGG_STATE_NO16_PASS__SHIFT, util_bitcount(NGG_STATE_NO16_PASS__MASK)), 0);

   util_dynarray_foreach (&writes, nir_instr *, it) {
      nir_instr *instr = *it;
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      b.cursor = nir_before_instr(instr);
      nir_push_if(&b, first_pass);
      nir_instr_remove(instr);
      nir_builder_instr_insert(&b, instr);
      nir_pop_if(&b, NULL);
      /* radv_no16_lower_fs refuses a used result; a later pass that made one
       * gets an undefined value in pass 1. */
      if (nir_intrinsic_infos[in->intrinsic].has_dest && !nir_def_is_unused(&in->def)) {
         nir_def *undef = nir_undef(&b, in->def.num_components, in->def.bit_size);
         nir_def *phi = nir_if_phi(&b, &in->def, undef);
         nir_def_rewrite_uses_after(&in->def, phi);
      }
   }
   util_dynarray_fini(&writes);
   return nir_progress(true, impl, nir_metadata_none);
}
