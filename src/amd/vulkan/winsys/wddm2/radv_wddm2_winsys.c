#include <stdlib.h>
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

#include <inttypes.h>
#include <stdint.h>

#include "radv_wddm2_winsys.h"
#include "radv_wddm2_bc250.h"

#include "util/macros.h"
#include "util/u_string.h"
#include "ac_surface.h"
#include "tools/radv_debug.h"
#include "radv_wddm2_bo.h"
#include "radv_wddm2_cs.h"
#include "radv_wddm2_wsi.h"
#include "radv_wddm2_winsys_public.h"
#include "vk_dxcore.h"
#include "vk_instance.h"
#include "vk_sync_dummy.h"
#include "vk_wddm2_dispatch_table.h"
#include "vk_wddm2_monitored_fence.h"
#include "common/ac_linux_drm.h"
#include "common/amd_family.h"

#include <locale.h>

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#include "wsl/winadapter.h"
#include "d3dkmthk.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

static simple_mtx_t winsys_creation_mutex = SIMPLE_MTX_INITIALIZER;
static struct hash_table *winsyses = NULL;

static NTSTATUS
query_adapter_info(struct radv_wddm2_winsys *ws,
                   KMTQUERYADAPTERINFOTYPE info_type,
                   void *info, size_t info_size)
{
   D3DKMT_QUERYADAPTERINFO adapter_info = {
      .hAdapter = ws->adapter_h,
      .Type = info_type,
      .pPrivateDriverData = info,
      .PrivateDriverDataSize = info_size,
   };
   return WDDM2_DISPATCH(QueryAdapterInfo(&adapter_info));
}

static enum amd_ip_type
convert_node_type(char node_type)
{
   switch (node_type) {
   case 0x00:
      return AMD_IP_GFX;
   case 0x04:
      return AMD_IP_COMPUTE;
   case 0x05:
      return AMD_IP_SDMA;
   default:
      UNREACHABLE("Unknown node type");
   }
}

static uint32_t
kmd_to_amdgpu_vram_type(uint32_t kmd_type)
{
   switch (kmd_type) {
   case 6:
      return AMDGPU_VRAM_TYPE_GDDR6;
   default:
      fprintf(stderr, "Unknown KMD VRAM type: %u\n", kmd_type);
      return AMDGPU_VRAM_TYPE_UNKNOWN;
   }
}

/* Offsets of driver/contract/bc250_umd_private.h version 3, checked by blob_off.c. */
#define BC250_OFF_DEVICE 16u
#define BC250_OFF_MEMORY 464u
#define BC250_OFF_HW_IP 560u
#define BC250_OFF_HW_IP_MASK 960u
#define BC250_OFF_HW_IP_INST 964u
#define BC250_OFF_GB_ADDR 1008u
#define BC250_OFF_MC_ARB 1012u
#define BC250_OFF_FW_ME 1208u
#define BC250_OFF_FW_PFP 1216u
#define BC250_OFF_FW_MEC 1232u
#define BC250_OFF_DRM_MAJOR 1272u
#define BC250_OFF_ADDR32 1284u
#define BC250_OFF_MAX_IBS 1344u

static uint32_t
bc250_rd32(const uint8_t *p, uint32_t off)
{
   uint32_t v;
   memcpy(&v, p + off, sizeof(v));
   return v;
}

static uint64_t
bc250_rd64(const uint8_t *p, uint32_t off)
{
   uint64_t v;
   memcpy(&v, p + off, sizeof(v));
   return v;
}

/* True when QueryAdapterInfo returned our caps blob and radeon_info was filled from it.
 * The AMD private-data parser below is the wrong shape for this blob (1472 bytes, not 17248).
 */
static bool
radv_wddm2_try_bc250(struct radv_wddm2_winsys *ws, struct drm_amdgpu_info_device *dev,
                     struct drm_amdgpu_memory_info *mem, bool compiler_compat_mode)
{
   struct radeon_info *info = &ws->gpu_info;
   uint8_t blob[1472];
   struct amdgpu_gpu_info amdinfo = {0};
   NTSTATUS status;
   uint32_t mask, i;

   _Static_assert(sizeof(*dev) == 448, "BC-250 device blob is the kernel's 448-byte structure");
   /* ac_linux_drm.h's hw-ip struct drops the kernel's capabilities_flags, so it is not the
    * 40-byte record. The fields below are copied from those kernel offsets.
    */

   status = query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, blob, sizeof(blob));
   if (!NT_SUCCESS(status) || bc250_rd32(blob, 0) != BC250_CAPS_MAGIC || bc250_rd32(blob, 4) < 3)
      return false;

   memcpy(dev, blob + BC250_OFF_DEVICE, sizeof(*dev));
   mem->vram.total_heap_size = bc250_rd64(blob, BC250_OFF_MEMORY + 0 * 32);
   mem->cpu_accessible_vram.total_heap_size = bc250_rd64(blob, BC250_OFF_MEMORY + 1 * 32);
   mem->gtt.total_heap_size = bc250_rd64(blob, BC250_OFF_MEMORY + 2 * 32);

   info->drm_major = bc250_rd32(blob, BC250_OFF_DRM_MAJOR);
   info->drm_minor = bc250_rd32(blob, BC250_OFF_DRM_MAJOR + 4);
   info->drm_patchlevel = bc250_rd32(blob, BC250_OFF_DRM_MAJOR + 8);
   info->is_amdgpu = true;
   /* This is a winsys VA policy, not a hardware capability. The reference
    * blob carries Linux's high VA aperture; WDDM allocates 32-bit BOs in
    * RADV_WDDM2_32BIT_HEAP_START. Shader pointer expansion must match it. */
   info->address32_hi = RADV_WDDM2_32BIT_HEAP_START >> 32;
   info->userq_ip_mask = 0;

   mask = bc250_rd32(blob, BC250_OFF_HW_IP_MASK);
   for (i = 0; i < 10 && i < AMD_NUM_IP_TYPES; i++) {
      struct drm_amdgpu_info_hw_ip ip;
      uint32_t base;
      if (!(mask & (1u << i)))
         continue;
      base = BC250_OFF_HW_IP + i * 40u;
      memset(&ip, 0, sizeof(ip));
      ip.hw_ip_version_major = bc250_rd32(blob, base + 0);
      ip.hw_ip_version_minor = bc250_rd32(blob, base + 4);
      /* kernel +8 is capabilities_flags (u64); Mesa's struct has no such field */
      ip.ib_start_alignment = bc250_rd32(blob, base + 16);
      ip.ib_size_alignment = bc250_rd32(blob, base + 20);
      ip.available_rings = bc250_rd32(blob, base + 24);
      ip.ip_discovery_version = bc250_rd32(blob, base + 28);
      if (ac_fill_hw_ip_info(info, dev, i, &ip))
         info->ip[i].num_instances = bc250_rd32(blob, BC250_OFF_HW_IP_INST + i * 4);
   }
   for (i = 0; i < 10 && i < AMD_NUM_IP_TYPES; i++)
      info->max_submitted_ibs[i] = bc250_rd32(blob, BC250_OFF_MAX_IBS + i * 4);

   info->me_fw_version = bc250_rd32(blob, BC250_OFF_FW_ME);
   info->me_fw_feature = bc250_rd32(blob, BC250_OFF_FW_ME + 4);
   info->pfp_fw_version = bc250_rd32(blob, BC250_OFF_FW_PFP);
   info->pfp_fw_feature = bc250_rd32(blob, BC250_OFF_FW_PFP + 4);
   info->mec_fw_version = bc250_rd32(blob, BC250_OFF_FW_MEC);
   info->mec_fw_feature = bc250_rd32(blob, BC250_OFF_FW_MEC + 4);

   if (!ac_identify_chip(info, dev)) {
      fprintf(stderr, "bc250: ac_identify_chip refused the caps blob\n");
      return false;
   }

   ac_fill_memory_info(info, dev, mem);
   ac_fill_hw_info(info, dev);
   amdinfo.gb_addr_cfg = bc250_rd32(blob, BC250_OFF_GB_ADDR);
   amdinfo.mc_arb_ramcfg = bc250_rd32(blob, BC250_OFF_MC_ARB);
   ac_fill_tiling_info(info, &amdinfo);
   ac_fill_feature_info(info, dev);
   ac_fill_bug_info(info);
   ac_fill_tess_info(info);
   ac_fill_compiler_info(info, dev, compiler_compat_mode);
   ac_fill_raster_config(info);
   ac_fill_scratch_info(info);

   /* Native paired-view controls pass (E33). Sparse remains experimental
    * until shader/image and queued PTE relocation coverage is complete. */
   info->address_prt_wa_control_bit = RADV_WDDM2_PRT_CONTROL_BIT;
   info->has_sparse = false;
   info->has_vm_always_valid = true;
   info->has_timeline_syncobj = true;
   info->spi_cu_en = ~0u;
   info->pcie_gen = dev->pcie_gen;
   info->pcie_num_lanes = dev->pcie_num_lanes;
   info->max_gflops = (info->gfx_level >= GFX11 ? 256 : 128) * info->num_cu * info->max_gpu_freq_mhz / 1000;

   ws->bc250 = true;
   fprintf(stderr, "bc250: caps blob, device 0x%x family %u gfx %u\n", dev->device_id, info->family,
           info->gfx_level);
   return true;
}

static NTSTATUS
radv_wddm2_fill_gpu_info(struct radv_wddm2_winsys *ws,
                         const struct vk_dx_adapter_info *adapter_info, bool compiler_compat_mode)
{
   struct radeon_info *info = &ws->gpu_info;
   uint32_t node, node_count = 0;
   NTSTATUS status;

   struct drm_amdgpu_memory_info mem = {};
   struct drm_amdgpu_info_device dev = {
      .min_engine_clock = UINT64_C(500000),
      .max_engine_clock = UINT64_C(2371000),
      .min_memory_clock = UINT64_C(96000),
      .max_memory_clock = UINT64_C(1249000),
      .gart_page_size = 4096,
      .tcp_cache_size = 32,
   };

   /* PCI device ids */
   {
      D3DKMT_QUERY_DEVICE_IDS query_ids = {
         .PhysicalAdapterIndex = adapter_info->physical_adapter_index,
      };
      status = query_adapter_info(ws, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &query_ids, sizeof(query_ids));
      if (!NT_SUCCESS(status))
         return status;

      dev.device_id = query_ids.DeviceIds.DeviceID;
      dev.pci_rev = query_ids.DeviceIds.RevisionID;
      fprintf(stderr, "phys adapter = %i device = %x rev %x\n", adapter_info->physical_adapter_index, dev.device_id,
              dev.pci_rev);
   }

   /* PCI bus address */
   {
      D3DKMT_ADAPTERADDRESS address = {};
      if (NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_ADAPTERADDRESS,
                                        &address, sizeof(address)))) {
         info->pci.domain = 0;
         info->pci.bus = address.BusNumber;
         info->pci.dev = address.DeviceNumber;
         info->pci.func = address.FunctionNumber;
         info->pci.valid = true;
      } else {
         memset(&info->pci, 0, sizeof(info->pci));
      }
   }

   info->valid_luid = true;
   memcpy(info->luid, &ws->adapter_luid, sizeof(info->luid));

   /* GTT size */
   {
      D3DKMT_SEGMENTSIZEINFO segment = {};
      if (NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_GETSEGMENTSIZE,
                                        &segment, sizeof(segment)))) {
         mem.gtt.total_heap_size = segment.SharedSystemMemorySize;
      }
   }

   /* Nodes information */
   {
      D3DKMT_QUERYSTATISTICS stats = {
         .Type = D3DKMT_QUERYSTATISTICS_ADAPTER,
         .AdapterLuid = {
            .LowPart = adapter_info->adapter_luid.LowPart,
            .HighPart = adapter_info->adapter_luid.HighPart,
         },
      };
      if (NT_SUCCESS(WDDM2_DISPATCH(QueryStatistics(&stats))))
         node_count = stats.QueryResult.AdapterInformation.NodeCount;

      for (uint32_t i = 0; i < node_count; ++i) {
         D3DKMT_NODEMETADATA metadata = {
            .NodeOrdinalAndAdapterIndex = MAKEWORD(i, adapter_info->physical_adapter_index),
         };
         if (NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_NODEMETADATA,
                                           &metadata, sizeof(metadata)))) {
            fprintf(stderr, "node %i (type: %i) = %ls, flags = %x\n",
                    i, metadata.NodeData.EngineType, metadata.NodeData.FriendlyName,
                    metadata.NodeData.Flags.Value);
            if (metadata.NodeData.EngineType == DXGK_ENGINE_TYPE_3D)
               node = i;
         }
      }
   }

   (void)node;
   if (radv_wddm2_try_bc250(ws, &dev, &mem, compiler_compat_mode))
      return STATUS_SUCCESS;

   struct PACKED umdprivatedata_header {
      uint32_t flags[3];
      uint32_t vendor_id;
      uint32_t unknown0;
      uint32_t pci_id;
      uint32_t unknown1[9];
      uint16_t registry_path[100];
      uint32_t padding[490];
      uint32_t type;
      uint32_t something;
      uint32_t unknown2[42];
      uint32_t size;
   };

   struct PACKED basic_properties {
      struct umdprivatedata_header header;
      uint32_t unk0[11];
      uint32_t device_id;
      uint32_t family_id;
      uint32_t chip_external_rev;
      uint32_t subsystem_id;
      uint32_t unk1[6];
      uint32_t vram_type;
      uint32_t vram_bit_width;
      uint32_t unk2[43];
      char umd_registry_path[256];
      uint32_t unk3[18];
      uint32_t num_rb_pipes;
      uint32_t num_enabled_rb_pipes;
      uint32_t enabled_rb_pipes_mask;
      uint32_t enabled_rb_pipes_mask_hi;
      uint32_t unk4[8];
   };

   struct PACKED gpu_properties {
      struct umdprivatedata_header header;
      uint32_t unk0[550];
      uint32_t num_engines;
      char engines[64];
      uint32_t unk1[5];
      uint32_t me_fw_version;
      uint32_t mec_fw_version;
      uint32_t pfp_fw_version;
      uint32_t unk3[65];
      uint32_t device_id;
      uint32_t family_id;
      uint32_t chip_external_rev;
      uint32_t chip_rev;
      uint32_t gfx_engine_id;
      uint32_t unk4[4];
      uint32_t clock_crystal_freq_hz;
      uint32_t unk5;
      uint32_t unk6;
      uint64_t vram_vis_size;
      uint64_t vram_inv_size;
      uint32_t unk7[112];
      uint32_t gb_addr_cfg;
      uint32_t unk8[21];
      uint32_t num_shader_visible_vgprs;
      uint32_t wave_front_size;
      uint32_t num_shader_engines;
      uint32_t num_active_rb_per_se;
      uint32_t num_shader_arrays_per_engine;
      uint32_t unk9[2];
      uint32_t num_tcc_blocks;
      uint32_t max_gs_waves_per_vgt;
      uint32_t unk10;
      uint32_t gs_vgt_table_depth;
      uint32_t gs_prim_buffer_depth;
      uint32_t unk11[23];
      uint32_t num_cu_per_wgp;
      uint32_t unk12[7];
      uint16_t wgp_bitmap[6][2];
      uint16_t wgp_ao_bitmap[6][2];
      uint32_t unk13[10];
      uint32_t num_sqc_per_wgp;
      uint32_t sqc_inst_cache_size;
      uint32_t sqc_data_cache_size;
      uint32_t gl1c_cache_size;
      uint32_t unk14;
      uint32_t gl2c_cache_size;
      uint32_t unk15[4];
      uint32_t mall_size_mb;
      uint32_t unk16[1824];
      uint64_t va_start;
      uint64_t va_end;
      uint32_t unk17[256];
      uint32_t large_page;
      uint32_t unk18[220];
      struct {
         uint32_t start;
         uint32_t size;
      } ib_alignments[8];
      uint32_t unk19[525];
   };

   struct basic_properties props1 = {0};
   struct gpu_properties props2 = {0};

   // type 0, 0, 3, 3, 0, 0
   // uint32_t sizes[] = {2400, 2912, 2915, 2915, 3008, 17248};
   query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props1, 3008);
   query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props2, 17248);

   /* IP Engines */
   struct drm_amdgpu_info_hw_ip hw_ip_gfx = {
      .hw_ip_version_major = 11,
      .hw_ip_version_minor = 0,
      .available_rings = 0x1,
      .ib_start_alignment = props2.ib_alignments[0].start,
      .ib_size_alignment = props2.ib_alignments[0].size,
   };
   struct drm_amdgpu_info_hw_ip hw_ip_compute = {
      .hw_ip_version_major = 11,
      .hw_ip_version_minor = 0,
      .available_rings = 0xf,
      .ib_start_alignment = props2.ib_alignments[1].start,
      .ib_size_alignment = props2.ib_alignments[1].size,
   };

   /* Firmware versions */
   info->me_fw_version = props2.me_fw_version;
   info->me_fw_feature = 29;
   info->mec_fw_version = props2.mec_fw_version;
   info->mec_fw_feature = 29;
   info->pfp_fw_version = props2.pfp_fw_version;
   info->pfp_fw_feature = 29;

   /* Device identification */
   dev.chip_rev = props2.chip_rev;
   dev.external_rev = props2.chip_external_rev;
   dev.family = props2.family_id;

   /* Shader engine info */
   dev.num_shader_engines = props2.num_shader_engines;
   dev.num_shader_arrays_per_engine = props2.num_shader_arrays_per_engine;
   dev.gpu_counter_freq = props2.clock_crystal_freq_hz / 1000;
   dev.wave_front_size = props2.wave_front_size;
   dev.num_shader_visible_vgprs = props2.num_shader_visible_vgprs;
   dev.num_tcc_blocks = props2.num_tcc_blocks;
   dev.num_rb_pipes = props1.num_rb_pipes;
   dev.enabled_rb_pipes_mask = props1.enabled_rb_pipes_mask;
   dev.enabled_rb_pipes_mask_hi = props1.enabled_rb_pipes_mask_hi;

   /* GS info */
   dev.max_gs_waves_per_vgt = props2.max_gs_waves_per_vgt;
   dev.gs_vgt_table_depth = props2.gs_vgt_table_depth;
   dev.gs_prim_buffer_depth = props2.gs_prim_buffer_depth;

   /* Cache sizes */
   dev.num_sqc_per_wgp = props2.num_sqc_per_wgp;
   dev.sqc_inst_cache_size = props2.sqc_inst_cache_size;
   dev.sqc_data_cache_size = props2.sqc_data_cache_size;
   dev.gl1c_cache_size = props2.gl1c_cache_size;
   dev.gl2c_cache_size = props2.gl2c_cache_size;
   dev.mall_size = (uint64_t)props2.mall_size_mb * 1024 * 1024;

   /* Memory */
#define PAGE_SIZE 4096
   dev.pte_fragment_size = (1 << (props2.large_page & 0x3f)) * PAGE_SIZE;
   dev.vram_type = kmd_to_amdgpu_vram_type(props1.vram_type);
   dev.vram_bit_width = props1.vram_bit_width;
   dev.virtual_address_offset = props2.va_start;
   dev.virtual_address_max = props2.va_end;
   mem.vram.total_heap_size = props2.vram_vis_size + props2.vram_inv_size;
   mem.cpu_accessible_vram.total_heap_size = props2.vram_vis_size;

   /* Cu Mask */
   uint32_t w = props2.num_shader_arrays_per_engine;
   for (uint32_t i = 0; i < props2.num_shader_engines; ++i) {
      for (uint32_t j = 0; j < props2.num_shader_arrays_per_engine; ++j) {
         dev.cu_bitmap[i % 4][i / 4 * w + j] = util_widen_mask(props2.wgp_bitmap[i][j], props2.num_cu_per_wgp);
         dev.cu_ao_bitmap[i % 4][i / 4 * w + j] = util_widen_mask(props2.wgp_ao_bitmap[i][j], props2.num_cu_per_wgp);
      }
   }

   info->gb_addr_config = props2.gb_addr_cfg;
   info->num_tile_pipes = 1 << G_0098F8_NUM_PIPES(info->gb_addr_config);

   info->pcie_gen = 4;
   info->pcie_num_lanes = 16;
   info->has_timeline_syncobj = true;
   info->has_vm_always_valid = true;
   info->spi_cu_en = ~0;
   info->address32_hi = RADV_WDDM2_32BIT_HEAP_START >> 32;

   ac_fill_hw_ip_info(info, &dev, AMD_IP_GFX, &hw_ip_gfx);
   ac_fill_hw_ip_info(info, &dev, AMD_IP_COMPUTE, &hw_ip_compute);
   ac_identify_chip(info, &dev);
   ac_fill_memory_info(info, &dev, &mem);
   ac_fill_hw_info(info, &dev);
   //ac_fill_tiling_info(info, &gpu_info);
   ac_fill_feature_info(info, &dev);
   ac_fill_bug_info(info);
   ac_fill_tess_info(info);
   ac_fill_compiler_info(info, &dev, compiler_compat_mode);
   ac_get_default_max_submitted_ibs(info);
   ac_fill_attribute_ring_info(info);
   //set_custom_cu_en_mask(info);
   ac_fill_raster_config(info);
   ac_fill_scratch_info(info);

   info->compiler_info.has_image_bvh_intersect_ray = false;

   return STATUS_SUCCESS;
}

static NTSTATUS
radv_wddm2_query_marketing_name(struct radv_wddm2_winsys *ws, char *name, size_t name_len)
{
   D3DKMT_ADAPTERREGISTRYINFO registry = {};
   NTSTATUS status = query_adapter_info(ws, KMTQAITYPE_ADAPTERREGISTRYINFO_RENDER,
                                        &registry, sizeof(registry));
   if (!NT_SUCCESS(status))
      return status;

   char *old_locale = setlocale(LC_CTYPE, "en_US.utf8");
   size_t len = wcstombs(name, registry.AdapterString, name_len);
   setlocale(LC_CTYPE, old_locale);
   if (len >= name_len)
      return STATUS_INVALID_PARAMETER;

   return STATUS_SUCCESS;
}

static bool
radv_wddm2_inline_gfx_preamble(struct radeon_winsys *rws)
{
   return radv_wddm2_winsys(rws)->bc250;
}

static struct radeon_info *
radv_wddm2_winsys_query_info(struct radeon_winsys *rws)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(rws);

   return &ws->gpu_info;
}

static uint64_t
radv_wddm2_winsys_query_value(struct radeon_winsys *_ws, enum radeon_value_id value)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   NTSTATUS status;

   switch (value) {
   case RADEON_ALLOCATED_VRAM:
   case RADEON_ALLOCATED_VRAM_VIS:
   case RADEON_ALLOCATED_GTT:
   case RADEON_VRAM_USAGE:
   case RADEON_VRAM_VIS_USAGE:
   case RADEON_GTT_USAGE: {
      D3DKMT_QUERYVIDEOMEMORYINFO mem_info = {
         .hAdapter = ws->adapter_h,
      };
      switch (value) {
      case RADEON_ALLOCATED_VRAM:
      case RADEON_ALLOCATED_VRAM_VIS:
      case RADEON_VRAM_USAGE:
      case RADEON_VRAM_VIS_USAGE:
         mem_info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
         break;
      case RADEON_ALLOCATED_GTT:
      case RADEON_GTT_USAGE:
         mem_info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL;
         break;
      default:
         UNREACHABLE("Invalid value");
      }

      status = WDDM2_DISPATCH(QueryVideoMemoryInfo(&mem_info));
      if (!NT_SUCCESS(status))
         return 0;

      switch (value) {
      case RADEON_ALLOCATED_VRAM:
      case RADEON_ALLOCATED_VRAM_VIS:
      case RADEON_ALLOCATED_GTT:
         return mem_info.CurrentReservation;

      case RADEON_VRAM_USAGE:
      case RADEON_VRAM_VIS_USAGE:
      case RADEON_GTT_USAGE:
         return mem_info.CurrentUsage;
      default:
         UNREACHABLE("Invalid value");
      }
   }

   default:
      UNREACHABLE("Unimplemented query");
   }
}

/* ------------------------------------------------------------------ C70: device-local pre-check */

/* The budget the OS gives this process in each segment group
 * (D3DKMT_QUERYVIDEOMEMORYINFO.Budget, d3dkmthk.h). False when either query fails. */
static bool
radv_wddm2_query_budget_groups(struct radv_wddm2_winsys *ws, uint64_t *local, uint64_t *non_local)
{
   static const D3DKMT_MEMORY_SEGMENT_GROUP groups[2] = {
      D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL,
      D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL,
   };
   uint64_t budget[2] = {0, 0};

   for (unsigned i = 0; i < 2; i++) {
      D3DKMT_QUERYVIDEOMEMORYINFO mem_info = {
         .hAdapter = ws->adapter_h,
         .MemorySegmentGroup = groups[i],
      };
      if (!NT_SUCCESS(WDDM2_DISPATCH(QueryVideoMemoryInfo(&mem_info))))
         return false;
      budget[i] = mem_info.Budget;
   }
   if (!budget[0] || !budget[1])
      return false;
   *local = budget[0];
   *non_local = budget[1];
   return true;
}

/* BD-096: the same budget for VK_EXT_memory_budget (radv_physical_device.c). With the default
 * memory overflow policy an allocation fails only above the maximum budget, and the current budget
 * is the share the OS asks the process to keep to. The application is told that share, so that it
 * can size itself to it. False when either query fails, and the caller then keeps the heap-size
 * arithmetic. */
bool
radv_wddm2_query_budget(struct radeon_winsys *base, uint64_t *budget)
{
   uint64_t local, non_local;
   if (!radv_wddm2_query_budget_groups(radv_wddm2_winsys(base), &local, &non_local))
      return false;
   *budget = local + non_local;
   return true;
}

/* The local segment of the adapter, and the two segments together. Zero when the query fails. */
static void
radv_wddm2_query_segments(struct radv_wddm2_winsys *ws, uint64_t *local, uint64_t *total)
{
   D3DKMT_SEGMENTSIZEINFO segment = {};
   *local = *total = 0;
   if (!NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_GETSEGMENTSIZE, &segment, sizeof(segment))))
      return;
   *local = segment.DedicatedVideoMemorySize;
   *total = segment.DedicatedVideoMemorySize + segment.SharedSystemMemorySize;
}

/* C70 (BD-096): read the limits once and say whether the rule is on. Without a budget there is no
 * threshold, so the rule stays off and no allocation is refused by it. */
static void
radv_wddm2_mem_init(struct radv_wddm2_winsys *ws)
{
   const struct radv_wddm2_mem_precheck_switch choice =
      radv_wddm2_mem_precheck_parse(getenv(RADV_WDDM2_MEM_PRECHECK_ENV));

   simple_mtx_init(&ws->mem.lock, mtx_plain);
   radv_wddm2_query_segments(ws, &ws->mem.limits.segment_local, &ws->mem.limits.segment_total);
   if (!radv_wddm2_query_budget_groups(ws, &ws->mem.limits.budget_local,
                                      &ws->mem.limits.budget_nonlocal))
      ws->mem.limits.budget_local = ws->mem.limits.budget_nonlocal = 0;
   ws->mem.enabled = choice.enabled && radv_wddm2_mem_budget_total(ws->mem.limits) != 0;

   fprintf(stderr,
           "BC250 memory precheck: %s%s budget %.1f MiB (local %.1f, non-local %.1f) "
           "segments %.1f MiB (local %.1f) threshold %.1f MiB\n",
           ws->mem.enabled ? "on" : "off", choice.invalid ? " (invalid value, default used)" : "",
           radv_wddm2_mem_budget_total(ws->mem.limits) / (1024.0 * 1024.0),
           ws->mem.limits.budget_local / (1024.0 * 1024.0),
           ws->mem.limits.budget_nonlocal / (1024.0 * 1024.0),
           ws->mem.limits.segment_total / (1024.0 * 1024.0),
           ws->mem.limits.segment_local / (1024.0 * 1024.0),
           radv_wddm2_mem_threshold(ws->mem.limits) / (1024.0 * 1024.0));
}

static void
radv_wddm2_mem_fini(struct radv_wddm2_winsys *ws)
{
   if (ws->mem.refusals)
      fprintf(stderr, "BC250 memory precheck: refused %" PRIu64 " device-local allocations\n",
              ws->mem.refusals);
   simple_mtx_destroy(&ws->mem.lock);
}

/* C70: true when the allocation may go to the kernel. A device-local allocation that no eviction can
 * fit is refused here, because the MakeResident that would take it evicts the whole process first and
 * fails all the same (K245: 86 s, then STATUS_NO_MEMORY). The two budgets are read again when the
 * cached numbers say refuse, and only then, so the second read can turn a refusal into an admission
 * and never the other way: the OS moves the budget of a process as other processes come and go. */
bool
radv_wddm2_mem_admit(struct radv_wddm2_winsys *ws, uint64_t size, bool device_local)
{
   if (!ws->mem.enabled || !device_local)
      return true;

   simple_mtx_lock(&ws->mem.lock);
   const uint64_t held_local = ws->mem.local_bytes, held_nonlocal = ws->mem.nonlocal_bytes;
   enum radv_wddm2_mem_verdict verdict =
      radv_wddm2_mem_check(ws->mem.limits, true, held_local, held_nonlocal, size);
   if (verdict != RADV_WDDM2_MEM_ADMIT) {
      uint64_t local, non_local;
      if (radv_wddm2_query_budget_groups(ws, &local, &non_local)) {
         ws->mem.limits.budget_local = local;
         ws->mem.limits.budget_nonlocal = non_local;
      }
      verdict = radv_wddm2_mem_check(ws->mem.limits, true, held_local, held_nonlocal, size);
   }
   if (verdict != RADV_WDDM2_MEM_ADMIT) {
      ws->mem.refusals++;
      if (!ws->mem.logged) {
         ws->mem.logged = true;
         fprintf(stderr,
                 "BC250 memory precheck: %s, %" PRIu64 " bytes device-local, held %.1f MiB "
                 "local and %.1f MiB non-local, threshold %.1f MiB (C70; %s=off turns this off)\n",
                 radv_wddm2_mem_verdict_name(verdict), size, held_local / (1024.0 * 1024.0),
                 held_nonlocal / (1024.0 * 1024.0),
                 radv_wddm2_mem_threshold(ws->mem.limits) / (1024.0 * 1024.0),
                 RADV_WDDM2_MEM_PRECHECK_ENV);
      }
   }
   simple_mtx_unlock(&ws->mem.lock);
   return verdict == RADV_WDDM2_MEM_ADMIT;
}

/* The bytes this winsys holds, counted on a created allocation and uncounted on its destruction. */
void
radv_wddm2_mem_account(struct radv_wddm2_winsys *ws, uint64_t size, bool device_local, bool add)
{
   simple_mtx_lock(&ws->mem.lock);
   uint64_t *counter = device_local ? &ws->mem.local_bytes : &ws->mem.nonlocal_bytes;
   if (add) {
      *counter += size;
   } else {
      assert(*counter >= size);
      *counter -= MIN2(*counter, size);
   }
   simple_mtx_unlock(&ws->mem.lock);
}

static void
radv_wddm2_winsys_destroy(struct radeon_winsys *_ws)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   ASSERTED NTSTATUS status;
   bool destroy = false;

   simple_mtx_lock(&winsys_creation_mutex);
   if (!--ws->refcount) {
      _mesa_hash_table_remove_key(winsyses, ws->cache_key);

      /* Clean the hashtable up if empty, though there is no
       * empty function. */
      if (_mesa_hash_table_num_entries(winsyses) == 0) {
         _mesa_hash_table_destroy(winsyses, NULL);
         winsyses = NULL;
      }

      destroy = true;
   }
   simple_mtx_unlock(&winsys_creation_mutex);
   if (!destroy)
      return;

   if (ws->null_prt.bo)
      ws->base.buffer_destroy(&ws->base, ws->null_prt.bo);
   simple_mtx_destroy(&ws->null_prt.lock);
   radv_wddm2_mem_fini(ws);

   D3DDDI_DESTROYPAGINGQUEUE destroy_paging_queue = {
      .hPagingQueue = ws->paging_queue_h,
   };
   if (ws->host.dispatch) {
      struct bc250_host_paging paging = { .queue = ws->paging_queue_h };
      status = ws->host.dispatch(ws->host.userdata, BC250_HOST_DESTROY_PAGING, &paging);
      fprintf(stderr, "BC250 hosted paging destroy identity=%p queue=%x status=%08x\n", ws->host.identity, paging.queue, status);
   } else {
      status = WDDM2_DISPATCH(DestroyPagingQueue(&destroy_paging_queue));
   }
   assert(NT_SUCCESS(status));

   D3DKMT_DESTROYDEVICE destroy_device = {
      .hDevice = ws->device_h,
   };
   status = ws->host.dispatch ? STATUS_SUCCESS : WDDM2_DISPATCH(DestroyDevice(&destroy_device));
   assert(NT_SUCCESS(status));

   D3DKMT_CLOSEADAPTER close_adapter = {
      .hAdapter = ws->adapter_h,
   };
   status = WDDM2_DISPATCH(CloseAdapter(&close_adapter));
   assert(NT_SUCCESS(status));

   radv_winsys_bo_list_destroy(&ws->global_bo_list);
   radv_winsys_bo_log_destroy(&ws->bo_log);

   radv_wddm2_wsi_finish(ws);

   FREE(ws);
}

static uint32_t
radv_wddm2_winsys_get_wddm2_handle(struct radeon_winsys *_ws)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   return ws->device_h;
}

static const struct vk_sync_type *const *
radv_wddm2_winsys_get_sync_types(struct radeon_winsys *rws)
{
   return radv_wddm2_winsys(rws)->sync_types;
}

static struct util_sync_provider *
radv_wddm2_winsys_get_sync_provider(struct radeon_winsys *rws)
{
   return NULL;
}

static bool
radv_wddm2_winsys_query_gpuvm_fault(struct radeon_winsys *rws, struct radv_winsys_gpuvm_fault_info *fault_info)
{
   struct radv_wddm2_winsys *ws = (struct radv_wddm2_winsys *)rws;
   NTSTATUS status;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_PAGE_FAULT,
   };
   status = BC250_WDDM_CALL(&ws->host, GetDeviceState, &get_state);
   fprintf(stderr, "GetDeviceState: 0x%X\n", status);
   if (unlikely(!NT_SUCCESS(status)))
      return false;

   D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

   fprintf(stderr, "faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i)\n",
           fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode, fault.FaultErrorCode.DeviceSpecificCode);
   if (!fault.FaultedVirtualAddress)
      return false;

   fault_info->addr = fault.FaultedVirtualAddress;
   fault_info->status = fault.FaultErrorCode.GeneralErrorCode;
   fault_info->vmhub = 0;

   return true;
}

VkResult
radv_wddm2_winsys_create(const struct vk_dx_adapter_info *adapter_info,
                         const BITSET_WORD *debug_flags, const struct bc250_host *host, struct radeon_winsys **winsys)
{
   VkResult result = VK_SUCCESS;
   struct radv_wddm2_winsys *ws = NULL;
   NTSTATUS status;
   fprintf(stderr, "radv_wddm2_winsys_create\n");

   const void *key = host ? host->identity : (void *)1;
   if (host && memcmp(&host->adapter_luid, &adapter_info->adapter_luid, sizeof(uint64_t)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   /* We have to keep this lock till insertion. */
   simple_mtx_lock(&winsys_creation_mutex);
   if (!winsyses)
      winsyses = _mesa_pointer_hash_table_create(NULL);
   if (!winsyses) {
      fprintf(stderr, "radv/amdgpu: failed to alloc winsys hash table.\n");
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   struct hash_entry *entry = _mesa_hash_table_search(winsyses, key);
   if (entry) {
      ws = (struct radv_wddm2_winsys *)entry->data;
      ++ws->refcount;
   }
   
   if (ws) {
      simple_mtx_unlock(&winsys_creation_mutex);
      *winsys = &ws->base;
      return VK_SUCCESS;
   }

   ws = calloc(1, sizeof(struct radv_wddm2_winsys));
   if (!ws) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   ws->refcount = 1;
   ws->cache_key = key;
   if (host) ws->host = *host;
   ws->adapter_luid = adapter_info->adapter_luid;
   ws->chain_ib = !(BITSET_TEST(debug_flags, RADV_DEBUG_NO_IB_CHAINING));
   ws->debug_all_bos = !!(BITSET_TEST(debug_flags, RADV_DEBUG_ALL_BOS));
   ws->debug_log_bos = BITSET_TEST(debug_flags, RADV_DEBUG_HANG);
   ws->dump_ibs = !!(BITSET_TEST(debug_flags, RADV_DEBUG_DUMP_IBS));
   const char *trace_submits = getenv("BC250_TRACE_SUBMITS");
   ws->bc250_trace_submits = trace_submits && strcmp(trace_submits, "1") == 0;
   radv_winsys_bo_list_init(&ws->global_bo_list);
   radv_winsys_bo_log_init(&ws->bo_log, debug_flags);

   D3DKMT_OPENADAPTERFROMLUID open_adapter = {
      .AdapterLuid = ws->adapter_luid,
   };
   fprintf(stderr, "OpenAdapterFromLuid 0x%x 0x%x\n",
           adapter_info->adapter_luid.HighPart, adapter_info->adapter_luid.LowPart);
   status = WDDM2_DISPATCH(OpenAdapterFromLuid(&open_adapter));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Can't open adapter with luid %X%X\n", adapter_info->adapter_luid.LowPart, adapter_info->adapter_luid.HighPart);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_ptr_alloc;
   }

   ws->adapter_h = open_adapter.hAdapter;

   status = radv_wddm2_fill_gpu_info(ws, adapter_info, !BITSET_TEST(debug_flags, RADV_DEBUG_NO_CACHE_COMPAT));
   if (ws->bc250)
      /* One IB on the ring. Do not also link CS chunks with INDIRECT_BUFFER:
       * that packet's VMID field is left 0. Skipping the gfx-init IB itself is
       * inline_gfx_preamble, below; this flag does not do that. */
      ws->chain_ib = false;
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Can't fill info for luid %X%X\n", adapter_info->adapter_luid.LowPart, adapter_info->adapter_luid.HighPart);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_open_adapter;
   }

   status = radv_wddm2_query_marketing_name(ws, ws->gpu_info.marketing_name,
                                            sizeof(ws->gpu_info.marketing_name));
   if (ws->bc250) {
      /* The render-adapter string is "Microsoft Basic Display Adapter". The part's
       * name is not in that query. The Linux ICD reports AMD BC-250.
       */
      snprintf(ws->gpu_info.marketing_name, sizeof(ws->gpu_info.marketing_name), "AMD BC-250");
   } else if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Can't query marketing name\n");
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_open_adapter;
   }

   D3DKMT_CLOSEADAPTER close_adapter = {
      .hAdapter = ws->adapter_h,
   };

   D3DKMT_CREATEDEVICE create_device = {
      .hAdapter = ws->adapter_h,
   };
   status = host ? STATUS_SUCCESS : WDDM2_DISPATCH(CreateDevice(&create_device));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Couldn't create device for adapter %i\n", ws->adapter_h);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_open_adapter;
   }

   ws->device_h = create_device.hDevice;

   D3DKMT_DESTROYDEVICE destroy_device = {
      .hDevice = ws->device_h,
   };

   D3DKMT_CREATEPAGINGQUEUE create_paging_queue = {
      .hDevice = ws->device_h,
   };
   if (host) {
      struct bc250_host_paging paging = {0};
      status = host->dispatch(host->userdata, BC250_HOST_CREATE_PAGING, &paging);
      create_paging_queue.hPagingQueue = paging.queue;
      create_paging_queue.hSyncObject = paging.sync;
      fprintf(stderr, "BC250 hosted paging create identity=%p queue=%x status=%08x\n", host->identity, paging.queue, status);
   } else {
      status = WDDM2_DISPATCH(CreatePagingQueue(&create_paging_queue));
   }
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Couldn't create paging queue\n");
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_create_device;
   }

   ws->paging_queue_h = create_paging_queue.hPagingQueue;
   ws->paging_fence_h = create_paging_queue.hSyncObject;

   /* Initialize the heaps */
   radv_wddm2_mem_init(ws);
   simple_mtx_init(&ws->heap_mtx, mtx_plain);
   simple_mtx_init(&ws->null_prt.lock, mtx_plain);
   util_vma_heap_init(&ws->_32bit_heap, RADV_WDDM2_32BIT_HEAP_START, 1ull << 32);
   util_vma_heap_init(&ws->heap, RADV_WDDM2_HEAP_START, RADV_WDDM2_REPLAY_HEAP_START - RADV_WDDM2_HEAP_START);
   util_vma_heap_init(&ws->replay_heap, RADV_WDDM2_REPLAY_HEAP_START, 1ull << 32);

   ws->base.destroy = radv_wddm2_winsys_destroy;
   ws->base.query_info = radv_wddm2_winsys_query_info;
   ws->base.query_value = radv_wddm2_winsys_query_value;
   ws->base.get_wddm2_handle = radv_wddm2_winsys_get_wddm2_handle;
   ws->base.get_sync_types = radv_wddm2_winsys_get_sync_types;
   ws->base.get_sync_provider = radv_wddm2_winsys_get_sync_provider;
   ws->base.query_gpuvm_fault = radv_wddm2_winsys_query_gpuvm_fault;
   ws->base.init_wsi = radv_wddm2_wsi_init;
   ws->base.inline_gfx_preamble = radv_wddm2_inline_gfx_preamble;
   radv_wddm2_bo_init_functions(ws);
   radv_wddm2_cs_init_functions(ws);

   ws->sync_binary_type = vk_sync_binary_get_type(&vk_wddm2_monitored_fence_type);
   ws->sync_types[0] = &vk_wddm2_monitored_fence_type;
   ws->sync_types[1] = &ws->sync_binary_type.sync;
   ws->sync_types[2] = NULL;

   _mesa_hash_table_insert(winsyses, key, ws);
   simple_mtx_unlock(&winsys_creation_mutex);

   *winsys = &ws->base;

   return result;

error_create_device:
   status = ws->host.dispatch ? STATUS_SUCCESS : WDDM2_DISPATCH(DestroyDevice(&destroy_device));
   assert(NT_SUCCESS(status));

error_open_adapter:
   status = WDDM2_DISPATCH(CloseAdapter(&close_adapter));
   assert(NT_SUCCESS(status));

error_ptr_alloc:
   FREE(ws);

fail:
   simple_mtx_unlock(&winsys_creation_mutex);
   return result;
}
