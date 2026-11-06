/* SPDX-License-Identifier: MIT
 * Private hosted-driver contract (BD-309): the kernel allocation behind a VkDeviceMemory.
 *
 * This is not a Vulkan extension. The D3D11 shell of this driver offers and reclaims the
 * allocations of resources it does not create through the runtime (DXGI OfferResources,
 * ReclaimResources). It gets the allocation handle of such a resource's memory from this query.
 * The shell keeps its own copy of these declarations; the name, the revision and the layout
 * are the contract.
 *
 * bc250GetMemoryAllocation(device, memory, out):
 *   VK_SUCCESS                      out->allocation is the one D3DKMT allocation handle of the
 *                                   memory. The ICD created that allocation itself, on the device
 *                                   of the host (the D3D runtime device), so the runtime's offer
 *                                   and reclaim callbacks accept it. out->bytes is its size.
 *                                   out->flags has BC250_MEMORY_ALLOCATION_CPU_MAPPED while the
 *                                   memory is mapped (the allocation is CPU-locked): unmap it
 *                                   before an offer.
 *   VK_ERROR_FEATURE_NOT_PRESENT    the memory is not such an allocation: an import (runtime
 *                                   surface, host pointer, opened handle), a sparse reservation,
 *                                   or a winsys without kernel allocations. The caller treats an
 *                                   offer of it as a hint.
 *   VK_ERROR_INITIALIZATION_FAILED  a NULL argument, or out->revision or out->size other than
 *                                   BC250_MEMORY_ALLOCATION_REVISION and sizeof(*out).
 * The query reads state only. It makes no kernel call and takes no lock that a submission holds.
 */
#ifndef RADV_BC250_MEMORY_H
#define RADV_BC250_MEMORY_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define BC250_MEMORY_ALLOCATION_QUERY_NAME "bc250GetMemoryAllocation"
#define BC250_MEMORY_ALLOCATION_REVISION 1u
#define BC250_MEMORY_ALLOCATION_CPU_MAPPED 0x1u

struct bc250_memory_allocation {
   uint32_t revision;   /* in: BC250_MEMORY_ALLOCATION_REVISION */
   uint32_t size;       /* in: sizeof(struct bc250_memory_allocation) */
   uint32_t allocation; /* out: D3DKMT_HANDLE */
   uint32_t flags;      /* out: BC250_MEMORY_ALLOCATION_* */
   uint64_t bytes;      /* out: the allocation's size */
};

typedef VkResult (VKAPI_PTR *PFN_bc250_get_memory_allocation)(VkDevice device, VkDeviceMemory memory,
                                                              struct bc250_memory_allocation *out);

#endif
