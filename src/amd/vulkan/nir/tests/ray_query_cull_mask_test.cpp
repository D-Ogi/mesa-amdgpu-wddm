/* SPDX-License-Identifier: MIT */
#include "../radv_nir_ray_query_cull_mask.h"
#include "nir/nir_builder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static nir_intrinsic_instr *
initialize(nir_builder *b, nir_deref_instr *q, nir_def *mask)
{
   return nir_rq_initialize(b, &q->def, nir_imm_int64(b, 0), nir_imm_int(b, 0), mask,
                     nir_imm_vec3(b, 0, 0, 0), nir_imm_float(b, 0),
                     nir_imm_vec3(b, 0, 0, 1), nir_imm_float(b, 100));
}

int main()
{
   glsl_type_singleton_init_or_ref();
   nir_shader_compiler_options options = {};
   unsigned checks = 0;
   const char *names[] = {"full", "partial", "dynamic", "absent", "after", "reinit-full", "reinit-mixed",
                         "branch-only", "both-branches", "dominating-branch", "loop-before", "loop-after",
                         "array", "array-dynamic", "other-query", "freeze-after-free"};
   for (unsigned c = 0; c < sizeof(names) / sizeof(names[0]); ++c) {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, names[c]);
      const bool array = c == 12 || c == 13;
      nir_variable *v = nir_local_variable_create(b.impl, array ? glsl_array_type(glsl_uint_type(), 2, 0)
                                                               : glsl_uint_type(), "query");
      v->data.ray_query = true;
      nir_deref_instr *q = nir_build_deref_var(&b, v);
      if (array)
         q = nir_build_deref_array(&b, q, c == 13 ? nir_load_local_invocation_index(&b) : nir_imm_int(&b, 0));
      nir_intrinsic_instr *init = nullptr;
      if (c == 10 || c == 11)
         nir_push_loop(&b);
      if (c == 7 || c == 8) {
         nir_push_if(&b, nir_ieq_imm(&b, nir_load_local_invocation_index(&b), 0));
         initialize(&b, q, nir_imm_int(&b, 255));
         if (c == 8) {
            nir_push_else(&b, nullptr);
            initialize(&b, q, nir_imm_int(&b, 255));
         }
         nir_pop_if(&b, nullptr);
      } else if (c != 3 && c != 4 && c != 11) {
         init = initialize(&b, q, c == 2 ? nir_load_local_invocation_index(&b) : nir_imm_int(&b, c == 1 ? 127 : 255));
      }
      if (c == 5 || c == 6)
         initialize(&b, q, nir_imm_int(&b, c == 6 ? 127 : 255));
      if (c == 9) {
         nir_push_if(&b, nir_ieq_imm(&b, nir_load_local_invocation_index(&b), 0));
         initialize(&b, q, nir_imm_int(&b, 255));
         nir_pop_if(&b, nullptr);
      }
      if (c == 14) {
         nir_variable *other = nir_local_variable_create(b.impl, glsl_uint_type(), "other");
         other->data.ray_query = true;
         initialize(&b, nir_build_deref_var(&b, other), nir_imm_int(&b, 127));
      }
      nir_def *p = nir_rq_proceed(&b, 1, &q->def);
      nir_instr *proceed = nir_def_instr(p);
      if (c == 4 || c == 11)
         initialize(&b, q, nir_imm_int(&b, 255));
      if (c == 10 || c == 11) {
         nir_push_if(&b, nir_imm_true(&b));
         nir_jump(&b, nir_jump_break);
         nir_pop_if(&b, nullptr);
         nir_pop_loop(&b, nullptr);
      }
      nir_validate_shader(b.shader, "ray query analysis input");
      struct set *set = radv_nir_ray_query_full_mask_proceeds(b.shader);
      const bool expected = c == 0 || c == 5 || c == 9 || c == 10 || c == 14 || c == 15;
      if (c == 15) {
         /* Model the real lowering's deletion followed by CFG insertion. The
          * decision is already complete and needs no freed instruction or CFG.
          */
         nir_instr_remove(&init->instr);
         nir_instr_free(&init->instr);
         b.cursor = nir_before_impl(b.impl);
         nir_push_if(&b, nir_imm_true(&b));
         nir_pop_if(&b, nullptr);
      }
      const bool actual = _mesa_set_search(set, proceed) != nullptr;
      if (actual != expected) {
         fprintf(stderr, "FAIL %s: got %d expected %d\n", names[c], actual, expected);
         return 1;
      }
      printf("PASS %s elide=%d\n", names[c], actual);
      ++checks;
      _mesa_set_destroy(set, nullptr);
      ralloc_free(b.shader);
   }
   glsl_type_singleton_decref();
   printf("PASS %u actual NIR analysis cases\n", checks);
   return 0;
}
