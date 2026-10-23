/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/* BC250_MESH_TIMER: not ported.
 *
 * DirectMesh v1.3.1 carries an in-driver GPU timer for Mesh draws
 * (radv_bc250_timer.c, 1181 lines, plus a 225-line header). It is instrumentation only: with the
 * switch off its driver records byte-identical command streams. This port leaves the
 * implementation out and keeps this header as the empty interface, so the call sites stay in place
 * and the timer can be ported later without touching them.
 *
 * Everything below is a no-op: radv_device::bc250_timer.enabled is always false, every entry point
 * is a static inline that does nothing, and a draw bracket always answers
 * RADV_BC250_TIMER_NOT_TIMED (counted, no packets emitted).
 */

#ifndef RADV_BC250_TIMER_H
#define RADV_BC250_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct radv_device;
struct radv_queue;
struct radv_cmd_buffer;
struct radv_shader;
struct radeon_winsys_bo;
struct vk_queue_submit;

enum radv_bc250_timer_kind {
   RADV_BC250_TIMER_DIRECT,
   RADV_BC250_TIMER_INDIRECT,
   RADV_BC250_TIMER_COUNT,
   RADV_BC250_TIMER_KINDS,
};

/* Per device. `enabled` stays false, so no call site ever reaches a timer packet. */
struct radv_bc250_mesh_timer {
   bool enabled;
};

/* Per command buffer. */
struct radv_bc250_timer_cmd {
   uint32_t draws, vs_draws, skipped;
};

static inline void
radv_bc250_timer_init(struct radv_device *device)
{
}

static inline void
radv_bc250_timer_finish(struct radv_device *device)
{
}

static inline void
radv_bc250_timer_cmd_reset(struct radv_cmd_buffer *cmd_buffer)
{
}

static inline void
radv_bc250_timer_cmd_destroy(struct radv_cmd_buffer *cmd_buffer)
{
}

static inline void
radv_bc250_timer_cmd_begin(struct radv_cmd_buffer *cmd_buffer)
{
}

static inline void
radv_bc250_timer_cmd_end(struct radv_cmd_buffer *cmd_buffer)
{
}

static inline void
radv_bc250_timer_cmd_execute(struct radv_cmd_buffer *primary, struct radv_cmd_buffer *secondary)
{
}

#define RADV_BC250_TIMER_SKIP_DRAW (-2)
#define RADV_BC250_TIMER_NOT_TIMED (-1)

static inline int32_t
radv_bc250_timer_draw_begin(struct radv_cmd_buffer *cmd_buffer, enum radv_bc250_timer_kind kind)
{
   return RADV_BC250_TIMER_NOT_TIMED;
}

static inline void
radv_bc250_timer_draw_end(struct radv_cmd_buffer *cmd_buffer, int32_t entry)
{
}

static inline void
radv_bc250_timer_submit(struct radv_device *device, const struct radv_queue *queue,
                        const struct vk_queue_submit *submission, uint64_t call_ns)
{
}

static inline void
radv_bc250_timer_present(struct radv_device *device, const struct radv_queue *queue)
{
}

#endif /* RADV_BC250_TIMER_H */
