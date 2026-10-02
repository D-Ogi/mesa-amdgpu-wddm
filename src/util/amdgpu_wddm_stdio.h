/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: forced into every C and C++ file of an MSVC build (meson.build, /FI), so that a driver DLL built
 * from this tree never writes to the application's stdio unless asked. The ICD runs inside someone else's process;
 * 3DMark's helper processes pipe stderr without reading it and blocked in WriteFile once ~12 KB of driver lines had
 * filled the pipe (lab session 283). Mesa has hundreds of fprintf(stderr, ...) and printf calls, most of them behind
 * debug options, so the streams are redirected here once instead of call by call.
 *
 * stdout, stderr, printf, vprintf, puts and putchar resolve through amdgpu_wddm_stdio() (u_amdgpu_wddm_stdio.c).
 * In a DLL, AMDGPU_WDDM_LOG decides, the switch the D3D12 shell and the vkd3d-proton engine read too:
 *   unset, empty, "0" or anything else: both streams go to NUL, unless an explicit Mesa debug variable is set
 *     (RADV_DEBUG, ACO_DEBUG, NIR_DEBUG, MESA_DEBUG, MESA_LOG, MESA_SHADER_CACHE_SHOW_STATS): then as upstream.
 *   "stderr": the process's own stdout and stderr, as upstream.
 *   "file:<path>": both streams are appended to <path>, opened for append only and shared.
 * So mesa_log's default info and warn lines and the error-only messages (the shader cache's "Failed to create ...")
 * print only when asked; the debug options print where their variable expects them.
 * An executable built from this tree (a test or a tool) keeps its streams. The C runtime is linked statically into
 * the ICD (b_vscrt=mt), so these streams are the DLL's own; the application's are never touched. Debugger output
 * (OutputDebugString, MESA_LOG's windbg logger) and the winsys log files are not stdio and stay as they are.
 */
#ifndef AMDGPU_WDDM_STDIO_H
#define AMDGPU_WDDM_STDIO_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *amdgpu_wddm_stdio(int fd);
int amdgpu_wddm_stdio_puts(const char *text);
int amdgpu_wddm_stdio_putchar(int c);

#ifdef __cplusplus
}
#endif

#undef stdout
#undef stderr
#define stdout (amdgpu_wddm_stdio(1))
#define stderr (amdgpu_wddm_stdio(2))
#define printf(...) fprintf(stdout, __VA_ARGS__)
#define vprintf(format, args) vfprintf(stdout, format, args)
#define puts(text) amdgpu_wddm_stdio_puts(text)
#define putchar(c) amdgpu_wddm_stdio_putchar(c)

#endif /* AMDGPU_WDDM_STDIO_H */
