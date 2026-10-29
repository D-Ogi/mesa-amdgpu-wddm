/*
 * Copyright 2026 The BC-250 Windows driver project
 *
 * SPDX-License-Identifier: MIT
 *
 * Offline instruction-count harness for the packed 4x8 integer dot products on
 * parts without hardware dot instructions (gfx1013 is the case this exists for:
 * all nine GFX10.1 v_dot* instructions give wrong results there, so
 * has_[s|u|su]dot_4x8 are false and NIR lowers every packed dot in software).
 *
 * It needs no GPU and no driver: it builds NIR by hand, runs a NIR pipeline,
 * then runs the whole ACO pipeline and classifies the final machine
 * instructions. For every case it prints one CSV row, so the same binary
 * measures a tree before and after a lowering change.
 *
 * Two suites:
 *
 * - basic (the default): each case is compiled twice, with 1 and with 9
 *   independent dot products whose operands and results do not alias, so the
 *   marginal cost of one dot product is (count at 9 - count at 1) / 8, free of
 *   the shader's fixed prologue and epilogue.
 * - dense: shaders shaped like an INT8 network layer, with many dot products
 *   into a few accumulator chains and high register pressure. Each row reports
 *   the whole shader: instructions, VGPRs, spills, scratch and occupancy.
 *
 * Two NIR pipelines:
 *
 * - simple (the default for basic): the NIR optimiser to a fixed point, then
 *   the late algebraic loop. This is the pipeline of the first measurements.
 * - radv (--pipeline radv, the default for dense): a copy of the pass order of
 *   radv_optimize_nir(), radv_postprocess_nir() and
 *   radv_optimize_nir_algebraic() for a compute shader, without the passes that
 *   need RADV state (descriptors, ABI). --defer-sdot runs the first
 *   radv_optimize_nir() round with has_sdot_4x8 set in a copy of the options,
 *   as radv_shader_spirv_to_nir() does in a tree that defers the software
 *   lowering. --dense-prepass mad3|twochain runs the dense signed-dot prepass of
 *   bc250-fsr4 v3 on every run-time x run-time sdot_4x8_iadd first, without
 *   its shader-specific gate. Both need a tree whose ACO selects imad24_ir3.
 *
 * Build: meson -Dbuild-aco-dot4-count=true, target src/amd/compiler/aco_dot4_count
 */

#include "aco_ir.h"

#include "common/ac_gpu_info.h"
#include "common/amd_family.h"
#include "common/nir/ac_nir.h"

#include "nir.h"
#include "nir_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include <vector>

using namespace aco;

/* ---------------------------------------------------------------- settings */

static bool pipeline_radv = false;
static bool pipeline_set = false;
static bool defer_sdot = false;
enum class Prepass { none, mad3, twochain };
static Prepass dense_prepass = Prepass::none;

/* ---------------------------------------------------------------- NIR helpers */

/* nir_builder_opcodes.h builds intrinsics with designated initialisers, which
 * c++17 does not have, so the intrinsics used here are built by hand.
 */
/* The lane index, as radv gets it: nir_lower_subgroups turns
 * load_subgroup_invocation into mbcnt_amd over the full mask. It is the only
 * divergent source this harness needs; everything the shader loads per lane is
 * indexed by it, so no such operand of a dot product can become uniform and go
 * to the scalar unit, which would measure a different thing entirely.
 */
static nir_def*
lane_index(nir_builder* b)
{
   nir_intrinsic_instr* intr = nir_intrinsic_instr_create(b->shader, nir_intrinsic_mbcnt_amd);
   intr->src[0] = nir_src_for_ssa(nir_imm_int64(b, ~0ull));
   intr->src[1] = nir_src_for_ssa(nir_imm_int(b, 0));
   nir_def_init(&intr->instr, &intr->def, 1, 32);
   nir_builder_instr_insert(b, &intr->instr);
   return &intr->def;
}

static nir_def*
load_global_dwords(nir_builder* b, uint64_t addr, unsigned comps, nir_def* offset,
                   gl_access_qualifier access = (gl_access_qualifier)0)
{
   nir_intrinsic_instr* intr = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_global_amd);
   intr->num_components = comps;
   intr->src[0] = nir_src_for_ssa(nir_imm_int64(b, addr));
   intr->src[1] = nir_src_for_ssa(offset);
   nir_def_init(&intr->instr, &intr->def, comps, 32);
   nir_intrinsic_set_base(intr, 0);
   nir_intrinsic_set_align(intr, 16, 0);
   nir_intrinsic_set_access(intr, access);
   nir_builder_instr_insert(b, &intr->instr);
   return &intr->def;
}

static void
store_global_dword(nir_builder* b, uint64_t addr, nir_def* value, nir_def* offset)
{
   nir_intrinsic_instr* intr = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_global_amd);
   intr->num_components = 1;
   intr->src[0] = nir_src_for_ssa(value);
   intr->src[1] = nir_src_for_ssa(nir_imm_int64(b, addr));
   intr->src[2] = nir_src_for_ssa(offset);
   nir_intrinsic_set_base(intr, 0);
   nir_intrinsic_set_align(intr, 4, 0);
   nir_intrinsic_set_access(intr, (gl_access_qualifier)0);
   nir_intrinsic_set_write_mask(intr, 0x1);
   nir_builder_instr_insert(b, &intr->instr);
}

/* ---------------------------------------------------------------- case matrix */

enum class DotKind {
   sdot, /* signed x signed   */
   udot, /* unsigned x unsigned */
   sudot /* signed x unsigned */
};

enum class Operands {
   data_data,    /* a, b, c all run-time values                     */
   const_weight, /* b is a compile-time constant: the NN weight case */
   const_acc,    /* c is a compile-time constant                    */
   both_const    /* a and b are both compile-time constants         */
};

struct Case {
   const char* name;
   DotKind kind;
   bool sat;
   Operands operands;
};

static const char*
kind_name(DotKind k)
{
   switch (k) {
   case DotKind::sdot: return "sdot_4x8_iadd";
   case DotKind::udot: return "udot_4x8_uadd";
   default: return "sudot_4x8_iadd";
   }
}

static nir_def*
build_dot(nir_builder* b, DotKind kind, bool sat, nir_def* x, nir_def* y, nir_def* acc)
{
   switch (kind) {
   case DotKind::sdot:
      return sat ? nir_sdot_4x8_iadd_sat(b, x, y, acc) : nir_sdot_4x8_iadd(b, x, y, acc);
   case DotKind::udot:
      return sat ? nir_udot_4x8_uadd_sat(b, x, y, acc) : nir_udot_4x8_uadd(b, x, y, acc);
   default:
      return sat ? nir_sudot_4x8_iadd_sat(b, x, y, acc) : nir_sudot_4x8_iadd(b, x, y, acc);
   }
}

/* Weight and accumulator constants with every interesting byte in them: the
 * extremes of both signed and unsigned ranges and zero.
 */
/* No byte is 0x00, 0x01 or 0xff: those three fold a whole lane away (a zero
 * product, or a multiply that becomes an add or a subtract), which would
 * measure a sparse weight vector instead of a general one. The extremes 0x7f
 * and 0x80 are in, because they are what the exactness proof turns on.
 */
static const uint32_t weights_main[9] = {
   0x7f800502, 0x234efbf0, 0x05060708, 0x80027f05, 0x40302010,
   0xfefdfcfb, 0x0a141e28, 0x7f7f7f7f, 0x80808080,
};

/* The alternative table additionally has no byte that is a power of two. A
 * product by a power of two becomes a shift, which ACO can fold into a
 * two-input add (v_lshl_add_u32) but not into a three-input one, so the two
 * tables separate the dot product's own cost from that interaction.
 */
static const uint32_t weights_nopow2[9] = {
   0x7f0b0503, 0x234efbf9, 0x05060709, 0x7d037f05, 0x6d553323,
   0xfefdfcfb, 0x0b151f29, 0x7f7f7f7f, 0x7d7d7d7d,
};

static const uint32_t* weights = weights_main;
static const uint32_t accums[9] = {
   0x5a5a5a5au, 3u,       0xfffffffeu,  0x7ffffff0u, 0x80000010u,
   1234u,       0xffffu,  0x40000000u,  7u,
};

/* ---------------------------------------------------------------- measurement */

struct Counts {
   unsigned all = 0;
   unsigned valu = 0;
   unsigned vop1 = 0;
   unsigned vop2 = 0;
   unsigned vop3 = 0;
   unsigned vop3p = 0;
   unsigned sdwa = 0;
   unsigned dpp = 0;
   unsigned salu = 0;
   unsigned smem = 0;
   unsigned vmem = 0;
   unsigned flat = 0;
   unsigned ds = 0;
   unsigned other = 0;
   unsigned code_bytes = 0;
};

struct Pressure {
   unsigned presched_vgpr = 0;
   unsigned presched_sgpr = 0;
   unsigned vgprs = 0;
   unsigned sgprs = 0;
   unsigned spilled_vgprs = 0;
   unsigned spilled_sgprs = 0;
   unsigned scratch_bytes_per_wave = 0;
   unsigned lds_bytes = 0;
   unsigned waves_per_simd = 0;
};

/* Replicates aco_interface.cpp's aco_postprocess_shader(), which is file-local
 * there. Kept in the same order; no option is changed. It also records the
 * register demand before scheduling and spilling, as RADV's "Pre-Sched VGPRs"
 * statistic does.
 */
static void
postprocess(Program* program, Pressure* p)
{
   dominator_tree(program);
   if (program->should_repair_ssa)
      repair_ssa(program);
   lower_phis(program);

   value_numbering(program);
   optimize(program);
   if (program->should_repair_ssa && repair_ssa(program))
      lower_phis(program);

   setup_reduce_temp(program);
   insert_exec_mask(program);

   live_var_analysis(program);
   p->presched_vgpr = program->max_reg_demand.vgpr;
   p->presched_sgpr = program->max_reg_demand.sgpr;
   spill(program);

   schedule_program(program);

   register_allocation(program);

   optimize_postRA(program);

   spill_preserved(program);

   ssa_elimination(program);
   lower_to_hw_instr(program);
   lower_branches(program);

   schedule_vopd(program);
   schedule_ilp(program);

   disable_wqm(program);

   if (program->needs_fp_mode_insertion)
      insert_fp_mode(program);

   insert_waitcnt(program);
   insert_NOPs(program);
   if (program->gfx_level >= GFX10)
      form_hard_clauses(program);
}

static Counts
classify(Program* program)
{
   Counts c;
   for (Block& block : program->blocks) {
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (instr->isPseudo())
            continue;
         /* The second half of the contract, checked on the emitted code itself and
          * not on a capability bit: no v_dot* instruction may appear, because every
          * one of them is wrong on this silicon (M865).
          */
         const char* opname = instr_info.name[(int)instr->opcode];
         if (opname && strstr(opname, "_dot")) {
            fprintf(stderr, "FATAL: contract violated: %s emitted for CHIP_GFX1013\n", opname);
            exit(9);
         }
         c.all++;
         if (instr->isVALU()) {
            c.valu++;
            if (instr->isSDWA())
               c.sdwa++;
            else if (instr->isDPP())
               c.dpp++;
            else if (instr->isVOP3P())
               c.vop3p++;
            else if (instr->isVOP3())
               c.vop3++;
            else if (instr->isVOP2())
               c.vop2++;
            else if (instr->isVOP1())
               c.vop1++;
         } else if (instr->isSALU()) {
            c.salu++;
         } else if (instr->isSMEM()) {
            c.smem++;
         } else if (instr->isVMEM()) {
            c.vmem++;
         } else if (instr->isFlatLike()) {
            c.flat++;
         } else if (instr->isDS()) {
            c.ds++;
         } else {
            c.other++;
         }
      }
   }
   return c;
}

/* The ALU op count of the lowered NIR, for the row that compares with the
 * rule shapes in nir_opt_algebraic.py.
 */
static unsigned
count_nir_alu(nir_shader* shader)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, shader)
   {
      nir_foreach_block(block, impl)
      {
         nir_foreach_instr(instr, block)
         {
            if (instr->type == nir_instr_type_alu)
               n++;
         }
      }
   }
   return n;
}

static unsigned
count_nir_op(nir_shader* shader, nir_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, shader)
   {
      nir_foreach_block(block, impl)
      {
         nir_foreach_instr(instr, block)
         {
            if (instr->type == nir_instr_type_alu && nir_instr_as_alu(instr)->op == op)
               n++;
         }
      }
   }
   return n;
}

struct Result {
   Counts counts;
   Pressure pressure;
   unsigned nir_alu;
};

/* The contract this harness exists to keep: all nine GFX10.1 v_dot* instructions
 * give wrong results on gfx1013 (facts M865/M866), so every packed-dot capability
 * bit has to be false and NIR has to lower every packed dot in software.
 *
 * This is a hard check and not an assert on purpose: the harness is built with
 * the release option set, which defines NDEBUG, so an assert() here would be
 * compiled out and the contract would be checked on no run at all.
 */
static void
require_no_hw_dot(const radeon_info& rad_info, const nir_shader_compiler_options& o)
{
   struct {
      const char* name;
      bool value;
   } bits[] = {
      {"has_accelerated_dot_product", rad_info.compiler_info.has_accelerated_dot_product},
      {"has_sdot_4x8", o.has_sdot_4x8},
      {"has_udot_4x8", o.has_udot_4x8},
      {"has_sudot_4x8", o.has_sudot_4x8},
      {"has_sdot_4x8_sat", o.has_sdot_4x8_sat},
      {"has_udot_4x8_sat", o.has_udot_4x8_sat},
      {"has_sudot_4x8_sat", o.has_sudot_4x8_sat},
   };

   bool bad = false;
   for (const auto& bit : bits) {
      if (bit.value) {
         fprintf(stderr, "FATAL: contract violated: %s is true for CHIP_GFX1013\n", bit.name);
         bad = true;
      }
   }
   if (bad)
      exit(9);

   static bool reported = false;
   if (!reported) {
      reported = true;
      fprintf(stderr, "contract (hard-checked, not an assert): ");
      for (const auto& bit : bits)
         fprintf(stderr, "%s=0 ", bit.name);
      fprintf(stderr, "for gfx_level GFX10, family CHIP_GFX1013\n");
      fflush(stderr);
   }
}

/* ------------------------------------------------------------ NIR pipelines */

static void
simple_pipeline(nir_shader* s)
{
   /* The NIR optimiser to a fixed point: the packed dot products are lowered in
    * nir_opt_algebraic and the result has to be folded and cleaned up the way
    * RADV does it before instruction selection.
    */
   bool progress;
   do {
      progress = false;
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_cse(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_generate_bfi(s);
      progress |= nir_opt_algebraic(s);
   } while (progress);

   /* The late half, as radv_optimize_nir_algebraic_late() runs it: it is where
    * options->lower_ineg and the other late lowerings happen.
    */
   do {
      progress = false;
      progress |= nir_opt_algebraic_late(s);
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_cse(s);
   } while (progress);
}

static uint8_t
vectorize_vec2_16bit(const nir_instr* instr, const void*)
{
   if (instr->type != nir_instr_type_alu)
      return 0;
   const unsigned bit_size = nir_instr_as_alu(instr)->def.bit_size;
   return bit_size == 16 ? 2 : bit_size == 8 ? 4 : 1;
}

/* The pass order of radv_optimize_nir() at the measured tree, without the
 * variable passes (the harness builds SSA) and without gather_info.
 */
static void
radv_optimize_nir_copy(nir_shader* s)
{
   nir_lower_alu_width(s, vectorize_vec2_16bit, NULL);
   nir_opt_fp_math_ctrl(s);

   bool progress;
   do {
      progress = false;
      nir_lower_phis_to_scalar(s, ac_nir_lower_phis_to_scalar_cb, NULL);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_remove_phis(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_dead_cf(s);
      if (nir_opt_loop(s)) {
         progress = true;
         nir_opt_copy_prop(s);
         nir_opt_remove_phis(s);
         nir_opt_dce(s);
      }
      progress |= nir_opt_if(s, nir_opt_if_optimize_phi_true_false);
      progress |= nir_opt_cse(s);
      nir_opt_uub_options uub = {};
      progress |= nir_opt_uub(s, &uub);
      nir_opt_peephole_select_options ps = {};
      ps.limit = 8;
      ps.indirect_load_ok = true;
      ps.expensive_alu_ok = true;
      ps.discard_ok = true;
      progress |= nir_opt_peephole_select(s, &ps);
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_intrinsics(s);
      progress |= nir_opt_algebraic(s);
      progress |= nir_opt_phi_to_bool(s);
      progress |= nir_opt_phi_precision(s);
      progress |= nir_opt_undef(s);
      if (s->options->max_unroll_iterations && nir_opt_loop_unroll(s)) {
         progress = true;
         nir_opt_remove_phis(s);
      }
   } while (progress);

   nir_opt_shrink_vectors(s, true);
}

static void
radv_algebraic_early_copy(nir_shader* s)
{
   bool progress = true;
   bool had_fp_math_ctrl = false;
   while (progress) {
      progress = false;
      nir_opt_copy_prop(s);
      nir_opt_dce(s);
      nir_opt_constant_folding(s);
      nir_opt_cse(s);
      nir_opt_peephole_select_options ps = {};
      ps.limit = 8;
      ps.indirect_load_ok = true;
      ps.expensive_alu_ok = true;
      ps.discard_ok = true;
      progress |= nir_opt_peephole_select(s, &ps);
      nir_opt_undef(s);
      nir_opt_phi_to_bool(s);
      nir_opt_generate_bfi(s);
      progress |= nir_opt_remove_phis(s);
      progress |= nir_opt_dead_cf(s);
      progress |= nir_opt_algebraic(s);
      if (!had_fp_math_ctrl) {
         progress |= nir_opt_fp_math_ctrl(s);
         had_fp_math_ctrl = true;
      }
   }
}

static void
radv_algebraic_late_copy(nir_shader* s)
{
   bool more = true;
   while (more) {
      more = false;
      more |= nir_opt_algebraic_late(s);
      nir_opt_constant_folding(s);
      nir_opt_copy_prop(s);
      nir_opt_dce(s);
      nir_opt_cse(s);
   }
   nir_opt_uadd_carry(s, 32 | 64);
}

/* The dense signed-dot prepass of bc250-fsr4 v3 (radv_shader.c,
 * bc250_lower_dense_sdot4x8_one), applied to every run-time x run-time
 * sdot_4x8_iadd and without the gate on the number of dot products in the
 * shader, so that its two lowering shapes can be measured on any case.
 */
static bool
prepass_one(nir_builder* b, nir_alu_instr* alu, void* data)
{
   const bool two_chain = *(const bool*)data;
   if (alu->op != nir_op_sdot_4x8_iadd)
      return false;
   if (nir_src_is_const(alu->src[0].src) || nir_src_is_const(alu->src[1].src))
      return false;

   b->cursor = nir_before_instr(&alu->instr);
   nir_def* a = nir_ssa_for_alu_src(b, alu, 0);
   nir_def* bv = nir_ssa_for_alu_src(b, alu, 1);
   nir_def* c = nir_ssa_for_alu_src(b, alu, 2);
   nir_def* r;
   if (two_chain) {
      nir_def* r01 = nir_build_alu2(b, nir_op_imul24_relaxed, nir_extract_i8_imm(b, a, 0),
                                    nir_extract_i8_imm(b, bv, 0));
      r01 = nir_imad24_ir3(b, nir_extract_i8_imm(b, a, 1), nir_extract_i8_imm(b, bv, 1), r01);
      nir_def* r23 = nir_build_alu2(b, nir_op_imul24_relaxed, nir_extract_i8_imm(b, a, 2),
                                    nir_extract_i8_imm(b, bv, 2));
      r23 = nir_imad24_ir3(b, nir_extract_i8_imm(b, a, 3), nir_extract_i8_imm(b, bv, 3), r23);
      r = nir_iadd(b, r01, r23);
   } else {
      r = nir_build_alu2(b, nir_op_imul24_relaxed, nir_extract_i8_imm(b, a, 0),
                         nir_extract_i8_imm(b, bv, 0));
      for (unsigned i = 1; i < 4; i++)
         r = nir_imad24_ir3(b, nir_extract_i8_imm(b, a, i), nir_extract_i8_imm(b, bv, i), r);
   }
   r = nir_iadd(b, r, c);
   nir_def_replace(&alu->def, r);
   return true;
}

static void
radv_pipeline(nir_shader* s, amd_gfx_level gfx)
{
   if (dense_prepass != Prepass::none) {
      bool two_chain = dense_prepass == Prepass::twochain;
      nir_shader_alu_pass(s, prepass_one, nir_metadata_control_flow, &two_chain);
   }

   /* radv_shader_spirv_to_nir(): the first radv_optimize_nir(), optionally
    * after a round in which the signed packed dot products stay unlowered.
    */
   if (defer_sdot) {
      const nir_shader_compiler_options* saved = s->options;
      nir_shader_compiler_options deferred = *saved;
      deferred.has_sdot_4x8 = true;
      s->options = &deferred;
      radv_optimize_nir_copy(s);
      s->options = saved;
   }
   radv_optimize_nir_copy(s);

   /* radv_postprocess_nir() for a compute shader. */
   ac_nir_flag_smem_for_loads(s, gfx, false);
   nir_opt_constant_folding(s);
   nir_lower_alu_width(s, ac_nir_opt_vectorize_cb, &gfx);

   nir_move_options sink_opts = (nir_move_options)(nir_move_const_undef | nir_move_copies |
                                                   nir_dont_move_byte_word_vecs |
                                                   nir_move_load_input | nir_move_load_frag_coord);
   nir_opt_licm(s, NULL);
   nir_opt_sink(s, sink_opts);
   nir_opt_move(s, sink_opts);

   nir_opt_dce(s);
   nir_opt_copy_prop(s);
   nir_opt_constant_folding(s);
   nir_opt_cse(s);
   nir_opt_if(s, nir_opt_if_optimize_phi_true_false);
   nir_opt_shrink_vectors(s, true);
   ac_nir_flag_smem_for_loads(s, gfx, false);
   nir_opt_intrinsics(s);
   nir_opt_uub_options uub = {};
   nir_opt_uub(s, &uub);

   radv_algebraic_early_copy(s);
   radv_algebraic_late_copy(s);
   nir_opt_dce(s);

   sink_opts = (nir_move_options)(sink_opts | nir_move_comparisons | nir_move_load_ubo |
                                  nir_move_load_ssbo | nir_move_alu);
   nir_opt_sink(s, sink_opts);
   nir_move_options move_opts =
      (nir_move_options)(nir_move_const_undef | nir_move_load_ubo | nir_move_load_input |
                         nir_move_load_frag_coord | nir_move_comparisons | nir_move_copies |
                         nir_dont_move_byte_word_vecs | nir_move_alu);
   nir_opt_move(s, move_opts);
   nir_opt_move(s, nir_move_comparisons);
}

/* ----------------------------------------------------------- shader compile */

struct Target {
   radeon_info rad_info;
   nir_shader_compiler_options nir_options;
};

static void
init_target(Target* t)
{
   memset(t, 0, sizeof(*t));
   t->rad_info.gfx_level = GFX10;
   t->rad_info.family = CHIP_GFX1013;
   ac_fill_compiler_info(&t->rad_info, NULL, false);
   ac_nir_set_options(&t->rad_info.compiler_info, false, &t->nir_options);
   /* What get_nir_options_for_stage() in radv_shader.c adds for a compute shader. */
   t->nir_options.max_unroll_iterations = 32;
   t->nir_options.max_unroll_iterations_aggressive = 128;
   require_no_hw_dot(t->rad_info, t->nir_options);
}

static Result
compile(Target& t, nir_shader* shader, bool use_radv, bool print_ir)
{
   if (use_radv)
      radv_pipeline(shader, t.rad_info.gfx_level);
   else
      simple_pipeline(shader);

   nir_validate_shader(shader, "aco_dot4_count");
   nir_lower_continue_constructs(shader);

   /* Every packed dot product must be lowered in NIR before ACO: ACO would
    * select a v_dot* instruction for any that is left.
    */
   const nir_op dots[] = {nir_op_sdot_4x8_iadd, nir_op_udot_4x8_uadd, nir_op_sudot_4x8_iadd,
                          nir_op_sdot_4x8_iadd_sat, nir_op_udot_4x8_uadd_sat,
                          nir_op_sudot_4x8_iadd_sat};
   for (nir_op op : dots) {
      if (count_nir_op(shader, op)) {
         fprintf(stderr, "FATAL: %s is left in the NIR given to ACO\n", nir_op_infos[op].name);
         exit(9);
      }
   }

   Result res = {};
   res.nir_alu = count_nir_alu(shader);

   if (print_ir)
      nir_print_shader(shader, stderr);

   ac_shader_config config = {};
   aco_shader_info info = {};
   info.hw_stage = AC_HW_COMPUTE_SHADER;
   info.wave_size = 64;
   info.workgroup_size = 64;

   aco_compiler_options options = {};
   options.family = t.rad_info.family;
   options.gfx_level = t.rad_info.gfx_level;
   options.compiler_info = &t.rad_info.compiler_info;

   ac_shader_args args = {};
   nir_shader* shaders[1] = {shader};

   Program program;
   program.collect_statistics = false;
   program.debug.func = NULL;
   program.debug.private_data = NULL;

   select_program(&program, 1, shaders, &config, &options, &info, &args);
   postprocess(&program, &res.pressure);

   if (!validate_ir(&program)) {
      fprintf(stderr, "FATAL: ACO IR validation failed\n");
      exit(2);
   }

   if (print_ir)
      aco_print_program(&program, stderr);

   std::vector<uint32_t> code;
   std::vector<aco_symbol> symbols;
   aco_callback_params params = {};
   emit_program(&program, code, symbols, true, &params);

   res.counts = classify(&program);
   res.counts.code_bytes = (unsigned)code.size() * 4u;

   Pressure& p = res.pressure;
   p.vgprs = config.num_vgprs;
   p.sgprs = config.num_sgprs;
   p.spilled_vgprs = config.spilled_vgprs;
   p.spilled_sgprs = config.spilled_sgprs;
   p.scratch_bytes_per_wave = config.scratch_bytes_per_wave;
   p.lds_bytes = config.lds_size;
   const unsigned granule = program.dev.vgpr_alloc_granule;
   const unsigned alloc = MAX2(granule, DIV_ROUND_UP(p.vgprs, granule) * granule);
   p.waves_per_simd = MIN2((unsigned)program.dev.max_waves_per_simd,
                           (unsigned)program.dev.physical_vgprs / alloc);
   return res;
}

/* ------------------------------------------------------------ basic suite */

static Result
measure_basic(Target& t, const Case& cs, unsigned dots, bool print_ir)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &t.nir_options, "dot4_count");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   /* Independent operands per dot product, from addresses that cannot be
    * common-subexpression-eliminated into each other.
    */
   const uint64_t base = 0x100000ull;
   nir_def* off = nir_ishl_imm(&b, lane_index(&b), 6);
   for (unsigned i = 0; i < dots; i++) {
      nir_def* x;
      nir_def* y;
      nir_def* acc;

      if (cs.operands == Operands::both_const) {
         x = nir_imm_int(&b, weights[i]);
         y = nir_imm_int(&b, weights[(i + 3) % 9]);
      } else {
         nir_def* v = load_global_dwords(&b, base + i * 0x800, 4, off);
         x = nir_channel(&b, v, 0);
         y = cs.operands == Operands::const_weight ? nir_imm_int(&b, weights[i])
                                                   : nir_channel(&b, v, 1);
      }

      if (cs.operands == Operands::const_acc) {
         acc = nir_imm_int(&b, accums[i]);
      } else if (cs.operands == Operands::both_const) {
         acc = nir_channel(&b, load_global_dwords(&b, base + 0x20000 + i * 0x800, 4, off), 0);
      } else {
         acc = nir_channel(&b, load_global_dwords(&b, base + 0x40000 + i * 0x800, 4, off), 2);
      }

      store_global_dword(&b, base + 0x80000 + i * 0x800,
                         build_dot(&b, cs.kind, cs.sat, x, y, acc), off);
   }

   Result r = compile(t, b.shader, pipeline_radv, print_ir);
   ralloc_free(b.shader);
   return r;
}

static void
run_basic(Target& t, const char* only, bool print_ir)
{
   static const Operands opsets[4] = {Operands::data_data, Operands::const_weight,
                                      Operands::const_acc, Operands::both_const};
   static const char* opset_names[4] = {"data_data", "const_weight", "const_acc", "both_const"};
   static const DotKind kinds[3] = {DotKind::sdot, DotKind::udot, DotKind::sudot};

   printf("case,nir_alu_1,nir_alu_9,nir_alu_per_dot,valu_1,valu_9,valu_per_dot,vop1,vop2,vop3,"
          "vop3p,sdwa,salu_9,flat_9,bytes_1,bytes_9,bytes_per_dot\n");

   for (DotKind kind : kinds) {
      for (bool sat : {false, true}) {
         for (int o = 0; o < 4; o++) {
            char name[128];
            snprintf(name, sizeof(name), "%s%s/%s", kind_name(kind), sat ? "_sat" : "",
                     opset_names[o]);
            if (only && !strstr(name, only))
               continue;

            Case cs = {name, kind, sat, opsets[o]};
            Result r1 = measure_basic(t, cs, 1, print_ir);
            Result r9 = measure_basic(t, cs, 9, print_ir);

            printf("%s,%u,%u,%.3f,%u,%u,%.3f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f\n", name, r1.nir_alu,
                   r9.nir_alu, (r9.nir_alu - (double)r1.nir_alu) / 8.0, r1.counts.valu,
                   r9.counts.valu, (r9.counts.valu - (double)r1.counts.valu) / 8.0,
                   r9.counts.vop1, r9.counts.vop2, r9.counts.vop3, r9.counts.vop3p,
                   r9.counts.sdwa, r9.counts.salu, r9.counts.flat, r1.counts.code_bytes,
                   r9.counts.code_bytes,
                   (r9.counts.code_bytes - (double)r1.counts.code_bytes) / 8.0);
            fflush(stdout);
         }
      }
   }
}

/* ------------------------------------------------------------ dense suite */

/* Where the second operand of each dot product (the weight) comes from. */
enum class Weight {
   cnst, /* a compile-time constant                                     */
   unif, /* a uniform buffer load, the same for every lane (SGPR)       */
   lane  /* a per-lane load (VGPR): both operands are divergent         */
};

/* How each dot product reaches its accumulator, as dxil-spirv and
 * vtn_handle_integer_dot() translate HLSL dot4add_i8packed:
 * acc:  acc = dot4add_i8packed(a, w, acc)    -> iadd(sdot(a, w, 0), acc)
 * zero: acc += dot4add_i8packed(a, w, 0)     -> iadd(acc, iadd(sdot(a, w, 0), 0))
 */
enum class Form { acc, zero };

struct DenseCase {
   const char* name;
   unsigned k;     /* activation dwords (4 bytes each) per loop iteration */
   unsigned m;     /* output channels, one accumulator each               */
   unsigned trips; /* loop iterations; 0 = straight-line code            */
   Weight weight;
   Form form;
   bool m_outer; /* m outer, k inner; otherwise k outer, m inner */
};

/* Weight dwords with no byte 0x00, 0x01 or 0xff, as in weights_main, and with
 * no two weights equal: two equal weights on the same activation would make two
 * equal dot products, which CSE merges, and the case would measure fewer dot
 * products than it names. A fixed hash sequence, filtered, so every run and
 * every build sees the same weights.
 */
static uint32_t
dense_weight(unsigned i)
{
   static std::vector<uint32_t> table;
   static std::unordered_set<uint32_t> seen;
   static uint32_t state = 0x2545f491u;
   while (table.size() <= i) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      bool ok = true;
      for (unsigned byte = 0; byte < 4; byte++) {
         const uint32_t v = (state >> (byte * 8)) & 0xffu;
         ok &= v != 0x00 && v != 0x01 && v != 0xff;
      }
      if (ok && seen.insert(state).second)
         table.push_back(state);
   }
   return table[i];
}
static void
dense_body(nir_builder* b, const DenseCase& dc, nir_def* lane_off, nir_def* iter,
           std::vector<nir_def*>& acc, unsigned iter_const)
{
   const uint64_t act_base = 0x1000000ull;
   const uint64_t w_base = 0x4000000ull;
   const gl_access_qualifier ro = (gl_access_qualifier)(ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER);
   /* What ac_nir_flag_smem_for_loads() sets on a uniform read-only load_global
    * before ac_nir_lower_global_access() makes it load_global_amd; the harness
    * builds load_global_amd directly, which that pass does not look at.
    */
   const gl_access_qualifier smem = (gl_access_qualifier)(ro | ACCESS_SMEM_AMD);

   /* The activations of this iteration: k dwords per lane. */
   std::vector<nir_def*> act(dc.k);
   for (unsigned k = 0; k < dc.k; k += 4) {
      nir_def* off = nir_iadd(b, lane_off, nir_imul_imm(b, iter, dc.k * 4u));
      nir_def* v = load_global_dwords(b, act_base + k * 4u, 4, off, ro);
      for (unsigned c = 0; c < 4 && k + c < dc.k; c++)
         act[k + c] = nir_channel(b, v, c);
   }

   auto weight_of = [&](unsigned m, unsigned k) -> nir_def* {
      const unsigned idx = m * dc.k + k;
      switch (dc.weight) {
      case Weight::cnst:
         return nir_imm_int(b, dense_weight(idx + iter_const * dc.m * dc.k));
      case Weight::unif: {
         nir_def* off = nir_imul_imm(b, iter, dc.m * dc.k * 4u);
         nir_def* v = load_global_dwords(b, w_base + (idx & ~3u) * 4u, 4, off, smem);
         return nir_channel(b, v, idx & 3u);
      }
      default: {
         nir_def* off = nir_iadd(b, lane_off, nir_imul_imm(b, iter, dc.m * dc.k * 4u));
         nir_def* v = load_global_dwords(b, w_base + 0x1000000ull + (idx & ~3u) * 4u, 4, off, ro);
         return nir_channel(b, v, idx & 3u);
      }
      }
   };

   auto step = [&](unsigned m, unsigned k) {
      nir_def* d = nir_sdot_4x8_iadd(b, act[k], weight_of(m, k), nir_imm_int(b, 0));
      if (dc.form == Form::zero)
         acc[m] = nir_iadd(b, acc[m], nir_iadd(b, d, nir_imm_int(b, 0)));
      else
         acc[m] = nir_iadd(b, d, acc[m]);
   };

   if (dc.m_outer) {
      for (unsigned m = 0; m < dc.m; m++)
         for (unsigned k = 0; k < dc.k; k++)
            step(m, k);
   } else {
      for (unsigned k = 0; k < dc.k; k++)
         for (unsigned m = 0; m < dc.m; m++)
            step(m, k);
   }
}

static Result
measure_dense(Target& t, const DenseCase& dc, bool print_ir, unsigned* nir_sdots)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &t.nir_options, "dot4_dense");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def* lane_off = nir_ishl_imm(&b, lane_index(&b), 4);

   /* The bias: one per-lane accumulator start value per output channel. */
   std::vector<nir_def*> acc(dc.m);
   for (unsigned m = 0; m < dc.m; m += 4) {
      nir_def* v = load_global_dwords(&b, 0x8000000ull + m * 4u, 4, lane_off);
      for (unsigned c = 0; c < 4 && m + c < dc.m; c++)
         acc[m + c] = nir_channel(&b, v, c);
   }

   if (dc.trips == 0) {
      dense_body(&b, dc, lane_off, nir_imm_int(&b, 0), acc, 0);
   } else {
      /* A counted loop over the input-channel tiles, built with local variables
       * and lowered to SSA, as vtn gives it to radv_optimize_nir().
       */
      nir_function_impl* impl = nir_shader_get_entrypoint(b.shader);
      nir_variable* ivar = nir_local_variable_create(impl, glsl_uint_type(), "i");
      std::vector<nir_variable*> avar(dc.m);
      for (unsigned m = 0; m < dc.m; m++) {
         avar[m] = nir_local_variable_create(impl, glsl_uint_type(), "acc");
         nir_store_var(&b, avar[m], acc[m], 0x1);
      }
      nir_store_var(&b, ivar, nir_imm_int(&b, 0), 0x1);
      nir_push_loop(&b);
      {
         nir_def* i = nir_load_var(&b, ivar);
         nir_push_if(&b, nir_uge_imm(&b, i, dc.trips));
         nir_jump(&b, nir_jump_break);
         nir_pop_if(&b, NULL);
         std::vector<nir_def*> cur(dc.m);
         for (unsigned m = 0; m < dc.m; m++)
            cur[m] = nir_load_var(&b, avar[m]);
         dense_body(&b, dc, lane_off, i, cur, 0);
         for (unsigned m = 0; m < dc.m; m++)
            nir_store_var(&b, avar[m], cur[m], 0x1);
         nir_store_var(&b, ivar, nir_iadd_imm(&b, i, 1), 0x1);
      }
      nir_pop_loop(&b, NULL);
      for (unsigned m = 0; m < dc.m; m++)
         acc[m] = nir_load_var(&b, avar[m]);
   }

   for (unsigned m = 0; m < dc.m; m++)
      store_global_dword(&b, 0xc000000ull + m * 0x100u, acc[m], lane_off);

   /* After the last instruction is inserted: lowering the variables removes
    * instructions, and the builder cursor must not point at one of them.
    */
   if (dc.trips) {
      nir_lower_vars_to_ssa(b.shader);
      nir_remove_dead_variables(b.shader, nir_var_function_temp, NULL);
   }

   *nir_sdots = count_nir_op(b.shader, nir_op_sdot_4x8_iadd) * MAX2(dc.trips, 1u);

   Result r = compile(t, b.shader, true, print_ir);
   ralloc_free(b.shader);
   return r;
}

static void
run_dense(Target& t, const char* only, bool print_ir)
{
   /* Three sizes: a moderate straight-line layer, a large one that cannot fit
    * in 256 VGPRs if the compiler keeps every product live, and a counted loop
    * with 2048 dot products in all.
    */
   static const DenseCase cases[] = {
      {"s16x16/unif/zero/kouter", 16, 16, 0, Weight::unif, Form::zero, false},
      {"s16x16/unif/acc/kouter", 16, 16, 0, Weight::unif, Form::acc, false},
      {"s16x16/unif/zero/mouter", 16, 16, 0, Weight::unif, Form::zero, true},
      {"s16x16/lane/zero/kouter", 16, 16, 0, Weight::lane, Form::zero, false},
      {"s16x16/cnst/zero/kouter", 16, 16, 0, Weight::cnst, Form::zero, false},
      {"s16x16/cnst/acc/kouter", 16, 16, 0, Weight::cnst, Form::acc, false},
      {"l32x32/unif/zero/kouter", 32, 32, 0, Weight::unif, Form::zero, false},
      {"l32x32/unif/zero/mouter", 32, 32, 0, Weight::unif, Form::zero, true},
      {"l32x32/lane/zero/kouter", 32, 32, 0, Weight::lane, Form::zero, false},
      {"l32x32/cnst/zero/kouter", 32, 32, 0, Weight::cnst, Form::zero, false},
      {"loop8x32x8/unif/zero/kouter", 8, 32, 8, Weight::unif, Form::zero, false},
      {"loop8x32x8/unif/acc/kouter", 8, 32, 8, Weight::unif, Form::acc, false},
      {"loop8x32x8/lane/zero/kouter", 8, 32, 8, Weight::lane, Form::zero, false},
      {"loop8x32x8/unif/zero/mouter", 8, 32, 8, Weight::unif, Form::zero, true},
      {"loop4x64x16/unif/zero/kouter", 4, 64, 16, Weight::unif, Form::zero, false},
   };

   printf("case,sdots,nir_alu,instrs,valu,salu,smem,vmem,vop3,sdwa,presched_vgpr,vgprs,sgprs,"
          "spilled_vgprs,spilled_sgprs,scratch_bytes_per_wave,lds_bytes,waves_per_simd,code_bytes\n");
   for (const DenseCase& dc : cases) {
      if (only && !strstr(dc.name, only))
         continue;
      unsigned sdots = 0;
      Result r = measure_dense(t, dc, print_ir, &sdots);
      const Counts& c = r.counts;
      const Pressure& p = r.pressure;
      printf("%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", dc.name, sdots,
             r.nir_alu, c.all, c.valu, c.salu, c.smem, c.vmem + c.flat, c.vop3, c.sdwa,
             p.presched_vgpr, p.vgprs, p.sgprs, p.spilled_vgprs, p.spilled_sgprs,
             p.scratch_bytes_per_wave, p.lds_bytes, p.waves_per_simd, c.code_bytes);
      fflush(stdout);
   }
}

/* ---------------------------------------------------------------------- main */

int
main(int argc, char** argv)
{
   bool print_ir = false;
   bool selftest_gate = false;
   bool dense = false;
   const char* only = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--ir"))
         print_ir = true;
      else if (!strcmp(argv[i], "--only") && i + 1 < argc)
         only = argv[++i];
      else if (!strcmp(argv[i], "--nopow2-weights"))
         weights = weights_nopow2;
      else if (!strcmp(argv[i], "--selftest-gate"))
         selftest_gate = true;
      else if (!strcmp(argv[i], "--suite") && i + 1 < argc && !strcmp(argv[i + 1], "dense"))
         dense = true, i++;
      else if (!strcmp(argv[i], "--suite") && i + 1 < argc && !strcmp(argv[i + 1], "basic"))
         dense = false, i++;
      else if (!strcmp(argv[i], "--pipeline") && i + 1 < argc && !strcmp(argv[i + 1], "radv"))
         pipeline_radv = true, pipeline_set = true, i++;
      else if (!strcmp(argv[i], "--pipeline") && i + 1 < argc && !strcmp(argv[i + 1], "simple"))
         pipeline_radv = false, pipeline_set = true, i++;
      else if (!strcmp(argv[i], "--defer-sdot"))
         defer_sdot = true;
      else if (!strcmp(argv[i], "--dense-prepass") && i + 1 < argc && !strcmp(argv[i + 1], "mad3"))
         dense_prepass = Prepass::mad3, i++;
      else if (!strcmp(argv[i], "--dense-prepass") && i + 1 < argc &&
               !strcmp(argv[i + 1], "twochain"))
         dense_prepass = Prepass::twochain, i++;
      else {
         fprintf(stderr, "usage: aco_dot4_count [--suite basic|dense] [--pipeline simple|radv] "
                         "[--defer-sdot] [--dense-prepass mad3|twochain] [--ir] "
                         "[--nopow2-weights] [--only <case substring>] [--selftest-gate]\n");
         return 1;
      }
   }

   /* The negative control for the contract gate: feed it a violating input and
    * it must kill the process with exit 9. A gate that cannot be shown to fire
    * is not a gate.
    */
   if (selftest_gate) {
      radeon_info fake_info = {};
      fake_info.compiler_info.has_accelerated_dot_product = true;
      nir_shader_compiler_options fake_options = {};
      fake_options.has_sdot_4x8 = true;
      fake_options.has_sudot_4x8_sat = true;
      require_no_hw_dot(fake_info, fake_options);
      fprintf(stderr, "FATAL: the contract gate returned on a violating input\n");
      return 10;
   }

   if (!pipeline_set)
      pipeline_radv = dense;
   if ((defer_sdot || dense_prepass != Prepass::none) && !pipeline_radv) {
      fprintf(stderr, "--defer-sdot and --dense-prepass need --pipeline radv\n");
      return 1;
   }
   fprintf(stderr, "suite=%s pipeline=%s defer_sdot=%d dense_prepass=%s\n",
           dense ? "dense" : "basic", pipeline_radv ? "radv" : "simple", defer_sdot,
           dense_prepass == Prepass::none ? "none"
           : dense_prepass == Prepass::mad3 ? "mad3" : "twochain");

   aco::init();
   glsl_type_singleton_init_or_ref();

   Target* t = new Target;
   init_target(t);
   if (dense)
      run_dense(*t, only, print_ir);
   else
      run_basic(*t, only, print_ir);
   delete t;

   glsl_type_singleton_decref();
   return 0;
}
