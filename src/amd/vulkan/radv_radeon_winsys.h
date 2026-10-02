/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Based on radeon_winsys.h which is:
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2010 Marek Olšák <maraeo@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_RADEON_WINSYS_H
#define RADV_RADEON_WINSYS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/u_math.h"
#include "util/u_memory.h"
#include <vulkan/vulkan_core.h>
#include "ac_cmdbuf.h"
#include "amd_family.h"

struct ac_addr_info;
struct radeon_info;
struct vk_device;
struct vk_sync_type;
struct vk_sync_wait;
struct vk_sync_signal;
struct wsi_device;

enum radeon_bo_domain { /* bitfield */
                        RADEON_DOMAIN_GTT = 2,
                        RADEON_DOMAIN_VRAM = 4,
                        RADEON_DOMAIN_VRAM_GTT = RADEON_DOMAIN_VRAM | RADEON_DOMAIN_GTT,
                        RADEON_DOMAIN_GDS = 8,
                        RADEON_DOMAIN_OA = 16,
};

enum radeon_bo_flag { /* bitfield */
                      RADEON_FLAG_GTT_WC = (1 << 0),
                      RADEON_FLAG_CPU_ACCESS = (1 << 1),
                      RADEON_FLAG_NO_CPU_ACCESS = (1 << 2),
                      RADEON_FLAG_VIRTUAL = (1 << 3),
                      RADEON_FLAG_GL2_BYPASS = (1 << 4),
                      RADEON_FLAG_IMPLICIT_SYNC = (1 << 5),
                      RADEON_FLAG_NO_INTERPROCESS_SHARING = (1 << 6),
                      RADEON_FLAG_READ_ONLY = (1 << 7),
                      RADEON_FLAG_32BIT = (1 << 8),
                      RADEON_FLAG_PREFER_LOCAL_BO = (1 << 9),
                      RADEON_FLAG_REPLAYABLE = (1 << 10),
                      RADEON_FLAG_DISCARDABLE = (1 << 11),
                      RADEON_FLAG_GFX12_ALLOW_DCC = (1 << 12),
                      RADEON_FLAG_VM_UPDATE_WAIT = (1 << 13),
                      RADEON_FLAG_VM_PAD_1PAGE = (1 << 14),
                      RADEON_FLAG_ENCRYPTED = (1 << 15),
                      RADEON_FLAG_EMULATE_SPARSE_RESIDENCY = (1 << 16),
};

enum radeon_ctx_priority {
   RADEON_CTX_PRIORITY_INVALID = -1,
   RADEON_CTX_PRIORITY_LOW = 0,
   RADEON_CTX_PRIORITY_MEDIUM,
   RADEON_CTX_PRIORITY_HIGH,
   RADEON_CTX_PRIORITY_REALTIME,
};

enum radeon_ctx_pstate {
   RADEON_CTX_PSTATE_NONE = 0,
   RADEON_CTX_PSTATE_STANDARD,
   RADEON_CTX_PSTATE_MIN_SCLK,
   RADEON_CTX_PSTATE_MIN_MCLK,
   RADEON_CTX_PSTATE_PEAK,
};

enum radeon_value_id {
   RADEON_ALLOCATED_VRAM,
   RADEON_ALLOCATED_VRAM_VIS,
   RADEON_ALLOCATED_GTT,
   RADEON_TIMESTAMP,
   RADEON_NUM_BYTES_MOVED,
   RADEON_NUM_EVICTIONS,
   RADEON_NUM_VRAM_CPU_PAGE_FAULTS,
   RADEON_VRAM_USAGE,
   RADEON_VRAM_VIS_USAGE,
   RADEON_GTT_USAGE,
   RADEON_GPU_TEMPERATURE,
   RADEON_CURRENT_SCLK,
   RADEON_CURRENT_MCLK,
};

#define RADEON_SURF_TYPE_MASK     0xFF
#define RADEON_SURF_TYPE_SHIFT    0
#define RADEON_SURF_TYPE_1D       0
#define RADEON_SURF_TYPE_2D       1
#define RADEON_SURF_TYPE_3D       2
#define RADEON_SURF_TYPE_CUBEMAP  3
#define RADEON_SURF_TYPE_1D_ARRAY 4
#define RADEON_SURF_TYPE_2D_ARRAY 5
#define RADEON_SURF_MODE_MASK     0xFF
#define RADEON_SURF_MODE_SHIFT    8

#define RADEON_SURF_GET(v, field) (((v) >> RADEON_SURF_##field##_SHIFT) & RADEON_SURF_##field##_MASK)
#define RADEON_SURF_SET(v, field) (((v) & RADEON_SURF_##field##_MASK) << RADEON_SURF_##field##_SHIFT)
#define RADEON_SURF_CLR(v, field) ((v) & ~(RADEON_SURF_##field##_MASK << RADEON_SURF_##field##_SHIFT))

enum radeon_bo_layout {
   RADEON_LAYOUT_LINEAR = 0,
   RADEON_LAYOUT_TILED,
   RADEON_LAYOUT_SQUARETILED,

   RADEON_LAYOUT_UNKNOWN
};

enum radeon_bo_metadata_type {
   RADEON_METADATA_TYPE_NONE,
   RADEON_METADATA_TYPE_UMD,
   RADEON_METADATA_TYPE_KMW,
};

/* Tiling info for display code, DRI sharing, and other data. */
struct radeon_bo_metadata {
   /* Tiling flags describing the texture layout for display code
    * and DRI sharing.
    */
   union {
      struct {
         enum radeon_bo_layout microtile;
         enum radeon_bo_layout macrotile;
         unsigned pipe_config;
         unsigned bankw;
         unsigned bankh;
         unsigned tile_split;
         unsigned mtilea;
         unsigned num_banks;
         unsigned stride;
         bool scanout;
      } legacy;

      struct {
         /* surface flags */
         unsigned swizzle_mode : 5;
         bool scanout;
         uint32_t dcc_offset_256b;
         uint32_t dcc_pitch_max;
         bool dcc_independent_64b_blocks;
         bool dcc_independent_128b_blocks;
         unsigned dcc_max_compressed_block_size;
      } gfx9;

      struct {
         unsigned swizzle_mode : 3;
         unsigned dcc_max_compressed_block : 3;
         unsigned dcc_data_format : 6;
         unsigned dcc_number_type : 3;
         bool dcc_write_compress_disable;
         bool scanout;
      } gfx12;
   } u;

   enum radeon_bo_metadata_type metadata_type;
   union {
      /* Additional metadata associated with the buffer, in bytes.
       * The maximum size is 64 * 4. This is opaque for the winsys & kernel.
       * Supported by amdgpu only.
       */
      struct {
         uint32_t size_metadata;
         uint32_t metadata[64];
      } umd;

      /* Non-opaque surface metadata (WDDM2/KMW path).
       */
      struct {
         uint16_t tile_swizzle;
         uint32_t pitch_bytes;
         uint64_t surf_size;

         uint64_t dcc_offset;
         uint64_t display_dcc_offset;
         bool dcc_pipe_aligned;
         bool dcc_rb_aligned;

         uint64_t cmask_offset;
         uint64_t fmask_offset;
         uint64_t fmask_xor;
         uint8_t fmask_swizzle_mode;

         uint64_t htile_offset;

         /* GFX12 HiSZ */
         uint64_t hi_z_offset;
         uint64_t hi_s_offset;
         uint8_t hi_z_swizzle_mode;
         uint8_t hi_s_swizzle_mode;
      } kmw;
   };
};

struct radeon_winsys_ctx;

struct radeon_winsys_bo {
   uint32_t handle;
   uint64_t va;
   uint64_t size;
   /* buffer is created with AMDGPU_GEM_CREATE_VM_ALWAYS_VALID */
   bool is_local;
   bool vram_no_cpu_access;
   /* buffer is added to the BO list of all submissions */
   bool use_global_list;
   bool gfx12_allow_dcc;
   bool is_virtual; /* sparse buffers */
   enum radeon_bo_domain initial_domain;
   uint64_t obj_id;
};

struct radv_winsys_submit_info {
   enum amd_ip_type ip_type;
   int queue_index;
   bool is_gang;
   unsigned cs_count;
   unsigned initial_preamble_count;
   unsigned continue_preamble_count;
   unsigned postamble_count;
   struct ac_cmdbuf **cs_array;
   struct ac_cmdbuf **initial_preamble_cs;
   struct ac_cmdbuf **continue_preamble_cs;
   struct ac_cmdbuf **postamble_cs;
   bool uses_shadow_regs;
   bool secure;
};

/* Kernel effectively allows 0-31. This sets some priorities for fixed
 * functionality buffers */
enum {
   RADV_BO_PRIORITY_APPLICATION_MAX = 28,

   /* virtual buffers have 0 priority since the priority is not used. */
   RADV_BO_PRIORITY_VIRTUAL = 0,

   RADV_BO_PRIORITY_METADATA = 10,
   /* This should be considerably lower than most of the stuff below,
    * but how much lower is hard to say since we don't know application
    * assignments. Put it pretty high since it is GTT anyway. */
   RADV_BO_PRIORITY_QUERY_POOL = 29,

   RADV_BO_PRIORITY_DESCRIPTOR = 30,
   RADV_BO_PRIORITY_UPLOAD_BUFFER = 30,
   RADV_BO_PRIORITY_FENCE = 30,
   RADV_BO_PRIORITY_SHADER = 31,
   RADV_BO_PRIORITY_SCRATCH = 31,
   RADV_BO_PRIORITY_CS = 31,
};

struct radv_winsys_gpuvm_fault_info {
   uint64_t addr;
   uint32_t status;
   uint32_t vmhub;
};

enum radv_cs_dump_type {
   RADV_CS_DUMP_TYPE_PREAMBLE_IBS,
   RADV_CS_DUMP_TYPE_MAIN_IBS,
   RADV_CS_DUMP_TYPE_POSTAMBLE_IBS,
   RADV_CS_DUMP_TYPE_CTX_ROLLS,
};

/* bc250: the draw-path counters of the application's command buffers (radv_cmd_buffer.c), in the order
 * radeon_winsys::draw_stats_add takes them. A render pass is one vkCmdBeginRendering of the application;
 * a restart is a pass on the same attachments, render area, layers and view mask as the pass before it
 * in the command buffer, counted by the strongest kind of command recorded between the two (dispatch,
 * transfer, a clear load op of the restart, query, barrier with a write or layout change, read-only
 * barrier, none); end_* is the first command after a pass ends. A read-only barrier has no write access
 * in either scope and no layout change. pipelines_* and push_calls include the binds radv_meta_end
 * makes to restore the application's state, meta_restores_* counts those. vb_desc_writes counts the
 * vertex buffer descriptor uploads, vb_desc_reusable those the reuse of radv_flush_vertex_descriptors
 * would have skipped (it is off while counting), vb_desc_reuse_wrong those of them whose descriptors
 * differed from the ones uploaded before, vb_desc_records_wrong the per-attribute vertex counts that
 * differ from the direct division (radv_vb_attrib_records), which must stay zero, in uploads of
 * application and meta commands alike. pc_stage_emits counts the push constant emissions of one
 * shader stage, pc_stage_same those that wrote what the stage's previous counted emission wrote (same
 * shader, inline push constants and pointer), pc_regs_wrong those whose precomputed registers
 * (radv_shader_push_const_regs) differ from the shader info, which must stay zero. Of the binds counted
 * in pipelines_gfx, pipe_same rebind the bound pipeline, and of the others pipe_vs_same keep the bound
 * vertex shader (radv_get_shader, so the merged shader that holds it), pipe_ps_same the fragment shader
 * and pipe_shaders_same every graphics stage (the same radv_shader objects, which the shader cache shares
 * between pipelines with identical binaries). */
#define RADV_DRAW_STATS_DRAW(X)                                                                             \
   X(cmdbufs) X(passes) X(passes_clear) X(restarts) X(restart_none) X(restart_barrier_ro) X(restart_barrier) \
   X(restart_query) X(restart_clear) X(restart_transfer) X(restart_dispatch) X(end_begin) X(end_barrier)     \
   X(end_query) X(end_transfer) X(end_dispatch) X(end_close) X(draws) X(dgc) X(pipelines_gfx)               \
   X(pipelines_cs) X(meta_restores_gfx) X(meta_restores_cs) X(vb_calls) X(vb_bindings) X(ib_calls)          \
   X(push_calls) X(barrier_calls) X(barrier_back_to_back) X(barrier_in_pass) X(barrier_ro) X(barrier_mem)   \
   X(barrier_buf) X(barrier_img) X(barrier_layout) X(transfers) X(clear_attachments) X(dispatches)          \
   X(queries) X(vb_desc_writes) X(vb_desc_reusable) X(vb_desc_reuse_wrong) X(pc_stage_emits)                \
   X(pc_stage_same) X(pc_regs_wrong) X(pipe_same) X(pipe_vs_same) X(pipe_ps_same) X(pipe_shaders_same)  \
   X(vb_desc_records_wrong)

/* bc250: the synchronization counters, on a line of their own in the dump. fl_* count the cache flushes
 * radv_emit_cache_flush emits, in RADV's meta operations too (fl_meta of them inside one, fl_end at
 * vkEndCommandBuffer), by what ac_gfx10_emit_barrier makes of the flags: fl_eop_* a RELEASE_MEM with a
 * TS event that the CP then waits for with WAIT_REG_MEM, a full drain of the pipeline (cbdb:
 * CACHE_FLUSH_AND_INV_TS; cb: FLUSH_AND_INV_CB_DATA_TS and the CB_META event; db: FLUSH_AND_INV_DB_DATA_TS
 * and the DB_META event; bop: BOTTOM_OF_PIPE_TS), which already waits for the shaders, so fl_vs, fl_ps and
 * fl_cs (the VS/PS/CS_PARTIAL_FLUSH events) count only flushes without one; fl_vgt VGT_FLUSH; fl_l2_inv a
 * write-back and invalidation of the whole L2 (INV_L2), fl_l2_wb a write-back only, fl_l2_meta the
 * metadata cache alone; fl_vmem the vector L0 and L1 caches, fl_smem the scalar cache, fl_icache the
 * instruction cache; fl_pfp PFP_SYNC_ME. bar_* and cause_* count the application's vkCmdPipelineBarrier2
 * calls by the flushes their own access masks ask for (before they merge with pending ones): bar_cb and
 * bar_db a CB or DB flush (so a drain), bar_l2 an L2 invalidation on the source side; cause_rt_write,
 * cause_ds_write and cause_xfer_write a source scope with color attachment, depth-stencil attachment or
 * transfer writes (each makes radv_src_access_flush flush CB, DB or both), cause_meta_storage storage
 * writes to an image without the storage usage, cause_dst_cbdb attachment reads in the destination scope
 * of a memory or buffer barrier or of a storage image (radv_dst_access_flush flushes CB or DB for them),
 * cause_l2_global and cause_l2_image a source-side L2 invalidation from a memory or buffer barrier (no
 * image, so not known to be coherent) or from an image barrier (an image not coherent with L2). tr_* count
 * the image layout transitions radv_handle_image_transition handles (tr_calls, a layout or queue change)
 * and the metadata work they do: HTILE initialization and expansion (decompression), CMASK/FMASK/DCC
 * initialization, DCC decompression, fast clear eliminate, FMASK decompression and color expansion, DCC
 * retiling. wait_events counts the WAIT_REG_MEM of vkCmdWaitEvents2, wait_cp_dma the CP DMA syncs,
 * query_copies the vkCmdCopyQueryPoolResults calls and query_copy_waits the queries they copy with
 * VK_QUERY_RESULT_WAIT_BIT. The barrier tracking of BC250_BARRIER_TRACK (radv_barrier_track): skip_l2 and
 * skip_cbdb count the barrier calls whose source-side L2 invalidation, or CB or DB flush for transfer
 * writes, it leaves out (bar_* and cause_l2_* count what remains; counted before a barrier inside a pass
 * marks a pipe-misaligned attachment, so such a barrier's L2 invalidation counts as left out);
 * rp_misaligned the render passes that bind a pipe-misaligned attachment (an L2 invalidation before
 * them), cmdbuf_misaligned the command buffers that end, or call secondaries, with such an attachment's
 * writes not yet made coherent (a flush there); xfer_img the
 * transfers to an image (they keep the next transfer-write barrier's CB and DB flush); img_created and
 * img_misaligned the images created, and of them those with a pipe-misaligned mip level.
 * The waits (radv_draw_stats_wait): wt_vs_*, wt_ps_* and wt_cs_* count the VS, PS and CS partial flushes
 * of the flushes without a TS event by what they wait for: idle, no draw (dispatch for CS) since the last
 * wait that covers that stage (the partial flush of the stage or a TS event); need, work that an
 * application barrier's source stages name (shader stages for the application's draws and dispatches,
 * transfer stages for RADV's meta operations; work from before the command buffer counts for both); over,
 * asked for by an application barrier whose source stages name none of the work pending; int, asked for by
 * RADV itself (meta operations, queries, layout transitions). wt_fl_* count those flushes by their waits:
 * all idle, none needed but one over, one needed or internal; wt_eop_* the flushes with a TS event by the
 * work pending: none, dispatches only, draws. src_* count the application's barrier calls by the stages
 * their source masks name (all commands, all graphics, compute, transfer, fragment, pre-rasterization, ray
 * tracing and acceleration structures, none); bar_req_* and bar_need_* the calls whose source stages ask
 * radv_stage_flush for a VS, PS or CS partial flush, and those for which it is needed at the call;
 * bar_no_work the calls with no draw, dispatch, transfer or query since the previous one; wt_xfer_xfer the
 * flushes of either kind emitted in a meta operation that wait for meta operations' work only, a transfer
 * waiting for the transfers before it. */
#define RADV_DRAW_STATS_SYNC(X)                                                                             \
   X(fl_emits) X(fl_meta) X(fl_end) X(fl_eop_cbdb) X(fl_eop_cb) X(fl_eop_db) X(fl_eop_bop) X(fl_vs) X(fl_ps) \
   X(fl_cs) X(fl_vgt) X(fl_l2_inv) X(fl_l2_wb) X(fl_l2_meta) X(fl_vmem) X(fl_smem) X(fl_icache) X(fl_pfp)    \
   X(bar_cb) X(bar_db) X(bar_l2) X(cause_rt_write) X(cause_ds_write) X(cause_xfer_write)                    \
   X(cause_meta_storage) X(cause_dst_cbdb) X(cause_l2_global) X(cause_l2_image) X(tr_calls) X(tr_htile_init) \
   X(tr_htile_expand) X(tr_color_init) X(tr_dcc_decompress) X(tr_fce) X(tr_fmask_decompress)               \
   X(tr_fmask_expand) X(tr_dcc_retile) X(wait_events) X(wait_cp_dma) X(query_copies) X(query_copy_waits)    \
   X(skip_l2) X(skip_cbdb) X(rp_misaligned) X(cmdbuf_misaligned) X(xfer_img) X(img_created) X(img_misaligned) \
   X(wt_vs_idle) X(wt_vs_need) X(wt_vs_over) X(wt_vs_int) X(wt_ps_idle) X(wt_ps_need) X(wt_ps_over)          \
   X(wt_ps_int) X(wt_cs_idle) X(wt_cs_need) X(wt_cs_over) X(wt_cs_int) X(wt_fl_idle) X(wt_fl_over)           \
   X(wt_fl_need) X(wt_eop_idle) X(wt_eop_cs) X(wt_eop_gfx) X(src_all_cmds) X(src_all_gfx) X(src_cs)          \
   X(src_xfer) X(src_frag) X(src_prerast) X(src_rt) X(src_none) X(bar_req_vs) X(bar_req_ps) X(bar_req_cs)    \
   X(bar_need_vs) X(bar_need_ps) X(bar_need_cs) X(bar_no_work) X(wt_xfer_xfer)

#define RADV_DRAW_STATS(X) RADV_DRAW_STATS_DRAW(X) RADV_DRAW_STATS_SYNC(X)

enum radv_draw_stat {
#define RADV_DRAW_STAT_ENUM(name) RADV_DRAW_STAT_##name,
   RADV_DRAW_STATS(RADV_DRAW_STAT_ENUM)
#undef RADV_DRAW_STAT_ENUM
   RADV_DRAW_STAT_COUNT
};

/* The first counter of RADV_DRAW_STATS_SYNC. */
#define RADV_DRAW_STAT_ONE(name) +1
enum { RADV_DRAW_STAT_SYNC_FIRST = 0 RADV_DRAW_STATS_DRAW(RADV_DRAW_STAT_ONE) };
#undef RADV_DRAW_STAT_ONE

struct radeon_winsys {
   void (*destroy)(struct radeon_winsys *ws);

   struct radeon_info *(*query_info)(struct radeon_winsys *ws);

   uint64_t (*query_value)(struct radeon_winsys *ws, enum radeon_value_id value);

   bool (*read_registers)(struct radeon_winsys *ws, unsigned reg_offset, unsigned num_registers, uint32_t *out);

   bool (*query_gpuvm_fault)(struct radeon_winsys *ws, struct radv_winsys_gpuvm_fault_info *fault_info);

   VkResult (*buffer_create)(struct radeon_winsys *ws, uint64_t size, unsigned alignment, enum radeon_bo_domain domain,
                             enum radeon_bo_flag flags, unsigned priority, uint64_t address, struct radv_image *image,
                             struct radeon_winsys_bo **out_bo);

   void (*buffer_destroy)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo);
   void *(*buffer_map)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, bool use_fixed_addr, void *fixed_addr);

   VkResult (*buffer_from_ptr)(struct radeon_winsys *ws, void *pointer, uint64_t size, unsigned priority,
                               struct radeon_winsys_bo **out_bo);

   VkResult (*buffer_from_fd)(struct radeon_winsys *ws, int fd, unsigned priority, struct radeon_winsys_bo **out_bo,
                              uint64_t *alloc_size);

   VkResult (*buffer_from_handle)(struct radeon_winsys *ws, void *handle, unsigned priority,
                                  struct radeon_winsys_bo **out_bo, uint64_t *alloc_size);

   bool (*buffer_get_fd)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, int *fd);

   bool (*buffer_get_flags_from_fd)(struct radeon_winsys *ws, int fd, enum radeon_bo_domain *domains,
                                    enum radeon_bo_flag *flags);

   bool (*buffer_get_handle)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, void **handle);

   bool (*buffer_get_flags_from_handle)(struct radeon_winsys *ws, void *handle, enum radeon_bo_domain *domains,
                                        enum radeon_bo_flag *flags);

   void (*buffer_unmap)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, bool replace);

   void (*buffer_set_metadata)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, struct radeon_bo_metadata *md);
   void (*buffer_get_metadata)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, struct radeon_bo_metadata *md);

   /* Optional queued mapping transaction. When present, end(commit=true)
    * consumes the input waits on the GPU and orders later queue work after
    * mapping completion. end(false) discards the unsubmitted mapping list. */
   VkResult (*buffer_virtual_bind_begin)(struct radeon_winsys *ws, struct radeon_winsys_ctx *ctx,
                                        enum amd_ip_type ip_type);
   VkResult (*buffer_virtual_bind_end)(struct radeon_winsys *ws, struct radeon_winsys_ctx *ctx,
                                      enum amd_ip_type ip_type, uint32_t queue_index,
                                      uint32_t wait_count, const struct vk_sync_wait *waits, bool commit);

   VkResult (*buffer_virtual_bind)(struct radeon_winsys *ws, struct radeon_winsys_ctx *ctx, enum amd_ip_type ip_type,
                                   struct radeon_winsys_bo *parent, uint64_t offset, uint64_t size,
                                   struct radeon_winsys_bo *bo, uint64_t bo_offset);

   VkResult (*buffer_make_resident)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, bool resident);

   VkResult (*ctx_create)(struct radeon_winsys *ws, enum radeon_ctx_priority priority, struct radeon_winsys_ctx **ctx);
   void (*ctx_destroy)(struct radeon_winsys_ctx *ctx);

   /* Optional, hosted WDDM only: a context whose kernel objects come from
    * the embedder's queue. It is created without any and cannot submit
    * until it is bound. */
   VkResult (*ctx_create_bindable)(struct radeon_winsys *ws, enum radeon_ctx_priority priority,
                                   struct radeon_winsys_ctx **ctx);
   /* Create and release those kernel objects, inside the embedder's bind
    * and unbind of the queue that cookie names. */
   VkResult (*ctx_bind)(struct radeon_winsys_ctx *ctx, void *cookie);
   VkResult (*ctx_unbind)(struct radeon_winsys_ctx *ctx);

   bool (*ctx_wait_idle)(struct radeon_winsys_ctx *ctx, enum amd_ip_type amd_ip_type, int ring_index);

   int (*ctx_set_pstate)(struct radeon_winsys_ctx *ctx, uint32_t pstate, uint64_t timeout);

   enum radeon_bo_domain (*cs_domain)(const struct radeon_winsys *ws);

   struct ac_cmdbuf *(*cs_create)(struct radeon_winsys *ws, enum amd_ip_type amd_ip_type, bool is_secondary);

   void (*cs_destroy)(struct ac_cmdbuf *cs);

   void (*cs_reset)(struct ac_cmdbuf *cs);

   bool (*cs_chain)(struct ac_cmdbuf *cs, struct ac_cmdbuf *next_cs, bool pre_en);

   void (*cs_unchain)(struct ac_cmdbuf *cs);

   VkResult (*cs_finalize)(struct ac_cmdbuf *cs);

   void (*cs_grow)(struct ac_cmdbuf *cs, size_t min_size);

   VkResult (*cs_submit)(struct radeon_winsys_ctx *ctx, const struct radv_winsys_submit_info *submit,
                         uint32_t wait_count, const struct vk_sync_wait *waits, uint32_t signal_count,
                         const struct vk_sync_signal *signals);

   void (*cs_add_buffer)(struct ac_cmdbuf *cs, struct radeon_winsys_bo *bo);

   void (*cs_execute_secondary)(struct ac_cmdbuf *parent, struct ac_cmdbuf *child, bool allow_ib2);

   void (*cs_execute_ib)(struct ac_cmdbuf *cs, struct radeon_winsys_bo *bo, const uint64_t va, const uint32_t cdw,
                         const bool predicate);

   void (*cs_chain_dgc_ib)(struct ac_cmdbuf *cs, uint64_t va, uint32_t cdw, uint64_t trailer_va, const bool predicate);

   void (*cs_dump)(struct ac_cmdbuf *cs, FILE *file, const int *trace_ids, int trace_id_count,
                   enum radv_cs_dump_type type);
   
   void (*cs_get_cpu_addr)(void *cs, uint64_t va, struct ac_addr_info *addr_info);

   void (*cs_annotate)(struct ac_cmdbuf *cs, const char *marker);

   void (*cs_pad)(struct ac_cmdbuf *cs, unsigned leave_dw_space);

   void (*dump_bo_ranges)(struct radeon_winsys *ws, FILE *file);

   void (*dump_bo_log)(struct radeon_winsys *ws, FILE *file);

   bool (*bo_wait_for_idle)(struct radeon_winsys *ws, struct radeon_winsys_bo *bo);
   uint32_t (*get_wddm2_handle)(struct radeon_winsys *ws);
   const struct vk_sync_type *const *(*get_sync_types)(struct radeon_winsys *ws);

   int (*get_fd)(struct radeon_winsys *ws);

   struct util_sync_provider *(*get_sync_provider)(struct radeon_winsys *ws);

   VkResult (*copy_sync_payloads)(struct vk_device *device,
                                  uint32_t wait_count,
                                  const struct vk_sync_wait *waits,
                                  uint32_t signal_count,
                                  const struct vk_sync_signal *signals);

   int (*reserve_vmid)(struct radeon_winsys *ws);
   void (*unreserve_vmid)(struct radeon_winsys *ws);

   void (*init_wsi)(struct radeon_winsys *ws, struct wsi_device *wsi);

   /* When non-NULL and true, do not build the indirect gfx-init IB. The state is
    * emitted inline, the same stream as RADV_DEBUG=noibchaining. NULL means no. */
   VkResult (*buffer_from_hosted)(struct radeon_winsys *, void *, uint32_t, uint32_t flags, uint64_t, uint64_t,
                                  struct radeon_winsys_bo **);
   bool (*inline_gfx_preamble)(struct radeon_winsys *ws);

   /* bc250: adds one command buffer's RADV_DRAW_STATS counters to the winsys's totals; NULL when the
    * winsys counts nothing (the wddm2 winsys sets it for BC250_DRAW_STATS=1). */
   void (*draw_stats_add)(struct radeon_winsys *ws, const uint32_t counts[RADV_DRAW_STAT_COUNT]);

   /* bc250: the barrier tracking of radv_barrier_track (radv_cmd_buffer.c) may be used; the wddm2 winsys
    * sets it unless BC250_BARRIER_TRACK=0. */
   bool barrier_track;
};

static inline uint64_t
radv_buffer_get_va(const struct radeon_winsys_bo *bo)
{
   return bo->va;
}

static inline bool
radv_buffer_is_resident(const struct radeon_winsys_bo *bo)
{
   return bo->use_global_list || bo->is_local;
}

static inline void
radv_cs_add_buffer(struct radeon_winsys *ws, struct ac_cmdbuf *cs, struct radeon_winsys_bo *bo)
{
   if (radv_buffer_is_resident(bo))
      return;

   ws->cs_add_buffer(cs, bo);
}

static inline void *
radv_buffer_map(struct radeon_winsys *ws, struct radeon_winsys_bo *bo)
{
   return ws->buffer_map(ws, bo, false, NULL);
}

#endif /* RADV_RADEON_WINSYS_H */
