/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2022 Collabora, Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Windows headers conflict with Wayland and XLib headers */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "radv_wddm2_bo.h"
#include "radv_wddm2_bc250.h"
#include "radv_wddm2_cs.h"
#include "util/u_memory.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "wsl/winadapter.h"
#endif
#include "d3dkmthk.h"

static const bool all_resident = true;

static struct util_vma_heap *
radv_wddm2_bo_heap(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags)
{
   if (flags & RADEON_FLAG_32BIT)
      return &ws->_32bit_heap;
   else if (flags & RADEON_FLAG_REPLAYABLE)
      return &ws->replay_heap;
   else
      return &ws->heap;
}

static uint64_t
radv_wddm2_bo_va_alloc(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags,
                       uint64_t size, uint32_t alignment)
{
   struct util_vma_heap *heap = radv_wddm2_bo_heap(ws, flags);

   simple_mtx_lock(&ws->heap_mtx);
   uint64_t addr = util_vma_heap_alloc(heap, size, alignment);
   simple_mtx_unlock(&ws->heap_mtx);

   return addr;
}

static void
radv_wddm2_bo_va_free(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags,
                      uint64_t addr, uint64_t size)
{
   struct util_vma_heap *heap = radv_wddm2_bo_heap(ws, flags);

   simple_mtx_lock(&ws->heap_mtx);
   util_vma_heap_free(heap, addr, size);
   simple_mtx_unlock(&ws->heap_mtx);
}

#pragma pack(push, 4)
struct create_alloc_pdata {
   uint32_t adapter_id;
   uint32_t _dw1;
   uint32_t flags; // 0x80
   uint32_t checksum;
   uint32_t reserved[11];
   uint32_t pdata_size;
};

struct alloc_header {
   uint32_t entry_size[4];
   uint32_t adapter_id;
   uint32_t checksum;
   uint32_t reserved[9];
   uint32_t num_entries;
};

struct alloc_bo_info {
   uint32_t section_size;
   uint32_t _dw1;
   uint32_t flags;
   uint32_t size;
   uint32_t alignment;
   uint32_t priority;
   uint8_t heaps[4];
   uint32_t _unknown[16];
   uint32_t flags2;
   uint64_t phys_size;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t reserved[2];
};

struct alloc_surf {
   uint32_t section_size;
   uint32_t flags;
   uint32_t swizzle_mode; // linear = 0x20
   uint32_t resource_type;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t width_in_texels;
   uint32_t height_in_texels;
   uint32_t depth;
   uint32_t slice_size;
   uint32_t _unknown0[16];
   uint32_t width2;
   uint32_t height2;
   uint32_t depth2;
   uint32_t size;
   uint32_t _unknown1[41];
};

struct alloc_metadata  {
   uint32_t section_size;
   uint32_t _pad;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t _unknown0[45];
   uint32_t mtype;
   uint32_t flags;
   uint32_t mall_policy;
   uint32_t mall_range[2];
   uint32_t _unknown2[13];
};

struct alloc_entry {
   uint32_t entry_size;
   uint32_t enabled_sections;
   struct alloc_bo_info bo_info;
   uint32_t num_planes;
   uint32_t _unknown0[2];
   uint16_t samples;
   uint16_t mask;
   uint32_t mip_levels;
   uint32_t layers;
   uint32_t _unknown1[4];
   uint32_t metadata_offset;
   uint32_t _unknown2[2];
   uint32_t version;
   uint32_t _unknown3;
};
#pragma pack(pop)

static inline uint32_t
calculate_checksum(const uint32_t *data, size_t dword_count)
{
   uint32_t acc[8] = {0};

   for (size_t i = 0; i < dword_count; i++)
      acc[i % 8] += i ^ data[i];
   return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

enum alloc_entry_section {
   ALLOC_SECTION_METADATA = 0x1,
   ALLOC_SECTION_SURF = 0xc,
};

enum alloc_flags {
   ALLOC_FLAG_NOT_VIRTUAL = 0x2000,
   ALLOC_FLAG_UDMA_BUFFER = 0x40000,
   ALLOC_FLAG_HOST_ALLOCATED = 0x4000000,
   ALLOC_FLAG_CPU_VISIBLE = 0x20000000,
   ALLOC_FLAG_SHARED = 0x80000000,
};

enum alloc_heap {
   ALLOC_HEAP_LOCAL = 0,
   ALLOC_HEAP_INVISIBLE = 1,
   ALLOC_HEAP_GART_USWC = 2,
   ALLOC_HEAP_GART_CACHEABLE = 3,
};

#define ADD_HEAP(heap) \
   do { \
      bo_info->flags |= 1 << ALLOC_HEAP_##heap; \
      bo_info->heaps[heap_count++] = ALLOC_HEAP_##heap + 1; \
   } while (0)

static void
fill_alloc_heaps(struct radv_wddm2_winsys *ws, enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 bool host_allocated, struct alloc_bo_info *bo_info)
{
   uint32_t heap_count = 0;

   if (initial_domain & RADEON_DOMAIN_VRAM) {
      assert(!host_allocated);
      if (!(flags & RADEON_FLAG_CPU_ACCESS))
         ADD_HEAP(INVISIBLE);
      if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
         ADD_HEAP(LOCAL);
   }

   if (initial_domain & RADEON_DOMAIN_GTT || initial_domain == 0) {
      if (!(flags & RADEON_FLAG_GTT_WC))
         ADD_HEAP(GART_CACHEABLE);
      if (!host_allocated)
         ADD_HEAP(GART_USWC);
   }
}

#define ADD_SECTION(section) \
   do { \
      section = (struct alloc_##section *)pdata; \
      pdata += sizeof(struct alloc_##section); \
      entry_size += sizeof(struct alloc_##section); \
   } while (0)

#define ADD_OPT_SECTION(section, type) \
   do { \
      entry->enabled_sections |= ALLOC_SECTION_##type; \
      ADD_SECTION(section); \
      section->section_size = sizeof(struct alloc_##section); \
   } while (0)

static uint32_t
fill_alloc_pdata(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment,
                 enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 unsigned priority, uint64_t address, void *cpu_ptr, uint8_t *pdata)
{
   struct alloc_header *header = (struct alloc_header *)pdata;
   struct alloc_entry *entry;
   struct alloc_surf *surf;
   struct alloc_metadata *metadata;
   uint32_t entry_size = 0;

   pdata += sizeof(struct alloc_header);
   ADD_SECTION(entry);

   // BO info
   entry->bo_info.section_size = sizeof(struct alloc_bo_info);
   entry->bo_info.flags = (flags & RADEON_FLAG_VIRTUAL) ? 0 : ALLOC_FLAG_NOT_VIRTUAL;
   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      entry->bo_info.flags |= ALLOC_FLAG_SHARED;
   if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
      entry->bo_info.flags |= ALLOC_FLAG_CPU_VISIBLE;
   if (cpu_ptr)
      entry->bo_info.flags |= ALLOC_FLAG_HOST_ALLOCATED;
   entry->bo_info.size = size;
   entry->bo_info.alignment = alignment;
   entry->bo_info.priority = 0x5;
   if (cpu_ptr)
      entry->bo_info.priority |= 0x800;
   entry->bo_info.phys_size = size;
   entry->bo_info.va_size = size;
   entry->bo_info.va_addr = address;
   entry->bo_info.flags2 = 0x1000000;
   fill_alloc_heaps(ws, initial_domain, flags, cpu_ptr != NULL, &entry->bo_info);

   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING)) {
      entry->num_planes = 1;
      entry->mip_levels = 1;
      entry->layers = 1;
      entry->samples = 1;

      ADD_OPT_SECTION(surf, SURF);
      surf->flags = 0x20000; // typed?
      surf->swizzle_mode = 0x20; // linear
      surf->resource_type = 1; // 1D
      surf->width = size;
      surf->height = 1;
      surf->width_in_texels = size;
      surf->height_in_texels = 1;
      surf->depth = 1;
      surf->slice_size = size;
      surf->width2 = size;
      surf->height2 = 1;
      surf->depth2 = 1;
      surf->size = size;
   }

   // Metadata
   entry->metadata_offset = entry_size;
   ADD_OPT_SECTION(metadata, METADATA);
   metadata->va_addr = address;
   metadata->va_size = size;
   if (flags & RADEON_FLAG_NO_CPU_ACCESS)
      metadata->flags |= 0x2000; // no CPU access
   if (flags & RADEON_FLAG_GL2_BYPASS) {
      metadata->mtype = 0x4; // L2_UNCACHED;
      metadata->mall_policy = 0x1; // never
   }

   entry->version = 0x9;
   entry->entry_size = entry_size;

   header->entry_size[0] = entry_size;
   header->adapter_id = 0;
   header->num_entries = 1;
   header->checksum = calculate_checksum((const uint32_t *)header, sizeof(*header) / 4);

   return entry_size + sizeof(*header);
}

static uint64_t
radv_wddm2_get_optimal_vm_alignment(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment)
{
   uint64_t vm_alignment = alignment;

   /* Increase the VM alignment for faster address translation. */
   if (size >= ws->gpu_info.pte_fragment_size)
      vm_alignment = MAX2(vm_alignment, ws->gpu_info.pte_fragment_size);

   /* Gfx9: Increase the VM alignment to the most significant bit set
    * in the size for faster address translation.
    */
   if (ws->gpu_info.gfx_level >= GFX9) {
      unsigned msb = util_last_bit64(size); /* 0 = no bit is set */
      uint64_t msb_alignment = msb ? 1ull << (msb - 1) : 0;

      vm_alignment = MAX2(vm_alignment, msb_alignment);
   }
   return vm_alignment;
}

static uint64_t
radv_wddm2_reserve_va_range(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo,
                           uint64_t size, unsigned alignment, enum radeon_bo_flag flags,
                           uint64_t address)
{
   /* WDDM reservations use 64 KiB granularity. Preserve the whole reservation
    * when a larger resource alignment requires an aligned interior address.
    * The OS owns VA placement; its reservation also excludes ordinary BO maps.
    * Local WDK26100: D3DDDI_RESERVEGPUVIRTUALADDRESS / FreeGpuVirtualAddress.
    */
   const unsigned granularity = 65536;
   uint64_t min, max;
   NTSTATUS status;

   alignment = MAX2(alignment, granularity);
   /* Vulkan sparse-binding images can occupy fewer than64KiB. WDDM rounds
    * the reservation, not the resource or its4KiB mapping/binding granularity. */
   if (!size || (size & 4095) || size > UINT64_MAX - (granularity - 1) ||
       (alignment & (alignment - 1)) || (address & (alignment - 1)))
      return 0;
   const uint64_t reserved_size = align64(size, granularity);
   if (reserved_size > UINT64_MAX - (alignment - granularity))
      return 0;

   if (flags & RADEON_FLAG_32BIT) {
      min = RADV_WDDM2_32BIT_HEAP_START;
      max = RADV_WDDM2_HEAP_START;
   } else if (flags & RADEON_FLAG_REPLAYABLE) {
      min = RADV_WDDM2_REPLAY_HEAP_START;
      max = min + (4ull << 32);
   } else {
      min = RADV_WDDM2_HEAP_START;
      max = RADV_WDDM2_REPLAY_HEAP_START;
   }

   if (address && (address < min || address >= max || reserved_size > max - address))
      return 0;

   D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {
      /* The current D3DKMT contract uses hAdapter, not obsolete hPagingQueue. */
      .hAdapter = ws->adapter_h,
      .BaseAddress = address,
      .MinimumAddress = min,
      .MaximumAddress = max,
      .Size = reserved_size + (address ? 0 : alignment - granularity),
   };
   status = BC250_WDDM_CALL(&ws->host, ReserveGpuVirtualAddress, &reserve);
   if (!NT_SUCCESS(status))
      return 0;

   const uint64_t va = align64(reserve.VirtualAddress, alignment);
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = va,
      .SizeInPages = size / 4096,
      .Protection = {
         .Zero = 1,
      },
   };
   status = BC250_WDDM_CALL(&ws->host, MapGpuVirtualAddress, &map);
   if (NT_SUCCESS(status)) {
      /* Initial zero mapping is asynchronous, just like physical BO mapping.
       * Do not return the BO before that paging operation has retired. */
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
         .hDevice = ws->device_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &ws->paging_fence_h,
         .FenceValueArray = &map.PagingFenceValue,
      };
      status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   }
   if (!NT_SUCCESS(status)) {
      const D3DKMT_FREEGPUVIRTUALADDRESS release = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = reserve.VirtualAddress,
         .Size = reserve.Size,
      };
      BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &release);
      return 0;
   }

   bo->reserved_va = reserve.VirtualAddress;
   bo->reserved_size = reserve.Size;
   return va;
}

/* Same two-view policy as radv_amdgpu_bo.c, using WDDM-owned reservations.
 * The compiler clears the control bit for scalar loads; vector loads/stores keep
 * the high PRT view. Low unbound ranges reference an actual read-only zero BO.
 */
static VkResult
radv_wddm2_init_null_prt_bo(struct radv_wddm2_winsys *ws)
{
   VkResult result = VK_SUCCESS;
   simple_mtx_lock(&ws->null_prt.lock);
   if (!ws->null_prt.bo) {
      struct radeon_winsys_bo *bo = NULL;
      result = ws->base.buffer_create(&ws->base, 8 * 1024 * 1024, 65536, RADEON_DOMAIN_VRAM,
                                      RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_READ_ONLY |
                                         RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_PREFER_LOCAL_BO |
                                         RADEON_FLAG_INTERNAL,
                                      RADV_BO_PRIORITY_VIRTUAL, 0, NULL, &bo);
      if (result == VK_SUCCESS) {
         /* BC2A does not implement ZERO_VRAM: initialise explicitly before
          * publishing the shared object. Drain WC CPU stores before unlocking.
          */
         void *map = ws->base.buffer_map(&ws->base, bo, false, NULL);
         if (map) {
            memset(map, 0, bo->size);
            MemoryBarrier();
            ws->base.buffer_unmap(&ws->base, bo, false);
            ws->null_prt.bo = bo;
         } else {
            ws->base.buffer_destroy(&ws->base, bo);
            result = VK_ERROR_MEMORY_MAP_FAILED;
         }
      }
   }
   simple_mtx_unlock(&ws->null_prt.lock);
   return result;
}

static bool
radv_wddm2_init_sparse_alias(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo)
{
   const uint64_t low = bo->base.va;
   const uint64_t high = low | RADV_WDDM2_PRT_CONTROL_MASK;
   D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {
      .hAdapter = ws->adapter_h,
      .BaseAddress = high,
      .Size = align64(bo->base.size, 65536),
   };
   NTSTATUS status = BC250_WDDM_CALL(&ws->host, ReserveGpuVirtualAddress, &reserve);
   if (!NT_SUCCESS(status))
      return false;
   bo->sparse_high_va = high;

   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = high,
      .SizeInPages = bo->base.size / 4096,
      .Protection.Zero = 1,
   };
   status = BC250_WDDM_CALL(&ws->host, MapGpuVirtualAddress, &map);
   if (!NT_SUCCESS(status))
      return false;

   uint64_t fence = map.PagingFenceValue;
   for (uint64_t offset = 0; offset < bo->base.size;) {
      const uint64_t bytes = MIN2(bo->base.size - offset, ws->null_prt.bo->size);
      map = (D3DDDI_MAPGPUVIRTUALADDRESS){
         .hPagingQueue = ws->paging_queue_h,
         .BaseAddress = low + offset,
         .hAllocation = ws->null_prt.bo->handle,
         .SizeInPages = bytes / 4096,
         /* Read-only; every chunk aliases offset zero of the shared BO. */
      };
      status = BC250_WDDM_CALL(&ws->host, MapGpuVirtualAddress, &map);
      if (!NT_SUCCESS(status))
         return false;
      fence = map.PagingFenceValue;
      offset += bytes;
   }

   /* One paging-queue wait covers both initial views, not one per zero chunk. */
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &fence,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   if (!NT_SUCCESS(status))
      return false;
   bo->base.va = high;
   return true;
}

static VkResult
radv_wddm2_virtual_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                             enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                             unsigned priority, uint64_t address, struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;
   bo->base.is_virtual = true;
   bo->base.size = size;
   bo->emulate_sparse_residency = flags & RADEON_FLAG_EMULATE_SPARSE_RESIDENCY;
   if (bo->emulate_sparse_residency) {
      result = radv_wddm2_init_null_prt_bo(ws);
      if (result != VK_SUCCESS)
         goto error_va_reserve;
      address &= ~RADV_WDDM2_PRT_CONTROL_MASK;
   }
   bo->base.va = radv_wddm2_reserve_va_range(ws, bo, size, alignment, flags, address);
   if (bo->base.va == 0) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_reserve;
   }

   if (bo->emulate_sparse_residency && !radv_wddm2_init_sparse_alias(ws, bo)) {
      const D3DKMT_FREEGPUVIRTUALADDRESS low = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = bo->reserved_va,
         .Size = bo->reserved_size,
      };
      BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &low);
      if (bo->sparse_high_va) {
         const D3DKMT_FREEGPUVIRTUALADDRESS high = {
            .hAdapter = ws->adapter_h,
            .BaseAddress = bo->sparse_high_va,
            .Size = align64(bo->base.size, 65536),
         };
         BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &high);
      }
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_reserve;
   }

   *out_bo = &bo->base;
   return VK_SUCCESS;

error_va_reserve:
   FREE(bo);
   return result;
}

static VkResult
radv_wddm2_bo_create_internal(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                              enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                              unsigned priority, uint64_t address, void *cpu_ptr,
                              struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   /* The contract has no userptr allocation. A normal BO would be the wrong object. */
   if (ws->bc250 && cpu_ptr)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (address && (address >= RADV_WDDM2_PRT_CONTROL_MASK ||
                   size > RADV_WDDM2_PRT_CONTROL_MASK - address))
      return VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;

   uint32_t phys_alignment = MAX2(alignment, 0x1000);
   if (initial_domain & RADEON_DOMAIN_VRAM) {
      if (size >= 0x10000)
         phys_alignment = MAX2(phys_alignment, 0x10000);
      if (size >= 0x40000)
         phys_alignment = MAX2(phys_alignment, 0x40000);
   }

   uint32_t virt_alignment = phys_alignment;
   if (size >= ws->gpu_info.pte_fragment_size)
      virt_alignment = MAX2(virt_alignment, ws->gpu_info.pte_fragment_size);
   const uint64_t phys_size = align64(size, phys_alignment);

   /* C70 (BD-096): a device-local allocation that no eviction can fit is refused here. The
    * MakeResident below would otherwise evict the whole process for about 80 s and fail all the
    * same (K245). The allocation's heap is the one the private blob asks for below. */
   const bool mem_local = !!(initial_domain & RADEON_DOMAIN_VRAM);
   if (!radv_wddm2_mem_admit(ws, phys_size, mem_local)) {
      FREE(bo);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   uint8_t alloc_pdata[824] = {0};
   uint32_t pdata_size;
   if (ws->bc250) {
      struct bc250_alloc_blob blob;
      memset(&blob, 0, sizeof(blob));
      blob.magic = BC250_ALLOC_MAGIC;
      /* BC2A v2 makes cache intent explicit; v1 KMD allocations retain WC. */
      blob.version = 2;
      blob.size = sizeof(blob);
      blob.alloc_size = phys_size;
      blob.phys_alignment = phys_alignment;
      blob.preferred_heap = (initial_domain & RADEON_DOMAIN_VRAM) ? BC250_HEAP_VRAM : BC250_HEAP_GTT;
      /* AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED / NO_CPU_ACCESS / CPU_GTT_USWC,
       * matching radv_amdgpu_bo.c. Other GEM flags have no new WDDM policy here. */
      if (flags & RADEON_FLAG_CPU_ACCESS)
         blob.gem_flags |= 1ull << 0;
      if (flags & RADEON_FLAG_NO_CPU_ACCESS)
         blob.gem_flags |= 1ull << 1;
      if (flags & RADEON_FLAG_GTT_WC)
         blob.gem_flags |= 1ull << 2;
      blob.requested_va = address;
      blob.va_size = phys_size;
      if (address)
         blob.flags = BC250_A_EXACT_VA;
      _Static_assert(sizeof(blob) == 192, "BC2A is 192 bytes");
      _Static_assert(sizeof(blob) <= sizeof(alloc_pdata), "BC2A fits the allocation private buffer");
      memcpy(alloc_pdata, &blob, sizeof(blob));
      pdata_size = sizeof(blob);
   } else {
      pdata_size = fill_alloc_pdata(ws, phys_size, phys_alignment, initial_domain,
                                    flags, priority, address, cpu_ptr, alloc_pdata);
   }

   D3DDDI_ALLOCATIONINFO2 alloc_info = {
      .pSystemMem = cpu_ptr,
      .pPrivateDriverData = alloc_pdata,
      .PrivateDriverDataSize = pdata_size,
      .VidPnSourceId = 0xffffffff,
      .Priority = D3DDDI_ALLOCATIONPRIORITY_NORMAL,
   };

   struct create_alloc_pdata create_pdata = {
      .adapter_id = 0,
      .flags = 0x80,
      .pdata_size = pdata_size,
   };
   create_pdata.checksum =
      calculate_checksum((const uint32_t *)&create_pdata, sizeof(create_pdata) / 4);

   D3DKMT_CREATEALLOCATION create = {
      .hDevice = ws->device_h,
      .pPrivateDriverData = &create_pdata,
      .PrivateDriverDataSize = sizeof(create_pdata),
      .NumAllocations = 1,
      .pAllocationInfo2 = &alloc_info,
      .Flags = {
         .CreateResource = 1,
         .CreateShared = !(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING),
         .NonSecure = 1,
      }
   };

#if 0
   fprintf(stdout, "pArgs (%zi bytes):\n", sizeof(create));
   print_hex_data(stdout, &create, sizeof(create));
   fprintf(stdout, "pPrivateDriverData (%zi bytes):\n", sizeof(create_pdata));
   print_hex_data(stdout, &create_pdata, sizeof(create_pdata));
   fprintf(stdout, "child (%zi bytes):\n", sizeof(alloc_info));
   print_hex_data(stdout, &alloc_info, sizeof(alloc_info));
   fprintf(stdout, "child.pPrivateDriverData (%i bytes):\n", pdata_size);
   print_hex_data(stdout, alloc_pdata, pdata_size);
   fflush(stdout);
#endif

   /*bo->base.va = radv_wddm2_bo_va_alloc(ws, flags, phys_size, virt_alignment);
   if (bo->base.va == 0) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_create;
   }*/

   status = BC250_WDDM_CALL(&ws->host, CreateAllocation2, &create);
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "CreateAllocation2 failed 0x%X\n", status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_ptr_alloc;
   }

   bo->base.obj_id = alloc_info.hAllocation;
   bo->base.size = phys_size;
   bo->base.handle = alloc_info.hAllocation;
   fprintf(stderr, "allocation handle=0x%x\n", bo->base.handle);

   const D3DKMT_DESTROYALLOCATION2 destroy = {
      .hDevice = ws->device_h,
      .phAllocationList = &bo->base.handle,
      .AllocationCount = 1,
   };

   D3DGPU_VIRTUAL_ADDRESS min = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_32BIT_HEAP_START : RADV_WDDM2_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      min = RADV_WDDM2_REPLAY_HEAP_START;
   D3DGPU_VIRTUAL_ADDRESS max = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_HEAP_START : RADV_WDDM2_REPLAY_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      max = RADV_WDDM2_REPLAY_HEAP_START + (4ull << 32);
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = address, //0, //bo->base.va == 0x100002000 ? 0 : bo->base.va,
      .MinimumAddress = min,
      .MaximumAddress = max,
      .hAllocation = bo->base.handle,
      .SizeInPages = phys_size / 4096,
      .Protection = {
         .Write = !(flags & RADEON_FLAG_READ_ONLY),
      },
   };
   status = BC250_WDDM_CALL(&ws->host, MapGpuVirtualAddress, &map);
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "mapping 0x%" PRIx64 " failed: 0x%X\n", bo->base.va, status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }
   bo->base.va = map.VirtualAddress;

   uint64_t paging_fence_value = map.PagingFenceValue;

   if (all_resident) {
      /* BD-096 (7f16adb0): MustSucceed puts the device in error when the allocation cannot be made
       * resident (d3dukmdt.h), so a budget failure became a device loss. CantTrimFurther may exceed
       * the current budget and fails only above the maximum budget, without removing the device.
       *
       * An allocation the driver makes for itself (RADEON_FLAG_INTERNAL: the shader arena, command
       * buffers, upload and scratch buffers) keeps MustSucceed with it. The application never asked for
       * that memory, cannot make it smaller and cannot free it, and a pipeline compile or a submission
       * that does not get it has no smaller choice to make; the video memory manager pages other memory
       * out first and only a request it cannot satisfy at all removes the device. Rise of the Tomb
       * Raider died in lab sessions 486 to 488 on a pipeline whose shader arena was refused. */
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .CantTrimFurther = 1,
            .MustSucceed = (flags & RADEON_FLAG_INTERNAL) ? 1u : 0u,
         },
      };
      status = BC250_WDDM_CALL(&ws->host, MakeResident, &make_resident);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "MakeResident failed\n");
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto error_va_alloc;
      }

      paging_fence_value = make_resident.PagingFenceValue;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &paging_fence_value,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "WaitForSynchronizationObjectFromCpu failed\n");
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   if (ws->debug_all_bos)
      radv_winsys_bo_list_add(&ws->global_bo_list, &bo->base);
   if (ws->debug_log_bos)
      radv_winsys_log_bo(&ws->bo_log, &bo->base, false);

   /* C70: count it only once it is whole, so no error path leaves bytes behind. */
   radv_wddm2_mem_account(ws, bo->base.size, mem_local, true);
   bo->counted = true;

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_va_alloc:
   //radv_wddm2_bo_va_free(ws, flags, bo->base.va, bo->base.size);

   fprintf(stderr, "destroy allocation\n");
   status = BC250_WDDM_CALL(&ws->host, DestroyAllocation2, &destroy);
   assert(NT_SUCCESS(status));

error_ptr_alloc:
   fprintf(stderr, "free va\n");
   FREE(bo);
   return result;
}

static VkResult
radv_wddm2_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                     enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                     unsigned priority, uint64_t address, struct radv_image *image,
                     struct radeon_winsys_bo **out_bo)
{
   if (flags & RADEON_FLAG_VIRTUAL)
      return radv_wddm2_virtual_bo_create(_ws, size, alignment, initial_domain, flags,
                                          priority, address, out_bo);
   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, address, NULL, out_bo);
}

static VkResult
radv_wddm2_bo_from_ptr(struct radeon_winsys *_ws, void *pointer, uint64_t size, unsigned priority,
                       struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   enum radeon_bo_domain initial_domain = RADEON_DOMAIN_GTT;
   enum radeon_bo_flag flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_CPU_ACCESS;
   unsigned alignment = ws->gpu_info.gart_page_size;

   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, 0, pointer, out_bo);
}

static bool
radv_wddm2_bo_get_handle(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, void **handle)
{
   return false;
}

static VkResult
radv_wddm2_bo_from_handle(struct radeon_winsys *_ws, void *handle, unsigned priority,
                           struct radeon_winsys_bo **out_bo, uint64_t *alloc_size)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = RADEON_DOMAIN_VRAM;
   bo->ws = ws;
   bo->flags = 0;

   /* Query resource info to determine private data sizes */
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_info = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
   };
   status = BC250_WDDM_CALL(&ws->host, QueryResourceInfoFromNtHandle, &query_info);
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "QueryResourceInfoFromNtHandle failed 0x%X\n", status);
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_alloc;
   }

   /* One allocation is the native D3D linear-surface sharing contract.
    * Resource, runtime and per-allocation private data are distinct buffers
    * (WDK D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE/OPENRESOURCEFROMNTHANDLE).
    */
   if (query_info.NumAllocations != 1) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_alloc;
   }
   size_t pdata_size = (size_t)query_info.TotalPrivateDriverDataSize +
                       query_info.ResourcePrivateDriverDataSize + query_info.PrivateRuntimeDataSize;
   void *pdata = calloc(1, MAX2(pdata_size, 1));
   D3DDDI_OPENALLOCATIONINFO2 alloc_info[1] = {0};
   if (!pdata) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto error_alloc;
   }
   void *res_pdata = (uint8_t *)pdata + query_info.TotalPrivateDriverDataSize;
   void *runtime_data = (uint8_t *)res_pdata + query_info.ResourcePrivateDriverDataSize;

   D3DKMT_OPENRESOURCEFROMNTHANDLE open_resource = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
      .NumAllocations = query_info.NumAllocations,
      .pOpenAllocationInfo2 = alloc_info,
      .TotalPrivateDriverDataBufferSize = query_info.TotalPrivateDriverDataSize,
      .pTotalPrivateDriverDataBuffer = pdata,
      .ResourcePrivateDriverDataSize = query_info.ResourcePrivateDriverDataSize,
      .pResourcePrivateDriverData = res_pdata,
      .PrivateRuntimeDataSize = query_info.PrivateRuntimeDataSize,
      .pPrivateRuntimeData = runtime_data,
   };
   status = BC250_WDDM_CALL(&ws->host, OpenResourceFromNtHandle, &open_resource);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_import;
   }

   bo->resource_handle = open_resource.hResource;
   bo->base.obj_id = bo->base.handle = alloc_info[0].hAllocation;

   if (ws->bc250) {
      /* KMD's LB7A v1 surface ABI, not the proprietary AMD allocation ABI. */
      struct bc250_linear_surface {
         uint32_t magic, version, width, height, pitch, format;
         uint64_t size;
      } surface;
      _Static_assert(sizeof(surface) == 32, "LB7A v1 ABI");
      if (!alloc_info[0].pPrivateDriverData ||
          alloc_info[0].PrivateDriverDataSize < sizeof(surface)) {
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto error_import;
      }
      memcpy(&surface, alloc_info[0].pPrivateDriverData, sizeof(surface));
      if (surface.magic != 0x4137424c || surface.version != 1 ||
          !surface.width || !surface.height || surface.width > 8192 || surface.height > 8192 ||
          surface.pitch < (uint64_t)surface.width * 4 || (surface.pitch & 15) ||
          surface.size < (uint64_t)surface.pitch * surface.height ||
          surface.size > UINT64_MAX - 4095 ||
          (surface.format != D3DDDIFMT_A8R8G8B8 && surface.format != D3DDDIFMT_X8R8G8B8 &&
           surface.format != D3DDDIFMT_A8B8G8R8)) {
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto error_import;
      }
      bo->base.size = align64(surface.size, 4096); /* KMD ROUND_TO_PAGES(Size) */
      bo->md.u.gfx9.swizzle_mode = 0; /* radv_patch_surface_from_metadata: linear */
      bo->md.metadata_type = RADEON_METADATA_TYPE_KMW;
      bo->md.kmw.pitch_bytes = surface.pitch;
      bo->md.kmw.surf_size = surface.size;
   } else {
      struct alloc_entry *entry = (struct alloc_entry *)((uint8_t *)pdata + sizeof(struct alloc_header));
      struct alloc_surf *surf = (struct alloc_surf *)((uint8_t *)entry + sizeof(struct alloc_entry));
      bo->base.size = entry->bo_info.phys_size;
      bo->md.u.gfx9.swizzle_mode = surf->swizzle_mode;
      bo->md.metadata_type = RADEON_METADATA_TYPE_KMW;
      bo->md.kmw.pitch_bytes = surf->width;
      bo->md.kmw.surf_size = surf->slice_size;
   }

   /* Map the opened allocation into GPU virtual address space */
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .MinimumAddress = RADV_WDDM2_HEAP_START,
      .MaximumAddress = RADV_WDDM2_REPLAY_HEAP_START,
      .hAllocation = bo->base.handle,
      .Protection = {
         .Write = 1,
      },
   };
   status = BC250_WDDM_CALL(&ws->host, MapGpuVirtualAddress, &map);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_import;
   }
   bo->base.va = map.VirtualAddress;

   if (alloc_size)
      *alloc_size = bo->base.size;

   /* Make the allocation resident. An opened allocation belongs to the application or to another
    * process, never to this driver, so it never takes MustSucceed (BD-096, 7f16adb0): a budget failure
    * is reported as VK_ERROR_OUT_OF_DEVICE_MEMORY and the device survives. */
   D3DDDI_MAKERESIDENT make_resident = {
      .hPagingQueue = ws->paging_queue_h,
      .NumAllocations = 1,
      .AllocationList = &bo->base.handle,
      .Flags = {
         .CantTrimFurther = 1,
      },
   };
   status = BC250_WDDM_CALL(&ws->host, MakeResident, &make_resident);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   /* Wait for the paging operation to complete */
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &make_resident.PagingFenceValue,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   free(pdata);
   *out_bo = &bo->base;
   return VK_SUCCESS;

error_map:
   {
      const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = bo->base.va,
         .Size = bo->base.size,
      };
      BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &unmap);
   }
error_import:
   if (bo->resource_handle) {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .hResource = bo->resource_handle,
      };
      BC250_WDDM_CALL(&ws->host, DestroyAllocation2, &destroy);
   }
   free(pdata);
error_alloc:
   FREE(bo);
   return result;
}

static bool
radv_wddm2_bo_get_flags_from_handle(struct radeon_winsys *_ws, void *handle,
                                    enum radeon_bo_domain *domains,
                                    enum radeon_bo_flag *flags)
{
   *domains = RADEON_DOMAIN_VRAM;
   *flags = RADEON_FLAG_CPU_ACCESS;

   return true;
}

static void
radv_wddm2_bo_get_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                           struct radeon_bo_metadata *md)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);

   memcpy(md, &bo->md, sizeof(*md));
}

static void *
radv_wddm2_bo_map(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                  bool use_fixed_addr, void *fixed_addr)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->borrowed) return NULL;

   if (bo->map && !fixed_addr)
      return bo->map;

   if (bo->flags & RADEON_FLAG_NO_CPU_ACCESS) {
      fprintf(stderr, "attempt to map non-CPU-accessible BO\n");
      return NULL;
   }

   D3DKMT_LOCK2 lock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = BC250_WDDM_CALL(&bo->ws->host, Lock2, &lock);
   if (!NT_SUCCESS(status))
      return NULL;

   bo->map = lock.pData;

   return lock.pData;
}

static void
radv_wddm2_bo_unmap(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool replace)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->map == NULL)
      return;

   const D3DKMT_UNLOCK2 unlock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = BC250_WDDM_CALL(&bo->ws->host, Unlock2, &unlock);
   assert(NT_SUCCESS(status));
   
   bo->map = NULL;
}

static VkResult
radv_wddm2_bo_make_resident(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                            bool resident)
{
   if (all_resident || radv_wddm2_bo(_bo)->borrowed)
      return VK_SUCCESS;

   //fprintf(stderr, "make resident \n");
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   NTSTATUS status;

   if (resident) {
      /* BD-096 (7f16adb0) with the driver's own allocations kept resident: see the comment at the
       * MakeResident of radv_wddm2_bo_create_internal. */
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .CantTrimFurther = 1,
            .MustSucceed = (bo->flags & RADEON_FLAG_INTERNAL) ? 1u : 0u,
         },
      };
      status = BC250_WDDM_CALL(&ws->host, MakeResident, &make_resident);
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
         .hDevice = ws->device_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &ws->paging_fence_h,
         .FenceValueArray = &make_resident.PagingFenceValue,
      };
      status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   } else {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = BC250_WDDM_CALL(&ws->host, Evict, &evict);
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   return VK_SUCCESS;
}

static void
radv_wddm2_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   /* C70: take the bytes out of the count first. Every exit of this function gives the BO up. */
   if (bo->counted) {
      bo->counted = false;
      radv_wddm2_mem_account(ws, bo->base.size, !!(bo->base.initial_domain & RADEON_DOMAIN_VRAM),
                             false);
   }

   if (all_resident && !bo->base.is_virtual && !bo->borrowed) {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = BC250_WDDM_CALL(&ws->host, Evict, &evict);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "*****  Evict failed\n");
         return;
      }
   }

   radv_wddm2_bo_unmap(_ws, _bo, false);

   if (bo->borrowed) { FREE(bo); return; }
   if (bo->sparse_high_va) {
      const D3DKMT_FREEGPUVIRTUALADDRESS high = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = bo->sparse_high_va,
         .Size = align64(bo->base.size, 65536),
      };
      status = BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &high);
   }
   const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
      .hAdapter = ws->adapter_h,
      .BaseAddress = bo->reserved_va ? bo->reserved_va : bo->base.va,
      .Size = bo->reserved_size ? bo->reserved_size : bo->base.size,
   };
   status = BC250_WDDM_CALL(&ws->host, FreeGpuVirtualAddress, &unmap);

   if (!bo->base.is_virtual) {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .hResource = bo->resource_handle,
         .phAllocationList = bo->resource_handle ? NULL : &bo->base.handle,
         .AllocationCount = bo->resource_handle ? 0 : 1,
      };
      status = BC250_WDDM_CALL(&ws->host, DestroyAllocation2, &destroy);
      //assert(NT_SUCCESS(status));

      if (ws->debug_all_bos)
         radv_winsys_bo_list_del(&ws->global_bo_list, &bo->base);
      if (ws->debug_log_bos)
         radv_winsys_log_bo(&ws->bo_log, &bo->base, true);
   }

   //radv_wddm2_bo_va_free(ws, bo->flags, bo->base.va, bo->base.size);

   FREE(bo);
}
struct radv_wddm2_sparse_group {
   uint64_t reservation;
   struct util_dynarray operations;
};

void
radv_wddm2_sparse_groups_clear(struct radv_wddm2_queue *queue)
{
   util_dynarray_foreach (&queue->sparse_ops, struct radv_wddm2_sparse_group, group)
      util_dynarray_fini(&group->operations);
   util_dynarray_clear(&queue->sparse_ops);
}

static VkResult
radv_wddm2_virtual_bind_begin(struct radeon_winsys *_ws, struct radeon_winsys_ctx *_ctx,
                             enum amd_ip_type ip_type)
{
   struct radv_wddm2_ctx *ctx = (struct radv_wddm2_ctx *)_ctx;
   struct radv_wddm2_queue *queue = &ctx->per_ip[ip_type].queue;

   /* RADV externally serializes submission to each advertised hardware queue. */
   if (queue->sparse_batch_active || !queue->context_h || !queue->vm_fence.handle)
      return VK_ERROR_DEVICE_LOST;
   radv_wddm2_sparse_groups_clear(queue);
   queue->sparse_batch_active = true;
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_virtual_bind_append(struct radv_wddm2_ctx *ctx, enum amd_ip_type ip_type,
                              uint64_t reservation, const D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION *op)
{
   struct radv_wddm2_queue *queue = &ctx->per_ip[ip_type].queue;
   if (!queue->sparse_batch_active)
      return VK_ERROR_DEVICE_LOST;

   /* MS D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION: one Update call can touch
    * only one ReserveGpuVirtualAddress reservation. Group by reservation,
    * preserving the original operation order within every resource/view.
    * Map/unmap operations in different reservations are independent.
    */
   struct radv_wddm2_sparse_group *dst_group = NULL;
   util_dynarray_foreach (&queue->sparse_ops, struct radv_wddm2_sparse_group, group) {
      if (group->reservation == reservation) {
         dst_group = group;
         break;
      }
   }
   if (!dst_group) {
      dst_group = util_dynarray_grow_bytes(&queue->sparse_ops, 1, sizeof(*dst_group));
      if (!dst_group)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      dst_group->reservation = reservation;
      util_dynarray_init(&dst_group->operations, NULL);
   }
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION *dst =
      util_dynarray_grow_bytes(&dst_group->operations, 1, sizeof(*op));
   if (!dst)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   *dst = *op;
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_virtual_bind_end(struct radeon_winsys *_ws, struct radeon_winsys_ctx *_ctx,
                           enum amd_ip_type ip_type, uint32_t queue_index,
                           uint32_t wait_count, const struct vk_sync_wait *waits, bool commit)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_ctx *ctx = (struct radv_wddm2_ctx *)_ctx;
   struct radv_wddm2_queue *queue = &ctx->per_ip[ip_type].queue;
   VkResult result = VK_SUCCESS;
   NTSTATUS status;

   if (!queue->sparse_batch_active)
      return VK_ERROR_DEVICE_LOST;
   queue->sparse_batch_active = false;
   if (!commit)
      goto done;

   /* An empty GPU submission queues application waits without a CPU wait.
    * The boundary signal below follows both those waits and earlier rendering.
    */
   const struct radv_winsys_submit_info submit = {
      .ip_type = ip_type,
      .queue_index = queue_index,
   };
   result = _ws->cs_submit(_ctx, &submit, wait_count, waits, 0, NULL);
   if (result != VK_SUCCESS)
      goto done;

   const unsigned count =
      util_dynarray_num_elements(&queue->sparse_ops, struct radv_wddm2_sparse_group);
   if (!count)
      goto done;
   if (queue->vm_fence.wait_value > UINT64_MAX - count - 1) {
      result = VK_ERROR_DEVICE_LOST;
      goto done;
   }
   const uint64_t boundary = queue->vm_fence.wait_value + 1;
   const uint64_t completed = boundary + count;
   if (queue->handle) {
      const D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {
         .BroadcastHwQueueCount = 1,
         .BroadcastHwQueueArray = &queue->handle,
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->vm_fence.handle,
         .FenceValueArray = &boundary,
      };
      status = BC250_WDDM_CALL(&ws->host, SubmitSignalSyncObjectsToHwQueue, &signal);
   } else {
      const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
         .BroadcastContextCount = 1,
         .BroadcastContextArray = &queue->context_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->vm_fence.handle,
         .MonitoredFenceValueArray = &boundary,
      };
      status = BC250_WDDM_CALL(&ws->host, SignalSynchronizationObjectFromGpu2, &signal);
   }
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_DEVICE_LOST;
      goto done;
   }

   /* Chain reservation batches on one monitored fence. The first waits for
    * earlier rendering/application dependencies; subsequent batches wait for
    * the previous update. Rendering waits once, for all views/resources.
    */
   uint64_t update_fence = boundary;
   util_dynarray_foreach (&queue->sparse_ops, struct radv_wddm2_sparse_group, group) {
      const D3DKMT_UPDATEGPUVIRTUALADDRESS update = {
         .hDevice = ws->device_h,
         .hContext = queue->context_h,
         .hFenceObject = queue->vm_fence.handle,
         .FenceValue = update_fence++,
         .Operations = group->operations.data,
         .NumOperations =
            util_dynarray_num_elements(&group->operations, D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION),
      };
      status = BC250_WDDM_CALL(&ws->host, UpdateGpuVirtualAddress, &update);
      if (!NT_SUCCESS(status)) {
         result = VK_ERROR_DEVICE_LOST;
         goto done;
      }
   }

   if (queue->handle) {
      const D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {
         .hHwQueue = queue->handle,
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->vm_fence.handle,
         .FenceValueArray = &completed,
      };
      status = BC250_WDDM_CALL(&ws->host, SubmitWaitForSyncObjectsToHwQueue, &wait);
   } else {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
         .hContext = queue->context_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->vm_fence.handle,
         .MonitoredFenceValueArray = &completed,
      };
      status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromGpu, &wait);
   }
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_DEVICE_LOST;
      goto done;
   }
   queue->vm_fence.wait_value = completed;

done:
   radv_wddm2_sparse_groups_clear(queue);
   return result;
}

static VkResult
radv_wddm2_virtual_bo_map(struct radv_wddm2_winsys *ws, struct radv_wddm2_ctx *ctx,
                            enum amd_ip_type ip_type, struct radv_wddm2_bo *parent,
                            uint64_t offset, uint64_t size, struct radv_wddm2_bo *bo,
                            uint64_t bo_offset)
{
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT,
      .MapProtect = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .hAllocation = bo->base.handle,
         .AllocationOffsetInBytes = bo_offset,
         .AllocationSizeInBytes = size,
         .Protection.Write = !(bo->flags & RADEON_FLAG_READ_ONLY),
         .DriverProtection = 1,
      },
   };
   VkResult result = radv_wddm2_virtual_bind_append(ctx, ip_type, parent->base.va, &op);
   if (result != VK_SUCCESS || !parent->emulate_sparse_residency)
      return result;
   op.MapProtect.BaseAddress &= ~RADV_WDDM2_PRT_CONTROL_MASK;
   return radv_wddm2_virtual_bind_append(ctx, ip_type, parent->base.va & ~RADV_WDDM2_PRT_CONTROL_MASK, &op);
}

static VkResult
radv_wddm2_virtual_bo_unmap(struct radv_wddm2_winsys *ws, struct radv_wddm2_ctx *ctx,
                            enum amd_ip_type ip_type, struct radv_wddm2_bo *parent,
                            uint64_t offset, uint64_t size)
{
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP,
      .Unmap = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .Protection.Zero = 1,
      },
   };
   VkResult result = radv_wddm2_virtual_bind_append(ctx, ip_type, parent->base.va, &op);
   if (result != VK_SUCCESS || !parent->emulate_sparse_residency)
      return result;

   uint64_t va = (parent->base.va & ~RADV_WDDM2_PRT_CONTROL_MASK) + offset;
   while (size) {
      const uint64_t bytes = MIN2(size, ws->null_prt.bo->size);
      op = (D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION){
         .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT,
         .MapProtect = {
            .BaseAddress = va,
            .SizeInBytes = bytes,
            .hAllocation = ws->null_prt.bo->handle,
            .AllocationSizeInBytes = bytes,
            .DriverProtection = 1,
         },
      };
      result = radv_wddm2_virtual_bind_append(ctx, ip_type, parent->base.va & ~RADV_WDDM2_PRT_CONTROL_MASK, &op);
      if (result != VK_SUCCESS)
         return result;
      size -= bytes;
      va += bytes;
   }
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_bo_virtual_bind(struct radeon_winsys *_ws, struct radeon_winsys_ctx *_ctx, enum amd_ip_type ip_type,
                           struct radeon_winsys_bo *_parent, uint64_t offset, uint64_t size,
                           struct radeon_winsys_bo *_bo, uint64_t bo_offset)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_ctx *ctx = (struct radv_wddm2_ctx *)_ctx;
   struct radv_wddm2_bo *parent = (struct radv_wddm2_bo *)_parent;
   struct radv_wddm2_bo *bo = (struct radv_wddm2_bo *)_bo;
   VkResult ret;

   assert(parent->base.is_virtual);
   assert(!bo || !bo->base.is_virtual);

   if (bo) {
      ret = radv_wddm2_virtual_bo_map(ws, ctx, ip_type, parent, offset, size, bo, bo_offset);
   } else {
      ret = radv_wddm2_virtual_bo_unmap(ws, ctx, ip_type, parent, offset, size);
   }

   return ret;
}

static void
radv_wddm2_dump_bo_log(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (!ws->debug_log_bos)
      return;

   radv_winsys_dump_bo_log(&ws->bo_log, file);
}

static void
radv_wddm2_dump_bo_ranges(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   fprintf(stderr, "dump bo ranges\n");
   if (ws->debug_all_bos)
      radv_winsys_dump_bo_ranges(&ws->global_bo_list, file);
   else
      fprintf(file, "  To get BO VA ranges, please specify RADV_DEBUG=allbos\n");
}

static VkResult
radv_wddm2_bo_from_hosted(struct radeon_winsys *rws, void *identity, uint32_t allocation,
                         uint64_t va, uint64_t size, struct radeon_winsys_bo **out)
{
   struct radv_wddm2_winsys *ws=radv_wddm2_winsys(rws);
   if (!ws->host.dispatch || identity!=ws->host.identity || !allocation || !va || !size ||
       (va & 4095) || (size & 4095) || va>=RADV_WDDM2_PRT_CONTROL_MASK || size>RADV_WDDM2_PRT_CONTROL_MASK-va)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct radv_wddm2_bo *bo=CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo) return VK_ERROR_OUT_OF_HOST_MEMORY;
   bo->ws=ws; bo->borrowed=true;
   bo->base.va=va; bo->base.size=size; bo->base.handle=allocation; bo->base.obj_id=allocation;
   bo->base.initial_domain=RADEON_DOMAIN_VRAM;
   *out=&bo->base;
   return VK_SUCCESS;
}

void
radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws)
{
   ws->base.buffer_from_hosted=radv_wddm2_bo_from_hosted;
   ws->base.buffer_create = radv_wddm2_bo_create;
   ws->base.buffer_destroy = radv_wddm2_bo_destroy;
   ws->base.buffer_map = radv_wddm2_bo_map;
   ws->base.buffer_unmap = radv_wddm2_bo_unmap;
   ws->base.buffer_make_resident = radv_wddm2_bo_make_resident;
   ws->base.buffer_from_ptr = radv_wddm2_bo_from_ptr;
   ws->base.buffer_get_handle = radv_wddm2_bo_get_handle;
   ws->base.buffer_from_handle = radv_wddm2_bo_from_handle;
   ws->base.buffer_get_flags_from_handle = radv_wddm2_bo_get_flags_from_handle;
   ws->base.buffer_get_metadata = radv_wddm2_bo_get_metadata;
   ws->base.buffer_virtual_bind_begin = radv_wddm2_virtual_bind_begin;
   ws->base.buffer_virtual_bind_end = radv_wddm2_virtual_bind_end;
   ws->base.buffer_virtual_bind = radv_wddm2_bo_virtual_bind;
   ws->base.dump_bo_ranges = radv_wddm2_dump_bo_ranges;
   ws->base.dump_bo_log = radv_wddm2_dump_bo_log;
}
