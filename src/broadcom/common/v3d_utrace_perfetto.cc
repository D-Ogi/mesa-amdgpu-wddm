/*
 * Copyright © 2026 Igalia S.L.
 * SPDX-License-Identifier: MIT
 *
 * Shared perfetto GPU render-stage plumbing for the broadcom drivers
 * (gallium v3d and Vulkan v3dv). Drivers pick a layout, call
 * v3d_utrace_perfetto_layout_init(), and forward their generated
 * tracepoint callbacks to the begin/end helpers below (see
 * V3D_UTRACE_PERFETTO_DEFINE_EVENT in the header).
 */
#include <perfetto.h>

#include <assert.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "util/hash_table.h"
#include "util/macros.h"
#include "util/perf/u_perfetto.h"
#include "util/perf/u_perfetto_renderpass.h"
#include "util/timespec.h"
#include "util/u_process.h"

#include "v3d_utrace_perfetto.h"

#define V3D_UTRACE_DATA_SOURCE_NAME "gpu.renderstages.broadcom"
#define V3D_UTRACE_CLOCK_DOMAIN     "org.freedesktop.mesa.broadcom"

/* Indexed by enum v3d_utrace_queue. The hw layout is a prefix of this. */
static const char *const v3d_queue_names[V3D_UTRACE_QUEUE_COUNT] = {
   "CL",
   "CSD",
   "TFU",
   "CPU",
   "_Driver",
};

/* Indexed by enum v3d_utrace_stage. The hw layout is a prefix of this. */
static const char *const v3d_stage_names[V3D_UTRACE_STAGE_COUNT] = {
   "CL",
   "CSD",
   "TFU",
   "CPU_RESET_QUERIES",
   "CPU_COPY_QUERY_RESULTS",
   "CPU_CSD_INDIRECT",
   "CPU_TIMESTAMP_QUERY",
   "CMDBUF",
};

static const uint32_t v3d_stage_to_queue[V3D_UTRACE_STAGE_COUNT] = {
   V3D_UTRACE_QUEUE_CL,
   V3D_UTRACE_QUEUE_CSD,
   V3D_UTRACE_QUEUE_TFU,
   V3D_UTRACE_QUEUE_CPU,
   V3D_UTRACE_QUEUE_CPU,
   V3D_UTRACE_QUEUE_CPU,
   V3D_UTRACE_QUEUE_CPU,
   V3D_UTRACE_QUEUE_DRIVER,
};

const struct v3d_utrace_perfetto_layout v3d_utrace_perfetto_hw_layout = {
   .queue_count = V3D_UTRACE_QUEUE_HW_COUNT,
   .stage_count = V3D_UTRACE_STAGE_HW_COUNT,
   .queue_names = v3d_queue_names,
   .stage_names = v3d_stage_names,
   .stage_to_queue = v3d_stage_to_queue,
   .data_source_name = V3D_UTRACE_DATA_SOURCE_NAME,
   .clock_domain_string = V3D_UTRACE_CLOCK_DOMAIN,
};

const struct v3d_utrace_perfetto_layout v3d_utrace_perfetto_vk_layout = {
   .queue_count = V3D_UTRACE_QUEUE_COUNT,
   .stage_count = V3D_UTRACE_STAGE_COUNT,
   .queue_names = v3d_queue_names,
   .stage_names = v3d_stage_names,
   .stage_to_queue = v3d_stage_to_queue,
   .data_source_name = V3D_UTRACE_DATA_SOURCE_NAME,
   .clock_domain_string = V3D_UTRACE_CLOCK_DOMAIN,
};

struct V3DRenderpassIncrementalState {
   bool was_cleared = true;
};

struct V3DRenderpassTraits : public perfetto::DefaultDataSourceTraits {
   using IncrementalStateType = V3DRenderpassIncrementalState;
};

class V3DRenderpassDataSource
    : public MesaRenderpassDataSource<V3DRenderpassDataSource,
                                      V3DRenderpassTraits> {};

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(V3DRenderpassDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(V3DRenderpassDataSource);

static void
emit_interned_data_packet(const struct v3d_utrace_perfetto *utp,
                          V3DRenderpassDataSource::TraceContext &ctx,
                          uint64_t now)
{
   const struct v3d_utrace_perfetto_layout *layout = utp->layout;

   auto packet = ctx.NewTracePacket();
   packet->set_timestamp(now);
   packet->set_sequence_flags(
      perfetto::protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);

   auto interned_data = packet->set_interned_data();

   for (uint32_t i = 0; i < layout->queue_count; i++) {
      char name[64];
      snprintf(name, sizeof(name), "%s-%s", util_get_process_name(),
               layout->queue_names[i]);
      auto specs = interned_data->add_gpu_specifications();
      specs->set_iid(utp->queue_iids[i]);
      specs->set_name(name);
   }

   for (uint32_t i = 0; i < layout->stage_count; i++) {
      auto specs = interned_data->add_gpu_specifications();
      specs->set_iid(utp->stage_iids[i]);
      specs->set_name(layout->stage_names[i]);
   }
}

static uint64_t
get_gpu_time_ns(void)
{
   /* v3d has no free-running GPU clock register, "GPU time" here is a CPU
    * clock sample the kernel takes via DRM_V3D_EXT_ID_CPU_TIMESTAMP_QUERY,
    * already returned in nanoseconds.
    * For clock-sync purposes we can just use the current CPU boottime.
    */
   struct timespec ts;
   clock_gettime(CLOCK_BOOTTIME, &ts);
   return (uint64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void
emit_clock_snapshot_packet(const struct v3d_utrace_perfetto *utp,
                           V3DRenderpassDataSource::TraceContext &ctx)
{
   const uint64_t gpu_ns = get_gpu_time_ns();
   const uint64_t cpu_ns = perfetto::base::GetBootTimeNs().count();

   MesaRenderpassDataSource<V3DRenderpassDataSource, V3DRenderpassTraits>::
      EmitClockSync(ctx, cpu_ns, gpu_ns,
                    perfetto::protos::pbzero::BUILTIN_CLOCK_BOOTTIME,
                    utp->gpu_clock_id);
}

static void
emit_setup_packets(struct v3d_utrace_perfetto *utp,
                   V3DRenderpassDataSource::TraceContext &ctx)
{
   const uint64_t now = perfetto::base::GetBootTimeNs().count();

   auto state = ctx.GetIncrementalState();
   if (state->was_cleared) {
      emit_interned_data_packet(utp, ctx, now);

      state->was_cleared = false;
      utp->next_clock_snapshot = 0;
   }

   if (now >= utp->next_clock_snapshot) {
      emit_clock_snapshot_packet(utp, ctx);

      utp->next_clock_snapshot = now + NSEC_PER_SEC;
   }
}

static struct v3d_utrace_perfetto_event *
begin_event(struct v3d_utrace_perfetto *utp, uint32_t stage)
{
   uint32_t queue_idx = utp->layout->stage_to_queue[stage];
   struct v3d_utrace_perfetto_queue *queue = &utp->queues[queue_idx];

   if (queue->stack_depth >= V3D_UTRACE_PERFETTO_STACK_DEPTH) {
      PERFETTO_ELOG("queue %d stage %d too deep", queue_idx, stage);
      return NULL;
   }

   struct v3d_utrace_perfetto_event *ev = &queue->stack[queue->stack_depth++];
   ev->stage = stage;
   return ev;
}

static struct v3d_utrace_perfetto_event *
end_event(struct v3d_utrace_perfetto *utp, uint32_t stage)
{
   uint32_t queue_idx = utp->layout->stage_to_queue[stage];
   struct v3d_utrace_perfetto_queue *queue = &utp->queues[queue_idx];

   if (!queue->stack_depth)
      return NULL;

   if (queue->stack_depth > V3D_UTRACE_PERFETTO_STACK_DEPTH)
      return NULL;

   struct v3d_utrace_perfetto_event *ev = &queue->stack[--queue->stack_depth];
   assert(ev->stage == stage);
   return ev;
}

void
v3d_utrace_perfetto_begin_event(struct v3d_utrace_perfetto *utp,
                                uint32_t stage, uint64_t ts_ns)
{
   assert(stage < utp->layout->stage_count);

   struct v3d_utrace_perfetto_event *ev = begin_event(utp, stage);
   if (!ev)
      return;

   ev->begin_ns = ts_ns;
}

void
v3d_utrace_perfetto_end_event(
   struct v3d_utrace_perfetto *utp, uint32_t stage, uint64_t ts_ns,
   std::function<void(perfetto::protos::pbzero::GpuRenderStageEvent *)>
      emit_event_extra)
{
   assert(stage < utp->layout->stage_count);

   const struct v3d_utrace_perfetto_event *ev = end_event(utp, stage);
   if (!ev)
      return;

   const uint32_t queue_idx = utp->layout->stage_to_queue[stage];

   V3DRenderpassDataSource::Trace(
      [=](V3DRenderpassDataSource::TraceContext ctx) {
         emit_setup_packets(utp, ctx);

         auto packet = ctx.NewTracePacket();
         packet->set_timestamp(ev->begin_ns);
         packet->set_timestamp_clock_id(utp->gpu_clock_id);

         auto event = packet->set_gpu_render_stage_event();
         event->set_event_id(utp->event_id++);
         event->set_duration(ts_ns - ev->begin_ns);
         event->set_hw_queue_iid(utp->queue_iids[queue_idx]);
         event->set_stage_iid(utp->stage_iids[stage]);
         event->set_context(utp->context_id);

         emit_event_extra(event);
      }
   );
}

void
v3d_utrace_perfetto_layout_init(struct v3d_utrace_perfetto *utp,
                                const struct v3d_utrace_perfetto_layout *layout,
                                v3d_utrace_perfetto_handle context,
                                uint64_t context_id)
{
   assert(layout->queue_count <= V3D_UTRACE_PERFETTO_MAX_QUEUES);
   assert(layout->stage_count <= V3D_UTRACE_PERFETTO_MAX_STAGES);

   memset(utp, 0, sizeof(*utp));

   utp->layout = layout;
   utp->context = context;
   utp->context_id = context_id;
   utp->gpu_clock_id =
      _mesa_hash_string(layout->clock_domain_string) | 0x80000000;

   uint64_t next_iid = 1;
   for (uint32_t i = 0; i < layout->queue_count; i++)
      utp->queue_iids[i] = next_iid++;
   for (uint32_t i = 0; i < layout->stage_count; i++)
      utp->stage_iids[i] = next_iid++;

   util_perfetto_init();

   /* One data source per process, shared by every broadcom driver and
    * device. Both layouts use the same data source name. */
   static std::once_flag register_ds_once;
   std::call_once(register_ds_once, [layout]() {
      perfetto::DataSourceDescriptor dsd;
      dsd.set_name(layout->data_source_name);
      V3DRenderpassDataSource::Register(dsd);
   });
}
