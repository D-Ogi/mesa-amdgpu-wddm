/* SPDX-License-Identifier: MIT
 *
 * Process-wide diagnostics switch of the amdgpu-wddm hosted D3D UMD (bc250d3d_zink.dll).
 *
 * Off by default. A registered UMD runs inside dwm.exe and other system processes, where stderr has no
 * console and no reader: routine lines are then not even formatted. Rare error events still reach a
 * kernel or user-mode debugger through OutputDebugString (bounded per process). The UMD's configuration
 * (frontends/d3d10umd/Bc250Config.cpp) sets the switch once, before the first device exists, from
 * BC250_UMD_DIAG or the registry, and then redirects stderr to a per-process log file.
 */
#ifndef BC250_DIAG_H
#define BC250_DIAG_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Written once under the configuration's init-once, read-only afterwards. */
extern bool bc250_diag_enabled;

/* printf-like. With diagnostics on: stderr (the log file). Off: OutputDebugString, the first 64 lines of
 * the process only. */
void bc250_diag_error(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
   __attribute__((format(printf, 1, 2)))
#endif
   ;

#ifdef __cplusplus
}
#endif

/* Routine diagnostic line: formatted and written only with diagnostics on. */
#define BC250_DIAG(...) do { if (bc250_diag_enabled) fprintf(stderr, __VA_ARGS__); } while (0)
#define BC250_DIAG_FLUSH() do { if (bc250_diag_enabled) fflush(stderr); } while (0)
/* A rare error event that a debugger should see even with diagnostics off. */
#define BC250_ERROR(...) bc250_diag_error(__VA_ARGS__)
/* A line that is an error event when `error` holds and a routine diagnostic otherwise. */
#define BC250_REPORT(error, ...) do { if (error) BC250_ERROR(__VA_ARGS__); else BC250_DIAG(__VA_ARGS__); } while (0)

#endif
