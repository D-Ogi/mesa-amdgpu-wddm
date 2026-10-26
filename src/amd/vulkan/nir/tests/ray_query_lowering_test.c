/* SPDX-License-Identifier: MIT */
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir/radv_nir.h"
#include "radv_shader.h"
#include "tools/radv_rra.h"
#include <stdio.h>
#include <stdlib.h>

/* RRA tracing is disabled in this CPU test. Trap rather than silently emulate
 * the two diagnostic-only callbacks from the separate pipeline tracer.
 */
nir_def *radv_build_token_begin(nir_builder *b, const struct radv_compiler_info *info,
                               enum radv_packed_token_type type, uint32_t size) { abort(); }
void radv_build_token_end(nir_builder *b) { abort(); }

int main(void)
{
   glsl_type_singleton_init_or_ref();
   nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "rq-real-pass");
   b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   b.shader->info.ray_queries = 1;
   nir_variable *v = nir_local_variable_create(b.impl, glsl_uint_type(), "query");
   v->data.ray_query = true;
   nir_deref_instr *q = nir_build_deref_var(&b, v);
   nir_rq_initialize(&b, &q->def, nir_imm_int64(&b, 0), nir_imm_int(&b, 0), nir_imm_int(&b, 255),
                     nir_imm_vec3(&b, 0, 0, 0), nir_imm_float(&b, 0), nir_imm_vec3(&b, 0, 0, 1), nir_imm_float(&b, 100));
   nir_push_loop(&b);
   nir_def *p = nir_rq_proceed(&b, 1, &q->def);
   nir_push_if(&b, nir_inot(&b, p));
   nir_jump(&b, nir_jump_break);
   nir_pop_if(&b, NULL);
   nir_pop_loop(&b, NULL);
   nir_metadata_require(b.impl, nir_metadata_block_index);
   struct ac_compiler_info ac = {0};
   ac.gfx_level = GFX10_3;
   ac.lds_size_per_workgroup = 65536;
   struct radv_compiler_info info = {.ac = &ac};
   puts("begin actual radv_nir_lower_ray_queries");
   fflush(stdout);
   if (!radv_nir_lower_ray_queries(b.shader, &info)) return 1;
   nir_validate_shader(b.shader, "after actual ray query lowering");
   puts("PASS actual ray query lowering completed");
   ralloc_free(b.shader);
   glsl_type_singleton_decref();
   return 0;
}
