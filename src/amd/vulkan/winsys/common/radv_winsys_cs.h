#ifndef RADV_WINSYS_CS_H
#define RADV_WINSYS_CS_H

#include "radv_radeon_winsys.h"
#include "ac_cmdbuf_cp.h"
#include "ac_debug.h"
#include "ac_gpu_info.h"
#include "util/u_hash_table.h"

struct radv_winsys_ib {
   struct radeon_winsys_bo *bo; /* NULL when not owned by the current CS object */
   uint64_t va;
   unsigned cdw;
};

struct radv_winsys_cs {
   struct ac_cmdbuf base;
   struct radeon_winsys *ws;

   struct radeon_winsys_bo *ib_buffer;
   uint8_t *ib_mapped;

   struct radv_winsys_ib *ib_buffers;
   unsigned num_ib_buffers;
   unsigned max_num_ib_buffers;
   unsigned *ib_size_ptr;
   VkResult status;
   struct radv_winsys_cs *chained_to;
   bool chain_ib;
   unsigned chain_ib_size;
   uint64_t chain_ib_va;
   bool is_secondary;
   /* The stream holds an INDIRECT_BUFFER packet that the CP runs as an IB2 (a secondary
    * command buffer, a DGC buffer, an execute_ib) or chains into GPU-written commands.
    * Such a stream must itself run at IB1 level: the CP has no third IB level. Cleared
    * by a reset. */
   bool calls_ib2;

   unsigned hw_ip;

   struct hash_table *annotations;
};


static void
radv_winsys_emit_unchecked(struct radv_winsys_cs *cs, uint32_t value)
{
   cs->base.buf[cs->base.cdw++] = value;
}

static inline struct radv_winsys_cs *
radv_winsys_cs(struct ac_cmdbuf *base)
{
   return (struct radv_winsys_cs *)base;
}

static void
radv_winsys_cs_free_annotation(struct hash_entry *entry)
{
   free(entry->data);
}

static void
radv_winsys_cs_destroy(struct radv_winsys_cs *cs)
{
   _mesa_hash_table_destroy(cs->annotations, radv_winsys_cs_free_annotation);

   if (cs->ib_buffer)
      cs->ws->buffer_destroy(cs->ws, cs->ib_buffer);

   for (unsigned i = 0; i < cs->num_ib_buffers; ++i)
      cs->ws->buffer_destroy(cs->ws, cs->ib_buffers[i].bo);

   free(cs->ib_buffers);
}

static enum radeon_bo_domain
radv_winsys_cs_domain(struct radeon_winsys *ws)
{
   struct radeon_info *info = ws->query_info(ws);

   uint64_t allocated_vram_vis = ws->query_value(ws, RADEON_ALLOCATED_VRAM_VIS);
   bool enough_vram = info->all_vram_visible ||
                      allocated_vram_vis * 2 <= (uint64_t)info->vram_vis_size_kb * 1024;

   /* Bandwidth should be equivalent to at least PCIe 3.0 x8.
    * If there is no PCIe info, assume there is enough bandwidth.
    */
   const uint32_t bandwidth_mbps_threshold = 8 * 0.985 * 1024;
   bool enough_bandwidth = info->pcie_bandwidth_mbps >= bandwidth_mbps_threshold;

   bool use_sam =
      (enough_vram && enough_bandwidth && info->has_dedicated_vram);// && !(ws->perftest & RADV_PERFTEST_NO_SAM)) ||
      //(ws->perftest & RADV_PERFTEST_SAM);
   return use_sam ? RADEON_DOMAIN_VRAM : RADEON_DOMAIN_GTT;
}

static VkResult
radv_winsys_cs_bo_create(struct radv_winsys_cs *cs, uint32_t ib_size)
{
   struct radeon_info *info = cs->ws->query_info(cs->ws);

   /* Avoid memcpy from VRAM when a secondary cmdbuf can't always rely on IB2. */
   const bool can_always_use_ib2 = info->gfx_level >= GFX8 && cs->hw_ip == AMD_IP_GFX;
   const bool avoid_vram = cs->is_secondary && !can_always_use_ib2;
   const enum radeon_bo_domain domain = avoid_vram ? RADEON_DOMAIN_GTT : cs->ws->cs_domain(cs->ws);
   const enum radeon_bo_flag gtt_wc_flag = avoid_vram ? 0 : RADEON_FLAG_GTT_WC;
   /* Bypass GL2 because command buffers are read only once and it's better for latency. */
   const enum radeon_bo_flag flags = RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING |
                                     RADEON_FLAG_READ_ONLY | RADEON_FLAG_GL2_BYPASS | gtt_wc_flag;

   return cs->ws->buffer_create(cs->ws, ib_size, info->ip[cs->hw_ip].ib_alignment, domain, flags,
                                RADV_BO_PRIORITY_CS, 0, NULL, &cs->ib_buffer);
}

static VkResult
radv_winsys_cs_get_new_ib(struct radv_winsys_cs *cs, uint32_t ib_size)
{
   VkResult result;

   result = radv_winsys_cs_bo_create(cs, ib_size);
   if (result != VK_SUCCESS)
      return result;

   cs->ib_mapped = radv_buffer_map(cs->ws, cs->ib_buffer);
   if (!cs->ib_mapped) {
      cs->ws->buffer_destroy(cs->ws, cs->ib_buffer);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   cs->base.buf = (uint32_t *)cs->ib_mapped;
   cs->base.cdw = 0;
   cs->base.reserved_dw = 0;
   cs->base.max_dw = ib_size / 4 - 4;
   cs->chain_ib_va = cs->ib_buffer->va;
   cs->chain_ib_size = 0;

   if (cs->chain_ib)
      cs->ib_size_ptr = &cs->chain_ib_size;

   cs->ws->cs_add_buffer(&cs->base, cs->ib_buffer);

   return VK_SUCCESS;
}

static unsigned
radv_winsys_cs_get_initial_size(struct radeon_winsys *ws, enum amd_ip_type ip_type)
{
   struct radeon_info *info = ws->query_info(ws);

   const uint32_t ib_alignment = info->ip[ip_type].ib_alignment;
   assert(util_is_power_of_two_nonzero(ib_alignment));
   return align(20 * 1024 * 4, ib_alignment);
}

static VkResult
radv_winsys_cs_init(struct radv_winsys_cs *cs, struct radeon_winsys *ws, enum amd_ip_type ip_type,
                      bool is_secondary, bool chain_ib)
{
   struct radeon_info *info = ws->query_info(ws);
   uint32_t ib_size = radv_winsys_cs_get_initial_size(ws, ip_type);

   cs->ws = ws;

   cs->annotations = _mesa_hash_table_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!cs->annotations)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   cs->is_secondary = is_secondary;
   cs->ws = ws;
   cs->hw_ip = ip_type;
   cs->is_secondary = is_secondary;

   cs->chain_ib = chain_ib && (ip_type == AMD_IP_GFX || ip_type == AMD_IP_COMPUTE) &&
                  !(is_secondary && !info->can_chain_ib2);

   VkResult result = radv_winsys_cs_get_new_ib(cs, ib_size);
   if (result != VK_SUCCESS)
      radv_winsys_cs_destroy(cs);
    return result;
}

static uint32_t
radv_winsys_cs_get_nop_packet(struct radv_winsys_cs *cs)
{
   struct radeon_info *info = cs->ws->query_info(cs->ws);

   switch (cs->hw_ip) {
   case AMD_IP_GFX:
   case AMD_IP_COMPUTE:
      return info->gfx_ib_pad_with_type2 ? PKT2_NOP_PAD : PKT3_NOP_PAD;
   case AMD_IP_SDMA:
      return info->gfx_level == GFX6 ? 0xF0000000 : SDMA_NOP_PAD;
   case AMD_IP_UVD:
   case AMD_IP_UVD_ENC:
      return PKT2_NOP_PAD;
   case AMD_IP_VCN_DEC:
      return 0x81FF;
   case AMD_IP_VCN_ENC:
      return 0; /* NOPs are illegal in encode, so don't pad */
   default:
      UNREACHABLE("Unknown IP type");
   }
}

/**
 * Emit a PKT3 NOP packet for the graphics or compute queue.
 *
 * Emit a single NOP packet to minimize CP overhead because NOP is a variable-sized
 * packet. The size of the packet body after the header is always count + 1.
 * If count == -1, there is no packet body. NOP is the only packet that can have
 * count == -1, which is the definition of PKT3_NOP_PAD (count == 0x3fff means -1).
 *
 * Note that GFX6 doesn't support PKT3_NOP with count == -1
 */
static void
radv_winsys_cs_emit_pkt3_nop(struct radv_winsys_cs *cs, const unsigned num_dw)
{
   struct radeon_info *info = cs->ws->query_info(cs->ws);

   assert(num_dw >= (info->gfx_ib_pad_with_type2 ? 2 : 1));

   radv_winsys_emit_unchecked(cs, PKT3(PKT3_NOP, num_dw - 2, 0));
   cs->base.cdw += num_dw - 1;
}

/**
 * Emit one or more NOP packets to fill the specified amount of dwords.
 * Should only be called for IP types that have a NOP packet.
 */
static void
radv_winsys_cs_emit_nops(struct radv_winsys_cs *cs, const unsigned num_dw)
{
   const enum amd_ip_type ip_type = cs->hw_ip;
   assert(ip_type != AMD_IP_VCN_ENC); /* VCN_ENC has no NOP packets. */

   if (!num_dw)
      return;

   /* Emit a single, larger PKT3 NOP packet to fill the specified amount of dwords. */
   if (num_dw > 1 && (ip_type == AMD_IP_GFX || ip_type == AMD_IP_COMPUTE)) {
      radv_winsys_cs_emit_pkt3_nop(cs, num_dw);
      return;
   }

   const uint32_t nop_packet = radv_winsys_cs_get_nop_packet(cs);

   for (uint32_t i = 0; i < num_dw; ++i)
      radv_winsys_emit_unchecked(cs, nop_packet);
}

static void
radv_winsys_cs_pad(struct ac_cmdbuf *_cs, unsigned leave_dw_space)
{
   struct radv_winsys_cs *cs = radv_winsys_cs(_cs);
   struct radeon_info *info = cs->ws->query_info(cs->ws);
   const enum amd_ip_type ip_type = cs->hw_ip;

   /* Don't pad on VCN encode/unified as no NOPs */
   if (ip_type == AMD_IP_VCN_ENC)
      return;

   /* Don't add padding to 0 length UVD due to kernel. */
   if (ip_type == AMD_IP_UVD && cs->base.cdw == 0)
      return;

   const uint32_t pad_dw_mask = info->ip[ip_type].ib_pad_dw_mask;
   const uint32_t unaligned_dw = (cs->base.cdw + leave_dw_space) & pad_dw_mask;

   if (unaligned_dw) {
      /* Pad the IB with NOP packets to ensure that the end of the IB is correctly aligned. */
      radv_winsys_cs_emit_nops(cs, pad_dw_mask + 1 - unaligned_dw);
   } else if (cs->base.cdw == 0 && leave_dw_space == 0) {
      /* Emit NOPs to avoid submitting a completely empty IB. */
      radv_winsys_cs_emit_nops(cs, pad_dw_mask + 1);
   }

   assert(((cs->base.cdw + leave_dw_space) & pad_dw_mask) == 0);
}

static void
radv_winsys_cs_add_ib_buffer(struct radv_winsys_cs *cs, struct radeon_winsys_bo *bo, uint64_t va, uint32_t cdw)
{
   if (cs->num_ib_buffers == cs->max_num_ib_buffers) {
      unsigned max_num_ib_buffers = MAX2(1, cs->max_num_ib_buffers * 2);
      struct radv_winsys_ib *ib_buffers = realloc(cs->ib_buffers, max_num_ib_buffers * sizeof(*ib_buffers));
      if (!ib_buffers) {
         cs->status = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }
      cs->max_num_ib_buffers = max_num_ib_buffers;
      cs->ib_buffers = ib_buffers;
   }

   cs->ib_buffers[cs->num_ib_buffers].bo = bo;
   cs->ib_buffers[cs->num_ib_buffers].va = va;
   cs->ib_buffers[cs->num_ib_buffers++].cdw = cdw;
}

static void
radv_winsys_restore_last_ib(struct radv_winsys_cs *cs)
{
   struct radv_winsys_ib *ib = &cs->ib_buffers[--cs->num_ib_buffers];
   assert(ib->bo);
   cs->ib_buffer = ib->bo;
}

static void
radv_winsys_cs_reset(struct radv_winsys_cs *cs)
{
   cs->base.cdw = 0;
   cs->base.reserved_dw = 0;
   cs->status = VK_SUCCESS;
   cs->calls_ib2 = false;

   /* When the CS is finalized and IBs are not allowed, use last IB. */
   assert(cs->ib_buffer || cs->num_ib_buffers);
   if (!cs->ib_buffer)
      radv_winsys_restore_last_ib(cs);

   cs->ws->cs_add_buffer(&cs->base, cs->ib_buffer);

   for (unsigned i = 0; i < cs->num_ib_buffers; ++i)
      cs->ws->buffer_destroy(cs->ws, cs->ib_buffers[i].bo);

   cs->num_ib_buffers = 0;
   cs->chain_ib_va = cs->ib_buffer->va;

   cs->chain_ib_size = 0;

   if (cs->chain_ib)
      cs->ib_size_ptr = &cs->chain_ib_size;

   _mesa_hash_table_destroy(cs->annotations, radv_winsys_cs_free_annotation);
   cs->annotations = NULL;
}

static void
radv_winsys_cs_grow(struct ac_cmdbuf *_cs, size_t min_size)
{
   struct radv_winsys_cs *cs = radv_winsys_cs(_cs);
   struct radeon_info *info = cs->ws->query_info(cs->ws);

   if (cs->status != VK_SUCCESS) {
      cs->base.cdw = 0;
      return;
   }

   const uint32_t ib_alignment = info->ip[cs->hw_ip].ib_alignment;

   cs->ws->cs_finalize(_cs);

   uint64_t ib_size = MAX2(min_size * 4 + 16, cs->base.max_dw * 4 * 2);

   ib_size = align(MIN2(ib_size, ~C_3F3_IB_SIZE), ib_alignment);

   VkResult result = radv_winsys_cs_bo_create(cs, ib_size);
   if (result != VK_SUCCESS) {
      cs->base.cdw = 0;
      cs->status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      radv_winsys_restore_last_ib(cs);
   }

   cs->ib_mapped = radv_buffer_map(cs->ws, cs->ib_buffer);
   if (!cs->ib_mapped) {
      cs->ws->buffer_destroy(cs->ws, cs->ib_buffer);
      cs->base.cdw = 0;

      fprintf(stderr, "Failed to map new IB buffer for IP %u\n", cs->hw_ip);
      /* VK_ERROR_MEMORY_MAP_FAILED is not valid for vkEndCommandBuffer. */
      cs->status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      radv_winsys_restore_last_ib(cs);
   }

   cs->ws->cs_add_buffer(&cs->base, cs->ib_buffer);

   if (cs->chain_ib) {
      cs->base.buf[cs->base.cdw - 4] = PKT3(PKT3_INDIRECT_BUFFER, 2, 0);
      cs->base.buf[cs->base.cdw - 3] = cs->ib_buffer->va;
      cs->base.buf[cs->base.cdw - 2] = cs->ib_buffer->va >> 32;
      cs->base.buf[cs->base.cdw - 1] = S_3F3_CHAIN(1) | S_3F3_VALID(1);

      cs->ib_size_ptr = cs->base.buf + cs->base.cdw - 1;
   }

   cs->base.buf = (uint32_t *)cs->ib_mapped;
   cs->base.cdw = 0;
   cs->base.reserved_dw = 0;
   cs->base.max_dw = ib_size / 4 - 4;
}

static VkResult
radv_winsys_cs_finalize(struct ac_cmdbuf *_cs)
{
   struct radv_winsys_cs *cs = radv_winsys_cs(_cs);

   assert(cs->base.cdw <= cs->base.reserved_dw);

   if (cs->chain_ib) {
      /* Pad with NOPs but leave 4 dwords for INDIRECT_BUFFER. */
      radv_winsys_cs_pad(_cs, 4);

      /* Emit 4 dwords of NOP, these will be replaced by the chaining INDIRECT_BUFFER. */
      radv_winsys_cs_emit_nops(cs, 4);

      assert(cs->base.cdw <= ~C_3F3_IB_SIZE);
      *cs->ib_size_ptr |= cs->base.cdw;
   } else {
      radv_winsys_cs_pad(_cs, 0);
   }

   /* Append the current (last) IB to the array of IB buffers. */
   radv_winsys_cs_add_ib_buffer(cs, cs->ib_buffer, cs->ib_buffer->va,
                                cs->chain_ib ? G_3F3_IB_SIZE(*cs->ib_size_ptr) : cs->base.cdw);

   /* Prevent freeing this BO twice. */
   cs->ib_buffer = NULL;

   cs->chained_to = NULL;

   assert(cs->base.cdw <= cs->base.max_dw + 4);

   return cs->status;
}

static void
radv_winsys_cs_unchain(struct ac_cmdbuf *cs)
{
   struct radv_winsys_cs *acs = radv_winsys_cs(cs);

   if (!acs->chained_to)
      return;

   assert(cs->cdw <= cs->max_dw + 4);

   acs->chained_to = NULL;
   cs->buf[cs->cdw - 4] = PKT3(PKT3_NOP, 2, 0);
}

static bool
radv_winsys_cs_chain(struct ac_cmdbuf *cs, struct ac_cmdbuf *next_cs, bool pre_ena)
{
   /* Chains together two CS (command stream) objects by editing
    * the end of the first CS to add a command that jumps to the
    * second CS.
    *
    * After this, it is enough to submit the first CS to the GPU
    * and not necessary to submit the second CS because it is already
    * executed by the first.
    */

   struct radv_winsys_cs *acs = radv_winsys_cs(cs);
   struct radv_winsys_cs *next_acs = radv_winsys_cs(next_cs);

   /* Only some HW IP types have packets that we can use for chaining. */
   if (!acs->chain_ib)
      return false;

   assert(cs->cdw <= cs->max_dw + 4);

   acs->chained_to = next_acs;

   cs->buf[cs->cdw - 4] = PKT3(PKT3_INDIRECT_BUFFER, 2, 0);
   cs->buf[cs->cdw - 3] = next_acs->chain_ib_va;
   cs->buf[cs->cdw - 2] = next_acs->chain_ib_va >> 32;
   cs->buf[cs->cdw - 1] = S_3F3_CHAIN(1) | S_3F3_VALID(1) | S_3F3_PRE_ENA(pre_ena) | next_acs->chain_ib_size;

   return true;
}

/**
 * Emit IB2 packets to execute a secondary CS.
 * IB2 are a special variant of the INDIRECT_BUFFER packet which are used inside an IB.
 * An IB2 packet can execute another IB and then continue execution of the current IB.
 *
 * When the secondary CS uses IB chaining: we only emit a single IB2 packet which
 * jumps to the first IB of the secondary, then executes the entire secondary and returns.
 *
 * When the secondary CS does not support IB chaining or IB2 chaining is disabled:
 * emit an IB2 packet for every IB inside the secondary CS.
 */
static void
radv_winsys_cs_emit_secondary_ib2(struct radv_winsys_cs *parent, struct radv_winsys_cs *child)
{
   //struct radeon_info *info = parent->ws->query_info(parent->ws);

   /* When IB2 chaining isn't allowed, the secondary CS shouldn't use IB chaining. */
   // FIXME assert(info->can_chain_ib2 || !child->chain_ib);
   const uint32_t num_ib2 = child->chain_ib ? 1 : child->num_ib_buffers;

   parent->calls_ib2 = true;

   for (uint32_t i = 0; i < num_ib2; ++i) {
      if (parent->base.cdw + 4 > parent->base.max_dw)
         radv_winsys_cs_grow(&parent->base, 4);

      parent->base.reserved_dw = MAX2(parent->base.reserved_dw, parent->base.cdw + 4);

      const uint64_t va = child->ib_buffers[i].va;
      const uint32_t size = child->ib_buffers[i].cdw;

      /* Not setting the CHAIN bit will launch an IB2. */
      ac_emit_cp_indirect_buffer(&parent->base, va, size, 0, false);

      assert(parent->base.cdw <= parent->base.max_dw);
   }
}

static void
radv_winsys_cs_execute_secondary(struct radv_winsys_cs *parent, struct radv_winsys_cs *child, bool allow_ib2)
{
   struct radeon_winsys *ws = parent->ws;
   const bool use_ib2 = !parent->is_secondary && allow_ib2 && parent->hw_ip == AMD_IP_GFX;

   if (use_ib2) {
      radv_winsys_cs_emit_secondary_ib2(parent, child);
   } else {
      /* The copied packets keep any IB2 call of the child. */
      parent->calls_ib2 |= child->calls_ib2;

      /* Grow the current CS and copy the contents of the secondary CS. */
      for (unsigned i = 0; i < child->num_ib_buffers; i++) {
         struct radv_winsys_ib *ib = &child->ib_buffers[i];
         uint32_t cdw = ib->cdw;
         uint8_t *mapped;

         /* Do not copy the original chain link for IBs. */
         if (child->chain_ib)
            cdw -= 4;

         assert(ib->bo);

         if (parent->base.cdw + cdw > parent->base.max_dw)
            radv_winsys_cs_grow(&parent->base, cdw);

         parent->base.reserved_dw = MAX2(parent->base.reserved_dw, parent->base.cdw + cdw);

         mapped = radv_buffer_map(ws, ib->bo);
         if (!mapped) {
            parent->status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            return;
         }

         memcpy(parent->base.buf + parent->base.cdw, mapped, 4 * cdw);
         parent->base.cdw += cdw;
      }
   }
}

static void
radv_winsys_cs_execute_ib(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *bo, uint64_t va, const uint32_t cdw,
                          const bool predicate)
{
   struct radv_winsys_cs *cs = radv_winsys_cs(_cs);
   struct radeon_info *info = cs->ws->query_info(cs->ws);
   const uint64_t ib_va = bo ? bo->va : va;

   if (cs->status != VK_SUCCESS)
      return;

   cs->calls_ib2 = true;

   assert(ib_va && ib_va % info->ip[cs->hw_ip].ib_alignment == 0);
   assert(cs->hw_ip == AMD_IP_GFX && cdw <= ~C_3F3_IB_SIZE);

   ac_emit_cp_indirect_buffer(&cs->base, ib_va, cdw, 0, predicate);
}

static void
radv_winsys_cs_chain_dgc_ib(struct ac_cmdbuf *_cs, uint64_t va, uint32_t cdw, uint64_t trailer_va, const bool predicate)
{
   struct radv_winsys_cs *cs = radv_winsys_cs(_cs);
   struct radeon_info *info = cs->ws->query_info(cs->ws);

   if (cs->status != VK_SUCCESS)
      return;

   assert(info->gfx_level >= GFX8);

   /* GFX calls the DGC buffer as an IB2; other queues chain into it and back. */
   cs->calls_ib2 = true;

   if (cs->hw_ip == AMD_IP_GFX) {
      /* Use IB2 for executing DGC CS on GFX. */
      cs->ws->cs_execute_ib(_cs, NULL, va, cdw, predicate);
   } else {
      assert(va && va % info->ip[cs->hw_ip].ib_alignment == 0);
      assert(cdw <= ~C_3F3_IB_SIZE);

      /* Emit a WRITE_DATA packet to patch the DGC CS. */
      const uint32_t chain_data[] = {
         PKT3(PKT3_INDIRECT_BUFFER, 2, 0),
         0,
         0,
         S_3F3_CHAIN(1) | S_3F3_VALID(1),
      };

      ac_emit_cp_write_data(&cs->base, V_371_MICRO_ENGINE, V_371_MEMORY, trailer_va, ARRAY_SIZE(chain_data), chain_data,
                            false);

      /* Keep pointers for patching later. */
      uint64_t *ib_va_ptr = (uint64_t *)(cs->base.buf + cs->base.cdw - 3);
      uint32_t *ib_size_ptr = cs->base.buf + cs->base.cdw - 1;

      /* Writeback L2 because CP isn't coherent with L2 on GFX6-8. */
      if (info->gfx_level == GFX8) {
         ac_emit_cp_acquire_mem(&cs->base, GFX8, AMD_IP_COMPUTE, V_581A_MICRO_ENGINE,
                                S_0301F0_TC_WB_ACTION_ENA(1) | S_0301F0_TC_NC_ACTION_ENA(1), NULL, NULL);
      }

      /* Finalize the current CS. */
      cs->ws->cs_finalize(_cs);

      /* Chain the current CS to the DGC CS. */
      _cs->buf[_cs->cdw - 4] = PKT3(PKT3_INDIRECT_BUFFER, 2, 0);
      _cs->buf[_cs->cdw - 3] = va;
      _cs->buf[_cs->cdw - 2] = va >> 32;
      _cs->buf[_cs->cdw - 1] = S_3F3_CHAIN(1) | S_3F3_VALID(1) | cdw;

      /* Allocate a new CS BO with initial size. */
      const uint64_t ib_size = radv_winsys_cs_get_initial_size(cs->ws, cs->hw_ip);

      VkResult result = radv_winsys_cs_bo_create(cs, ib_size);
      if (result != VK_SUCCESS) {
         cs->base.cdw = 0;
         cs->status = result;
         return;
      }

      cs->ib_mapped = radv_buffer_map(cs->ws, cs->ib_buffer);
      if (!cs->ib_mapped) {
         cs->base.cdw = 0;
         cs->status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         return;
      }

      cs->ws->cs_add_buffer(&cs->base, cs->ib_buffer);

      /* Chain back the trailer (DGC CS) to the newly created one. */
      *ib_va_ptr = cs->ib_buffer->va;
      cs->ib_size_ptr = ib_size_ptr;

      cs->base.buf = (uint32_t *)cs->ib_mapped;
      cs->base.cdw = 0;
      cs->base.reserved_dw = 0;
      cs->base.max_dw = ib_size / 4 - 4;
   }
}

static bool
radv_winsys_cs_get_cpu_addr(struct radv_winsys_cs *cs, uint64_t addr, void **cpu_addr)
{
   for (unsigned i = 0; i < cs->num_ib_buffers; ++i) {
      struct radeon_winsys_bo *bo = cs->ib_buffers[i].bo;

      if (addr >= bo->va && addr - bo->va < bo->size) {
         void *map = radv_buffer_map(cs->ws, bo);
         if (map) {
            *cpu_addr = (char *)map + (addr - bo->va);
            return true;
         }
      }
   }

   return false;
}

static const char *
radv_winsys_get_dump_ibs_str(enum radv_cs_dump_type type)
{
   switch (type) {
   case RADV_CS_DUMP_TYPE_PREAMBLE_IBS:
      return "Preamble";
   case RADV_CS_DUMP_TYPE_MAIN_IBS:
      return "Main";
   case RADV_CS_DUMP_TYPE_POSTAMBLE_IBS:
      return "Postamble";
   default:
      UNREACHABLE("invalid CS dump type");
   }
}

static void
radv_winsys_cs_dump(struct ac_cmdbuf *_cs, FILE *file, const int *trace_ids, int trace_id_count,
                     enum radv_cs_dump_type type)
{
   struct radv_winsys_cs *cs = (struct radv_winsys_cs *)_cs;
   struct radeon_winsys *ws = cs->ws;
   struct radeon_info *info = cs->ws->query_info(cs->ws);
   const bool dump_ibs = type == RADV_CS_DUMP_TYPE_PREAMBLE_IBS || type == RADV_CS_DUMP_TYPE_MAIN_IBS ||
                         type == RADV_CS_DUMP_TYPE_POSTAMBLE_IBS;
   const bool dump_ctx_rolls = type == RADV_CS_DUMP_TYPE_CTX_ROLLS;

   if (cs->chain_ib) {
      struct ac_addr_info addr_info;
      ws->cs_get_cpu_addr(cs, cs->ib_buffers[0].va, &addr_info);
      assert(addr_info.cpu_addr);

      if (dump_ibs) {
         struct ac_ib_parser ib_parser = {
            .f = file,
            .ib = addr_info.cpu_addr,
            .num_dw = cs->ib_buffers[0].cdw,
            .trace_ids = trace_ids,
            .trace_id_count = trace_id_count,
            .gfx_level = info->gfx_level,
            .vcn_version = info->vcn_ip_version,
            .family = info->family,
            .ip_type = cs->hw_ip,
            .addr_callback = ws->cs_get_cpu_addr,
            .addr_callback_data = cs,
            .annotations = cs->annotations,
         };

         char name[64];
         snprintf(name, sizeof(name), "%s IB", radv_winsys_get_dump_ibs_str(type));

         ac_parse_ib(&ib_parser, name);
      } else {
         uint32_t *ib_dw = addr_info.cpu_addr;
         ac_gather_context_rolls(file, &ib_dw, &cs->ib_buffers[0].cdw, 1, cs->annotations, info);
      }
   } else {
      uint32_t **ibs = dump_ctx_rolls ? malloc(cs->num_ib_buffers * sizeof(uint32_t *)) : NULL;
      uint32_t *ib_dw_sizes = dump_ctx_rolls ? malloc(cs->num_ib_buffers * sizeof(uint32_t)) : NULL;

      for (unsigned i = 0; i < cs->num_ib_buffers; i++) {
         struct radv_winsys_ib *ib = &cs->ib_buffers[i];
         char name[64];
         void *mapped;

         mapped = radv_buffer_map(ws, ib->bo);
         if (!mapped)
            continue;

         if (cs->num_ib_buffers > 1) {
            snprintf(name, sizeof(name), "%s IB (chunk %d)", radv_winsys_get_dump_ibs_str(type), i);
         } else {
            snprintf(name, sizeof(name), "%s IB", radv_winsys_get_dump_ibs_str(type));
         }

         if (dump_ibs) {
            struct ac_ib_parser ib_parser = {
               .f = file,
               .ib = mapped,
               .num_dw = ib->cdw,
               .trace_ids = trace_ids,
               .trace_id_count = trace_id_count,
               .gfx_level = info->gfx_level,
               .vcn_version = info->vcn_ip_version,
               .family = info->family,
               .ip_type = cs->hw_ip,
               .addr_callback = ws->cs_get_cpu_addr,
               .addr_callback_data = cs,
               .annotations = cs->annotations,
            };

            ac_parse_ib(&ib_parser, name);
         } else {
            ibs[i] = mapped;
            ib_dw_sizes[i] = ib->cdw;
         }
      }

      if (dump_ctx_rolls) {
         ac_gather_context_rolls(file, ibs, ib_dw_sizes, cs->num_ib_buffers, cs->annotations, info);

         free(ibs);
         free(ib_dw_sizes);
      }
   }
}

static void
radv_winsys_cs_annotate(struct ac_cmdbuf *_cs, const char *annotation)
{
   struct radv_winsys_cs *cs = (struct radv_winsys_cs *)_cs;

   if (!cs->annotations) {
      cs->annotations = _mesa_pointer_hash_table_create(NULL);
      if (!cs->annotations)
         return;
   }

   struct hash_entry *entry = _mesa_hash_table_search(cs->annotations, _cs->buf + _cs->cdw);
   if (entry) {
      char *old_annotation = entry->data;
      char *new_annotation = calloc(strlen(old_annotation) + strlen(annotation) + 5, 1);

      free(old_annotation);
      _mesa_hash_table_insert(cs->annotations, _cs->buf + _cs->cdw, new_annotation);
   } else {
      _mesa_hash_table_insert(cs->annotations, _cs->buf + _cs->cdw, strdup(annotation));
   }
}

#endif /* RADV_WINSYS_CS_H */
