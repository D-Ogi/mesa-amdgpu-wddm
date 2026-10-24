/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * BD-226: where the command buffers of this winsys go (radv_wddm2_cs_domain). Every Vulkan command buffer keeps
 * its last IB after a reset, and an IB may grow up to 1 MiB (radv_winsys_cs.h), so a D3D12 game with a few
 * hundred live command buffers can hold a few hundred MiB of them. In GTT that is system memory that Windows
 * must commit; in the local segment it is part of the carve-out that the firmware already took from Windows.
 *
 * This is a soft preference, not reserved headroom. The local segment is preferred only when the whole of it
 * is CPU-visible (the CPU writes the IBs) and, at the last budget query, the process's local usage was at
 * least the reserve below its budget. The reserve is a heuristic threshold: the answer is reused for
 * RADV_WDDM2_CS_DOMAIN_RECHECK_NS, the check knows neither the size of the next request nor the requests
 * admitted since the query, and many allocations can take the same observed headroom. So the command buffers
 * can still take the process over its budget, and VidMm can then evict other memory, the application's
 * included, or refuse the allocation or its residency. A refused or unlockable VRAM command buffer BO is asked
 * again in GTT when its failure is recoverable (radv_wddm2_bo_create_placed).
 * No Windows header, so that the host test includes it as it is.
 */
#ifndef RADV_WDDM2_CS_PLACEMENT_H
#define RADV_WDDM2_CS_PLACEMENT_H

#include <stdbool.h>
#include <stdint.h>

/* The heuristic headroom: VRAM is preferred while the last query saw at least this much of the local budget
 * unused, the larger of 512 MiB and one eighth of the budget. Nothing keeps it free. */
#define RADV_WDDM2_CS_VRAM_MIN_RESERVE (512ull << 20)
/* How long one answer stays valid before the budget is queried again. */
#define RADV_WDDM2_CS_DOMAIN_RECHECK_NS (250ull * 1000 * 1000)

static inline uint64_t
radv_wddm2_cs_vram_reserve(uint64_t budget)
{
   const uint64_t eighth = budget / 8;
   return eighth > RADV_WDDM2_CS_VRAM_MIN_RESERVE ? eighth : RADV_WDDM2_CS_VRAM_MIN_RESERVE;
}

/* True when the command buffers are preferred in the local segment. queried is false when the budget query
 * failed. */
static inline bool
radv_wddm2_cs_vram_fits(bool all_vram_visible, bool queried, uint64_t budget, uint64_t usage)
{
   if (!all_vram_visible || !queried || !budget || usage > budget)
      return false;
   return budget - usage >= radv_wddm2_cs_vram_reserve(budget);
}

#endif /* RADV_WDDM2_CS_PLACEMENT_H */
