/* SPDX-License-Identifier: MIT
 * Private hosted-driver contract for D3D target-independent rasterization.
 * This is not a Vulkan extension and is used only after the named query succeeds.
 */
#ifndef RADV_TIR_H
#define RADV_TIR_H

#define BC250_TIR_QUERY_NAME "bc250GetTargetIndependentRasterizationContract"
#define BC250_TIR_REVISION 1u
#define BC250_STRUCTURE_TYPE_TIR ((VkStructureType)0x7fbc2503)

struct bc250_tir_pipeline {
   VkStructureType sType;
   const void *pNext;
   uint32_t revision;
   uint32_t forced_samples;
   uint32_t output_samples;
   uint32_t output_mask;
};

typedef VkResult (VKAPI_PTR *PFN_bc250_tir_query)(VkPhysicalDevice physical_device,
                                               uint32_t revision, uint32_t structure_size);

static inline bool
bc250_tir_valid(const struct bc250_tir_pipeline *tir)
{
   if (!tir || tir->revision != BC250_TIR_REVISION ||
       (tir->forced_samples != 1 && tir->forced_samples != 4 &&
        tir->forced_samples != 8 && tir->forced_samples != 16))
      return false;
   if (tir->output_samples != 1 && tir->output_samples != 2 &&
       tir->output_samples != 4 && tir->output_samples != 8)
      return false;
   return tir->forced_samples == 1 || tir->output_samples == 1;
}

/* Center samples are replicated for the forced-one broadcast path. */
static inline unsigned
bc250_tir_raster_samples(unsigned forced, unsigned output)
{
   return forced == 1 ? output : forced;
}

#endif
