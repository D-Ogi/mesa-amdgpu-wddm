/*
 * Copyright © 2026 Igalia S.L.
 * SPDX-License-Identifier: MIT
 *
 * Vulkan (v3dv) glue for the shared broadcom/common perfetto code. The
 * layout, enums and event handling all live in common; this file only
 * binds them to struct v3dv_device and the generated tracepoint symbols.
 */

#include <perfetto.h>

#include "broadcom/common/v3d_utrace_perfetto.h"

#include "v3dv_device.h"
#include "v3dv_tracepoints.h"
#include "v3dv_tracepoints_perfetto.h"
#include "v3dv_utrace_perfetto.h"

#define V3DV_UTP(dev) (&(dev)->utrace.utp)

#define V3DV_UTRACE_PERFETTO_PROCESS_EVENT(tp, stage)                       \
   V3D_UTRACE_PERFETTO_DEFINE_EVENT(v3dv_utrace_perfetto,                   \
                                    struct v3dv_device, V3DV_UTP, tp, stage)

V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_cl, CL)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_csd, CSD)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_tfu, TFU)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_cpu_reset_queries, CPU_RESET_QUERIES)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_cpu_copy_query_results, CPU_COPY_QUERY_RESULTS)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_cpu_csd_indirect, CPU_CSD_INDIRECT)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(job_cpu_timestamp_query, CPU_TIMESTAMP_QUERY)
V3DV_UTRACE_PERFETTO_PROCESS_EVENT(cmdbuf, CMDBUF)

/* Public entry point, signature unchanged, so no call sites need to
 * change. queue_count is validated against the common queue count. */
void
v3dv_utrace_perfetto_init(struct v3dv_device *dev, uint32_t queue_count)
{
   if (queue_count > V3D_UTRACE_QUEUE_COUNT) {
      assert(!"V3D_UTRACE_QUEUE_COUNT too small");
      return;
   }

   v3d_utrace_perfetto_layout_init(V3DV_UTP(dev),
                                   &v3d_utrace_perfetto_vk_layout,
                                   dev, (uintptr_t)dev);
}
