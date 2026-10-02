/* SPDX-License-Identifier: MIT
 *
 * Colour exports to 10-bit UNORM targets. The FP16_ABGR export packs with
 * round-toward-zero and the CB converts the half floats with ROUND_BY_HALF,
 * which stores 0.6 as 613 where D3D asks for 614 (D3D11.3 functional spec,
 * 3.2.3.6 FLOAT -> UNORM: c * 1023, + 0.5, fraction dropped, within 0.6
 * ULP). radv_generate_ps_epilog_key marks unblended 10-bit UNORM targets in
 * color_round_unorm10, and the export (ac_nir_lower_ps_late for monolithic
 * shaders, the ACO PS epilog otherwise) rounds RGB to the UNORM10 grid
 * before the pack.
 *
 * Checks:
 *  - key: which formats, blend states and chips get the mask, and that
 *    radv_aco_convert_ps_epilog_key hands it to ACO;
 *  - NIR: the exports of ac_nir_lower_ps_late, evaluated instruction by
 *    instruction with nir_eval_const_opcode for about 1.3 million inputs and
 *    stored through a model of the CB: D3D's value exactly on a marked
 *    target, the round-toward-zero pack bit for bit on an unmarked target
 *    and for alpha;
 *  - ACO: the PS epilog, compiled for GFX10 as radv_create_ps_epilog does
 *    with the backend IR dumped, its IR after register allocation
 *    interpreted for the same inputs, with the same checks; ACO folds the
 *    last multiply into the fp16 conversion (v_fma_mix), which is checked
 *    with one rounding and with an fp32 result rounded again.
 *
 * The CB model (fp16 to UNORM10 with ROUND_BY_HALF, clamped, NaN to 0) is
 * the one assumption: CB_COLOR_INFO.ROUND_MODE is ROUND_BY_HALF for UNORM
 * (ac_descriptors.c; PAL documents the same). The rounded export also stays
 * within a quarter step of its grid point, which the test checks, so any
 * round-to-nearest conversion stores it exactly.
 *
 * Built outside meson, like winsys/wddm2/tests: compiled with the compile
 * command of radv_pipeline_graphics.c and linked against the
 * vulkan_radeon.dll objects and the libraries the DLL links.
 *
 * Usage: radv_unorm10_export_test [--no-round]
 *   --no-round: negative control. The marked target is lowered and compiled
 *   without the mask, and every check still expects D3D's values: it must
 *   fail.
 */
#include "radv_pipeline_graphics.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
/* After radv_shader.h, whose structures its inline functions read. */
#include "radv_aco_shader_info.h"

#include "compiler/glsl_types.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir/nir_constant_expressions.h"
#include "util/half_float.h"
#include "ac_gpu_info.h"
#include "ac_nir.h"
#include "ac_shader_util.h"
#include "aco_interface.h"
#include "amdgfxregs.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup    _dup
#define dup2   _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

static unsigned checks, failures;
static bool no_round;

#define CHECK(cond, ...)                                                                                               \
   do {                                                                                                                \
      checks++;                                                                                                        \
      if (!(cond)) {                                                                                                   \
         if (++failures <= 40) {                                                                                       \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);                                                     \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
         }                                                                                                             \
      }                                                                                                                \
   } while (0)

static float
f32(uint32_t bits)
{
   float f;
   memcpy(&f, &bits, 4);
   return f;
}

static uint32_t
bits32(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

/* D3D11.3 3.2.3.6 FLOAT -> UNORM for n = 10, each step in fp32. */
static unsigned
d3d_unorm10(float c)
{
   if (isnan(c))
      return 0;
   if (c > 1.0f)
      c = 1.0f;
   if (!(c >= 0.0f))
      c = 0.0f;
   volatile float t = c * 1023.0f;
   volatile float u = t + 0.5f;
   return (unsigned)u;
}

/* The CB storing a half float into a 10-bit UNORM channel: ROUND_BY_HALF, clamped, NaN as 0. */
static unsigned
cb_unorm10(uint16_t h)
{
   float f = _mesa_half_to_float(h);
   if (isnan(f))
      return 0;
   double s = floor((double)f * 1023.0 + 0.5);
   return s < 0.0 ? 0 : s > 1023.0 ? 1023 : (unsigned)s;
}

/* The five fp32 operations of the ACO epilog and of round_unorm10_for_rtz_pack. */
static float
host_round_unorm10(float c)
{
   volatile float t = c * 1023.0f;
   volatile float u = t + 0.5f;
   volatile float k = floorf(u);
   volatile float v = k + 0.25f;
   volatile float q = v * f32(0x3a802008u); /* 1 / 1023 */
   return q;
}

/* ---- key ---- */

static struct radeon_info gpu;
static struct radv_compiler_info compiler;

static void
init_compiler(bool rbplus)
{
   memset(&gpu, 0, sizeof(gpu));
   gpu.gfx_level = GFX10;
   gpu.family = CHIP_GFX1013;
   ac_fill_compiler_info(&gpu, NULL, false);

   memset(&compiler, 0, sizeof(compiler));
   compiler.ac = &gpu.compiler_info;
   compiler.hw.rbplus_allowed = rbplus;
   compiler.key.ps_wave_size = 64;
   compiler.debug.family = CHIP_GFX1013;
}

static struct radv_ps_epilog_key
epilog_key(VkFormat format, bool blend, uint32_t write_mask)
{
   struct radv_ps_epilog_state state = {0};
   state.color_attachment_count = 2;
   state.color_attachment_formats[0] = format;
   state.color_attachment_formats[1] = VK_FORMAT_B8G8R8A8_UNORM;
   state.color_attachment_mappings[0] = 0;
   state.color_attachment_mappings[1] = 1;
   state.color_write_mask = write_mask | 0xf0;
   state.color_blend_enable = blend ? 0x1 : 0x0;
   state.colors_written = 0xff;
   return radv_generate_ps_epilog_key(&compiler, &state);
}

static void
test_key(void)
{
   static const struct {
      VkFormat format;
      bool blend;
      uint32_t write_mask;
      bool rbplus;
      bool marked;
   } cases[] = {
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 0xf, false, true},
      {VK_FORMAT_A2R10G10B10_UNORM_PACK32, false, 0xf, false, true},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 0x7, false, true},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, true, 0xf, false, false},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 0xf, true, false},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 0x0, false, false},
      {VK_FORMAT_A2B10G10R10_UINT_PACK32, false, 0xf, false, false},
      {VK_FORMAT_A2B10G10R10_SNORM_PACK32, false, 0xf, false, false},
      {VK_FORMAT_B8G8R8A8_UNORM, false, 0xf, false, false},
      {VK_FORMAT_R16G16B16A16_UNORM, false, 0xf, false, false},
      {VK_FORMAT_R16G16B16A16_SFLOAT, false, 0xf, false, false},
      {VK_FORMAT_B10G11R11_UFLOAT_PACK32, false, 0xf, false, false},
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      init_compiler(cases[i].rbplus);
      struct radv_ps_epilog_key key = epilog_key(cases[i].format, cases[i].blend, cases[i].write_mask);
      CHECK((key.color_round_unorm10 & 1) == cases[i].marked, "case %u: format %d blend %d mask 0x%x rbplus %d: 0x%x",
            i, cases[i].format, cases[i].blend, cases[i].write_mask, cases[i].rbplus, key.color_round_unorm10);
      CHECK(!(key.color_round_unorm10 & ~1u), "case %u: the B8G8R8A8 target is marked: 0x%x", i,
            key.color_round_unorm10);
      if (cases[i].marked)
         CHECK((key.spi_shader_col_format & 0xf) == V_028714_SPI_SHADER_FP16_ABGR, "case %u: export 0x%x", i,
               key.spi_shader_col_format & 0xf);

      struct radv_shader_args args = {0};
      struct aco_ps_epilog_info info = {0};
      radv_aco_convert_ps_epilog_key(&info, &key, &args);
      CHECK(info.color_round_unorm10 == key.color_round_unorm10, "case %u: ACO gets 0x%x for 0x%x", i,
            info.color_round_unorm10, key.color_round_unorm10);
   }
}

/* ---- NIR: ac_nir_lower_ps_late, evaluated ---- */

#define NUM_MRTS 2

struct nir_case {
   nir_shader *nir;
   nir_instr **instrs;
   unsigned num_instrs;
   nir_const_value (*values)[NIR_MAX_VEC_COMPONENTS];
   float inputs[NUM_MRTS * 4];
   uint32_t exports[NUM_MRTS][2];
   unsigned exported;
};

static const nir_shader_compiler_options nir_options = {0};

static void
build_lowered_shader(struct nir_case *c, unsigned round_mask)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &nir_options, "unorm10 export");

   for (unsigned mrt = 0; mrt < NUM_MRTS; mrt++) {
      nir_def *chan[4];
      for (unsigned i = 0; i < 4; i++)
         chan[i] = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = (mrt * 4 + i) * 4, .range = 4);

      nir_io_semantics sem = {0};
      sem.location = FRAG_RESULT_DATA0 + mrt;
      sem.num_slots = 1;
      nir_store_output(&b, nir_vec(&b, chan, 4), nir_imm_int(&b, 0), .base = mrt, .write_mask = 0xf,
                       .src_type = nir_type_float32, .io_semantics = sem);
      b.shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0 + mrt);
   }

   ac_nir_lower_ps_late_options options = {
      .gfx_level = GFX10,
      .use_aco = true,
      .spi_shader_col_format = V_028714_SPI_SHADER_FP16_ABGR | (V_028714_SPI_SHADER_FP16_ABGR << 4),
      .color_round_unorm10 = round_mask,
   };
   ac_nir_lower_ps_late(b.shader, &options);
   nir_validate_shader(b.shader, "after ac_nir_lower_ps_late");

   nir_function_impl *impl = nir_shader_get_entrypoint(b.shader);
   nir_index_ssa_defs(impl);

   unsigned n = 0;
   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block)
         n++;
   }
   c->nir = b.shader;
   c->instrs = calloc(n, sizeof(*c->instrs));
   c->values = calloc(impl->ssa_alloc, sizeof(*c->values));
   c->num_instrs = 0;
   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block)
         c->instrs[c->num_instrs++] = instr;
   }
}

static int
count_alu(const struct nir_case *c, nir_op op)
{
   int n = 0;
   for (unsigned i = 0; i < c->num_instrs; i++)
      n += c->instrs[i]->type == nir_instr_type_alu && nir_instr_as_alu(c->instrs[i])->op == op;
   return n;
}

static bool
evaluate(struct nir_case *c)
{
   c->exported = 0;
   for (unsigned n = 0; n < c->num_instrs; n++) {
      nir_instr *instr = c->instrs[n];
      switch (instr->type) {
      case nir_instr_type_load_const: {
         nir_load_const_instr *lc = nir_instr_as_load_const(instr);
         memcpy(c->values[lc->def.index], lc->value, lc->def.num_components * sizeof(nir_const_value));
         break;
      }
      case nir_instr_type_undef: {
         nir_undef_instr *u = nir_instr_as_undef(instr);
         memset(c->values[u->def.index], 0, sizeof(c->values[0]));
         break;
      }
      case nir_instr_type_alu: {
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         const nir_op_info *info = &nir_op_infos[alu->op];
         nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
         nir_const_value *srcs[NIR_ALU_MAX_INPUTS];
         unsigned bit_size = 0;

         if (!nir_alu_type_get_type_size(info->output_type))
            bit_size = alu->def.bit_size;
         for (unsigned i = 0; i < info->num_inputs; i++) {
            if (bit_size == 0 && !nir_alu_type_get_type_size(info->input_types[i]))
               bit_size = alu->src[i].src.ssa->bit_size;
            for (unsigned j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++)
               src[i][j] = c->values[alu->src[i].src.ssa->index][alu->src[i].swizzle[j]];
            srcs[i] = src[i];
         }
         if (bit_size == 0)
            bit_size = 32;

         nir_const_value dest[NIR_MAX_VEC_COMPONENTS] = {0};
         nir_eval_const_opcode(alu->op, dest, NULL, alu->def.num_components, bit_size, srcs,
                               c->nir->info.float_controls_execution_mode);
         memcpy(c->values[alu->def.index], dest, sizeof(dest));
         break;
      }
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_push_constant) {
            unsigned index = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0])) / 4;
            c->values[intr->def.index][0].u32 = bits32(c->inputs[index]);
         } else if (intr->intrinsic == nir_intrinsic_export_amd) {
            unsigned mrt = nir_intrinsic_target(intr) - V_008DFC_SQ_EXP_MRT;
            if (mrt >= NUM_MRTS || nir_intrinsic_enabled_channels(intr) != 0xf ||
                !(nir_intrinsic_flags(intr) & AC_EXP_FLAG_COMPRESSED)) {
               printf("FAIL: unexpected export: target %u, channels 0x%x, flags 0x%x\n", nir_intrinsic_target(intr),
                      nir_intrinsic_enabled_channels(intr), nir_intrinsic_flags(intr));
               return false;
            }
            nir_const_value *v = c->values[intr->src[0].ssa->index];
            c->exports[mrt][0] = v[0].u32;
            c->exports[mrt][1] = v[1].u32;
            c->exported |= 1u << mrt;
         } else {
            printf("FAIL: the lowered shader has an intrinsic the evaluator does not know: %s\n",
                   nir_intrinsic_infos[intr->intrinsic].name);
            return false;
         }
         break;
      }
      default:
         printf("FAIL: the lowered shader has an instruction the evaluator does not know (type %d)\n", instr->type);
         return false;
      }
   }
   return c->exported == (1u << NUM_MRTS) - 1;
}

static uint16_t
half_of(const uint32_t words[2], unsigned chan)
{
   return (uint16_t)(words[chan / 2] >> (chan % 2 * 16));
}

/* The exports of one input on every channel: a target that must round (or, with rounded false, must
 * not) and a B8G8R8A8 target that never does. host: the export must also equal the five operations
 * evaluated on the host (the NIR path; ACO may fuse the last multiply into the pack).
 */
static void
check_exports(const char *path, const uint32_t target[2], const uint32_t other[2], float x, bool rounded, bool host)
{
   const uint16_t rtz = _mesa_float_to_float16_rtz(x);
   for (unsigned chan = 0; chan < 3; chan++) {
      uint16_t h = half_of(target, chan);
      if (rounded) {
         unsigned stored = cb_unorm10(h);
         CHECK(stored == d3d_unorm10(x), "%s, channel %u: %.9g (0x%08x) stored as %u, D3D %u (export 0x%04x)", path,
               chan, x, bits32(x), stored, d3d_unorm10(x), h);
         if (!isnan(x)) {
            /* The margin that makes the result independent of how the CB breaks ties. */
            double steps = (double)_mesa_half_to_float(h) * 1023.0 - (double)d3d_unorm10(x);
            CHECK(x < 0.0f || x > 1.0f || fabs(steps) <= 0.2501, "%s, channel %u: %.9g exported %.4f steps off", path,
                  chan, x, steps);
         } else {
            CHECK(isnan(_mesa_half_to_float(h)), "%s, channel %u: NaN exported as 0x%04x", path, chan, h);
         }
         if (host)
            CHECK(h == _mesa_float_to_float16_rtz(host_round_unorm10(x)),
                  "%s, channel %u: %.9g: export 0x%04x, the operations on the host 0x%04x", path, chan, x, h,
                  _mesa_float_to_float16_rtz(host_round_unorm10(x)));
      } else {
         CHECK(h == rtz, "%s, channel %u: %.9g exported 0x%04x, pack 0x%04x", path, chan, x, h, rtz);
      }
      CHECK(half_of(other, chan) == rtz, "%s, B8G8R8A8 channel %u: %.9g exported 0x%04x, pack 0x%04x", path, chan, x,
            half_of(other, chan), rtz);
   }
   CHECK(half_of(target, 3) == rtz, "%s, alpha: %.9g exported 0x%04x, pack 0x%04x", path, x, half_of(target, 3), rtz);
   CHECK(half_of(other, 3) == rtz, "%s, B8G8R8A8 alpha: %.9g exported 0x%04x", path, x, half_of(other, 3));
}

/* About 1.3 million inputs: special values, 64 ulps around every UNORM10 grid point and midpoint, and a
 * fixed-seed sweep of [0, 1].
 */
static unsigned
sweep(void (*check)(void *ctx, float x), void *ctx)
{
   unsigned inputs = 0;
   static const float special[] = {0.6f,   0.4f,  0.0f,       -0.0f, 1.0f, 0.5f,  0.25f,  0.75f,   0.1f,
                                   0.2f,   0.3f,  0.7f,       0.8f,  0.9f, 1e-8f, 1e-40f, -1e-40f, -1e-8f,
                                   -0.25f, -1.0f, 1.0000001f, 1.5f,  2.0f, 1e30f, -1e30f};
   for (unsigned i = 0; i < ARRAY_SIZE(special); i++, inputs++)
      check(ctx, special[i]);
   static const uint32_t special_bits[] = {0x7f7fffffu /* FLT_MAX */, 0x7f800000u /* +inf */, 0xff800000u /* -inf */,
                                           0x7fc00000u /* NaN */};
   for (unsigned i = 0; i < ARRAY_SIZE(special_bits); i++, inputs++)
      check(ctx, f32(special_bits[i]));

   for (unsigned k = 0; k <= 1023; k++) {
      const float points[2] = {(float)k / 1023.0f, ((float)k + 0.5f) / 1023.0f};
      for (unsigned p = 0; p < 2; p++) {
         for (int d = -64; d <= 64; d++) {
            int64_t b = (int64_t)bits32(points[p]) + d;
            if (b < 0)
               continue;
            check(ctx, f32((uint32_t)b));
            inputs++;
         }
      }
   }

   /* Uniform in value, and uniform in bit pattern (which favours small values). */
   uint64_t state = 0x9e3779b97f4a7c15ull;
   for (unsigned i = 0; i < (1u << 19); i++, inputs += 2) {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      uint32_t r = (uint32_t)(state >> 33);
      check(ctx, (float)(r >> 8) / (float)(1u << 23));
      check(ctx, f32(r % (bits32(1.0f) + 1)));
   }
   return inputs;
}

static void
check_nir_input(void *ctx, float x)
{
   struct nir_case *c = ctx;
   for (unsigned i = 0; i < NUM_MRTS * 4; i++)
      c->inputs[i] = x;
   if (!evaluate(c)) {
      CHECK(false, "evaluation failed for 0x%08x", bits32(x));
      return;
   }
   check_exports("NIR", c->exports[0], c->exports[1], x, true, true);
}

static unsigned
test_nir(void)
{
   struct nir_case c = {0};
   build_lowered_shader(&c, no_round ? 0x0 : 0x1);

   CHECK(count_alu(&c, nir_op_ffloor) == (no_round ? 0 : 3), "%d ffloor in the lowered shader",
         count_alu(&c, nir_op_ffloor));
   for (unsigned i = 0; i < c.num_instrs; i++) {
      if (c.instrs[i]->type == nir_instr_type_alu && nir_instr_as_alu(c.instrs[i])->op == nir_op_ffloor)
         CHECK(nir_alu_instr_is_exact(nir_instr_as_alu(c.instrs[i])), "the rounding is not marked exact");
   }

   unsigned inputs = sweep(check_nir_input, &c);

   ralloc_free(c.nir);
   free(c.instrs);
   free(c.values);
   return inputs;
}

/* ---- ACO: the PS epilog ---- */

struct part {
   unsigned code_dw;
};

static void
build_part(void **priv, const aco_callback_params *params)
{
   ((struct part *)*priv)->code_dw = params->code_dw;
}

/* Compiles the epilog as radv_create_ps_epilog does, with the backend IR dumped to stderr, and returns the dump. */
static char *
compile_epilog(const struct radv_ps_epilog_key *key, const char *dump_path)
{
   struct radv_shader_args args = {0};
   struct radv_shader_info info = {0};
   info.stage = MESA_SHADER_FRAGMENT;
   info.wave_size = 64;
   info.workgroup_size = 64;
   radv_declare_ps_epilog_args(&compiler, key, &args);

   struct aco_shader_info ac_info;
   struct aco_ps_epilog_info ac_epilog_info = {0};
   radv_aco_convert_shader_info(&ac_info, &info, &args, &compiler);
   radv_aco_convert_ps_epilog_key(&ac_epilog_info, key, &args);

   /* radv_aco_fill_compiler_options (static in radv_shader.c) for an epilog, with the backend IR dumped. */
   struct aco_compiler_options ac_opts = {0};
   ac_opts.dump_ir = true;
   ac_opts.compiler_info = compiler.ac;
   ac_opts.gfx_level = compiler.ac->gfx_level;
   ac_opts.family = compiler.debug.family;
   ac_opts.address32_hi = compiler.hw.address32_hi;

   struct part part = {0};
   void *priv = &part;
   fflush(stderr);
   FILE *dump = fopen(dump_path, "w+b");
   if (!dump)
      return NULL;
   int saved = dup(fileno(stderr));
   dup2(fileno(dump), fileno(stderr));
   aco_compile_ps_epilog(&ac_opts, &ac_info, &ac_epilog_info, &args.ac, build_part, &priv);
   fflush(stderr);
   dup2(saved, fileno(stderr));
#ifdef _WIN32
   _close(saved);
#else
   close(saved);
#endif

   fseek(dump, 0, SEEK_END);
   long size = ftell(dump);
   fseek(dump, 0, SEEK_SET);
   char *text = calloc(1, size + 1);
   if (fread(text, 1, size, dump) != (size_t)size)
      text[0] = 0;
   fclose(dump);
   CHECK(part.code_dw > 0, "the epilog has no code");
   return text;
}

/* An interpreter for the straight-line epilog in ACO's IR dump after register allocation: the opcodes an
 * FP16_ABGR colour export needs, nothing else. An opcode it does not know fails the test, which then
 * needs the opcode added here, not a weaker check.
 */
#define IR_MAX_TEMPS 256
#define IR_MAX_OPS   6

enum ir_operand_kind { IR_TEMP, IR_CONST, IR_UNDEF };

struct ir_operand {
   enum ir_operand_kind kind;
   unsigned temp;
   uint32_t value;
   bool neg, abs, lo, hi;
};

struct ir_instr {
   char op[48];
   unsigned num_defs, defs[16], def_halves[16];
   unsigned num_ops;
   struct ir_operand ops[IR_MAX_OPS];
   int mrt;
};

struct ir_program {
   struct ir_instr *instrs;
   unsigned num_instrs;
   unsigned halves[IR_MAX_TEMPS]; /* size of every temp in 16-bit units */
};

static bool
parse_operand(const char *s, struct ir_operand *o)
{
   memset(o, 0, sizeof(*o));
   while (*s == ' ')
      s++;
   for (;;) {
      if (!strncmp(s, "neg(", 4))
         o->neg = true, s += 4;
      else if (!strncmp(s, "abs(", 4))
         o->abs = true, s += 4;
      else if (!strncmp(s, "lo(", 3))
         o->lo = true, s += 3;
      else if (!strncmp(s, "hi(", 3))
         o->hi = true, s += 3;
      else if (!strncmp(s, "(kill)", 6) || !strncmp(s, "(lateKill)", 10))
         s = strchr(s, ')') + 1;
      else
         break;
   }
   if (strstr(s, "undef")) {
      o->kind = IR_UNDEF;
      return true;
   }
   if (*s == '%') {
      o->kind = IR_TEMP;
      o->temp = strtoul(s + 1, NULL, 10);
      return o->temp < IR_MAX_TEMPS;
   }
   o->kind = IR_CONST;
   if (!strncmp(s, "0x", 2)) {
      o->value = strtoul(s, NULL, 16);
      return true;
   }
   static const struct {
      const char *text;
      float value;
   } inline_floats[] = {{"0.5", 0.5f}, {"-0.5", -0.5f}, {"1.0", 1.0f}, {"-1.0", -1.0f},
                        {"2.0", 2.0f}, {"-2.0", -2.0f}, {"4.0", 4.0f}, {"-4.0", -4.0f}};
   for (unsigned i = 0; i < ARRAY_SIZE(inline_floats); i++) {
      size_t n = strlen(inline_floats[i].text);
      if (!strncmp(s, inline_floats[i].text, n) && (s[n] == 0 || s[n] == ')' || s[n] == ' ')) {
         o->value = bits32(inline_floats[i].value);
         return true;
      }
   }
   char *end;
   long v = strtol(s, &end, 10);
   if (end == s)
      return false;
   o->value = (uint32_t)v;
   return true;
}

static unsigned
regclass_halves(const char *rc)
{
   /* "v1", "v4": dwords; "v2b": bytes. */
   char *end;
   unsigned n = strtoul(rc + 1, &end, 10);
   return *end == 'b' ? n / 2 : n * 2;
}

static bool
parse_ir(const char *text, struct ir_program *p)
{
   memset(p, 0, sizeof(*p));
   unsigned capacity = 64;
   p->instrs = calloc(capacity, sizeof(*p->instrs));
   bool after_ra = false;

   for (const char *line = text; line && *line;) {
      const char *end = strchr(line, '\n');
      size_t len = end ? (size_t)(end - line) : strlen(line);
      char buf[1024];
      len = MIN2(len, sizeof(buf) - 1);
      memcpy(buf, line, len);
      buf[len] = 0;
      line = end ? end + 1 : NULL;

      char *s = buf;
      while (*s == ' ' || *s == '\t' || *s == '\r')
         s++;
      if (!strncmp(s, "After ", 6)) {
         if (after_ra)
            break;
         after_ra = !strncmp(s, "After RA", 8);
         continue;
      }
      if (!after_ra || !*s || !strncmp(s, "ACO", 3) || !strncmp(s, "BB", 2) || !strncmp(s, "/*", 2))
         continue;

      if (p->num_instrs == capacity)
         p->instrs = realloc(p->instrs, (capacity *= 2) * sizeof(*p->instrs));
      struct ir_instr *in = &p->instrs[p->num_instrs++];
      memset(in, 0, sizeof(*in));
      in->mrt = -1;

      char *rest = s;
      char *eq = strstr(s, " = ");
      if (eq) {
         *eq = 0;
         rest = eq + 3;
         for (char *d = strtok(s, ","); d; d = strtok(NULL, ",")) {
            while (*d == ' ')
               d++;
            char *pct = strchr(d, '%');
            if (!pct || in->num_defs == ARRAY_SIZE(in->defs))
               return false;
            in->defs[in->num_defs] = strtoul(pct + 1, NULL, 10);
            in->def_halves[in->num_defs] = regclass_halves(d);
            if (in->defs[in->num_defs] >= IR_MAX_TEMPS)
               return false;
            p->halves[in->defs[in->num_defs]] = in->def_halves[in->num_defs];
            in->num_defs++;
         }
      }

      char *sp = strchr(rest, ' ');
      size_t oplen = sp ? (size_t)(sp - rest) : strlen(rest);
      if (oplen >= sizeof(in->op))
         return false;
      memcpy(in->op, rest, oplen);
      in->op[oplen] = 0;
      if (!sp)
         continue;

      char *operands = sp + 1;
      if (!strcmp(in->op, "exp")) {
         char *mrt = strstr(operands, " mrt");
         if (!mrt || !strstr(operands, " compr"))
            return false;
         in->mrt = atoi(mrt + 4);
         *strstr(operands, " compr") = 0;
      }
      for (char *o = strtok(operands, ","); o; o = strtok(NULL, ",")) {
         if (in->num_ops == IR_MAX_OPS || !parse_operand(o, &in->ops[in->num_ops++]))
            return false;
      }
   }
   return after_ra && p->num_instrs > 0;
}

struct ir_state {
   uint32_t v[IR_MAX_TEMPS][8];
   uint32_t exports[MAX_RTS][2];
   unsigned exported;
   /* v_fma_mix with an fp16 result: one rounding (false) or an fp32 result rounded again (true). The
    * export must pass under both.
    */
   bool mix_rounds_twice;
};

static float
operand_f32(const struct ir_state *st, const struct ir_operand *o)
{
   float f;
   if (o->kind == IR_TEMP) {
      uint32_t bits = st->v[o->temp][0];
      f = (o->lo || o->hi) ? _mesa_half_to_float((uint16_t)(o->hi ? bits >> 16 : bits)) : f32(bits);
   } else if (o->kind == IR_CONST) {
      f = (o->lo || o->hi) ? _mesa_half_to_float((uint16_t)(o->hi ? o->value >> 16 : o->value)) : f32(o->value);
   } else {
      f = 0.0f;
   }
   if (o->abs)
      f = fabsf(f);
   if (o->neg)
      f = -f;
   return f;
}

/* v_mad_f32 is the unfused multiply-add, and it flushes fp32 denormals. */
static float
flush(float f)
{
   return fabsf(f) < FLT_MIN ? copysignf(0.0f, f) : f;
}

static bool
interpret(const struct ir_program *p, struct ir_state *st, const float *colors)
{
   st->exported = 0;
   for (unsigned n = 0; n < p->num_instrs; n++) {
      const struct ir_instr *in = &p->instrs[n];
      const char *op = in->op;
      uint32_t *d = in->num_defs ? st->v[in->defs[0]] : NULL;
#define F(i) operand_f32(st, &in->ops[i])

      if (!strcmp(op, "p_startpgm")) {
         /* The colour arguments in MRT order, each v4 (radv_declare_ps_epilog_args). */
         for (unsigned i = 0; i < in->num_defs; i++) {
            if (in->def_halves[i] != 8 || i >= MAX_RTS)
               return false;
            for (unsigned c = 0; c < 4; c++)
               st->v[in->defs[i]][c] = bits32(colors[c]);
         }
      } else if (!strcmp(op, "p_logical_start") || !strcmp(op, "p_logical_end") || !strcmp(op, "s_endpgm")) {
      } else if (!strcmp(op, "p_split_vector")) {
         const uint32_t *src = st->v[in->ops[0].temp];
         unsigned half = 0;
         for (unsigned i = 0; i < in->num_defs; i++) {
            for (unsigned h = 0; h < in->def_halves[i]; h++, half++) {
               uint16_t x = (uint16_t)(src[half / 2] >> (half % 2 * 16));
               uint32_t *dst = &st->v[in->defs[i]][h / 2];
               *dst = (*dst & ~(0xffffu << (h % 2 * 16))) | ((uint32_t)x << (h % 2 * 16));
            }
         }
      } else if (!strcmp(op, "p_create_vector") || !strcmp(op, "p_parallelcopy") || !strcmp(op, "v_mov_b32")) {
         if (strcmp(op, "p_create_vector") && in->num_defs != in->num_ops)
            return false;
         unsigned half = 0, def = 0;
         uint32_t tmp[8] = {0};
         for (unsigned i = 0; i < in->num_ops; i++) {
            const struct ir_operand *o = &in->ops[i];
            unsigned halves = o->kind == IR_TEMP ? p->halves[o->temp] : 2;
            for (unsigned h = 0; h < halves; h++, half++) {
               uint32_t word = o->kind == IR_TEMP ? st->v[o->temp][h / 2] : o->value;
               tmp[half / 2] |= ((word >> (h % 2 * 16)) & 0xffffu) << (half % 2 * 16);
            }
            if (strcmp(op, "p_create_vector")) {
               memcpy(st->v[in->defs[def++]], tmp, sizeof(tmp));
               memset(tmp, 0, sizeof(tmp));
               half = 0;
            }
         }
         if (!strcmp(op, "p_create_vector"))
            memcpy(d, tmp, sizeof(tmp));
      } else if (!strcmp(op, "v_mad_f32")) {
         volatile float t = flush(F(0)) * flush(F(1));
         d[0] = bits32(flush(flush(t) + flush(F(2))));
      } else if (!strcmp(op, "v_fma_f32")) {
         d[0] = bits32(fmaf(F(0), F(1), F(2)));
      } else if (!strcmp(op, "v_mul_f32")) {
         volatile float t = F(0) * F(1);
         d[0] = bits32(t);
      } else if (!strcmp(op, "v_add_f32")) {
         volatile float t = F(0) + F(1);
         d[0] = bits32(t);
      } else if (!strcmp(op, "v_floor_f32")) {
         d[0] = bits32(floorf(F(0)));
      } else if (!strcmp(op, "v_cvt_pkrtz_f16_f32")) {
         d[0] = _mesa_float_to_float16_rtz(F(0)) | (uint32_t)_mesa_float_to_float16_rtz(F(1)) << 16;
      } else if (!strcmp(op, "p_v_fma_mixlo_f16_rtz") || !strcmp(op, "p_v_fma_mixhi_f16_rtz")) {
         /* The double product of two floats is exact; the addend here is a zero. */
         double r = (double)F(0) * (double)F(1) + (double)F(2);
         d[0] = st->mix_rounds_twice ? _mesa_float_to_float16_rtz((float)r) : _mesa_double_to_float16_rtz(r);
      } else if (!strcmp(op, "exp")) {
         if (in->mrt < 0 || in->mrt >= MAX_RTS || in->num_ops < 2)
            return false;
         for (unsigned i = 0; i < 2; i++)
            st->exports[in->mrt][i] = in->ops[i].kind == IR_TEMP ? st->v[in->ops[i].temp][0] : 0;
         st->exported |= 1u << in->mrt;
      } else {
         printf("FAIL: the epilog has an opcode the interpreter does not know: %s\n", op);
         return false;
      }
#undef F
   }
   return true;
}

struct aco_case {
   struct ir_program marked, blended;
   struct ir_state state;
};

static void
check_aco_input(void *ctx, float x)
{
   struct aco_case *c = ctx;
   const float colors[4] = {x, x, x, x};

   for (unsigned twice = 0; twice < 2; twice++) {
      c->state.mix_rounds_twice = twice;
      if (!interpret(&c->marked, &c->state, colors) || c->state.exported != 0x3) {
         CHECK(false, "the marked epilog does not run for 0x%08x", bits32(x));
         return;
      }
      check_exports(twice ? "ACO epilog, mix rounded twice" : "ACO epilog", c->state.exports[0], c->state.exports[1], x,
                    true, false);
   }

   c->state.mix_rounds_twice = false;
   if (!interpret(&c->blended, &c->state, colors) || c->state.exported != 0x3) {
      CHECK(false, "the blended epilog does not run for 0x%08x", bits32(x));
      return;
   }
   check_exports("ACO epilog, blended", c->state.exports[0], c->state.exports[1], x, false, false);
}

static unsigned
count_lines(const char *text, const char *a)
{
   unsigned n = 0;
   for (const char *s = text; (s = strstr(s, a)); s++)
      n++;
   return n;
}

static unsigned
test_aco(const char *dump_dir)
{
   init_compiler(false);
   char path[1024];

   /* MRT0 a marked R10G10B10A2 target, MRT1 B8G8R8A8. */
   struct radv_ps_epilog_key key = epilog_key(VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 0xf);
   CHECK(key.color_round_unorm10 == 0x1, "key 0x%x", key.color_round_unorm10);
   if (no_round)
      key.color_round_unorm10 = 0;
   snprintf(path, sizeof(path), "%s/epilog-marked.txt", dump_dir);
   char *marked = compile_epilog(&key, path);

   /* The same target with blending: nothing rounded. */
   struct radv_ps_epilog_key blended = epilog_key(VK_FORMAT_A2B10G10R10_UNORM_PACK32, true, 0xf);
   snprintf(path, sizeof(path), "%s/epilog-blended.txt", dump_dir);
   char *plain = compile_epilog(&blended, path);

   struct aco_case *c = calloc(1, sizeof(*c));
   unsigned inputs = 0;
   if (!marked || !plain || !parse_ir(marked, &c->marked) || !parse_ir(plain, &c->blended)) {
      CHECK(false, "no IR dump, or one the parser does not read (see %s)", dump_dir);
   } else {
      CHECK(count_lines(marked, "v_floor_f32") == 3 && count_lines(plain, "v_floor_f32") == 0,
            "%u and %u floors in the marked and the blended epilog", count_lines(marked, "v_floor_f32"),
            count_lines(plain, "v_floor_f32"));
      inputs = sweep(check_aco_input, c);
   }

   free(c->marked.instrs);
   free(c->blended.instrs);
   free(c);
   free(marked);
   free(plain);
   return inputs;
}

int
main(int argc, char **argv)
{
   const char *dump_dir = ".";
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--no-round"))
         no_round = true;
      else
         dump_dir = argv[i];
   }
   glsl_type_singleton_init_or_ref();

   test_key();
   unsigned key_failures = failures;
   unsigned nir_inputs = test_nir();
   unsigned nir_failures = failures - key_failures;
   unsigned aco_inputs = test_aco(dump_dir);
   unsigned aco_failures = failures - key_failures - nir_failures;

   glsl_type_singleton_decref();
   printf("unorm10 export%s: %u NIR inputs, %u ACO epilog inputs, %u checks, %u failures (key %u, NIR %u, ACO %u)\n",
          no_round ? " (no rounding)" : "", nir_inputs, aco_inputs, checks, failures, key_failures, nir_failures,
          aco_failures);
   return failures ? 1 : 0;
}
