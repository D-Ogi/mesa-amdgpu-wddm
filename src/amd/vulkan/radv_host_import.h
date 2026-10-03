/* SPDX-License-Identifier: MIT
 *
 * What vkAllocateMemory accepts around a hosted import (bc250_host_import, util/bc250_host_bootstrap.h).
 *
 * A hosted import borrows an allocation the embedding runtime owns, at the VA it mapped. Nothing else may
 * describe the same memory, so the import is refused (VK_ERROR_INVALID_EXTERNAL_HANDLE) when the chain
 * carries (BD-039):
 * - a second host import block (either sType),
 * - another import with a handle type (Win32, fd, host pointer, Android hardware buffer),
 * - an export with a handle type: the memory is the host's, the ICD has nothing to share,
 * - an opaque capture address other than the import's own VA: the VA is the host's, never chosen here.
 * A host-visible memoryTypeIndex is accepted: the D3D12 shell imports CPU-visible heaps with
 * BC250_HOST_IMPORT_CPU_MAP, and on this APU device-local types may be host-visible too.
 * The memory keeps import_handle_type 0 (the device memory report says ALLOCATE): the host import is not
 * a Vulkan handle type.
 *
 * In hosted mode (the instance carries the host), a Win32 memory import is refused as well, with or
 * without a host import (BD-038): the host does not open NT handles, and the physical device reports no
 * Win32 handle type as importable or exportable there.
 *
 * Header-only, so that the winsys host tests check the same rules as radv_alloc_memory.
 */
#ifndef RADV_HOST_IMPORT_H
#define RADV_HOST_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "util/bc250_host_bootstrap.h"

struct radv_host_import_request {
   const struct bc250_host_import *import; /* NULL: no host import in the chain */
   uint32_t flags;                         /* BC250_HOST_IMPORT_* bits, 0 under BC250_HOST_IMPORT_STYPE */
   const char *refused;                    /* why the chain was refused, NULL when it was accepted */
};

static inline VkResult
radv_host_import_refuse(struct radv_host_import_request *out, const char *why)
{
   out->import = NULL;
   out->flags = 0;
   out->refused = why;
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static inline VkResult
radv_host_import_parse(const VkMemoryAllocateInfo *info, bool hosted, struct radv_host_import_request *out)
{
   const struct bc250_host_import *import = NULL;
   unsigned import_blocks = 0;
   uint32_t flags = 0;
   bool win32_import = false, other_import = false;
   VkExternalMemoryHandleTypeFlags exports = 0;
   uint64_t capture_address = 0;

   for (const VkBaseInStructure *ext = info->pNext; ext; ext = ext->pNext) {
      switch ((uint32_t)ext->sType) {
      case BC250_HOST_IMPORT_STYPE:
         import = (const struct bc250_host_import *)ext;
         flags = 0; /* no flags field under this type */
         import_blocks++;
         break;
      case BC250_HOST_IMPORT_FLAGS_STYPE:
         import = (const struct bc250_host_import *)ext;
         flags = import->flags;
         import_blocks++;
         break;
#ifdef VK_USE_PLATFORM_WIN32_KHR
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
         if (((const VkImportMemoryWin32HandleInfoKHR *)ext)->handleType)
            win32_import = true;
         break;
#endif
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         if (((const VkImportMemoryFdInfoKHR *)ext)->handleType)
            other_import = true;
         break;
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
         if (((const VkImportMemoryHostPointerInfoEXT *)ext)->handleType)
            other_import = true;
         break;
      case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
         other_import = true;
         break;
      case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
         exports |= ((const VkExportMemoryAllocateInfo *)ext)->handleTypes;
         break;
      case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO:
         capture_address = ((const VkMemoryOpaqueCaptureAddressAllocateInfo *)ext)->opaqueCaptureAddress;
         break;
      default:
         break;
      }
   }

   if (hosted && win32_import)
      return radv_host_import_refuse(out, "hosted mode: Win32 memory import is not supported");
   if (import) {
      if (!hosted)
         return radv_host_import_refuse(out, "host import without a host");
      if (import_blocks > 1)
         return radv_host_import_refuse(out, "more than one host import block");
      if (win32_import || other_import)
         return radv_host_import_refuse(out, "host import combined with another import");
      if (exports)
         return radv_host_import_refuse(out, "host import combined with an export");
      if (capture_address && capture_address != import->va)
         return radv_host_import_refuse(out, "host import with an opaque capture address other than its VA");
      if (import->size < info->allocationSize)
         return radv_host_import_refuse(out, "host import smaller than allocationSize");
   }
   out->import = import;
   out->flags = flags;
   out->refused = NULL;
   return VK_SUCCESS;
}

#endif /* RADV_HOST_IMPORT_H */
