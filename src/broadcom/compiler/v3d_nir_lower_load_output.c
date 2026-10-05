
#include "util/format/u_format.h"
#include "compiler/nir/nir_builder.h"
#include "v3d_compiler.h"

nir_def *
v3d_nir_get_tlb_color(nir_builder *b, struct v3d_compile *c, int rt, int sample,
                      unsigned component, unsigned num_components)
{
        assert(num_components > 0 && component + num_components <= 4);

        return nir_load_tlb_color_brcm(b, num_components, 32,
                                       nir_imm_int(b, rt),
                                       .base = sample,
                                       .component = component);
}

static bool
lower_tlb_load(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
        const bool is_tile_image =
                intr->intrinsic == nir_intrinsic_load_tile_image;
        if (!is_tile_image && intr->intrinsic != nir_intrinsic_load_output)
                return false;

        b->cursor = nir_before_instr(&intr->instr);

        struct v3d_compile *c = data;

        nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
        unsigned rt = sem.location - FRAG_RESULT_DATA0;
        if (is_tile_image)
                rt += nir_src_as_uint(intr->src[0]);

        unsigned component = nir_intrinsic_component(intr);
        unsigned num_components = intr->def.num_components;

        unsigned num_samples = c->fs_key->msaa ? V3D_MAX_SAMPLES : 1;
        assert(num_samples > 0);

        nir_def *sample_id = NULL;
        if (num_samples > 1) {
                /* FIXME: if sample_id is a const value we don't need to emit loads
                 * for all the samples and then bcsel, we can load directly the sample
                 * we need.
                 */
                sample_id = is_tile_image ? intr->src[1].ssa :
                                            nir_load_sample_id(b);
        }
        nir_def *out = v3d_nir_get_tlb_color(b, c, rt, 0,
                                             component, num_components);

        /* If msaa, run through every sample and, if it matches the current sample
         * id, use it.
         */
        for (unsigned i = 1; i < num_samples; i++) {
                nir_def *is_cur_sample = nir_ieq_imm(b, sample_id, i);
                nir_def *val = v3d_nir_get_tlb_color(b, c, rt, i,
                                                     component, num_components);
                out = nir_bcsel(b, is_cur_sample, val, out);
        }

        nir_def_replace(&intr->def, out);

        return true;
}

bool
v3d_nir_lower_tlb_loads(nir_shader *s, struct v3d_compile *c)
{
        return nir_shader_intrinsics_pass(s, lower_tlb_load,
                                          nir_metadata_control_flow,
                                          c);
}
