/* SPDX-License-Identifier: MIT
 * Private hosted PGP stream-output contract. Query before supplying this pNext.
 */
#ifndef RADV_PGP_SO_H
#define RADV_PGP_SO_H

#define BC250_PGP_SO_QUERY_NAME "bc250GetPartialStreamOutputContract"
#define BC250_PGP_SO_REVISION 4u
#define BC250_STRUCTURE_TYPE_PGP_SO ((VkStructureType)0x7fbc2504)
#include "ac_pgp_so.h"

struct bc250_pgp_so_pipeline {
   VkStructureType sType;
   const void *pNext;
   uint32_t revision;
   uint32_t mode;
   /* NULL for CAPTURE. LINK deep-copies this immutable, validated table. */
   const struct bc250_pgp_so_table *table;
};

typedef VkResult (VKAPI_PTR *PFN_bc250_pgp_so_query)(VkPhysicalDevice physical_device,
   uint32_t revision, uint32_t structure_size, uint32_t table_size);

#endif
