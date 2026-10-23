/*
 * Copyright © 2026 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef V3DV_UTRACE_PERFETTO_H
#define V3DV_UTRACE_PERFETTO_H

#include <stdint.h>

/* The queue/stage enums, event stacks and perfetto state now live in the
 * shared broadcom code. The device's utrace struct embeds a
 * struct v3d_utrace_perfetto (see v3dv_device.h / v3dv_private.h). */
#include "broadcom/common/v3d_utrace_perfetto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct v3dv_device;

#ifdef HAVE_PERFETTO

void v3dv_utrace_perfetto_init(struct v3dv_device *dev, uint32_t queue_count);

#else /* HAVE_PERFETTO */

static inline void
v3dv_utrace_perfetto_init(struct v3dv_device *dev, uint32_t queue_count)
{
}

#endif /* HAVE_PERFETTO */

#ifdef __cplusplus
}
#endif

#endif /* V3DV_UTRACE_PERFETTO_H */
