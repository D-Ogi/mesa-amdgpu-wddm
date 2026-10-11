/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/* BD-118 host gate: the two layouts of a cross-API shared surface, from the real surface code.
 *
 * It answers three questions without a GPU:
 *
 *   1. What layout does each side of the BD-105 dxgi-r1 arm S cell choose? The exporting side (our
 *      D3D12 shell) shares a LINEAR surface and records its pitch and size (BD-075, LB7A); the
 *      importing side computes its own layout from VkImageCreateInfo, and VK_IMAGE_TILING_OPTIMAL
 *      gives a tiled surface. Both layouts are computed here with ac_compute_surface and addrlib,
 *      the same code the ICD runs, for unit A's measured chip values.
 *
 *   2. Does that difference explain the measurement? The cell's own pattern is reproduced, moved
 *      through the two layouts, and compared texel by texel. The expected numbers are the lab's:
 *      65520 of 65536 texels differ, the first at (4,0), and the value that arrives there is the
 *      one printed in arm S's log, in both directions. Reproducing them bit for bit is what turns
 *      "most likely a tiling mismatch" into the cause.
 *
 *   3. Does the fix make the two sides agree, for this cell and for a sweep of shared formats and
 *      sizes, and would the code before the fix have accepted the mismatch? The decision function
 *      and the shape rule are the driver's own (src/amd/vulkan/radv_shared_layout.h), and the
 *      pre-BD-118 behaviour is kept next to them as the negative control.
 *
 * What this gate does not do: it never calls radv_choose_tiling. The mode of every surface here is
 * given to ac_compute_surface by this file, and the shape rule is called directly. That the driver
 * reaches the rule with the right description (VkImageCreateInfo -> radv_get_surface_flags ->
 * radv_choose_tiling) is shown on the lab by the arm S line "the layouts agree", not here.
 *
 * Measured inputs, each with its source:
 *   - chip: device 0x13FE, family_id 143 (FAMILY_NV), external_rev 132 (0x84), chip_rev 2,
 *     GB_ADDR_CONFIG 0x00100044, MC_ARB_RAMCFG 0 - bc250-win driver/contract/test/
 *     bc250_caps_unitA.c:339, :505-516 (E01 gca_config.bin, E14 radv-info.txt:186).
 *   - cell: 256x256 B8G8R8A8_UNORM, LAYOUT_UNKNOWN, ALLOW_RENDER_TARGET, committed, HEAP_FLAG_
 *     SHARED; the Vulkan side imports it as dedicated D3D12_RESOURCE memory into a
 *     VK_IMAGE_TILING_OPTIMAL image with TRANSFER_SRC|TRANSFER_DST|COLOR_ATTACHMENT usage;
 *     D3D12 footprint pitch 1024, Vulkan allocation 262144 bytes - sharecell12.cpp:516-643 and
 *     the arm S log (sharecell12.out:96).
 *   - the failure: "first mismatch at 4,0: want ff34001c have ffa000e0, 65520 texels differ"
 *     (sharecell12.out:191) and "have ff0d0b00" the other way (sharecellvk.out:189).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/u_math.h"
#include "ac_drm_fourcc.h"
#include "ac_gpu_info.h"
#include "ac_surface.h"
#include "amd_family.h"
#include "amdgfxregs.h"
#include "addrlib/inc/addrinterface.h"

/* The surface flag encoding (RADEON_SURF_SET, the mode and type constants) comes from the driver's
 * own header, so the gate cannot drift from it: src/amd/vulkan/radv_radeon_winsys.h:99-112.
 */
#include "radv_radeon_winsys.h"
#include "radv_shared_layout.h"

static unsigned failures;
static unsigned checks;

static void
check(bool ok, const char *what)
{
   checks++;
   if (ok) {
      printf("PASS %s\n", what);
   } else {
      failures++;
      printf("FAIL %s\n", what);
   }
}

/* --negative-control runs the whole gate over the driver as it was before BD-118: the import
 * applies no layout (the image keeps the tiled surface of VK_IMAGE_TILING_OPTIMAL) and the decision
 * accepts every pair. Every check that the fix exists for then fails, which is what makes the gate
 * a gate and not a tautology.
 */
static bool g_old_code;
static enum radv_shared_layout_verdict (*g_decide)(const struct radv_shared_layout *,
                                                   const struct radv_shared_layout *) =
   radv_shared_layout_compare;

/* The shape rule decides which descriptions the importer lays out linearly at all. Under
 * --negative-control it is the rule of the first BD-118 round (2D and single-sampled, nothing
 * else), which admitted depth, mip chains and arrays; the sweep then asks addrlib for a linear
 * depth surface, which GFX10 does not have, and the control fails on that too.
 *
 * What the gate does NOT run is radv_choose_tiling itself: the mode of each surface below is given
 * to ac_compute_surface by this file. The chain from VkImageCreateInfo through
 * radv_choose_tiling to this rule is covered by the lab arm (the "the layouts agree" line of arm
 * S), not here.
 */
static bool (*g_shape_decide)(const struct radv_shared_shape *) = radv_shared_shape_is_linear;

/* ---------------------------------------------------------------------------------------------
 * The cell's pattern, copied from bc250-win tools/win/wsi-dxgi/sharecell-common.h:51-59.
 * ------------------------------------------------------------------------------------------- */
static uint32_t
pattern_texel(uint32_t x, uint32_t y, uint32_t round)
{
   const uint32_t b = (x * 7u + round * 29u) & 0xffu;
   const uint32_t g = (y * 11u + round * 53u) & 0xffu;
   const uint32_t r = ((x ^ y) * 13u + round) & 0xffu;
   const uint32_t a = 0xffu;
   return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ---------------------------------------------------------------------------------------------
 * Unit A, from the measured values alone. Only the fields the surface code reads are set; every
 * one of them is printed below, so a field that is wrong is visible and not hidden in a pass.
 * ------------------------------------------------------------------------------------------- */
static void
unit_a_info(struct radeon_info *info)
{
   memset(info, 0, sizeof(*info));
   info->gfx_level = GFX10;
   info->family = CHIP_GFX1013;
   info->family_id = 143;         /* FAMILY_NV */
   info->chip_external_rev = 132; /* 0x84, inside the GFX1013 range */
   info->chip_rev = 2;
   info->pci_id = 0x13FE;
   info->gb_addr_config = 0x00100044;
   info->mc_arb_ramcfg = 0;
   info->num_tile_pipes = 1 << G_0098F8_NUM_PIPES(info->gb_addr_config);
   info->has_graphics = true;
   /* ac_gpu_info.c:1193-1194 derives it as "has_graphics || family < CHIP_GFX940"; both hold here.
    * Without it ac_compute_surface replaces every requested mode with LINEAR_ALIGNED
    * (ac_surface.c:4235-4237, the CDNA image emulation path) and no tiled surface exists at all.
    */
   info->has_image_opcodes = true;
   info->has_dedicated_vram = false; /* an APU: the carve-out is system memory */
   info->use_display_dcc_unaligned = false;
   info->use_display_dcc_with_retile_blit = false;
   info->has_dcc_constant_encode = false;
   info->max_render_backends = 8;
   info->num_se = 2;
}

/* The surface flags radv_get_surface_flags gives the arm S image: a 2D colour surface, no
 * metadata of any kind (DCC is off for every image with external handle types,
 * radv_image.c:231), no FMASK (single sample), and no texture (the cell asks for neither SAMPLED
 * nor STORAGE usage).
 */
static uint64_t
arm_s_surface_flags(unsigned mode, bool allow_texture, bool depth_or_stencil)
{
   uint64_t flags = RADEON_SURF_SET(mode, MODE) | RADEON_SURF_SET(RADEON_SURF_TYPE_2D, TYPE) |
                    RADEON_SURF_CONTIGUOUS_DCC_LAYERS | RADEON_SURF_DISABLE_DCC |
                    RADEON_SURF_NO_FMASK;

   /* The client's image asks for neither SAMPLED nor STORAGE usage, so radv sets NO_TEXTURE; the
    * engine's linear image behind the shared D3D12 resource does allow sampling. The sweep below
    * computes both and requires the same linear layout from them, because the exporter's recorded
    * pitch is the engine image's rowPitch (vkd3d-proton fork libs/vkd3d/resource.c:4731-4796).
    */
   if (!allow_texture)
      flags |= RADEON_SURF_NO_TEXTURE;
   /* radv_get_surface_flags adds this for a depth or stencil format (radv_image.c), and it is what
    * makes a linear surface impossible on GFX10. The sweep's depth rows carry it, so the gate sees
    * the same refusal the driver would.
    */
   if (depth_or_stencil)
      flags |= RADEON_SURF_ZBUFFER;
   return flags;
}

/* radv_surface_init, for one plane of a colour or depth image. */
static bool
compute_surface_shaped(struct ac_addrlib *addrlib, const struct radeon_info *info, unsigned width,
                       unsigned height, unsigned levels, unsigned bpe, unsigned num_channels,
                       unsigned mode, bool allow_texture, bool depth_or_stencil,
                       struct radeon_surf *surf)
{
   struct ac_surf_config config;

   memset(surf, 0, sizeof(*surf));
   surf->blk_w = 1;
   surf->blk_h = 1;
   surf->bpe = bpe;
   surf->flags = arm_s_surface_flags(mode, allow_texture, depth_or_stencil);
   surf->modifier = DRM_FORMAT_MOD_INVALID;

   memset(&config, 0, sizeof(config));
   config.info.width = width;
   config.info.height = height;
   config.info.depth = 1;
   config.info.samples = 1;
   config.info.storage_samples = 1;
   config.info.levels = levels;
   config.info.array_size = 1;
   config.info.num_channels = num_channels;
   config.blk_w = surf->blk_w;
   config.blk_h = surf->blk_h;
   config.bpe = surf->bpe;
   config.surf_flags = surf->flags;
   config.modifier = surf->modifier;

   return ac_compute_surface(addrlib, info, &config, mode, surf) == 0;
}

/* The client's flavour of a colour surface: the image of the share cell, which asks for no
 * sampling.
 */
static bool
compute_surface(struct ac_addrlib *addrlib, const struct radeon_info *info, unsigned width,
                unsigned height, unsigned levels, unsigned bpe, unsigned num_channels, unsigned mode,
                struct radeon_surf *surf)
{
   return compute_surface_shaped(addrlib, info, width, height, levels, bpe, num_channels, mode, false,
                                 false, surf);
}

/* The same, for a description that may be a depth or stencil one. */
static bool
compute_surface_z(struct ac_addrlib *addrlib, const struct radeon_info *info, unsigned width,
                  unsigned height, unsigned levels, unsigned bpe, unsigned num_channels,
                  unsigned mode, bool depth_or_stencil, struct radeon_surf *surf)
{
   return compute_surface_shaped(addrlib, info, width, height, levels, bpe, num_channels, mode, false,
                                 depth_or_stencil, surf);
}

/* The same again, for a description with more than one sample or more than one layer. The helpers
 * above fix both at 1, and the late-entry step below needs exactly the shapes they cannot build:
 * what addrlib answers when the relayout asks it for a linear surface of such a description.
 */
static bool
compute_surface_samples(struct ac_addrlib *addrlib, const struct radeon_info *info, unsigned width,
                        unsigned height, unsigned levels, unsigned layers, unsigned samples,
                        unsigned bpe, unsigned num_channels, unsigned mode, struct radeon_surf *surf)
{
   struct ac_surf_config config;

   memset(surf, 0, sizeof(*surf));
   surf->blk_w = 1;
   surf->blk_h = 1;
   surf->bpe = bpe;
   surf->flags = arm_s_surface_flags(mode, false, false);
   surf->modifier = DRM_FORMAT_MOD_INVALID;

   memset(&config, 0, sizeof(config));
   config.info.width = width;
   config.info.height = height;
   config.info.depth = 1;
   config.info.samples = samples;
   config.info.storage_samples = samples;
   config.info.levels = levels;
   config.info.array_size = layers;
   config.info.num_channels = num_channels;
   config.blk_w = surf->blk_w;
   config.blk_h = surf->blk_h;
   config.bpe = surf->bpe;
   config.surf_flags = surf->flags;
   config.modifier = surf->modifier;

   return ac_compute_surface(addrlib, info, &config, mode, surf) == 0;
}

/* The decision of the LATE import entry (radv_image.c, radv_image_apply_shared_layout), as the
 * driver makes it: first the depth or stencil surface flag, then the whole shared shape.
 *
 * This is a separate entry from radv_choose_tiling. Tiling selection sees VkImageCreateInfo and
 * leaves an ineligible image tiled; the late entry is then reached with that tiled image and an
 * exporter record it does not match, reads a MODE disagreement, and - before R1 of the review -
 * went on to ask addrlib for a linear surface of a description addrlib refuses. The shape rule
 * travels through g_shape_decide, so --negative-control drives this entry with the rule of the
 * first BD-118 round and the step below fails with it.
 */
static bool
late_entry_refuses(const struct radv_shared_shape *shape, bool depth_or_stencil_flag)
{
   if (depth_or_stencil_flag)
      return true;
   return !g_shape_decide(shape);
}

/* The late entry as it was before R1: the depth or stencil surface flag alone. Every other
 * ineligible shape passed it and reached the relayout. Used to show that the early predicate did
 * not cover this entry, which is the whole point of the step below.
 */
static bool
late_entry_refuses_pre_r1(const struct radv_shared_shape *shape, bool depth_or_stencil_flag)
{
   (void)shape;
   return depth_or_stencil_flag;
}

static struct radv_shared_layout
layout_of(const struct radeon_surf *surf, uint64_t memory_size)
{
   const struct radv_shared_layout layout = {
      .is_linear = surf->is_linear,
      .swizzle_mode = surf->u.gfx9.swizzle_mode,
      .pitch_bytes = (uint64_t)surf->u.gfx9.surf_pitch * surf->bpe,
      .surf_size = surf->total_size,
      .memory_size = memory_size,
   };
   return layout;
}

/* ---------------------------------------------------------------------------------------------
 * The address of one texel, from addrlib, for the layout the importer computed.
 * ------------------------------------------------------------------------------------------- */
static uint64_t
tiled_address(ADDR_HANDLE hlib, const struct radeon_surf *surf, unsigned width, unsigned height,
              unsigned bpe, unsigned x, unsigned y)
{
   ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in = {0};
   ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out = {0};

   in.size = sizeof(in);
   in.x = x;
   in.y = y;
   in.slice = 0;
   in.sample = 0;
   in.mipId = 0;
   in.swizzleMode = (AddrSwizzleMode)surf->u.gfx9.swizzle_mode;
   in.resourceType = ADDR_RSRC_TEX_2D;
   in.bpp = bpe * 8;
   in.unalignedWidth = width;
   in.unalignedHeight = height;
   in.numSlices = 1;
   in.numMipLevels = 1;
   in.numSamples = 1;
   in.numFrags = 1;
   in.pipeBankXor = (UINT_32)surf->tile_swizzle << 8;
   in.pitchInElement = surf->u.gfx9.surf_pitch;
   out.size = sizeof(out);

   if (Addr2ComputeSurfaceAddrFromCoord(hlib, &in, &out) != ADDR_OK)
      return UINT64_MAX;
   return out.addr;
}

/* The exporter's side: a linear surface of the recorded pitch. */
static uint64_t
linear_address(uint64_t pitch_bytes, unsigned bpe, unsigned x, unsigned y)
{
   return (uint64_t)y * pitch_bytes + (uint64_t)x * bpe;
}

struct replay {
   uint64_t mismatches;
   uint32_t first_x, first_y;
   uint32_t want, have;
   bool addressing_failed;
};

/* Move the pattern through two layouts and compare it where the reader looks for it.
 *
 * `writer` is the layout of the side that fills the surface, `reader` the layout of the side that
 * reads it back. Every texel the writer puts at address A is read by whoever asks for the texel
 * whose reader address is A. Both layouts of this cell cover the same 262144 bytes, so the map is
 * a permutation of the texels, and the readback is the pattern permuted. When the two layouts are
 * the same, the permutation is the identity and nothing differs.
 */
static struct replay
replay_round(ADDR_HANDLE hlib, const struct radeon_surf *tiled, bool writer_is_tiled,
             uint64_t linear_pitch, unsigned width, unsigned height, unsigned bpe, uint32_t round)
{
   struct replay out = {0};
   uint32_t *memory = calloc((size_t)width * height, sizeof(uint32_t));
   const uint64_t bytes = (uint64_t)width * height * bpe;

   if (!memory) {
      out.addressing_failed = true;
      return out;
   }

   /* The writer fills the surface. */
   for (unsigned y = 0; y < height; y++) {
      for (unsigned x = 0; x < width; x++) {
         const uint64_t addr = writer_is_tiled ? tiled_address(hlib, tiled, width, height, bpe, x, y)
                                               : linear_address(linear_pitch, bpe, x, y);
         if (addr == UINT64_MAX || addr + bpe > bytes || addr % bpe) {
            out.addressing_failed = true;
            free(memory);
            return out;
         }
         memory[addr / bpe] = pattern_texel(x, y, round);
      }
   }

   /* The reader reads it back where its own layout says each texel is. */
   for (unsigned y = 0; y < height; y++) {
      for (unsigned x = 0; x < width; x++) {
         const uint64_t addr = writer_is_tiled ? linear_address(linear_pitch, bpe, x, y)
                                               : tiled_address(hlib, tiled, width, height, bpe, x, y);
         const uint32_t want = pattern_texel(x, y, round);
         uint32_t have;

         if (addr == UINT64_MAX || addr + bpe > bytes || addr % bpe) {
            out.addressing_failed = true;
            free(memory);
            return out;
         }
         have = memory[addr / bpe];
         if (have == want)
            continue;
         if (!out.mismatches) {
            out.first_x = x;
            out.first_y = y;
            out.want = want;
            out.have = have;
         }
         out.mismatches++;
      }
   }

   free(memory);
   return out;
}

/* ---------------------------------------------------------------------------------------------
 * The sweep: the shared formats of the contract table and sizes around the powers of two.
 * ------------------------------------------------------------------------------------------- */
struct sweep_format {
   const char *name;
   unsigned bpe;
   unsigned num_channels;
   bool is_depth;
};

static const struct sweep_format sweep_formats[] = {
   {"B8G8R8A8_UNORM", 4, 4, false},      {"R8G8B8A8_UNORM", 4, 4, false},
   {"A2B10G10R10_UNORM", 4, 4, false},   {"R16G16B16A16_SFLOAT", 8, 4, false},
   {"D32_SFLOAT", 4, 1, true},
};

static const unsigned sweep_sizes[] = {1, 2, 3, 7, 16, 63, 64, 100, 256, 257, 1280, 1920, 4096};

int
main(int argc, char **argv)
{
   const bool negative_control = argc > 1 && !strcmp(argv[1], "--negative-control");
   struct radeon_info info;
   if (negative_control) {
      g_old_code = true;
      g_decide = radv_shared_layout_compare_pre_bd118;
      g_shape_decide = radv_shared_shape_is_linear_r1;
      printf("STEP negative control: the driver as it was before BD-118, with the shape rule of the "
             "first BD-118 round\n");
   }

   struct ac_addrlib *addrlib;
   ADDR_HANDLE hlib;
   uint64_t max_alignment = 0;
   struct radeon_surf tiled, linear;

   unit_a_info(&info);
   printf("STEP unit A, from the measured values (bc250_caps_unitA.c)\n");
   printf("INFO chip family_id %u external_rev %u chip_rev %u pci_id 0x%04x gfx_level %u "
          "gb_addr_config 0x%08x num_tile_pipes %u num_se %u max_render_backends %u "
          "has_graphics %u has_image_opcodes %u\n",
          info.family_id, info.chip_external_rev, info.chip_rev, info.pci_id, (unsigned)info.gfx_level,
          info.gb_addr_config, info.num_tile_pipes, info.num_se, info.max_render_backends,
          (unsigned)info.has_graphics, (unsigned)info.has_image_opcodes);

   addrlib = ac_addrlib_create(&info, &max_alignment);
   check(addrlib != NULL, "addrlib accepts unit A's chip values");
   if (!addrlib)
      goto done;
   hlib = (ADDR_HANDLE)ac_addrlib_get_handle(addrlib);
   printf("INFO addrlib max alignment %llu\n", (unsigned long long)max_alignment);

   printf("STEP the arm S cell: 256x256 B8G8R8A8_UNORM, one mip\n");
   check(compute_surface(addrlib, &info, 256, 256, 1, 4, 4, RADEON_SURF_MODE_2D, &tiled),
         "the importer's VK_IMAGE_TILING_OPTIMAL surface was computed");
   check(compute_surface(addrlib, &info, 256, 256, 1, 4, 4, RADEON_SURF_MODE_LINEAR_ALIGNED, &linear),
         "the exporter's linear surface was computed");

   {
      const struct radv_shared_layout importer = layout_of(&tiled, 262144);
      const struct radv_shared_layout importer_fixed = layout_of(&linear, 262144);
      /* What the allocation carried in the measured cell: the LB7A record of the shared surface,
       * linear, pitch 1024, size 262144 (sharecell12.out:96 and the D3D12 footprint).
       */
      const struct radv_shared_layout exporter = {
         .is_linear = true,
         .swizzle_mode = 0,
         .pitch_bytes = 1024,
         .surf_size = 262144,
      };
      enum radv_shared_layout_verdict before, after, old;

      printf("INFO importer (OPTIMAL): linear %u swizzle %u pitch %llu size %llu\n",
             (unsigned)importer.is_linear, importer.swizzle_mode, (unsigned long long)importer.pitch_bytes,
             (unsigned long long)importer.surf_size);
      printf("INFO importer after the fix (linear): linear %u swizzle %u pitch %llu size %llu\n",
             (unsigned)importer_fixed.is_linear, importer_fixed.swizzle_mode,
             (unsigned long long)importer_fixed.pitch_bytes, (unsigned long long)importer_fixed.surf_size);
      printf("INFO exporter (LB7A, measured): linear %u swizzle %u pitch %llu size %llu\n",
             (unsigned)exporter.is_linear, exporter.swizzle_mode, (unsigned long long)exporter.pitch_bytes,
             (unsigned long long)exporter.surf_size);

      check(!importer.is_linear && importer.swizzle_mode != 0,
            "the imported image chose a tiled surface, which is the defect");
      check(importer_fixed.is_linear && importer_fixed.pitch_bytes == 1024 &&
               importer_fixed.surf_size == 262144,
            "the linear surface of this description has the measured pitch 1024 and size 262144");

      before = g_decide(&importer, &exporter);
      after = g_decide(&importer_fixed, &exporter);
      old = radv_shared_layout_compare_pre_bd118(&importer, &exporter);
      printf("INFO verdicts: before the fix %s, after the fix %s, pre-BD-118 decision %s\n",
             radv_shared_layout_verdict_name(before), radv_shared_layout_verdict_name(after),
             radv_shared_layout_verdict_name(old));

      check(before == RADV_SHARED_LAYOUT_MODE, "the driver refuses the tiled image over the linear surface");
      check(after == RADV_SHARED_LAYOUT_AGREE, "the driver accepts the image the fix gives it");
      check(old == RADV_SHARED_LAYOUT_AGREE,
            "negative control: the code before BD-118 accepted the mismatch");

      {
         /* The cell's own description must be inside the shape the exporter shares, or the fix would
          * never make its image linear. sharecellvk.cpp/sharecell12.cpp: 256x256, one mip, one
          * layer, one sample, BGRA.
          */
         const struct radv_shared_shape shape = {
            .is_2d = true,
            .is_depth_or_stencil = false,
            .samples = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .width = 256,
            .height = 256,
         };

         check(radv_shared_shape_is_linear(&shape),
               "the arm S cell's description is one the exporter shares linearly, so the fix lays its "
               "image out linearly");
      }

      printf("STEP the size rule with a bind offset\n");
      {
         struct radv_shared_layout bound = importer_fixed;

         bound.memory_offset = 0;
         check(g_decide(&bound, &exporter) == RADV_SHARED_LAYOUT_AGREE,
               "a 262144-byte image bound at offset 0 of a 262144-byte allocation is accepted");
         bound.memory_offset = 4096;
         check(g_decide(&bound, &exporter) == RADV_SHARED_LAYOUT_SIZE,
               "the same image bound at offset 4096 does not fit the allocation and is refused");
      }

      printf("STEP the measured failure, replayed through the two layouts\n");
      for (uint32_t round = 0; round < 4; round++) {
         const struct replay d12 =
            replay_round(hlib, &tiled, false, 1024, 256, 256, 4, round); /* D3D12 writes, RADV reads */
         const struct replay rdv =
            replay_round(hlib, &tiled, true, 1024, 256, 256, 4, round); /* RADV writes, D3D12 reads */
         char what[160];

         check(!d12.addressing_failed && !rdv.addressing_failed, "both directions are addressable");
         printf("INFO round %u: D3D12 to RADV %llu differ, first at %u,%u want %08x have %08x; "
                "RADV to D3D12 %llu differ, first at %u,%u want %08x have %08x\n",
                round, (unsigned long long)d12.mismatches, d12.first_x, d12.first_y, d12.want, d12.have,
                (unsigned long long)rdv.mismatches, rdv.first_x, rdv.first_y, rdv.want, rdv.have);

         snprintf(what, sizeof(what),
                  "round %u, D3D12 to RADV: 65520 differ, first at 4,0, as arm S measured", round);
         check(d12.mismatches == 65520 && d12.first_x == 4 && d12.first_y == 0 &&
                  d12.want == pattern_texel(4, 0, round),
               what);
         snprintf(what, sizeof(what),
                  "round %u, RADV to D3D12: 65520 differ, first at 4,0, as arm S measured", round);
         check(rdv.mismatches == 65520 && rdv.first_x == 4 && rdv.first_y == 0 &&
                  rdv.want == pattern_texel(4, 0, round),
               what);

         if (round == 0) {
            check(d12.want == 0xff34001cu && d12.have == 0xffa000e0u,
                  "round 0, D3D12 to RADV: the exact values of sharecell12.out:191 (want ff34001c "
                  "have ffa000e0)");
            check(rdv.want == 0xff34001cu && rdv.have == 0xff0d0b00u,
                  "round 0, RADV to D3D12: the exact values of sharecellvk.out:189 (want ff34001c "
                  "have ff0d0b00)");
         }
      }

      printf("STEP the same cell with the fix: one layout on both sides\n");
      for (uint32_t round = 0; round < 4; round++) {
         /* With the fix the importer's image is linear, so writer and reader share one layout.
          * Under the negative control the importer keeps the tiled surface, which is the measured
          * failure, and these checks fail.
          */
         const struct replay fixed = g_old_code
                                        ? replay_round(hlib, &tiled, false, 1024, 256, 256, 4, round)
                                        : replay_round(hlib, &linear, false, 1024, 256, 256, 4, round);
         char what[128];

         snprintf(what, sizeof(what), "round %u: every texel arrives where the reader looks for it",
                  round);
         check(!fixed.addressing_failed && fixed.mismatches == 0, what);
      }
   }

   printf("STEP the late import entry: an LB7A record the importing image's shape does not match\n");
   {
      /* R1 of the combined-ICD review. The exporter's record is the arm S one: a 256x256
       * single-sample colour surface, shared linearly. Each image below is a description the
       * exporter can never have shared, paired with that record by a caller that did not have to
       * match it. radv_choose_tiling leaves every one of them tiled, so each arrives at the late
       * entry with a MODE disagreement and, without the full shape rule there, drives the relayout.
       */
      static const struct {
         const char *what;
         struct radv_shared_shape shape;
         bool depth_flag;
      } cases[] = {
         {"a multisample image (4 samples)",
          {.is_2d = true, .samples = 4, .mip_levels = 1, .array_layers = 1, .width = 256, .height = 256}},
         {"a mip chain (2 mips)",
          {.is_2d = true, .samples = 1, .mip_levels = 2, .array_layers = 1, .width = 256, .height = 256}},
         {"an array (2 layers)",
          {.is_2d = true, .samples = 1, .mip_levels = 1, .array_layers = 2, .width = 256, .height = 256}},
         {"a 3D image",
          {.is_2d = false, .samples = 1, .mip_levels = 1, .array_layers = 1, .width = 256, .height = 256}},
         {"an edge beyond the contract (16384 wide)",
          {.is_2d = true, .samples = 1, .mip_levels = 1, .array_layers = 1, .width = 16384, .height = 256}},
         {"a depth image",
          {.is_2d = true, .is_depth_or_stencil = true, .samples = 1, .mip_levels = 1, .array_layers = 1,
           .width = 256, .height = 256},
          true},
      };
      unsigned reached_relayout_before_r1 = 0;

      for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
         char what[192];

         snprintf(what, sizeof(what),
                  "the late entry refuses %s against a single-sample linear record", cases[i].what);
         check(late_entry_refuses(&cases[i].shape, cases[i].depth_flag), what);
         if (!late_entry_refuses_pre_r1(&cases[i].shape, cases[i].depth_flag))
            reached_relayout_before_r1++;
      }
      printf("INFO late entry: %u of %u of these descriptions passed the entry's own check before R1 "
             "and reached the relayout; only the depth one was refused there\n",
             reached_relayout_before_r1, (unsigned)ARRAY_SIZE(cases));
      check(reached_relayout_before_r1 == ARRAY_SIZE(cases) - 1,
            "before R1 the late entry refused the depth description alone, so the other ineligible "
            "shapes reached the relayout: the early tiling predicate does not cover this entry");

      /* Why the refusal is necessary and not cosmetic: the relayout those descriptions reached asks
       * addrlib for a linear surface, and addrlib has none for a multisample description. In a
       * release build the two asserts in ac_compute_surface's LINEAR_ALIGNED case are compiled out
       * (NDEBUG), so what comes back is an unwritten surface.
       */
      {
         struct radeon_surf msaa_linear, msaa_tiled;
         const bool linear_ok = compute_surface_samples(addrlib, &info, 256, 256, 1, 1, 4, 4, 4,
                                                        RADEON_SURF_MODE_LINEAR_ALIGNED, &msaa_linear);
         const bool tiled_ok = compute_surface_samples(addrlib, &info, 256, 256, 1, 1, 4, 4, 4,
                                                       RADEON_SURF_MODE_2D, &msaa_tiled);

         printf("INFO late entry: a 4-sample 256x256 description: linear %s, tiled %s (size %" PRIu64
                ")\n",
                linear_ok ? "computed" : "refused by addrlib", tiled_ok ? "computed" : "refused",
                tiled_ok ? (uint64_t)msaa_tiled.total_size : (uint64_t)0);
         check(!linear_ok || msaa_linear.total_size == 0,
               "addrlib has no linear surface for a multisample description, so the relayout the "
               "late entry used to reach could only produce an unwritten surface");
         check(tiled_ok && msaa_tiled.total_size > 0,
               "the same multisample description does have a tiled surface, which is the layout the "
               "refused image keeps");
      }
   }

   printf("STEP the sweep: shared formats, sizes and mip counts, both directions\n");
   {
      unsigned pairs = 0, refused = 0, agreed = 0, flavours_agree = 0;
      unsigned admitted = 0, admitted_depth = 0, admitted_mip = 0, kept_tiled = 0;
      unsigned linear_rows = 0, depth_rows = 0, depth_without_linear = 0, shape_regressions = 0;

      for (unsigned f = 0; f < ARRAY_SIZE(sweep_formats); f++) {
         for (unsigned w = 0; w < ARRAY_SIZE(sweep_sizes); w++) {
            for (unsigned h = 0; h < ARRAY_SIZE(sweep_sizes); h++) {
               for (unsigned m = 0; m < 2; m++) {
                  const unsigned width = sweep_sizes[w];
                  const unsigned height = sweep_sizes[h];
                  const unsigned levels = m ? util_logbase2(MAX2(width, height)) + 1 : 1;
                  const bool depth = sweep_formats[f].is_depth;
                  const struct radv_shared_shape shape = {
                     .is_2d = true,
                     .is_depth_or_stencil = depth,
                     .samples = 1,
                     .mip_levels = levels,
                     .array_layers = 1,
                     .width = width,
                     .height = height,
                  };
                  const bool admit = g_shape_decide(&shape);
                  struct radeon_surf t, l, engine;
                  struct radv_shared_layout exporter, importer, importer_fixed;
                  bool have_linear;

                  /* The importer's own choice for this description, which is what it keeps when the
                   * shape is not one the exporter shares.
                   */
                  if (!compute_surface_z(addrlib, &info, width, height, levels, sweep_formats[f].bpe,
                                         sweep_formats[f].num_channels, RADEON_SURF_MODE_2D, depth,
                                         &t)) {
                     printf("FAIL the tiled surface of %s %ux%u, %u mips could not be computed\n",
                            sweep_formats[f].name, width, height, levels);
                     failures++;
                     checks++;
                     continue;
                  }

                  /* Can this description be a linear surface at all? For a depth or stencil one on
                   * GFX10 it cannot: addrlib refuses ADDR_SW_LINEAR with the Z/S flags, and that is
                   * exactly why the shape rule must exclude depth. The r1 rule did not, which is
                   * what the check below catches under --negative-control.
                   */
                  have_linear =
                     compute_surface_z(addrlib, &info, width, height, levels, sweep_formats[f].bpe,
                                       sweep_formats[f].num_channels, RADEON_SURF_MODE_LINEAR_ALIGNED,
                                       depth, &l);
                  if (depth) {
                     depth_rows++;
                     if (!have_linear)
                        depth_without_linear++;
                  }
                  if (have_linear)
                     linear_rows++;

                  pairs++;
                  if (admit) {
                     admitted++;
                     if (depth)
                        admitted_depth++;
                     if (levels > 1)
                        admitted_mip++;
                  }

                  if (admit && !have_linear) {
                     /* The r1 shape rule reaches this for every depth description. One aggregate
                      * check below counts them, so the control's output stays readable.
                      */
                     if (shape_regressions < 3)
                        printf("INFO %s %ux%u, %u mips is admitted as a shared linear surface but has "
                               "no linear layout on this chip: the image would be left with size 0\n",
                               sweep_formats[f].name, width, height, levels);
                     shape_regressions++;
                     continue;
                  }

                  /* The exporter records what its own linear surface is: the same computation, in
                   * the other process. For a description it can never share (depth, a mip chain),
                   * a record cannot exist; the sweep still asks what the decision does with a
                   * record claiming the obvious linear pitch, and requires a refusal rather than
                   * wrong texels.
                   */
                  if (have_linear) {
                     exporter = layout_of(&l, 0);
                  } else {
                     memset(&exporter, 0, sizeof(exporter));
                     exporter.is_linear = true;
                     exporter.swizzle_mode = 0;
                     exporter.pitch_bytes = (uint64_t)width * sweep_formats[f].bpe;
                     exporter.surf_size = (uint64_t)width * height * sweep_formats[f].bpe;
                  }
                  exporter.memory_size = 0;
                  importer = layout_of(&t, exporter.surf_size);
                  /* With the fix the image is linear only for an admitted shape; otherwise it keeps
                   * the layout it chose.
                   */
                  importer_fixed = admit ? layout_of(&l, exporter.surf_size) : importer;

                  if (g_decide(&importer, &exporter) != RADV_SHARED_LAYOUT_AGREE)
                     refused++;
                  else if (t.is_linear && have_linear &&
                           t.u.gfx9.swizzle_mode == l.u.gfx9.swizzle_mode &&
                           t.u.gfx9.surf_pitch == l.u.gfx9.surf_pitch)
                     refused++; /* the two modes coincide for this shape: counted as settled */

                  if (admit) {
                     if (g_decide(g_old_code ? &importer : &importer_fixed, &exporter) ==
                            RADV_SHARED_LAYOUT_AGREE &&
                         (g_old_code ? importer.is_linear == exporter.is_linear &&
                                          importer.pitch_bytes == exporter.pitch_bytes
                                     : true))
                        agreed++;
                  } else if (g_decide(&importer_fixed, &exporter) != RADV_SHARED_LAYOUT_AGREE ||
                             (importer_fixed.is_linear == exporter.is_linear &&
                              importer_fixed.pitch_bytes == exporter.pitch_bytes)) {
                     /* Either the record is refused, or the image's own layout happens to be the
                      * recorded one (a tiny surface addrlib makes linear anyway). Both are sound.
                      */
                     kept_tiled++;
                  }

                  /* The exporter's record comes from the engine's linear image, which allows
                   * sampling; the importer's image of the same description may not. If the two
                   * flavours ever had different linear layouts, the fix would refuse a pair that is
                   * in fact sound, so the gate watches them, for the shapes that have a linear
                   * layout at all.
                   */
                  if (!have_linear)
                     continue;
                  if (!compute_surface_shaped(addrlib, &info, width, height, levels,
                                              sweep_formats[f].bpe, sweep_formats[f].num_channels,
                                              RADEON_SURF_MODE_LINEAR_ALIGNED, true, depth, &engine)) {
                     printf("FAIL the engine's linear surface of %s %ux%u, %u mips could not be "
                            "computed\n",
                            sweep_formats[f].name, width, height, levels);
                     failures++;
                     checks++;
                     continue;
                  }
                  if (engine.is_linear == l.is_linear &&
                      engine.u.gfx9.swizzle_mode == l.u.gfx9.swizzle_mode &&
                      engine.u.gfx9.surf_pitch == l.u.gfx9.surf_pitch &&
                      engine.total_size == l.total_size)
                     flavours_agree++;
                  else
                     printf("INFO flavours differ for %s %ux%u, %u mips: engine pitch %u size %llu, "
                            "client pitch %u size %llu\n",
                            sweep_formats[f].name, width, height, levels, engine.u.gfx9.surf_pitch,
                            (unsigned long long)engine.total_size, l.u.gfx9.surf_pitch,
                            (unsigned long long)l.total_size);
               }
            }
         }
      }

      printf("INFO sweep: %u descriptions, %u admitted as shared linear surfaces, %u settled before "
             "the fix, %u agree after it, %u keep their own layout and refuse\n",
             pairs, admitted, refused, agreed, kept_tiled);
      check(pairs == ARRAY_SIZE(sweep_formats) * ARRAY_SIZE(sweep_sizes) * ARRAY_SIZE(sweep_sizes) * 2,
            "every description of the sweep was computed");
      check(admitted > 0 && admitted_depth == 0 && admitted_mip == 0,
            "only single-mip colour descriptions are admitted as shared linear surfaces: no depth or "
            "stencil, no mip chain");
      printf("INFO sweep: %u admitted descriptions have no linear layout on this chip (depth %u, mip "
             "chains %u of the admitted set)\n",
             shape_regressions, admitted_depth, admitted_mip);
      check(shape_regressions == 0,
            "every admitted description has a linear layout, so the fix never leaves an image with a "
            "surface addrlib refused to compute");
      check(refused == pairs,
            "before the fix every description either disagrees and is refused, or its two modes "
            "coincide");
      check(agreed == admitted, "after the fix every admitted description of the sweep agrees");
      check(kept_tiled == pairs - admitted,
            "a description the exporter cannot share linearly keeps its own layout and its import is "
            "refused, never silently accepted");
      printf("INFO sweep: %u of %u descriptions with a linear layout have one layout for both usage "
             "flavours; %u of %u depth descriptions have no linear layout on this chip\n",
             flavours_agree, linear_rows, depth_without_linear, depth_rows);
      check(linear_rows > 0 && flavours_agree == linear_rows,
            "the engine's linear image and the importer's have the same linear layout, so the "
            "exporter's recorded pitch is the one the fixed importer computes");
      check(depth_rows > 0 && depth_without_linear == depth_rows,
            "addrlib refuses a linear surface for every depth description, which is why the shape "
            "rule excludes depth and stencil");
   }

   ac_addrlib_destroy(addrlib);

done:
   printf("bc250_shared_layout_test: %u checks, %u failed\n", checks, failures);
   if (negative_control) {
      /* The control is the gate run over the old driver, and it must fail. If it ever passed, the
       * gate would not discriminate the fix from the defect and the whole run means nothing: that
       * is reported as a failure of the control itself, with exit 2.
       */
      if (failures) {
         printf("NEGATIVE CONTROL: the gate fails on the code before BD-118, as it must (%u of %u "
                "checks)\n",
                failures, checks);
         return 1;
      }
      printf("NEGATIVE CONTROL BROKEN: the gate passes on the code before BD-118, so it does not "
             "test the fix\n");
      return 2;
   }
   return failures ? 1 : 0;
}
