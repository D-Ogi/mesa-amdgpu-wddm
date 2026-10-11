/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/* BD-118: the layout agreement of a cross-API shared surface.
 *
 * A D3D12 or D3D11 resource that this driver shares is a LINEAR surface with a recorded pitch and
 * size (driver/contract/bc250_shared_surface.h, BD-075; the KMD publishes it per allocation as the
 * LB7A v1 record, which radv_wddm2_bo_from_handle copies into radeon_bo_metadata). A Vulkan image
 * created over that memory computes its own layout from VkImageCreateInfo alone, and
 * VK_IMAGE_TILING_OPTIMAL gives a tiled surface. The two layouts then describe the same bytes at
 * different addresses, and every texel but the first few arrives in the wrong place (BD-118, arm S
 * of the dxgi-r1 round: 65520 of 65536 texels differ, first at (4,0)).
 *
 * This header holds the comparison and the shape rule alone: no radv, no Vulkan and no winsys
 * types, so the host gate (src/amd/common/tools/bc250_shared_layout_test.c) decides with the same
 * code that the driver decides and refuses with.
 */
#ifndef RADV_SHARED_LAYOUT_H
#define RADV_SHARED_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* One side's view of the shared surface. The importer fills it from its radeon_surf, the exporter
 * side from the LB7A record the import carried in.
 */
struct radv_shared_layout {
   bool is_linear;
   unsigned swizzle_mode;
   uint64_t pitch_bytes;
   uint64_t surf_size;     /* what the side needs: image->size, or the exporter's recorded Size */
   uint64_t memory_size;   /* the opened allocation, importer side only; 0 = not known */
   uint64_t memory_offset; /* where in that allocation the image is bound, importer side only */
};

/* The widest edge a shared linear surface of this contract may have. Both ends of the channel bound
 * it at 8192: the exporter's admission (bc250-win driver/umd/d3d12/engine-ddi/resources.cpp,
 * linear_surface_shape, kLinearMaxEdge) and the LB7A record's own validity rule
 * (winsys/wddm2/radv_wddm2_wsi_route.h, radv_wddm2_lb7a_valid).
 */
#define RADV_SHARED_SURFACE_MAX_EDGE 8192

/* The description of an image, as the shape rule below sees it. */
struct radv_shared_shape {
   bool is_2d;
   bool is_depth_or_stencil;
   unsigned samples;
   unsigned mip_levels;
   unsigned array_layers;
   unsigned width;
   unsigned height;
};

/* May an image of this description be the Vulkan half of a shared linear surface?
 *
 * This is the exporter's own admission, mirrored: our D3D12 shell shares a linear surface only for a
 * 2D, single-sampled, single-mip, single-slice colour texture of an admitted format whose edges are
 * inside RADV_SHARED_SURFACE_MAX_EDGE (resources.cpp, linear_surface_shape), and LB7A carries
 * nothing else (radv_wddm2_lb7a_valid). An image outside that shape can never alias a surface this
 * driver shares, so forcing it linear would only buy a worse layout - and for a depth or stencil
 * image a linear surface does not exist at all on GFX10: ac_compute_surface asks addrlib for
 * ADDR_SW_LINEAR with the Z/S flags set (ac_surface.c, the LINEAR_ALIGNED case), gfx10 addrlib
 * refuses that combination in ValidateSwModeParams, and in a release build (NDEBUG) the two asserts
 * there are gone, so the surface would come back unwritten with size 0.
 */
static inline bool
radv_shared_shape_is_linear(const struct radv_shared_shape *shape)
{
   if (!shape->is_2d || shape->is_depth_or_stencil)
      return false;
   if (shape->samples > 1 || shape->mip_levels != 1 || shape->array_layers != 1)
      return false;
   if (!shape->width || !shape->height || shape->width > RADV_SHARED_SURFACE_MAX_EDGE ||
       shape->height > RADV_SHARED_SURFACE_MAX_EDGE)
      return false;
   return true;
}

/* The shape rule of the first BD-118 round (r1), kept for the host gate's negative control. It asked
 * only for a 2D single-sampled image, so it also forced a linear surface for a depth or stencil
 * image, for a mip chain, for an array and for an edge no shared surface can have. The gate runs the
 * sweep with this rule under --negative-control and must fail with it.
 */
static inline bool
radv_shared_shape_is_linear_r1(const struct radv_shared_shape *shape)
{
   return shape->is_2d && shape->samples <= 1;
}

enum radv_shared_layout_verdict {
   RADV_SHARED_LAYOUT_AGREE = 0,
   RADV_SHARED_LAYOUT_NO_METADATA, /* the exporter recorded nothing: no opinion, not a refusal */
   RADV_SHARED_LAYOUT_MODE,        /* one side tiled, the other linear */
   RADV_SHARED_LAYOUT_SWIZZLE,     /* both tiled, different swizzle modes */
   RADV_SHARED_LAYOUT_PITCH,       /* same mode, different row pitch */
   RADV_SHARED_LAYOUT_SIZE,        /* the importer needs more bytes than the allocation holds */
};

static inline const char *
radv_shared_layout_verdict_name(enum radv_shared_layout_verdict verdict)
{
   switch (verdict) {
   case RADV_SHARED_LAYOUT_AGREE:
      return "agree";
   case RADV_SHARED_LAYOUT_NO_METADATA:
      return "no-metadata";
   case RADV_SHARED_LAYOUT_MODE:
      return "mode";
   case RADV_SHARED_LAYOUT_SWIZZLE:
      return "swizzle";
   case RADV_SHARED_LAYOUT_PITCH:
      return "pitch";
   case RADV_SHARED_LAYOUT_SIZE:
      return "size";
   default:
      return "unknown";
   }
}

/* Do the two sides address the same texel at the same byte?
 *
 * The order of the tests is the order of the diagnosis: the mode decides the whole address
 * equation, the swizzle mode decides it inside a tile, the pitch decides the row stride, and the
 * size says whether the last row is inside the allocation at all.
 */
static inline enum radv_shared_layout_verdict
radv_shared_layout_compare(const struct radv_shared_layout *importer,
                           const struct radv_shared_layout *exporter)
{
   if (!exporter->pitch_bytes)
      return RADV_SHARED_LAYOUT_NO_METADATA;

   if (importer->is_linear != exporter->is_linear)
      return RADV_SHARED_LAYOUT_MODE;

   if (!importer->is_linear && importer->swizzle_mode != exporter->swizzle_mode)
      return RADV_SHARED_LAYOUT_SWIZZLE;

   if (importer->pitch_bytes != exporter->pitch_bytes)
      return RADV_SHARED_LAYOUT_PITCH;

   /* The bound that matters is the allocation, not the recorded Size: the opened allocation is
    * what the GPU may touch (the KMD rounds the record's Size up to whole pages, and
    * radv_wddm2_bo_from_handle keeps that rounded size), while the record's Size is reported for
    * the log alone. A rule against the record's Size would refuse a swapchain image that the WSI
    * DXGI route presents correctly today (BD-105 dxgi-r1 arm A2).
    *
    * The image starts at memory_offset inside that allocation (vkBindImageMemory2's memoryOffset),
    * so the bound is offset + size: this is the only size check an imported allocation gets, since
    * radv_BindImageMemory2's own one needs mem->alloc_size, which an import never sets.
    */
   if (importer->memory_size &&
       importer->memory_offset + importer->surf_size > importer->memory_size)
      return RADV_SHARED_LAYOUT_SIZE;

   return RADV_SHARED_LAYOUT_AGREE;
}

/* What the driver did before BD-118: the import applied no layout and compared nothing, so every
 * pair was accepted. The host gate's negative control calls this one and must fail with it.
 */
static inline enum radv_shared_layout_verdict
radv_shared_layout_compare_pre_bd118(const struct radv_shared_layout *importer,
                                     const struct radv_shared_layout *exporter)
{
   (void)importer;
   (void)exporter;
   return RADV_SHARED_LAYOUT_AGREE;
}

#endif /* RADV_SHARED_LAYOUT_H */
