/*
 * Copyright © 2026 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef V3D_UTRACE_PERFETTO_H
#define V3D_UTRACE_PERFETTO_H

#include <stdint.h>

#ifdef __cplusplus
#include <functional>
namespace perfetto { namespace protos { namespace pbzero {
class GpuRenderStageEvent;
} } }
extern "C" {
#endif

#define V3D_UTRACE_PERFETTO_STACK_DEPTH 8

/*
 * Single source of truth for every queue and stage any broadcom driver
 * emits. The first V3D_UTRACE_*_HW_COUNT entries are the real hardware
 * queues/stages (CL, CSD, TFU) shared by gallium v3d and v3dv. The rest
 * are v3dv-only bookkeeping (CPU jobs, a command-buffer pseudo-queue).
 *
 * A driver picks the layout matching what it emits:
 *   - gallium v3d: v3d_utrace_perfetto_hw_layout (hw prefix only)
 *   - v3dv:        v3d_utrace_perfetto_vk_layout (everything)
 */
enum v3d_utrace_queue {
   V3D_UTRACE_QUEUE_CL = 0,
   V3D_UTRACE_QUEUE_CSD,
   V3D_UTRACE_QUEUE_TFU,
   V3D_UTRACE_QUEUE_HW_COUNT,

   V3D_UTRACE_QUEUE_CPU = V3D_UTRACE_QUEUE_HW_COUNT,
   V3D_UTRACE_QUEUE_DRIVER, /* dedicated non-hw queue */
   V3D_UTRACE_QUEUE_COUNT,
};

/* FIXME: Job CL is only one stage despite having a separate render CL and
 * bin CL in the drivers because the kernel does not expose a sync for
 * "bin finished" to userspace yet.
 */
enum v3d_utrace_stage {
   V3D_UTRACE_STAGE_CL = 0,
   V3D_UTRACE_STAGE_CSD,
   V3D_UTRACE_STAGE_TFU,
   V3D_UTRACE_STAGE_HW_COUNT,

   /* These are all CPU jobs and go on V3D_UTRACE_QUEUE_CPU */
   V3D_UTRACE_STAGE_CPU_RESET_QUERIES = V3D_UTRACE_STAGE_HW_COUNT,
   V3D_UTRACE_STAGE_CPU_COPY_QUERY_RESULTS,
   V3D_UTRACE_STAGE_CPU_CSD_INDIRECT,
   V3D_UTRACE_STAGE_CPU_TIMESTAMP_QUERY,

   /* Not a real job, tracks all jobs in a single command buffer. */
   V3D_UTRACE_STAGE_CMDBUF,
   V3D_UTRACE_STAGE_COUNT,
};

#define V3D_UTRACE_PERFETTO_MAX_QUEUES V3D_UTRACE_QUEUE_COUNT
#define V3D_UTRACE_PERFETTO_MAX_STAGES V3D_UTRACE_STAGE_COUNT

/*
 * Opaque per-driver context (struct v3d_context* for gallium, struct
 * v3dv_device* for Vulkan). The common implementation never dereferences
 * this; it's only threaded through init() so it ends up alongside
 * anything driver-specific if a future caller needs it.
 */
typedef void *v3d_utrace_perfetto_handle;

struct v3d_utrace_perfetto_event {
   uint32_t stage;
   uint64_t begin_ns;
};

struct v3d_utrace_perfetto_queue {
   struct v3d_utrace_perfetto_event stack[V3D_UTRACE_PERFETTO_STACK_DEPTH];
   uint32_t stack_depth;
};

/*
 * Static description of a queue/stage layout. Stage -> queue mapping and
 * display names are plain arrays instead of switch statements, so the
 * common implementation never needs to know a driver's enum values.
 * The only instances are v3d_utrace_perfetto_{hw,vk}_layout below.
 */
struct v3d_utrace_perfetto_layout {
   uint32_t queue_count;
   uint32_t stage_count;
   const char *const *queue_names;  /* indexed [0, queue_count) */
   const char *const *stage_names;  /* indexed [0, stage_count) */
   const uint32_t *stage_to_queue;  /* indexed [0, stage_count) */
   const char *data_source_name;    /* e.g. "gpu.renderstages.broadcom" */
   const char *clock_domain_string; /* e.g. "org.freedesktop.mesa.broadcom",
                                      * hashed to build the perfetto clock id */
};

extern const struct v3d_utrace_perfetto_layout v3d_utrace_perfetto_hw_layout;
extern const struct v3d_utrace_perfetto_layout v3d_utrace_perfetto_vk_layout;

struct v3d_utrace_perfetto {
   const struct v3d_utrace_perfetto_layout *layout;
   v3d_utrace_perfetto_handle context;

   uint32_t gpu_clock_id;
   uint64_t context_id;

   /* Per-instance storage, sized for the largest layout. The iids are the
    * interned-data ids used for the queue/stage gpu_specifications; they
    * are assigned in layout_init() (queues first, then stages, from 1). */
   uint64_t queue_iids[V3D_UTRACE_PERFETTO_MAX_QUEUES];
   uint64_t stage_iids[V3D_UTRACE_PERFETTO_MAX_STAGES];
   struct v3d_utrace_perfetto_queue queues[V3D_UTRACE_PERFETTO_MAX_QUEUES];

   uint64_t next_clock_snapshot;
   uint64_t event_id;
};

#ifdef HAVE_PERFETTO

void v3d_utrace_perfetto_layout_init(
   struct v3d_utrace_perfetto *utp,
   const struct v3d_utrace_perfetto_layout *layout,
   v3d_utrace_perfetto_handle context, uint64_t context_id);

void v3d_utrace_perfetto_begin_event(struct v3d_utrace_perfetto *utp,
                                      uint32_t stage, uint64_t ts_ns);

#ifdef __cplusplus
} /* extern "C" */

void v3d_utrace_perfetto_end_event(
   struct v3d_utrace_perfetto *utp, uint32_t stage, uint64_t ts_ns,
   std::function<void(perfetto::protos::pbzero::GpuRenderStageEvent *)>
      emit_event_extra);

/*
 * Defines the begin/end tracepoint callbacks for one tracepoint.
 *   prefix   - function name prefix, e.g. v3dv_utrace_perfetto
 *   dev_type - device type taken by the callbacks, e.g. struct v3dv_device
 *   utp_of   - macro/function: utp_of(dev) -> struct v3d_utrace_perfetto *
 *   tp       - tracepoint name (job_cl, cmdbuf, ...)
 *   stage    - suffix of a V3D_UTRACE_STAGE_* value (JOB_CL, CMDBUF, ...)
 *
 * Expands in the driver's .cc, where trace_begin_<tp>, trace_end_<tp> and
 * trace_payload_as_extra_end_<tp> are visible.
 */
#define V3D_UTRACE_PERFETTO_DEFINE_EVENT(prefix, dev_type, utp_of, tp, stage) \
   void prefix##_begin_##tp(                                                 \
      dev_type *dev, uint64_t ts_ns, uint16_t tp_idx,                        \
      const void *flush_data, const struct trace_begin_##tp *payload,        \
      const void *indirect_data)                                             \
   {                                                                         \
      v3d_utrace_perfetto_begin_event(utp_of(dev),                           \
                                      V3D_UTRACE_STAGE_##stage, ts_ns);      \
   }                                                                         \
                                                                             \
   void prefix##_end_##tp(                                                   \
      dev_type *dev, uint64_t ts_ns, uint16_t tp_idx,                        \
      const void *flush_data, const struct trace_end_##tp *payload,          \
      const void *indirect_data)                                             \
   {                                                                         \
      auto emit_event_extra =                                                \
         [=](perfetto::protos::pbzero::GpuRenderStageEvent *event) {         \
            trace_payload_as_extra_end_##tp(event, payload, indirect_data);  \
         };                                                                  \
      v3d_utrace_perfetto_end_event(utp_of(dev),                             \
                                    V3D_UTRACE_STAGE_##stage, ts_ns,         \
                                    emit_event_extra);                       \
   }

extern "C" {
#endif

#else /* HAVE_PERFETTO */

static inline void
v3d_utrace_perfetto_layout_init(
   struct v3d_utrace_perfetto *utp,
   const struct v3d_utrace_perfetto_layout *layout,
   v3d_utrace_perfetto_handle context, uint64_t context_id)
{
}

static inline void
v3d_utrace_perfetto_begin_event(struct v3d_utrace_perfetto *utp,
                                 uint32_t stage, uint64_t ts_ns)
{
}

#endif /* HAVE_PERFETTO */

#ifdef __cplusplus
}
#endif

#endif /* V3D_UTRACE_PERFETTO_H */
