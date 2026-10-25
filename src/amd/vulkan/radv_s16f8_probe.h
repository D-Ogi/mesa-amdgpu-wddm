/* SPDX-License-Identifier: MIT
 * Isolated S16F8 diagnostic. Not a capability implementation.
 * Requires an explicit build define AND process opt-in. Never use in a release build.
 */
#ifndef RADV_S16F8_PROBE_H
#define RADV_S16F8_PROBE_H
#include <stdlib.h>
#include <string.h>
#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE && (!defined(AMDGPU_WDDM_LAB_FLAVOUR) || !AMDGPU_WDDM_LAB_FLAVOUR)
#error S16F8 diagnostics require the lab build flavour
#endif
static inline unsigned
radv_s16f8_probe_mode(void)
{
#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
   const char *mode = getenv("RADV_S16F8_PROBE_MODE");
   if (mode && !strcmp(mode, "control8")) return 1;
   if (mode && !strcmp(mode, "pal8")) return 2;
   if (mode && !strcmp(mode, "exposed16")) return 3;
#endif
   return 0;
}
#endif
