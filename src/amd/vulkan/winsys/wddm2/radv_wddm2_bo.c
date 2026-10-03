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
#include "util/amdgpu_wddm_stdio.h"
#include "radv_wddm2_cs.h"
#include "util/os_time.h"
#include "util/u_memory.h"

#include <inttypes.h>
#include <stdarg.h>

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

/* Every operation on a device paging queue gets a unique value of the queue's
 * monitored fence, signaled when the operation completes, and zero when it
 * completed immediately ("Device paging queues" in the WDDM docs). A monitored
 * fence value only grows, so waiting for the largest returned value waits for
 * all of them. A later value must never replace an earlier one: a zero or
 * smaller later result would drop the wait for an operation still pending.
 *
 * MapGpuVirtualAddress always reports the value to wait for. MakeResident
 * documents it only for STATUS_PENDING (E_PENDING in the UMD callback). It is
 * taken for any success here, as the d3d10umd shared-surface import does: the
 * field is zero-initialized, and an under-wait lets the GPU touch an unmapped
 * page, which this part cannot recover from.
 */
static inline void
radv_wddm2_require_paging_fence(uint64_t *required, uint64_t value)
{
   *required = MAX2(*required, value);
}

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

/* BO structs (ws->deferred.pool). A destroyed struct waits behind RADV_WDDM2_BO_POOL_KEEP others before
 * reuse and is freed only with the winsys. A command stream's BO set that still names a destroyed BO (an
 * application or driver fault) then names a BO struct, and the witness's stamp (radv_wddm2_cs.c) writes
 * into it, never into freed memory. Every struct that reached RADV goes back through the pool. */
#define RADV_WDDM2_BO_POOL_KEEP 4096u

static struct radv_wddm2_bo *
radv_wddm2_bo_struct_alloc(struct radv_wddm2_winsys *ws)
{
   struct radv_wddm2_bo *bo = NULL;
   simple_mtx_lock(&ws->deferred.pool_lock);
   if (ws->deferred.pool_count > RADV_WDDM2_BO_POOL_KEEP) {
      bo = list_first_entry(&ws->deferred.pool, struct radv_wddm2_bo, pool_link);
      list_del(&bo->pool_link);
      ws->deferred.pool_count--;
   }
   simple_mtx_unlock(&ws->deferred.pool_lock);
   if (bo)
      memset(bo, 0, sizeof(*bo));
   else
      bo = CALLOC_STRUCT(radv_wddm2_bo);
   return bo;
}

static void
radv_wddm2_bo_struct_free(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo)
{
   bo->destroyed = true;
   simple_mtx_lock(&ws->deferred.pool_lock);
   list_addtail(&bo->pool_link, &ws->deferred.pool);
   ws->deferred.pool_count++;
   simple_mtx_unlock(&ws->deferred.pool_lock);
}

void
radv_wddm2_bo_pool_finish(struct radv_wddm2_winsys *ws)
{
   list_for_each_entry_safe (struct radv_wddm2_bo, bo, &ws->deferred.pool, pool_link)
      FREE(bo);
   list_inithead(&ws->deferred.pool);
   ws->deferred.pool_count = 0;
   simple_mtx_destroy(&ws->deferred.pool_lock);
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
                                         RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_PREFER_LOCAL_BO,
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
      radv_wddm2_require_paging_fence(&fence, map.PagingFenceValue);
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

   bo = radv_wddm2_bo_struct_alloc(ws);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;
   bo->priority = MIN2(priority, UINT8_MAX);
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
   radv_wddm2_bo_struct_free(ws, bo);
   return result;
}

/* Match amdgpu's initial-domain accounting. Imported/borrowed BOs without
 * NO_CPU_ACCESS belong to visible VRAM; virtual reservations have no backing.
 * Publish only fully constructed BOs. Failed teardown retains its byte charge.
 */
static void
radv_wddm2_bo_account(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo, bool add)
{
   if (bo->base.is_virtual)
      return;
   const uint64_t bytes = bo->base.size;
   const uint64_t delta = add ? bytes : (uint64_t)0 - bytes;
   if (bo->base.initial_domain & RADEON_DOMAIN_VRAM) {
      if (bo->flags & RADEON_FLAG_NO_CPU_ACCESS)
         p_atomic_add(&ws->allocated_vram, delta);
      else
         p_atomic_add(&ws->allocated_vram_vis, delta);
   }
   if (bo->base.initial_domain & RADEON_DOMAIN_GTT)
      p_atomic_add(&ws->allocated_gtt, delta);
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

   bo = radv_wddm2_bo_struct_alloc(ws);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;
   bo->priority = MIN2(priority, UINT8_MAX);

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
      amdgpu_wddm_log("CreateAllocation2 failed 0x%X\n", status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_ptr_alloc;
   }

   bo->base.obj_id = alloc_info.hAllocation;
   bo->base.size = phys_size;
   bo->base.handle = alloc_info.hAllocation;
   amdgpu_wddm_log("allocation handle=0x%x\n", bo->base.handle);

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
      amdgpu_wddm_log("mapping 0x%" PRIx64 " failed: 0x%X\n", bo->base.va, status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }
   bo->base.va = map.VirtualAddress;

   uint64_t paging_fence_value = map.PagingFenceValue;

   if (all_resident) {
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
         },
      };
      status = BC250_WDDM_CALL(&ws->host, MakeResident, &make_resident);
      if (!NT_SUCCESS(status)) {
         amdgpu_wddm_log("MakeResident failed\n");
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto error_va_alloc;
      }

      radv_wddm2_require_paging_fence(&paging_fence_value, make_resident.PagingFenceValue);
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &paging_fence_value,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   if (!NT_SUCCESS(status)) {
      amdgpu_wddm_log("WaitForSynchronizationObjectFromCpu failed\n");
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   if (ws->debug_all_bos)
      radv_winsys_bo_list_add(&ws->global_bo_list, &bo->base);
   if (ws->debug_log_bos)
      radv_winsys_log_bo(&ws->bo_log, &bo->base, false);

   radv_wddm2_bo_account(ws, bo, true);
   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_va_alloc:
   //radv_wddm2_bo_va_free(ws, flags, bo->base.va, bo->base.size);

   amdgpu_wddm_log("destroy allocation\n");
   status = BC250_WDDM_CALL(&ws->host, DestroyAllocation2, &destroy);
   assert(NT_SUCCESS(status));

error_ptr_alloc:
   amdgpu_wddm_log("free va\n");
   radv_wddm2_bo_struct_free(ws, bo);
   return result;
}

static VkResult
radv_wddm2_bo_create_once(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                          enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                          unsigned priority, uint64_t address, struct radeon_winsys_bo **out_bo)
{
   if (flags & RADEON_FLAG_VIRTUAL)
      return radv_wddm2_virtual_bo_create(_ws, size, alignment, initial_domain, flags,
                                          priority, address, out_bo);
   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, address, NULL, out_bo);
}

static bool radv_wddm2_deferred_wait_oldest(struct radv_wddm2_winsys *ws, uint64_t *waited_ns);
static void radv_wddm2_deferred_line(const char *format, ...);

static VkResult
radv_wddm2_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                     enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                     unsigned priority, uint64_t address, struct radv_image *image,
                     struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   /* Held BOs whose work retired go first: their memory and VA are free before this allocation. */
   radv_wddm2_deferred_drain(ws);
   VkResult result = radv_wddm2_bo_create_once(_ws, size, alignment, initial_domain, flags, priority, address, out_bo);
   if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY || !p_atomic_read(&ws->deferred.count))
      return result;
   /* Held BOs keep memory and VA a destroy has already given back to the application: before the
    * allocation fails, CPU-wait for the oldest held BO, release what retired and try again, until it
    * succeeds or nothing held is released any more. */
   simple_mtx_lock(&ws->deferred.lock);
   const uint32_t held = ws->deferred.count;
   const uint64_t held_bytes = ws->deferred.bytes;
   simple_mtx_unlock(&ws->deferred.lock);
   uint64_t waits = 0, waited = 0;
   while (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
      uint64_t ns;
      const bool released = radv_wddm2_deferred_wait_oldest(ws, &ns);
      waited += ns;
      if (!released)
         break;
      waits++;
      result = radv_wddm2_bo_create_once(_ws, size, alignment, initial_domain, flags, priority, address, out_bo);
   }
   simple_mtx_lock(&ws->deferred.lock);
   ws->deferred.retries++;
   ws->deferred.oom_waits += waits;
   radv_wddm2_deferred_line("a %" PRIu64 "-byte allocation failed with %u BOs (%" PRIu64 " MiB) held; %" PRIu64
                            " CPU waits for the oldest held BO, %" PRIu64 " ms; %u still held (%" PRIu64
                            " MiB); the retry %s",
                            size, held, held_bytes >> 20, waits, waited / 1000000u, ws->deferred.count,
                            ws->deferred.bytes >> 20, result == VK_SUCCESS ? "succeeded" : "failed too");
   simple_mtx_unlock(&ws->deferred.lock);
   return result;
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

   if (ws->host.dispatch) {
      amdgpu_wddm_log("BC250 hosted: NT-handle memory import refused\n");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   bo = radv_wddm2_bo_struct_alloc(ws);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = RADEON_DOMAIN_VRAM;
   bo->ws = ws;
   bo->flags = 0;
   bo->priority = MIN2(priority, UINT8_MAX);

   /* Query resource info to determine private data sizes */
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_info = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
   };
   status = BC250_WDDM_CALL(&ws->host, QueryResourceInfoFromNtHandle, &query_info);
   if (!NT_SUCCESS(status)) {
      amdgpu_wddm_log("QueryResourceInfoFromNtHandle failed 0x%X\n", status);
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
   uint64_t paging_fence_value = map.PagingFenceValue;

   if (alloc_size)
      *alloc_size = bo->base.size;

   /* Make the allocation resident */
   D3DDDI_MAKERESIDENT make_resident = {
      .hPagingQueue = ws->paging_queue_h,
      .NumAllocations = 1,
      .AllocationList = &bo->base.handle,
      .Flags = {
         .MustSucceed = 1,
      },
   };
   status = BC250_WDDM_CALL(&ws->host, MakeResident, &make_resident);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }
   radv_wddm2_require_paging_fence(&paging_fence_value, make_resident.PagingFenceValue);

   /* Wait for both paging operations to complete */
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &paging_fence_value,
   };
   status = BC250_WDDM_CALL(&ws->host, WaitForSynchronizationObjectFromCpu, &wait);
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   free(pdata);
   radv_wddm2_bo_account(ws, bo, true);
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
   radv_wddm2_bo_struct_free(ws, bo);
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

   /* A borrowed allocation is mapped only when its import asked for it. */
   if (bo->borrowed && !bo->host_mappable) return NULL;

   if (bo->map && !fixed_addr)
      return bo->map;

   if (bo->flags & RADEON_FLAG_NO_CPU_ACCESS) {
      amdgpu_wddm_log("attempt to map non-CPU-accessible BO\n");
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

/* Releases the BO's CPU lock. False if Unlock2 failed: the lock and the mapping are then still valid, and
 * bo->map keeps them, so a later map returns the same pointer and a later destroy retries (BD-045). */
static bool
radv_wddm2_bo_unlock(struct radv_wddm2_bo *bo)
{
   if (bo->map == NULL)
      return true;

   const D3DKMT_UNLOCK2 unlock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   const NTSTATUS status = BC250_WDDM_CALL(&bo->ws->host, Unlock2, &unlock);
   if (!NT_SUCCESS(status)) {
      if (p_atomic_inc_return(&bo->ws->deferred.unlock_failed) <= 16)
         amdgpu_wddm_log("radv: Unlock2 of allocation 0x%x (%" PRIu64 " bytes%s) failed 0x%08lx; the lock is kept\n",
                         bo->base.handle, bo->base.size, bo->borrowed ? ", host import" : "",
                         (unsigned long)status);
      return false;
   }
   bo->map = NULL;
   return true;
}

static void
radv_wddm2_bo_unmap(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool replace)
{
   radv_wddm2_bo_unlock(radv_wddm2_bo(_bo));
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
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
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

/* BD-045: an owned BO whose CPU lock the host would not release. A host refuses to free the VA of a locked
 * allocation or to destroy it (the D3D12 shell's HostedDispatch answers E_INVALIDARG), so nothing more is
 * released: the BO keeps its allocation, VA and byte charge on ws->deferred.locked until a retry unlocks it.
 * The struct is not in the pool, so pool_link is free to carry it. */
static void
radv_wddm2_bo_keep_locked(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo)
{
   simple_mtx_lock(&ws->deferred.lock);
   if (!bo->lock_kept)
      radv_wddm2_deferred_line("a %" PRIu64 "-byte BO kept: its allocation stays CPU-locked, retried at the drain points",
                               bo->base.size);
   bo->lock_kept = true;
   bo->destroyed = true;
   /* The first kept BO starts the interval: the destroy that just failed was its first try. */
   if (!ws->deferred.locked_count)
      ws->deferred.locked_next_ns = os_time_get_nano() + ws->deferred.locked_interval_ns;
   list_addtail(&bo->pool_link, &ws->deferred.locked);
   ws->deferred.locked_bytes += bo->base.size;
   p_atomic_inc(&ws->deferred.locked_count);
   simple_mtx_unlock(&ws->deferred.lock);
}

/* Unlock, evict, free the VA and destroy the allocation, now. The unlock comes first: a BO the host keeps
 * locked is kept whole (radv_wddm2_bo_keep_locked), not half released. */
static void
radv_wddm2_bo_destroy_now(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo)
{
   ASSERTED NTSTATUS status;

   if (!radv_wddm2_bo_unlock(bo)) {
      if (!bo->borrowed) {
         radv_wddm2_bo_keep_locked(ws, bo);
         return;
      }
      /* A host import: the allocation and its lock are the host's, which releases both with the
       * allocation. The ICD only forgets its pointer. */
      bo->map = NULL;
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
         amdgpu_wddm_log("*****  Evict failed\n");
         return;
      }
   }

   if (bo->borrowed) {
      radv_wddm2_bo_account(ws, bo, false);
      radv_wddm2_bo_struct_free(ws, bo);
      return;
   }
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
      if (NT_SUCCESS(status))
         radv_wddm2_bo_account(ws, bo, false);
      else
         amdgpu_wddm_log("radv: DestroyAllocation2 failed; retaining allocation byte charge\n");

      if (ws->debug_all_bos)
         radv_winsys_bo_list_del(&ws->global_bo_list, &bo->base);
      if (ws->debug_log_bos)
         radv_winsys_log_bo(&ws->bo_log, &bo->base, true);
   }

   //radv_wddm2_bo_va_free(ws, bo->flags, bo->base.va, bo->base.size);

   radv_wddm2_bo_struct_free(ws, bo);
}

/* Deferred destruction (amdgpu-wddm). The amdgpu kernel keeps a freed BO, its pages and its VA mapping
 * until the fences of the work that used it signal, so a Vulkan-level free of memory still in use is
 * harmless on Linux. radv_wddm2_bo_destroy_now evicts, frees the VA and destroys the allocation at once,
 * and the OS may hand either to the next allocation while the GPU still uses them. Resident BOs are in
 * no command stream's buffer list, so the work that uses a BO is unknown: a destroyed BO instead waits
 * for the progress value published on every queue at its destroy, which covers all work in flight then.
 * The check reads the fences' CPU values, with no kernel call, at every submission, BO creation and BO
 * destruction, and at a queue's idle point. A held BO stays in the winsys's byte accounting (the memory
 * budget) until its release. Over a cap of held bytes (BC250_DEFERRED_CAP_MB, 512 by default, 0 for
 * none), and when an allocation fails for want of device memory, the CPU waits for the oldest held BO's
 * work (WaitForSynchronizationObjectFromCpu) and releases what retired. Teardown waits for all of it.
 * Borrowed BOs are the host's, which retires them itself. BC250_DEFERRED_DESTROY=0 turns this off.
 *
 * The witness, which needs no fault: every submission stamps the BOs it names with its queue and the
 * progress value it signals (radv_wddm2_cs.c), and a destroy whose stamp has not retired counts as
 * destroyed in flight, by class. The first RADV_WDDM2_WITNESS_LINES such destroys are logged with
 * their stack, totals at powers of two, and a finish line at teardown. BC250_DEFERRED_WITNESS=0 turns
 * it off. Application memory that a submission reaches only through addresses (descriptor buffers,
 * buffer device addresses) is in no BO set: the witness cannot see its use, the hold covers it anyway.
 *
 * The log: nothing is written unless BC250_DEFERRED_LOG names a file. There is no default path any
 * more: the driver that an application gets writes into no directory of its own, and a line asked for
 * is written through and flushed, and goes to stderr as well with AMDGPU_WDDM_LOG=stderr
 * (amdgpu_wddm_log). The submission-path knobs are the host's (struct bc250_host_policy version 2);
 * every other knob, and every knob the host did not name, is read from the environment alone.
 */
struct radv_wddm2_deferred_bo {
   struct list_head link;
   struct radv_wddm2_bo *bo;
   uint64_t since_ns;
   unsigned wait_count;
   struct {
      struct radv_wddm2_tracker *tracker;
      uint64_t value;
   } waits[];
};

static bool
radv_wddm2_deferred_debugger_lines(void)
{
   static int mode = -1; /* benign race: every thread computes the same value */
   if (mode < 0) {
      const char *value = getenv("AMDGPU_WDDM_DDI_TRACE");
      mode = value && !strcmp(value, "2");
   }
   return mode == 1;
}

static int32_t radv_wddm2_deferred_line_budget = 256;

static simple_mtx_t radv_wddm2_deferred_log_mtx = SIMPLE_MTX_INITIALIZER;
static bool radv_wddm2_deferred_log_opened;
static char radv_wddm2_deferred_log_knob[512];
static char radv_wddm2_deferred_log_name[512] = "(none)";
#ifdef _WIN32
static HANDLE radv_wddm2_deferred_log_file = INVALID_HANDLE_VALUE;

static HANDLE
radv_wddm2_deferred_log_try(const char *path)
{
   return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
}
#endif

static void
radv_wddm2_deferred_log_open_locked(void)
{
   if (radv_wddm2_deferred_log_opened)
      return;
   radv_wddm2_deferred_log_opened = true;
#ifdef _WIN32
   /* A file is written only where BC250_DEFERRED_LOG names one. An installed driver creates no file of its
    * own: until 2026-10-03 this opened C:\BC250\tmp\amdgpu_wddm_radv-deferred-<pid>.log on every bc250 winsys
    * and fell back to %TEMP%, so every process that loaded the ICD, dwm.exe included, left a write-through
    * flushed log behind in a hardcoded lab directory. The witness still runs and its lines still reach
    * amdgpu_wddm_log and, under AMDGPU_WDDM_DDI_TRACE=2, the debugger; they just have nowhere to land by
    * default. %s of the name is "(none)" then, which the header line prints. */
   char path[512];
   HANDLE file = INVALID_HANDLE_VALUE;
   if (radv_wddm2_deferred_log_knob[0]) {
      /* %p in the name is this process's id, so one setting gives a game and its helper a file each, the
       * way the old hardcoded name did. Without it they would share one file and interleave. */
      const char *mark = strstr(radv_wddm2_deferred_log_knob, "%p");
      if (mark)
         snprintf(path, sizeof(path), "%.*s%lu%s", (int)(mark - radv_wddm2_deferred_log_knob),
                  radv_wddm2_deferred_log_knob, (unsigned long)GetCurrentProcessId(), mark + 2);
      else
         snprintf(path, sizeof(path), "%s", radv_wddm2_deferred_log_knob);
      file = radv_wddm2_deferred_log_try(path);
   }
   if (file != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER zero = {0};
      SetFilePointerEx(file, zero, NULL, FILE_END);
      snprintf(radv_wddm2_deferred_log_name, sizeof(radv_wddm2_deferred_log_name), "%s", path);
   }
   radv_wddm2_deferred_log_file = file;
#endif
}

/* The log file's name; opens it on first use. */
static const char *
radv_wddm2_deferred_log_path(void)
{
   simple_mtx_lock(&radv_wddm2_deferred_log_mtx);
   radv_wddm2_deferred_log_open_locked();
   simple_mtx_unlock(&radv_wddm2_deferred_log_mtx);
   return radv_wddm2_deferred_log_name;
}

/* The log file when BC250_DEFERRED_LOG named one (by default there is none), stderr with
 * AMDGPU_WDDM_LOG=stderr; with AMDGPU_WDDM_DDI_TRACE=2 also the debugger, at most 256 lines. */
static void
radv_wddm2_deferred_line(const char *format, ...)
{
   char text[3584];
   va_list args;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   amdgpu_wddm_log("bc250: deferred destroy: %s\n", text);
#ifdef _WIN32
   char line[3840];
   SYSTEMTIME utc;
   LARGE_INTEGER now;
   GetSystemTime(&utc);
   QueryPerformanceCounter(&now);
   int len = snprintf(line, sizeof(line), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ qpc=%lld tid=%lu deferred destroy: %s\n",
                      utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
                      (long long)now.QuadPart, (unsigned long)GetCurrentThreadId(), text);
   if (len < 0)
      return;
   if (len >= (int)sizeof(line)) {
      len = sizeof(line) - 1;
      line[len - 1] = '\n';
   }
   simple_mtx_lock(&radv_wddm2_deferred_log_mtx);
   radv_wddm2_deferred_log_open_locked();
   if (radv_wddm2_deferred_log_file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(radv_wddm2_deferred_log_file, line, (DWORD)len, &written, NULL);
      FlushFileBuffers(radv_wddm2_deferred_log_file);
   }
   simple_mtx_unlock(&radv_wddm2_deferred_log_mtx);
   if (radv_wddm2_deferred_debugger_lines() && p_atomic_dec_return(&radv_wddm2_deferred_line_budget) >= 0) {
      char debugger[3840];
      snprintf(debugger, sizeof(debugger), "amdgpu_wddm_radv deferred destroy: %s qpc=%lld thread=%lu\n", text,
               (long long)now.QuadPart, (unsigned long)GetCurrentThreadId());
      OutputDebugStringA(debugger);
   }
#endif
}

/* The knobs. The host decides the submission-path ones in its own binary (struct bc250_host_policy,
 * version 2); for every knob the host did not name, the environment is read, so a bisect needs no new
 * binary. The machine-wide file C:\BC250\tmp\amdgpu_wddm_radv.cfg is gone: every process that loaded
 * this ICD read it, dwm.exe on the GPU desktop route included, and no trial ever recorded its contents,
 * so it could change a measured run invisibly. */
static const char *
radv_wddm2_knob(const char *key, const char **source)
{
   const char *env = getenv(key);
   if (env && *env) {
      *source = "env";
      return env;
   }
   *source = "default";
   return NULL;
}

/* The witness's classes: which RADV owner the creation looks like (radv_radeon_winsys.h priorities). */
enum radv_wddm2_bo_class {
   RADV_WDDM2_CLASS_VIRTUAL,
   RADV_WDDM2_CLASS_COMMAND_STREAM,
   RADV_WDDM2_CLASS_SHADER_RING,
   RADV_WDDM2_CLASS_CPU_UPLOAD,
   RADV_WDDM2_CLASS_DESCRIPTOR_POOL,
   RADV_WDDM2_CLASS_QUERY_POOL,
   RADV_WDDM2_CLASS_APPLICATION,
   RADV_WDDM2_CLASS_OTHER,
};
_Static_assert(RADV_WDDM2_CLASS_OTHER + 1 == RADV_WDDM2_BO_CLASSES, "the witness counts every class");

static const char *const radv_wddm2_bo_class_names[RADV_WDDM2_BO_CLASSES] = {
   "virtual", "command-stream", "shader-ring", "cpu-upload", "descriptor-pool", "query-pool", "application", "other",
};

static enum radv_wddm2_bo_class
radv_wddm2_bo_class_of(const struct radv_wddm2_bo *bo)
{
   const uint32_t f = bo->flags;
   if (bo->base.is_virtual)
      return RADV_WDDM2_CLASS_VIRTUAL;
   /* radv_winsys_cs_bo_create */
   if (bo->priority == RADV_BO_PRIORITY_CS && (f & RADEON_FLAG_GL2_BYPASS) && (f & RADEON_FLAG_READ_ONLY))
      return RADV_WDDM2_CLASS_COMMAND_STREAM;
   /* radv_shader.c arenas, radv_queue.c rings and scratch */
   if (bo->priority == RADV_BO_PRIORITY_SHADER)
      return RADV_WDDM2_CLASS_SHADER_RING;
   /* radv_cmd_buffer.c upload BOs, fence and EOP BOs, the shader DMA BO */
   if (bo->priority == RADV_BO_PRIORITY_UPLOAD_BUFFER && (f & RADEON_FLAG_CPU_ACCESS) && (f & RADEON_FLAG_GTT_WC))
      return RADV_WDDM2_CLASS_CPU_UPLOAD;
   /* radv_descriptor_pool.c */
   if (bo->priority == RADV_BO_PRIORITY_DESCRIPTOR && (f & RADEON_FLAG_READ_ONLY) && !(f & RADEON_FLAG_CPU_ACCESS))
      return RADV_WDDM2_CLASS_DESCRIPTOR_POOL;
   if (bo->priority == RADV_BO_PRIORITY_QUERY_POOL)
      return RADV_WDDM2_CLASS_QUERY_POOL;
   /* application memory, and radv_device.c's zero BO (priority 0) */
   if (bo->priority <= RADV_BO_PRIORITY_APPLICATION_MAX)
      return RADV_WDDM2_CLASS_APPLICATION;
   return RADV_WDDM2_CLASS_OTHER;
}

static void
radv_wddm2_flag_names(uint32_t flags, char *out, size_t size)
{
   static const char *const names[] = {
      "GTT_WC",       "CPU_ACCESS",   "NO_CPU_ACCESS",   "VIRTUAL",       "GL2_BYPASS",       "IMPLICIT_SYNC",
      "NO_INTERPROCESS_SHARING",      "READ_ONLY",       "32BIT",         "PREFER_LOCAL_BO",  "REPLAYABLE",
      "DISCARDABLE",  "GFX12_ALLOW_DCC", "VM_UPDATE_WAIT", "VM_PAD_1PAGE", "ENCRYPTED",
      "EMULATE_SPARSE_RESIDENCY",
   };
   size_t used = 0;
   out[0] = 0;
   for (unsigned i = 0; i < 32; i++) {
      if (!(flags & (1u << i)))
         continue;
      char unknown[16];
      const char *name = unknown;
      if (i < ARRAY_SIZE(names))
         name = names[i];
      else
         snprintf(unknown, sizeof(unknown), "bit%u", i);
      const int n = snprintf(out + used, size - used, "%s%s", used ? "|" : "", name);
      if (n < 0 || (size_t)n >= size - used)
         break;
      used += n;
   }
}

/* module+offset per frame, module base names only (no paths). */
static void
radv_wddm2_symbolize(void *const *frames, unsigned n, char *out, size_t size)
{
   size_t used = 0;
   out[0] = 0;
   for (unsigned i = 0; i < n; i++) {
      char name[260] = "?";
      uintptr_t base = 0;
#ifdef _WIN32
      HMODULE module = NULL;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCSTR)frames[i], &module) &&
          module) {
         char path[MAX_PATH];
         const DWORD len = GetModuleFileNameA(module, path, sizeof(path));
         if (len && len < sizeof(path)) {
            const char *slash = strrchr(path, '\\');
            snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
         }
         base = (uintptr_t)module;
      }
#endif
      const int k = snprintf(out + used, size - used, "%s%s+0x%" PRIxPTR, i ? " " : "", name,
                             (uintptr_t)frames[i] - base);
      if (k < 0 || (size_t)k >= size - used)
         break;
      used += k;
   }
}

/* Physical bytes a held BO keeps; a virtual BO keeps only its VA reservation. */
static uint64_t
radv_wddm2_held_bytes(const struct radv_wddm2_bo *bo)
{
   return bo->base.is_virtual ? 0 : bo->base.size;
}

static uint64_t
radv_wddm2_ms(uint64_t ns)
{
   return ns / 1000000u;
}

struct radv_wddm2_tracker *
radv_wddm2_tracker_attach(struct radv_wddm2_winsys *ws, uint32_t fence, const uint64_t *value_map, uint32_t context)
{
   if ((!ws->deferred.enabled && !ws->deferred.witness) || !value_map)
      return NULL;
   struct radv_wddm2_tracker *tracker = CALLOC_STRUCT(radv_wddm2_tracker);
   if (!tracker) {
      radv_wddm2_deferred_line("no memory for the tracker of context 0x%x: its work is not waited for", context);
      return NULL;
   }
   tracker->value_map = value_map;
   tracker->fence = fence;
   tracker->context = context;
   tracker->refs = 1;
   tracker->attached = true;
   simple_mtx_lock(&ws->deferred.lock);
   tracker->serial = ++ws->deferred.next_serial;
   if (!tracker->serial) /* 0 means "no submission" in a stamp */
      tracker->serial = ++ws->deferred.next_serial;
   list_addtail(&tracker->link, &ws->deferred.trackers);
   simple_mtx_unlock(&ws->deferred.lock);
   return tracker;
}

static void
radv_wddm2_tracker_unref_locked(struct radv_wddm2_tracker *tracker)
{
   assert(tracker->refs > 0);
   if (--tracker->refs == 0) {
      list_del(&tracker->link);
      FREE(tracker);
   }
}

void
radv_wddm2_tracker_detach(struct radv_wddm2_winsys *ws, struct radv_wddm2_tracker *tracker, bool retired)
{
   if (!tracker)
      return;
   simple_mtx_lock(&ws->deferred.lock);
   assert(tracker->attached);
   /* A CPU wait of the cap, of an allocation or of teardown may be using the fence: the fence goes only
    * after that wait. Every value the queue signalled retired, so that wait returns at once. */
   while (retired && tracker->waiters) {
      simple_mtx_unlock(&ws->deferred.lock);
      os_time_sleep(100);
      simple_mtx_lock(&ws->deferred.lock);
   }
   /* Retired: the fence may go right after this; nothing reads it again. Otherwise it stays with the
    * device, and held BOs keep polling it. */
   if (retired)
      tracker->value_map = NULL;
   tracker->attached = false;
   radv_wddm2_tracker_unref_locked(tracker);
   simple_mtx_unlock(&ws->deferred.lock);
}

/* A lost device reads UINT64_MAX, above every value: the GPU runs nothing any more. */
static bool
radv_wddm2_tracker_retired_locked(const struct radv_wddm2_tracker *tracker, uint64_t value)
{
   return !tracker->value_map || p_atomic_read(tracker->value_map) >= value;
}

/* Moves the held BOs whose waits all retired (all: every held BO) to ready. The entries are in destroy
 * order and a queue's published value only grows: a later entry waits on the queue that holds an
 * earlier one, for a value at least as large, so the first entry still held ends the scan. */
static void
radv_wddm2_deferred_take_locked(struct radv_wddm2_winsys *ws, struct list_head *ready, bool all)
{
   const uint64_t now = os_time_get_nano();
   list_for_each_entry_safe (struct radv_wddm2_deferred_bo, e, &ws->deferred.entries, link) {
      bool retired = true;
      for (unsigned i = 0; i < e->wait_count && retired; i++)
         retired = radv_wddm2_tracker_retired_locked(e->waits[i].tracker, e->waits[i].value);
      if (!retired && !all)
         break;
      if (!retired)
         ws->deferred.forced++;
      for (unsigned i = 0; i < e->wait_count; i++)
         radv_wddm2_tracker_unref_locked(e->waits[i].tracker);
      e->wait_count = 0;
      const uint64_t held = now - e->since_ns;
      ws->deferred.max_hold_ns = MAX2(ws->deferred.max_hold_ns, held);
      p_atomic_set(&ws->deferred.count, ws->deferred.count - 1);
      ws->deferred.bytes -= radv_wddm2_held_bytes(e->bo);
      ws->deferred.released++;
      list_del(&e->link);
      list_addtail(&e->link, ready);
      if (held >= ws->deferred.report_hold_ns) {
         radv_wddm2_deferred_line("a %" PRIu64 "-byte BO was held %" PRIu64 " ms%s (%u BOs, %" PRIu64
                                  " MiB still held)",
                                  e->bo->base.size, radv_wddm2_ms(held), retired ? "" : ", destroyed unretired",
                                  ws->deferred.count, ws->deferred.bytes >> 20);
         while (ws->deferred.report_hold_ns <= held)
            ws->deferred.report_hold_ns *= 2;
      }
   }
}

static void
radv_wddm2_deferred_destroy_list(struct radv_wddm2_winsys *ws, struct list_head *ready)
{
   list_for_each_entry_safe (struct radv_wddm2_deferred_bo, e, ready, link) {
      list_del(&e->link);
      radv_wddm2_bo_destroy_now(ws, e->bo);
      FREE(e);
   }
}

/* BD-045: destroys the kept BOs again, which retries their unlock; one that still fails is kept again.
 * At most once per locked_interval_ns, unless final (teardown). */
static void
radv_wddm2_locked_retry(struct radv_wddm2_winsys *ws, bool final)
{
   struct list_head kept;
   list_inithead(&kept);
   simple_mtx_lock(&ws->deferred.lock);
   const uint64_t now = os_time_get_nano();
   if (final || now >= ws->deferred.locked_next_ns) {
      list_splicetail(&ws->deferred.locked, &kept);
      list_inithead(&ws->deferred.locked);
      ws->deferred.locked_bytes = 0;
      p_atomic_set(&ws->deferred.locked_count, 0);
      ws->deferred.locked_next_ns = now + ws->deferred.locked_interval_ns;
   }
   simple_mtx_unlock(&ws->deferred.lock);
   list_for_each_entry_safe (struct radv_wddm2_bo, bo, &kept, pool_link) {
      list_del(&bo->pool_link);
      radv_wddm2_bo_destroy_now(ws, bo);
   }
}

void
radv_wddm2_deferred_drain(struct radv_wddm2_winsys *ws)
{
   if (p_atomic_read(&ws->deferred.locked_count))
      radv_wddm2_locked_retry(ws, false);
   if (!p_atomic_read(&ws->deferred.count))
      return;
   struct list_head ready;
   list_inithead(&ready);
   simple_mtx_lock(&ws->deferred.lock);
   radv_wddm2_deferred_take_locked(ws, &ready, false);
   simple_mtx_unlock(&ws->deferred.lock);
   radv_wddm2_deferred_destroy_list(ws, &ready);
}

/* One CPU wait of a held BO on one queue's fence, with references that keep the tracker and its fence. */
struct radv_wddm2_cpu_wait {
   struct radv_wddm2_tracker *tracker;
   uint32_t fence;
   const uint64_t *value_map;
   uint64_t value;
};

#define RADV_WDDM2_CPU_WAITS 16u

/* The waits of e that have not retired, at most RADV_WDDM2_CPU_WAITS (a later round takes the rest). */
static unsigned
radv_wddm2_cpu_waits_locked(struct radv_wddm2_deferred_bo *e, struct radv_wddm2_cpu_wait *out)
{
   unsigned n = 0;
   for (unsigned i = 0; i < e->wait_count && n < RADV_WDDM2_CPU_WAITS; i++) {
      struct radv_wddm2_tracker *tracker = e->waits[i].tracker;
      if (radv_wddm2_tracker_retired_locked(tracker, e->waits[i].value) || !tracker->fence)
         continue;
      tracker->refs++;
      tracker->waiters++;
      out[n++] = (struct radv_wddm2_cpu_wait){tracker, tracker->fence, tracker->value_map, e->waits[i].value};
   }
   return n;
}

/* Without the lock. False if a wait failed or timed out. */
static bool
radv_wddm2_cpu_waits(struct radv_wddm2_winsys *ws, const struct radv_wddm2_cpu_wait *waits, unsigned n)
{
   bool ok = true;
   for (unsigned i = 0; i < n; i++)
      ok &= radv_wddm2_fence_wait_value(ws, waits[i].fence, waits[i].value_map, waits[i].value);
   simple_mtx_lock(&ws->deferred.lock);
   for (unsigned i = 0; i < n; i++) {
      waits[i].tracker->waiters--;
      radv_wddm2_tracker_unref_locked(waits[i].tracker);
   }
   simple_mtx_unlock(&ws->deferred.lock);
   return ok;
}

/* CPU-waits for the oldest held BO's work, then destroys every held BO that retired. False when
 * nothing was held, or a wait failed, or nothing was released. */
static bool
radv_wddm2_deferred_wait_oldest(struct radv_wddm2_winsys *ws, uint64_t *waited_ns)
{
   struct radv_wddm2_cpu_wait waits[RADV_WDDM2_CPU_WAITS];
   *waited_ns = 0;
   simple_mtx_lock(&ws->deferred.lock);
   if (list_is_empty(&ws->deferred.entries)) {
      simple_mtx_unlock(&ws->deferred.lock);
      return false;
   }
   struct radv_wddm2_deferred_bo *oldest = list_first_entry(&ws->deferred.entries, struct radv_wddm2_deferred_bo, link);
   const unsigned n = radv_wddm2_cpu_waits_locked(oldest, waits);
   const uint64_t released = ws->deferred.released;
   simple_mtx_unlock(&ws->deferred.lock);

   const uint64_t start = os_time_get_nano();
   const bool ok = radv_wddm2_cpu_waits(ws, waits, n);
   *waited_ns = os_time_get_nano() - start;
   radv_wddm2_deferred_drain(ws);
   simple_mtx_lock(&ws->deferred.lock);
   const bool progress = ws->deferred.released != released;
   simple_mtx_unlock(&ws->deferred.lock);
   return ok && progress;
}

/* Over the cap: CPU-wait for the oldest held BO and release what retired, until the held bytes fit. */
static void
radv_wddm2_deferred_bound(struct radv_wddm2_winsys *ws)
{
   for (;;) {
      simple_mtx_lock(&ws->deferred.lock);
      const uint64_t bytes = ws->deferred.bytes;
      simple_mtx_unlock(&ws->deferred.lock);
      if (!ws->deferred.cap_bytes || bytes <= ws->deferred.cap_bytes)
         return;
      uint64_t waited;
      const bool progress = radv_wddm2_deferred_wait_oldest(ws, &waited);
      simple_mtx_lock(&ws->deferred.lock);
      ws->deferred.cap_waits++;
      ws->deferred.cap_wait_ns += waited;
      ws->deferred.cap_wait_max_ns = MAX2(ws->deferred.cap_wait_max_ns, waited);
      if (!progress)
         ws->deferred.cap_failed++;
      if (!progress || ws->deferred.cap_waits >= ws->deferred.report_cap_waits) {
         radv_wddm2_deferred_line("cap: %" PRIu64 " MiB held over the %" PRIu64 " MiB cap; a CPU wait of %" PRIu64
                                  " us for the oldest held BO %s (%" PRIu64 " MiB now; %" PRIu64
                                  " cap waits, %" PRIu64 " ms in all, longest %" PRIu64 " ms, %" PRIu64 " failed)",
                                  bytes >> 20, ws->deferred.cap_bytes >> 20, waited / 1000u,
                                  progress ? "released it" : "failed or released nothing", ws->deferred.bytes >> 20,
                                  ws->deferred.cap_waits, radv_wddm2_ms(ws->deferred.cap_wait_ns),
                                  radv_wddm2_ms(ws->deferred.cap_wait_max_ns), ws->deferred.cap_failed);
         while (ws->deferred.report_cap_waits <= ws->deferred.cap_waits)
            ws->deferred.report_cap_waits *= 2;
      }
      simple_mtx_unlock(&ws->deferred.lock);
      if (!progress)
         return;
   }
}

/* The witness's counters, copied under the lock for a line written after it. */
struct radv_wddm2_witness_totals {
   uint64_t destroys[RADV_WDDM2_BO_CLASSES], held[RADV_WDDM2_BO_CLASSES], in_flight[RADV_WDDM2_BO_CLASSES];
   uint64_t destroys_total, in_flight_total, in_flight_32bit, in_flight_bytes, stale_names;
};

static void
radv_wddm2_witness_totals_locked(const struct radv_wddm2_winsys *ws, struct radv_wddm2_witness_totals *t)
{
   memcpy(t->destroys, ws->deferred.destroys, sizeof(t->destroys));
   memcpy(t->held, ws->deferred.held_by_class, sizeof(t->held));
   memcpy(t->in_flight, ws->deferred.in_flight, sizeof(t->in_flight));
   t->destroys_total = ws->deferred.destroys_total;
   t->in_flight_total = ws->deferred.in_flight_total;
   t->in_flight_32bit = ws->deferred.in_flight_32bit;
   t->in_flight_bytes = ws->deferred.in_flight_bytes;
   t->stale_names = p_atomic_read(&ws->deferred.stale_names);
}

/* class:destroyed/held/in_flight for each class. */
static void
radv_wddm2_witness_classes(const struct radv_wddm2_witness_totals *t, char *classes, size_t size)
{
   size_t used = 0;
   classes[0] = 0;
   for (unsigned c = 0; c < RADV_WDDM2_BO_CLASSES; c++) {
      const int n = snprintf(classes + used, size - used, "%s%s:%" PRIu64 "/%" PRIu64 "/%" PRIu64,
                             c ? " " : "", radv_wddm2_bo_class_names[c], t->destroys[c], t->held[c],
                             t->in_flight[c]);
      if (n < 0 || (size_t)n >= size - used)
         break;
      used += n;
   }
}

static void
radv_wddm2_witness_totals_line(const char *what, const struct radv_wddm2_witness_totals *t)
{
   char classes[1024];
   radv_wddm2_witness_classes(t, classes, sizeof(classes));
   radv_wddm2_deferred_line("witness %s destroys=%" PRIu64 " in_flight=%" PRIu64 " in_flight_32bit=%" PRIu64
                            " in_flight_kib=%" PRIu64 " stale_names=%" PRIu64
                            " classes(destroyed/held/in_flight)=%s",
                            what, t->destroys_total, t->in_flight_total, t->in_flight_32bit, t->in_flight_bytes >> 10,
                            t->stale_names, classes);
}

/* What the witness logs of one destroy, copied under the lock: a held BO may go on another thread. */
struct radv_wddm2_witness_note {
   bool log;
   uint32_t n;
   enum radv_wddm2_bo_class cls;
   uint64_t size, va;
   uint32_t flags, domain, priority, context;
   uint64_t last_use, completed, published;
   bool held;
};

/* Counts one non-borrowed destroy. True if a totals line is due (copied to totals). */
static bool
radv_wddm2_witness_count_locked(struct radv_wddm2_winsys *ws, const struct radv_wddm2_bo *bo, bool held,
                                bool in_flight, struct radv_wddm2_witness_note *note,
                                struct radv_wddm2_witness_totals *totals)
{
   const enum radv_wddm2_bo_class c = radv_wddm2_bo_class_of(bo);
   bool line = false;
   ws->deferred.destroys[c]++;
   ws->deferred.destroys_total++;
   if (held)
      ws->deferred.held_by_class[c]++;
   if (in_flight) {
      ws->deferred.in_flight[c]++;
      ws->deferred.in_flight_total++;
      ws->deferred.in_flight_bytes += bo->base.size;
      if (bo->flags & RADEON_FLAG_32BIT)
         ws->deferred.in_flight_32bit++;
      if (ws->deferred.in_flight_logged < RADV_WDDM2_WITNESS_LINES) {
         note->log = true;
         note->n = ++ws->deferred.in_flight_logged;
         note->cls = c;
         note->size = bo->base.size;
         note->va = bo->base.va;
         note->flags = bo->flags;
         note->domain = bo->base.initial_domain;
         note->priority = bo->priority;
         note->last_use = bo->last_use_value;
         note->held = held;
      }
      if (ws->deferred.in_flight_total >= ws->deferred.report_in_flight) {
         line = true;
         while (ws->deferred.report_in_flight <= ws->deferred.in_flight_total)
            ws->deferred.report_in_flight *= 2;
      }
   }
   if (ws->deferred.destroys_total >= ws->deferred.report_destroys) {
      line = true;
      while (ws->deferred.report_destroys <= ws->deferred.destroys_total)
         ws->deferred.report_destroys *= 2;
   }
   if (line)
      radv_wddm2_witness_totals_locked(ws, totals);
   return line;
}

/* Without the lock, on the destroying thread: the in-flight line with this thread's stack. */
static void
radv_wddm2_witness_lines(const struct radv_wddm2_witness_note *note, const struct radv_wddm2_witness_totals *totals)
{
   if (note->log) {
      char bt[24 * 64] = "";
#ifdef _WIN32
      void *frames[24];
      const unsigned n = RtlCaptureStackBackTrace(0, ARRAY_SIZE(frames), frames, NULL);
      radv_wddm2_symbolize(frames, n, bt, sizeof(bt));
#endif
      char flags[256];
      radv_wddm2_flag_names(note->flags, flags, sizeof(flags));
      radv_wddm2_deferred_line("witness in flight #%u class=%s size=%" PRIu64 " flags=0x%x(%s) domain=0x%x(%s%s) prio=%u"
                               " va=0x%" PRIx64 " 32bit=%s context=0x%x last_use=%" PRIu64 " completed=%" PRIu64
                               " published=%" PRIu64 " held=%s bt=%s",
                               note->n, radv_wddm2_bo_class_names[note->cls], note->size, note->flags, flags,
                               note->domain, (note->domain & RADEON_DOMAIN_VRAM) ? "VRAM" : "",
                               (note->domain & RADEON_DOMAIN_GTT) ? "GTT" : "", note->priority, note->va,
                               (note->flags & RADEON_FLAG_32BIT) ? "yes" : "no", note->context, note->last_use,
                               note->completed, note->published, note->held ? "yes" : "no", bt);
   }
   if (totals)
      radv_wddm2_witness_totals_line("totals", totals);
}

void
radv_wddm2_witness_stale(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *first, uint32_t stale, uint32_t context)
{
   const uint64_t total = p_atomic_add_return(&ws->deferred.stale_names, (uint64_t)stale);
   if (p_atomic_inc_return(&ws->deferred.stale_logged) > 16)
      return;
   /* first may be reused meanwhile; the pool keeps it a BO struct. */
   char flags[256];
   radv_wddm2_flag_names(first->flags, flags, sizeof(flags));
   radv_wddm2_deferred_line("witness stale name: a submission on context 0x%x names %u destroyed BOs (%" PRIu64
                            " so far); the first: class=%s size=%" PRIu64 " flags=0x%x(%s) prio=%u va=0x%" PRIx64,
                            context, stale, total, radv_wddm2_bo_class_names[radv_wddm2_bo_class_of(first)],
                            first->base.size, (unsigned)first->flags, flags, first->priority, first->base.va);
}

/* The periodic summary. The lines above are written as counters cross thresholds that double (256 BOs
 * or 256 MiB held, a 1 s hold, 1024 BOs held or destroyed, 64 in-flight destroys), and the summary at
 * teardown; a session that crosses none and is killed at its end writes the first held BO only
 * (session 219: the game device's header, then one line in its five minutes). So every
 * BC250_DEFERRED_SUMMARY_S seconds (30 by default, 0 off) a submission writes two lines, each only
 * when its counters changed since its last one, both again at teardown: the deferred-destroy counters (held now and at the peak, held and released since
 * creation, destroys, host imports, the witness's in-flight destroys by class, stale names, cap waits,
 * allocation retries) and the submit path's (submissions, progress signals in their own call or
 * merged or written by the GPU, application signals, waits queued and left out, gather-slot waits, queues
 * that fell back to the kernel signal). Every count is since the
 * winsys was created; t= is the time since then. */
struct radv_wddm2_summary_deferred {
   uint64_t held, held_bytes, peak, peak_bytes, total, total_bytes, released, immediate, max_hold_ns, forced;
   uint64_t borrowed, cap_waits, cap_failed, cap_wait_ns, retries, oom_waits;
   struct radv_wddm2_witness_totals witness;
};

struct radv_wddm2_summary_submit {
   uint64_t submits, progress_separate, progress_merged, signal_calls, signal_objects;
   uint64_t wait_objects, wait_dropped, wait_calls, wait_skipped, gather_waits, gather_wait_ns, gather_wait_max_ns;
   uint64_t progress_gpu, kernel_queues;
};

_Static_assert(sizeof(struct radv_wddm2_summary_deferred) <= sizeof(((struct radv_wddm2_winsys *)0)->summary.deferred_snapshot),
              "the deferred snapshot fits");
_Static_assert(sizeof(struct radv_wddm2_summary_submit) <= sizeof(((struct radv_wddm2_winsys *)0)->summary.submit_snapshot),
              "the submit snapshot fits");

/* BC250_DRAW_STATS: radeon_winsys::draw_stats_add, called at the end of every command buffer the
 * application records. */
static void
radv_wddm2_draw_stats_add(struct radeon_winsys *_ws, const uint32_t counts[RADV_DRAW_STAT_COUNT])
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT; i++) {
      if (counts[i])
         p_atomic_add(&ws->draw_stats.totals[i], (uint64_t)counts[i]);
   }
}

static const char *const radv_wddm2_draw_stat_names[RADV_DRAW_STAT_COUNT] = {
#define RADV_DRAW_STAT_NAME(name) #name,
   RADV_DRAW_STATS(RADV_DRAW_STAT_NAME)
#undef RADV_DRAW_STAT_NAME
};

static const char *
radv_wddm2_coalesce_name(const struct radv_wddm2_winsys *ws)
{
   return ws->bc250_merge_signals ? (ws->bc250_drop_waits ? "on" : "signals") : (ws->bc250_drop_waits ? "waits" : "off");
}

void
radv_wddm2_summary_write(struct radv_wddm2_winsys *ws, uint64_t now, uint64_t due, bool final)
{
   /* One writer per period: every other submission that finds the deadline passed goes on. */
   if (!final && p_atomic_cmpxchg(&ws->summary.next_ns, due, now + ws->summary.interval_ns) != due)
      return;

   struct radv_wddm2_summary_deferred d;
   memset(&d, 0, sizeof(d));
   simple_mtx_lock(&ws->deferred.lock);
   d.held = ws->deferred.count;
   d.held_bytes = ws->deferred.bytes;
   d.peak = ws->deferred.peak_count;
   d.peak_bytes = ws->deferred.peak_bytes;
   d.total = ws->deferred.total;
   d.total_bytes = ws->deferred.total_bytes;
   d.released = ws->deferred.released;
   d.immediate = ws->deferred.immediate;
   d.max_hold_ns = ws->deferred.max_hold_ns;
   d.forced = ws->deferred.forced;
   d.borrowed = p_atomic_read(&ws->deferred.borrowed);
   d.cap_waits = ws->deferred.cap_waits;
   d.cap_failed = ws->deferred.cap_failed;
   d.cap_wait_ns = ws->deferred.cap_wait_ns;
   d.retries = ws->deferred.retries;
   d.oom_waits = ws->deferred.oom_waits;
   radv_wddm2_witness_totals_locked(ws, &d.witness);
   simple_mtx_unlock(&ws->deferred.lock);

   struct radv_wddm2_summary_submit s = {
      .submits = p_atomic_read(&ws->submit_stats.submits),
      .progress_separate = p_atomic_read(&ws->submit_stats.progress_separate),
      .progress_merged = p_atomic_read(&ws->submit_stats.progress_merged),
      .signal_calls = p_atomic_read(&ws->submit_stats.signal_calls),
      .signal_objects = p_atomic_read(&ws->submit_stats.signal_objects),
      .wait_objects = p_atomic_read(&ws->submit_stats.wait_objects),
      .wait_dropped = p_atomic_read(&ws->submit_stats.wait_dropped),
      .wait_calls = p_atomic_read(&ws->submit_stats.wait_calls),
      .wait_skipped = p_atomic_read(&ws->submit_stats.wait_skipped),
      .gather_waits = p_atomic_read(&ws->submit_stats.gather_waits),
      .gather_wait_ns = p_atomic_read(&ws->submit_stats.gather_wait_ns),
      .gather_wait_max_ns = p_atomic_read(&ws->submit_stats.gather_wait_max_ns),
      .progress_gpu = p_atomic_read(&ws->submit_stats.progress_gpu),
      .kernel_queues = p_atomic_read(&ws->submit_stats.kernel_queues),
   };

   uint64_t draw[RADV_DRAW_STAT_COUNT];
   for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT; i++)
      draw[i] = p_atomic_read(&ws->draw_stats.totals[i]);

   static const struct radv_wddm2_summary_deferred no_deferred;
   static const struct radv_wddm2_summary_submit no_submit;
   static const uint64_t no_draw[RADV_DRAW_STAT_COUNT];
   simple_mtx_lock(&ws->summary.lock);
   /* Teardown writes what was ever counted; a period, what changed since its last line. */
   const bool write_d = memcmp(&d, final ? (const void *)&no_deferred : ws->summary.deferred_snapshot, sizeof(d)) != 0;
   const bool write_s = memcmp(&s, final ? (const void *)&no_submit : ws->summary.submit_snapshot, sizeof(s)) != 0;
   const bool write_draw = ws->base.draw_stats_add &&
                           memcmp(draw, final ? no_draw : ws->draw_stats.snapshot, sizeof(draw)) != 0;
   if (write_d || write_s || write_draw) {
      char tag[32];
      if (final)
         snprintf(tag, sizeof(tag), "final");
      else
         snprintf(tag, sizeof(tag), "#%u", ++ws->summary.lines);
      const uint64_t t = (now - ws->summary.start_ns) / 1000000000u;
      if (write_d) {
         char classes[1024];
         radv_wddm2_witness_classes(&d.witness, classes, sizeof(classes));
         radv_wddm2_deferred_line("periodic %s t=%" PRIu64 "s deferred: held=%" PRIu64 " held_kib=%" PRIu64
                                  " peak=%" PRIu64 " peak_kib=%" PRIu64 " held_total=%" PRIu64
                                  " held_total_mib=%" PRIu64 " released=%" PRIu64 " immediate=%" PRIu64
                                  " longest_hold_ms=%" PRIu64 " forced=%" PRIu64 " destroys=%" PRIu64
                                  " borrowed=%" PRIu64 " in_flight=%" PRIu64 " in_flight_32bit=%" PRIu64
                                  " in_flight_kib=%" PRIu64 " stale_names=%" PRIu64 " cap_waits=%" PRIu64
                                  " cap_failed=%" PRIu64 " cap_wait_ms=%" PRIu64 " retries=%" PRIu64
                                  " oom_waits=%" PRIu64 " classes(destroyed/held/in_flight)=%s",
                                  tag, t, d.held, d.held_bytes >> 10, d.peak, d.peak_bytes >> 10, d.total,
                                  d.total_bytes >> 20, d.released, d.immediate, radv_wddm2_ms(d.max_hold_ns), d.forced,
                                  d.witness.destroys_total, d.borrowed, d.witness.in_flight_total,
                                  d.witness.in_flight_32bit, d.witness.in_flight_bytes >> 10, d.witness.stale_names,
                                  d.cap_waits, d.cap_failed, radv_wddm2_ms(d.cap_wait_ns), d.retries, d.oom_waits,
                                  classes);
         memcpy(ws->summary.deferred_snapshot, &d, sizeof(d));
      }
      if (write_s) {
         radv_wddm2_deferred_line("periodic %s t=%" PRIu64 "s submit: submits=%" PRIu64 " progress_separate=%" PRIu64
                                  " progress_merged=%" PRIu64 " signal_calls=%" PRIu64 " signal_objects=%" PRIu64
                                  " wait_objects=%" PRIu64 " wait_dropped=%" PRIu64 " wait_calls=%" PRIu64
                                  " wait_skipped=%" PRIu64 " gather_slots=%u gather_waits=%" PRIu64
                                  " gather_wait_ms=%" PRIu64 " gather_wait_max_us=%" PRIu64 " coalesce=%s"
                                  " progress_fence=%s progress_gpu=%" PRIu64 " kernel_queues=%" PRIu64,
                                  tag, t, s.submits, s.progress_separate, s.progress_merged, s.signal_calls,
                                  s.signal_objects, s.wait_objects, s.wait_dropped, s.wait_calls, s.wait_skipped,
                                  ws->bc250_gather_slots, s.gather_waits, radv_wddm2_ms(s.gather_wait_ns),
                                  s.gather_wait_max_ns / 1000u, radv_wddm2_coalesce_name(ws),
                                  ws->bc250_progress_gpu ? "gpu" : "kernel", s.progress_gpu, s.kernel_queues);
         memcpy(ws->summary.submit_snapshot, &s, sizeof(s));
      }
      if (write_draw) {
         char counts[2048];
         size_t len = 0;
         for (unsigned i = 0; i < RADV_DRAW_STAT_COUNT && len < sizeof(counts); i++) {
            const int n = snprintf(counts + len, sizeof(counts) - len, " %s=%" PRIu64, radv_wddm2_draw_stat_names[i],
                                   draw[i]);
            if (n < 0)
               break;
            len += (size_t)n;
         }
         radv_wddm2_deferred_line("periodic %s t=%" PRIu64 "s draw:%s", tag, t, counts);
         memcpy(ws->draw_stats.snapshot, draw, sizeof(draw));
      }
   }
   simple_mtx_unlock(&ws->summary.lock);
}

void
radv_wddm2_deferred_finish(struct radv_wddm2_winsys *ws)
{
   const uint64_t start = os_time_get_nano();
   uint64_t waits = 0;
   bool failed = false;
   /* Oldest first: one CPU wait for each held BO still unretired, which releases it and everything
    * older on the same queues. A failed wait ends the waiting. */
   while (p_atomic_read(&ws->deferred.count)) {
      uint64_t waited;
      if (!radv_wddm2_deferred_wait_oldest(ws, &waited)) {
         failed = p_atomic_read(&ws->deferred.count) != 0;
         break;
      }
      waits++;
   }
   const uint64_t waited = os_time_get_nano() - start;

   struct list_head rest;
   list_inithead(&rest);
   struct radv_wddm2_witness_totals totals;
   simple_mtx_lock(&ws->deferred.lock);
   const uint32_t left = ws->deferred.count;
   const uint64_t left_bytes = ws->deferred.bytes;
   radv_wddm2_deferred_take_locked(ws, &rest, true);
   if (left)
      radv_wddm2_deferred_line("%u BOs (%" PRIu64 " MiB) still held after %" PRIu64 " CPU waits (%" PRIu64
                               " ms) at teardown%s: destroyed anyway",
                               left, left_bytes >> 20, waits, radv_wddm2_ms(waited), failed ? ", a wait failed" : "");
   if (ws->deferred.enabled)
      radv_wddm2_deferred_line("summary: %" PRIu64 " BOs held (%" PRIu64 " MiB), %" PRIu64 " destroyed at once; "
                               "peak %u BOs, %" PRIu64 " MiB; longest hold %" PRIu64 " ms; %" PRIu64
                               " destroyed unretired; %" PRIu64 " allocations retried after %" PRIu64
                               " CPU waits; %" PRIu64 " cap waits (%" PRIu64 " ms in all, longest %" PRIu64
                               " ms, %" PRIu64 " failed); teardown: %" PRIu64 " CPU waits, %" PRIu64 " ms; %" PRIu64
                               " host imports destroyed, never held",
                               ws->deferred.total, ws->deferred.total_bytes >> 20, ws->deferred.immediate,
                               ws->deferred.peak_count, ws->deferred.peak_bytes >> 20,
                               radv_wddm2_ms(ws->deferred.max_hold_ns), ws->deferred.forced, ws->deferred.retries,
                               ws->deferred.oom_waits, ws->deferred.cap_waits, radv_wddm2_ms(ws->deferred.cap_wait_ns),
                               radv_wddm2_ms(ws->deferred.cap_wait_max_ns), ws->deferred.cap_failed, waits,
                               radv_wddm2_ms(waited), p_atomic_read(&ws->deferred.borrowed));
   if (ws->deferred.witness)
      radv_wddm2_witness_totals_locked(ws, &totals);
   simple_mtx_unlock(&ws->deferred.lock);
   radv_wddm2_deferred_destroy_list(ws, &rest);

   /* BD-045: one last unlock of the kept BOs. What the host still keeps locked is its to release with the
    * device; the ICD gives up only its structs. */
   radv_wddm2_locked_retry(ws, true);
   simple_mtx_lock(&ws->deferred.lock);
   const uint32_t locked = ws->deferred.locked_count;
   if (locked)
      radv_wddm2_deferred_line("%u BOs (%" PRIu64 " MiB) still CPU-locked at teardown after %" PRIu64
                               " failed unlocks: left to the host",
                               locked, ws->deferred.locked_bytes >> 20, p_atomic_read(&ws->deferred.unlock_failed));
   struct list_head kept;
   list_inithead(&kept);
   list_splicetail(&ws->deferred.locked, &kept);
   list_inithead(&ws->deferred.locked);
   ws->deferred.locked_bytes = 0;
   p_atomic_set(&ws->deferred.locked_count, 0);
   simple_mtx_unlock(&ws->deferred.lock);
   list_for_each_entry_safe (struct radv_wddm2_bo, bo, &kept, pool_link) {
      list_del(&bo->pool_link);
      radv_wddm2_bo_struct_free(ws, bo);
   }

   if (ws->deferred.witness)
      radv_wddm2_witness_totals_line("finish", &totals);
   /* The periodic lines once more, with the teardown's counts: what a session that ends cleanly did. */
   if (ws->summary.interval_ns)
      radv_wddm2_summary_write(ws, os_time_get_nano(), 0, true);
}

static void
radv_wddm2_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   const bool enabled = ws->deferred.enabled;

   if (bo->borrowed || (!enabled && !ws->deferred.witness)) {
      /* A host import is the host's to retire: never held, never witnessed, only counted. */
      if (bo->borrowed)
         p_atomic_inc(&ws->deferred.borrowed);
      radv_wddm2_bo_destroy_now(ws, bo);
      return;
   }

   radv_wddm2_deferred_drain(ws);

   struct radv_wddm2_witness_note note = {0};
   struct radv_wddm2_witness_totals totals;
   simple_mtx_lock(&ws->deferred.lock);
   bo->destroyed = true;
   unsigned trackers = 0;
   list_for_each_entry (struct radv_wddm2_tracker, tracker, &ws->deferred.trackers, link)
      trackers++;
   struct radv_wddm2_deferred_bo *e =
      enabled && trackers ? malloc(sizeof(*e) + trackers * sizeof(e->waits[0])) : NULL;
   unsigned waits = 0;
   bool in_flight = false;
   list_for_each_entry (struct radv_wddm2_tracker, tracker, &ws->deferred.trackers, link) {
      /* The witness: the queue of the BO's last submission has not retired it. */
      if (ws->deferred.witness && bo->last_use_serial == tracker->serial &&
          !radv_wddm2_tracker_retired_locked(tracker, bo->last_use_value)) {
         in_flight = true;
         note.context = tracker->context;
         note.completed = p_atomic_read(tracker->value_map);
         note.published = p_atomic_read(&tracker->published);
      }
      if (!e)
         continue;
      /* Detached trackers stay listed while held BOs name them: a retired one is skipped here, an
       * abandoned one (its queue's work did not retire) is still waited for. */
      const uint64_t value = p_atomic_read(&tracker->published);
      if (!value || radv_wddm2_tracker_retired_locked(tracker, value))
         continue;
      e->waits[waits].tracker = tracker;
      e->waits[waits].value = value;
      tracker->refs++;
      waits++;
   }
   if (enabled && trackers && !e)
      radv_wddm2_deferred_line("no memory to hold a %" PRIu64 "-byte BO: destroyed at once", bo->base.size);
   const bool totals_due =
      ws->deferred.witness && radv_wddm2_witness_count_locked(ws, bo, waits > 0, in_flight, &note, &totals);

   if (!waits) {
      if (enabled)
         ws->deferred.immediate++;
      simple_mtx_unlock(&ws->deferred.lock);
      free(e);
      radv_wddm2_witness_lines(&note, totals_due ? &totals : NULL);
      radv_wddm2_bo_destroy_now(ws, bo);
      return;
   }

   e->bo = bo;
   e->wait_count = waits;
   e->since_ns = os_time_get_nano();
   list_addtail(&e->link, &ws->deferred.entries);
   const uint64_t bytes = radv_wddm2_held_bytes(bo);
   p_atomic_set(&ws->deferred.count, ws->deferred.count + 1);
   ws->deferred.bytes += bytes;
   ws->deferred.total++;
   ws->deferred.total_bytes += bytes;
   ws->deferred.peak_count = MAX2(ws->deferred.peak_count, ws->deferred.count);
   ws->deferred.peak_bytes = MAX2(ws->deferred.peak_bytes, ws->deferred.bytes);
   if (ws->deferred.total == 1)
      radv_wddm2_deferred_line("first BO held: %" PRIu64 " bytes, until %u queues retire their work in flight",
                               bo->base.size, waits);
   if (ws->deferred.count >= ws->deferred.report_count || ws->deferred.bytes >= ws->deferred.report_bytes) {
      radv_wddm2_deferred_line("%u BOs, %" PRIu64 " MiB held (peak %u, %" PRIu64 " MiB; %" PRIu64
                               " held so far; longest hold %" PRIu64 " ms)",
                               ws->deferred.count, ws->deferred.bytes >> 20, ws->deferred.peak_count,
                               ws->deferred.peak_bytes >> 20, ws->deferred.total,
                               radv_wddm2_ms(ws->deferred.max_hold_ns));
      while (ws->deferred.report_count <= ws->deferred.count)
         ws->deferred.report_count *= 2;
      while (ws->deferred.report_bytes <= ws->deferred.bytes)
         ws->deferred.report_bytes *= 2;
   }
   if (ws->deferred.total >= ws->deferred.report_total) {
      radv_wddm2_deferred_line("progress: %" PRIu64 " BOs held so far (%" PRIu64 " MiB), %" PRIu64
                               " destroyed at once; now %u BOs, %" PRIu64 " MiB; peak %u BOs, %" PRIu64
                               " MiB; longest hold %" PRIu64 " ms; %" PRIu64 " allocations retried; %" PRIu64
                               " cap waits",
                               ws->deferred.total, ws->deferred.total_bytes >> 20, ws->deferred.immediate,
                               ws->deferred.count, ws->deferred.bytes >> 20, ws->deferred.peak_count,
                               ws->deferred.peak_bytes >> 20, radv_wddm2_ms(ws->deferred.max_hold_ns),
                               ws->deferred.retries, ws->deferred.cap_waits);
      ws->deferred.report_total *= 2;
   }
   const bool over = ws->deferred.cap_bytes && ws->deferred.bytes > ws->deferred.cap_bytes;
   simple_mtx_unlock(&ws->deferred.lock);
   radv_wddm2_witness_lines(&note, totals_due ? &totals : NULL);
   if (over)
      radv_wddm2_deferred_bound(ws);
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

   if (radv_wddm2_ctx_unbound(ctx))
      return VK_ERROR_VALIDATION_FAILED;

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

   amdgpu_wddm_log("dump bo ranges\n");
   if (ws->debug_all_bos)
      radv_winsys_dump_bo_ranges(&ws->global_bo_list, file);
   else
      fprintf(file, "  To get BO VA ranges, please specify RADV_DEBUG=allbos\n");
}

static VkResult
radv_wddm2_bo_from_hosted(struct radeon_winsys *rws, void *identity, uint32_t allocation, uint32_t flags,
                         uint64_t va, uint64_t size, struct radeon_winsys_bo **out)
{
   struct radv_wddm2_winsys *ws=radv_wddm2_winsys(rws);
   if (!ws->host.dispatch || identity!=ws->host.identity || !allocation || !va || !size ||
       (flags & ~BC250_HOST_IMPORT_KNOWN_FLAGS) ||
       (va & 4095) || (size & 4095) || va>=RADV_WDDM2_PRT_CONTROL_MASK || size>RADV_WDDM2_PRT_CONTROL_MASK-va)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct radv_wddm2_bo *bo=radv_wddm2_bo_struct_alloc(ws);
   if (!bo) return VK_ERROR_OUT_OF_HOST_MEMORY;
   bo->ws=ws; bo->borrowed=true; bo->host_mappable=!!(flags & BC250_HOST_IMPORT_CPU_MAP);
   bo->base.va=va; bo->base.size=size; bo->base.handle=allocation; bo->base.obj_id=allocation;
   bo->base.initial_domain=RADEON_DOMAIN_VRAM;
   radv_wddm2_bo_account(ws, bo, true);
   *out=&bo->base;
   return VK_SUCCESS;
}

void
radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws)
{
   simple_mtx_init(&ws->deferred.lock, mtx_plain);
   simple_mtx_init(&ws->deferred.pool_lock, mtx_plain);
   list_inithead(&ws->deferred.trackers);
   list_inithead(&ws->deferred.entries);
   list_inithead(&ws->deferred.pool);
   list_inithead(&ws->deferred.locked);
   ws->deferred.locked_interval_ns = 1000000000ull;

   /* Only the bc250 path has per-queue progress fences to wait on. */
   const struct bc250_host_policy_values *policy = &ws->bc250_policy;
   const char *destroy_from, *witness_from, *cap_from, *log_from;
   const char *destroy = radv_wddm2_knob("BC250_DEFERRED_DESTROY", &destroy_from);
   const char *witness = radv_wddm2_knob("BC250_DEFERRED_WITNESS", &witness_from);
   const char *cap = radv_wddm2_knob("BC250_DEFERRED_CAP_MB", &cap_from);
   const char *log_path = radv_wddm2_knob("BC250_DEFERRED_LOG", &log_from);
   if (bc250_host_policy_has(policy, BC250_HOST_POLICY_HAS_DEFERRED_DESTROY)) {
      ws->deferred.enabled = ws->bc250 && policy->deferred_destroy != 0;
      destroy_from = "host";
   } else {
      ws->deferred.enabled = ws->bc250 && !(destroy && !strcmp(destroy, "0"));
   }
   ws->deferred.witness = ws->bc250 && !(witness && !strcmp(witness, "0"));
   /* 512 MiB by default: a BO is held for the GPU's in-flight depth (a few frames), and 512 MiB covers
    * the frees of a heavy streaming burst over that window while it stays about 3 % of the 16 GiB the
    * BC-250 shares between CPU and GPU. The CPU waits only when frees outrun the GPU by far. */
   uint64_t cap_mb = 512;
   if (cap) {
      char *end = NULL;
      const unsigned long long value = strtoull(cap, &end, 10);
      if (end != cap && !*end && value <= (UINT64_MAX >> 20))
         cap_mb = value;
      else
         cap_from = "invalid, default";
   }
   ws->deferred.cap_bytes = cap_mb << 20;

   /* The periodic summary: 30 s is about 900 frames at the lab's 30 fps, 14 periods (up to two lines
    * each) in a 7-minute session. */
   const char *summary_from, *coalesce_from, *slots_from, *progress_from;
   const char *summary = radv_wddm2_knob("BC250_DEFERRED_SUMMARY_S", &summary_from);
   const char *coalesce = radv_wddm2_knob("BC250_SUBMIT_COALESCE", &coalesce_from);
   const char *slots = radv_wddm2_knob("BC250_GATHER_SLOTS", &slots_from);
   const char *progress = radv_wddm2_knob("BC250_PROGRESS_FENCE", &progress_from);
   uint64_t summary_s = 30;
   if (summary) {
      char *end = NULL;
      const unsigned long long value = strtoull(summary, &end, 10);
      if (end != summary && !*end && value <= 86400)
         summary_s = value;
      else
         summary_from = "invalid, default";
   }
   simple_mtx_init(&ws->summary.lock, mtx_plain);
   ws->summary.interval_ns = summary_s * 1000000000ull;
   ws->summary.start_ns = os_time_get_nano();
   ws->summary.next_ns = ws->bc250 && summary_s ? ws->summary.start_ns + ws->summary.interval_ns : 0;
   /* The submit path (radv_wddm2_cs.c). BC250_SUBMIT_COALESCE: 1 (the default) merges the progress
    * signal into the application's signal call and leaves out the GPU waits the CPU sees complete;
    * "signals" or "waits" does only that one; 0 neither, as before. */
   ws->bc250_merge_signals = true;
   ws->bc250_drop_waits = true;
   /* A knob the host named is the host's: the environment is not asked for it at all. */
   if (bc250_host_policy_has(policy, BC250_HOST_POLICY_HAS_COALESCE)) {
      ws->bc250_merge_signals = (policy->coalesce & BC250_HOST_POLICY_COALESCE_SIGNALS) != 0;
      ws->bc250_drop_waits = (policy->coalesce & BC250_HOST_POLICY_COALESCE_WAITS) != 0;
      coalesce_from = "host";
   } else if (coalesce) {
      if (!strcmp(coalesce, "0"))
         ws->bc250_merge_signals = ws->bc250_drop_waits = false;
      else if (!strcmp(coalesce, "signals"))
         ws->bc250_drop_waits = false;
      else if (!strcmp(coalesce, "waits"))
         ws->bc250_merge_signals = false;
      else if (strcmp(coalesce, "1"))
         coalesce_from = "invalid, default";
   }
   /* BC250_GATHER_SLOTS: how many submissions of a queue may be in flight before the next one waits on
    * the CPU for the oldest to retire. 16 lets the submitting thread run about three frames ahead at
    * the five submissions a frame of session 217, where 7 held it to 1.3. */
   ws->bc250_gather_slots = BC250_GATHER_SLOTS_DEFAULT;
   if (bc250_host_policy_has(policy, BC250_HOST_POLICY_HAS_GATHER_SLOTS)) {
      ws->bc250_gather_slots = policy->gather_slots; /* the instance validated the range */
      slots_from = "host";
   } else if (slots) {
      char *end = NULL;
      const unsigned long value = strtoul(slots, &end, 10);
      if (end != slots && !*end && value >= BC250_GATHER_SLOTS_MIN && value <= BC250_GATHER_SLOTS_MAX)
         ws->bc250_gather_slots = (unsigned)value;
      else
         slots_from = "invalid, default";
   }
   /* BC250_PROGRESS_FENCE: "gpu" (the default) has each queue's IB1 end with the GPU's write of its
    * progress value (radv_wddm2_cs.c, bc250_emit_progress_write), so that fence takes no kernel signal;
    * "kernel" signals it through SignalSynchronizationObjectFromGpu2, as version 2 did. */
   ws->bc250_progress_gpu = true;
   if (bc250_host_policy_has(policy, BC250_HOST_POLICY_HAS_PROGRESS_GPU)) {
      ws->bc250_progress_gpu = policy->progress_gpu != 0;
      progress_from = "host";
   } else if (progress) {
      if (!strcmp(progress, "kernel"))
         ws->bc250_progress_gpu = false;
      else if (strcmp(progress, "gpu"))
         progress_from = "invalid, default";
   }
   /* BC250_DRAW_STATS=1: RADV counts what the application records in its command buffers
    * (RADV_DRAW_STATS, radv_cmd_buffer.c) for a third summary line. Off by default; when off, a
    * command buffer pays one branch per counted command and the log is unchanged. */
   const char *draw_from;
   const char *draw = radv_wddm2_knob("BC250_DRAW_STATS", &draw_from);
   if (ws->bc250 && draw && !strcmp(draw, "1"))
      ws->base.draw_stats_add = radv_wddm2_draw_stats_add;

   ws->deferred.report_count = 256;
   ws->deferred.report_bytes = 256ull << 20;
   ws->deferred.report_hold_ns = 1000000000ull;
   ws->deferred.report_total = 1024;
   ws->deferred.report_cap_waits = 1;
   ws->deferred.report_destroys = 1024;
   ws->deferred.report_in_flight = RADV_WDDM2_WITNESS_LINES;
   if (ws->bc250) {
      simple_mtx_lock(&radv_wddm2_deferred_log_mtx);
      if (log_path && !radv_wddm2_deferred_log_opened)
         snprintf(radv_wddm2_deferred_log_knob, sizeof(radv_wddm2_deferred_log_knob), "%s", log_path);
      simple_mtx_unlock(&radv_wddm2_deferred_log_mtx);
      radv_wddm2_deferred_line("header version=4 destroy=%s(%s) witness=%s(%s) cap_mb=%" PRIu64
                               "(%s) policy=%s/%02x log=%s(%s) summary_s=%" PRIu64 "(%s) coalesce=%s(%s)"
                               " gather_slots=%u(%s) progress_fence=%s(%s)",
                               ws->deferred.enabled ? "on" : "off", destroy_from,
                               ws->deferred.witness ? "on" : "off", witness_from, cap_mb, cap_from,
                               policy->present ? "host" : "absent", policy->specified,
                               radv_wddm2_deferred_log_path(), log_from, summary_s,
                               summary_from, radv_wddm2_coalesce_name(ws), coalesce_from, ws->bc250_gather_slots,
                               slots_from, ws->bc250_progress_gpu ? "gpu" : "kernel", progress_from);
      if (ws->base.draw_stats_add)
         radv_wddm2_deferred_line("draw stats on (%s): counters of every application command buffer, in the "
                                  "summary's draw line", draw_from);
   }
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
