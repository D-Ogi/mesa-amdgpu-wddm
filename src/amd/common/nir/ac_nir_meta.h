/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_NIR_META_H
#define AC_NIR_META_H

#include "ac_gpu_info.h"
#include "nir_defines.h"
#include "util/box.h"
#include "util/macros.h"

/* Every shader key below overlays a single uint64_t on a bit-field struct, and callers use that
 * uint64_t as the whole key: they hash it, compare it, copy a key through it, and put it in
 * hash_table_u64. That only works while every bit of the struct is inside those 8 bytes.
 *
 * Therefore all bit-field members of these unions are declared with the same 64-bit type, and each
 * union has a static_assert on its size. The declared type matters: a bit-field starts a new
 * allocation unit whenever the declared type changes size (this is the Microsoft record layout, and
 * the standard allows it everywhere), so a mix of bool, uint8_t and unsigned members spreads the
 * bits over several allocation units. ac_cs_clear_copy_buffer_key used to mix bool and unsigned and
 * was 24 bytes with MSVC, where "key" covered only the first two fields; a key copied through "key"
 * silently dropped the rest. Keep one type per union, keep the asserts, and the keys stay whole on
 * every compiler. bin/check_bitfield_overlay_unions.py is the gate that keeps it that way for any
 * union of this shape, including the ones a future upstream commit adds.
 *
 * One thing did change with the type: a 1-bit member no longer narrows an assignment to 0 or 1.
 * "bool flag:1 = 2" stores 1, while "uint64_t flag:1 = 2" stores 0, because the value is truncated
 * instead of converted to bool. The members still read and compare like booleans, but assign a
 * 1-bit member from a boolean expression (a comparison, a "!", a bool variable) or write "!!value".
 * Never assign a raw mask or a count to one.
 */

typedef union {
   struct {
      uint64_t use_aco:1;
      uint64_t src_is_array:1;
      uint64_t log_samples:2;
      uint64_t last_src_channel:2; /* this shouldn't be greater than last_dst_channel */
      uint64_t last_dst_channel:2;
      uint64_t x_clamp_to_edge:1;
      uint64_t y_clamp_to_edge:1;
      uint64_t a16:1;
      uint64_t d16:1;
   };
   uint64_t key; /* use with hash_table_u64 */
} ac_ps_resolve_key;
static_assert(sizeof(ac_ps_resolve_key) == 8,
              "ac_ps_resolve_key.key must cover every bit-field of the union");

/* Only immutable settings. */
struct ac_ps_resolve_options {
   const nir_shader_compiler_options *nir_options;
   const struct radeon_info *info;
   bool use_aco;     /* global driver setting */
   bool no_fmask;    /* FMASK disabled by a debug option, ignored on GFX11+ */
   bool print_key;   /* print ac_ps_resolve_key into stderr */
};

nir_shader *
ac_create_resolve_ps(const struct ac_ps_resolve_options *options,
                     const ac_ps_resolve_key *key);

/* Universal optimized compute shader for image blits and clears. */
#define SI_MAX_COMPUTE_BLIT_LANE_SIZE  16
#define SI_MAX_COMPUTE_BLIT_SAMPLES    8

/* This describes all possible variants of the compute blit shader. */
typedef union {
   struct {
      uint64_t use_aco:1;
      /* Workgroup settings. */
      uint64_t wg_dim:2; /* 1, 2, or 3 */
      uint64_t has_start_xyz:1;
      uint64_t format_is_96bit:1;
      uint64_t addr_math_64bit:1;
      /* The size of a block of pixels that a single thread will process. */
      uint64_t log_lane_width:3;
      uint64_t log_lane_height:2;
      uint64_t log_lane_depth:2;
      /* Declaration modifiers. */
      uint64_t is_clear:1;
      uint64_t src_is_sampler:1; /* if not, it's a storage image */
      uint64_t src_is_1d:1;
      uint64_t dst_is_1d:1;
      uint64_t src_is_msaa:1;
      uint64_t dst_is_msaa:1;
      uint64_t src_has_z:1;
      uint64_t dst_has_z:1;
      uint64_t src_has_non_identity_fmask:1;
      uint64_t dst_is_rgb5:1;
      uint64_t a16:1;
      uint64_t d16:1;
      uint64_t log_samples:2;
      /* If clearing, dst is MSAA, and only sample 0 is cleared to the clear value.
       * If resolving, src is MSAA, dst is not MSAA, and only sample 0 is copied to dst.
       * log2_samples is set to 0 in both cases.
       */
      uint64_t sample0_only:1;
      /* Source coordinate modifiers. */
      uint64_t x_clamp_to_edge:1;
      uint64_t y_clamp_to_edge:1;
      uint64_t flip_x:1;
      uint64_t flip_y:1;
      /* Output modifiers. */
      uint64_t sint_to_uint:1;
      uint64_t uint_to_sint:1;
      uint64_t dst_is_srgb:1;
      uint64_t use_integer_one:1;
      uint64_t last_src_channel:2; /* this shouldn't be greater than last_dst_channel */
      uint64_t last_dst_channel:2;
   };
   uint64_t key;
} ac_cs_blit_key;
static_assert(sizeof(ac_cs_blit_key) == 8,
              "ac_cs_blit_key.key must cover every bit-field of the union");

typedef struct {
   /* Global options. */
   const nir_shader_compiler_options *nir_options;
   const struct radeon_info *info;
   bool use_aco;        /* global driver setting */
   bool print_key;      /* print ac_ps_resolve_key into stderr */
   bool fail_if_slow;   /* fail if a gfx blit is faster, set to false on compute queues */

   bool is_nested;      /* for internal use, don't set */
} ac_cs_blit_options;

typedef struct {
   struct {
      const struct radeon_surf *surf;
      uint8_t dim;            /* 1 = 1D texture, 2 = 2D texture, 3 = 3D texture */
      bool is_array;          /* array or cube texture */
      unsigned width0;        /* level 0 width */
      unsigned height0;       /* level 0 height */
      uint8_t num_samples;
      uint8_t level;
      struct pipe_box box;       /* negative width, height only legal for src */
      enum pipe_format format;   /* format reinterpretation */
   } dst, src;

   bool src_is_sampler;
   bool src_has_non_identity_fmask;

   /* When clearing, clear only sample 0. Only useful when FMASK is enabled.
    * When resolving, copy sample 0 to the single-sample destination.
    */
   bool sample0_only;

   /* Whether src and dst are 96-bit formats.
    * The bindings should be storage buffers, whose 64-bit address is extracted to get >4GB
    * addressing. All coordinates must be in bounds. X coordinates and widths should be
    * multiplied by 3 by the caller. If clearing, the clear value has 3 dwords.
    */
   bool format_is_96bit;

   /* If src.surf == NULL, this is the clear color. */
   union pipe_color_union clear_color;
} ac_cs_blit_description;

/* Dispatch parameters generated by the blit. */
typedef struct {
   ac_cs_blit_key shader_key;
   uint32_t user_data[8];        /* for nir_intrinsic_load_user_data_amd */
   unsigned num_user_data_terms;

   unsigned wg_size[3];          /* variable workgroup size (NUM_THREAD_FULL) */
   unsigned last_wg_size[3];     /* workgroup size of the last workgroup (NUM_THREAD_PARTIAL) */
   unsigned num_workgroups[3];   /* DISPATCH_DIRECT parameters */
   unsigned num_invocations[3];
} ac_cs_blit_dispatch;

typedef struct {
   unsigned num_dispatches;
   ac_cs_blit_dispatch dispatches[7];
} ac_cs_blit_dispatches;

nir_shader *
ac_create_blit_cs(const ac_cs_blit_options *options, const ac_cs_blit_key *key);

bool
ac_prepare_compute_blit(const ac_cs_blit_options *options,
                        const ac_cs_blit_description *blit,
                        ac_cs_blit_dispatches *dispatches);

/* clear_buffer/copy_buffer compute shader. */
typedef union {
   struct {
      uint64_t is_clear:1;
      uint64_t dwords_per_thread:3; /* 1..4 allowed */
      uint64_t clear_value_size_is_12:1;
      uint64_t clear_value_size_is_4:1;
      uint64_t src_scalarize_for_sparse:1;
      /* Unaligned clears and copies. */
      uint64_t src_align_offset:2; /* how much is the source address unaligned */
      uint64_t dst_align_offset:4; /* the first thread shouldn't write this many bytes */
      uint64_t dst_last_thread_bytes:4; /* if non-zero, the last thread should write this many bytes */
      uint64_t dst_single_thread_unaligned:1; /* only 1 thread executes, both previous fields apply */
      uint64_t has_start_thread:1; /* whether the first few threads should be skipped, making later
                                      waves start on a 256B boundary */
      uint64_t addr_user_data:1;   /* pass src/dst addresses in user data SGPRs */
   };
   uint64_t key;
} ac_cs_clear_copy_buffer_key;
static_assert(sizeof(ac_cs_clear_copy_buffer_key) == 8,
              "ac_cs_clear_copy_buffer_key.key must cover every bit-field of the union");

typedef struct {
   const nir_shader_compiler_options *nir_options;
   const struct radeon_info *info;
   uint64_t prefer_cp_dma_threshold; /* prefer CP DMA below the threshold when usable, even when slower */
   bool print_key;      /* print the shader key into stderr */
   bool fail_if_slow;   /* fail if a gfx blit is faster, set to false on compute queues */
   bool addr_user_data; /* pass src/dst addresses in user data SGPRs */
} ac_cs_clear_copy_buffer_options;

typedef struct {
   uint64_t dst_offset;
   uint64_t src_offset;
   uint64_t size;
   unsigned clear_value_size;
   uint32_t clear_value[4];
   unsigned dwords_per_thread;   /* Set to 0 to let the code choose the optimal value. */
   bool render_condition_enabled;
   bool dst_is_vram;
   bool src_is_vram;
   bool dst_is_sparse;
   bool src_is_sparse;
} ac_cs_clear_copy_buffer_info;

typedef struct {
   ac_cs_clear_copy_buffer_key shader_key;
   uint32_t user_data[16];        /* for nir_intrinsic_load_user_data_amd */
   unsigned num_user_data;
   unsigned num_ssbos;
   unsigned workgroup_size;
   unsigned num_threads;
   unsigned dispatch_interleave; /* COMPUTE_DISPATCH_INTERLEAVE.INTERLEAVE/INTERLEAVE_1D */

   struct {
      uint64_t offset;
      uint64_t size;
   } ssbo[2];
} ac_cs_clear_copy_buffer_dispatch;

nir_shader *
ac_create_clear_copy_buffer_cs(const ac_cs_clear_copy_buffer_options *options,
                               const ac_cs_clear_copy_buffer_key *key);

bool
ac_prepare_cs_clear_copy_buffer(const ac_cs_clear_copy_buffer_options *options,
                                const ac_cs_clear_copy_buffer_info *info,
                                ac_cs_clear_copy_buffer_dispatch *out);

#endif
