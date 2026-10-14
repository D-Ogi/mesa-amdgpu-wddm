/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: forced into every C and C++ file of an MSVC build (meson.build, /FI), so that a driver DLL built
 * from this tree never writes to the application's stdio unless asked. The ICD runs inside someone else's process;
 * 3DMark's helper processes pipe stderr without reading it and blocked in WriteFile once ~12 KB of driver lines had
 * filled the pipe (lab session 283). Mesa has hundreds of fprintf(stderr, ...) and printf calls, most of them behind
 * debug options, so the streams are redirected here once instead of call by call.
 *
 * stdout, stderr, printf, vprintf, puts and putchar resolve through amdgpu_wddm_stdio() (u_amdgpu_wddm_stdio.c).
 * In a DLL, AMDGPU_WDDM_LOG decides, the switch the D3D12 shell and the vkd3d-proton engine read too, after the timed
 * switch's marker AMDGPU_WDDM_GAMELOG=<pid>;<16 hex digits> (u_amdgpu_wddm_stdio.c):
 *   a marker that names this process: another module opened the timed switch's file. Both streams and the
 *     amdgpu-wddm lines are appended to the AMDGPU_WDDM_LOG "file:<path>" under the marker's bounds. Any other
 *     value, or none: no amdgpu-wddm lines, and both streams as for "0".
 *   a marker that names another process: both variables were inherited and are ignored, as if both were unset.
 *   unset or empty: the control app's timed switch first (HKLM\SOFTWARE\amdgpu-wddm\Diagnostics GameLogUntil).
 *     While it runs, both streams and the amdgpu-wddm lines go to a new file of this process in
 *     %ProgramData%\amdgpu-wddm\logs\games, bounded in time and size, and both variables are set for the modules
 *     this process loads later. Otherwise as the next value.
 *   "0" or anything else: both streams go to NUL, unless an explicit Mesa debug variable is set
 *     (RADV_DEBUG, ACO_DEBUG, NIR_DEBUG, MESA_DEBUG, MESA_LOG, MESA_SHADER_CACHE_SHOW_STATS): then as upstream.
 *   "stderr": the process's own stdout and stderr, as upstream.
 *   "file:<path>": both streams are appended to <path> with no bounds, opened for append only and shared.
 * That list is the lab flavour's (AMDGPU_WDDM_LAB_FLAVOUR, BD-167). The release flavour reads AMDGPU_WDDM_LOG only as
 * the carrier of the timed switch: beside a marker that names this process, and only as "file:" and a plain file
 * name in the switch's folder, a file that already exists. Without such a marker the switch decides, as with the
 * variable unset; "stderr", every other value and every other path give no output of their own (both streams go to
 * NUL, unless an explicit Mesa debug variable is set).
 * So mesa_log's default info and warn lines and the error-only messages (the shader cache's "Failed to create ...")
 * print only when asked; the debug options print where their variable expects them.
 * An executable built from this tree (a test or a tool) keeps its streams. The C runtime is linked statically into
 * the ICD (b_vscrt=mt), so these streams are the DLL's own; the application's are never touched. Debugger output
 * (OutputDebugString, MESA_LOG's windbg logger) and the winsys log files are not stdio and stay as they are.
 *
 * The amdgpu-wddm lines of the WDDM2 winsys (adapter, nodes, submission notices, deferred destroy) do not rely on the
 * redirect: they print with amdgpu_wddm_log(), which writes only with AMDGPU_WDDM_LOG=stderr (the process's stderr)
 * or file:<path>, or into the timed switch's file, whatever Mesa debug variable is set. AMDGPU_WDDM_LOG_PDD=1 adds
 * the private driver data of every submission, as hex, to the same stream (radv_wddm2_cs.c). In lab session 290 these
 * lines still reached a 3DMark helper's stderr with the switch unset, 234 bytes from radv_wddm2_winsys_create; the
 * gate does not depend on why.
 * Outside MSVC builds amdgpu_wddm_log() is plain fprintf(stderr).
 */
#ifndef AMDGPU_WDDM_STDIO_H
#define AMDGPU_WDDM_STDIO_H

#include <stdio.h>

#ifdef _MSC_VER

#ifdef __cplusplus
extern "C" {
#endif

FILE *amdgpu_wddm_stdio(int fd);
int amdgpu_wddm_stdio_puts(const char *text);
int amdgpu_wddm_stdio_putchar(int c);
/* The stream of the amdgpu-wddm lines: the process's stderr, the AMDGPU_WDDM_LOG file, the timed switch's file, or
 * NULL (no output). Once the timed switch's bounds stop that file, the same stream writes to NUL. */
FILE *amdgpu_wddm_log_stream(void);
int amdgpu_wddm_log(const char *format, ...);

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

#else

#define amdgpu_wddm_log_stream() (stderr)
#define amdgpu_wddm_log(...) fprintf(stderr, __VA_ARGS__)

#endif /* _MSC_VER */

#endif /* AMDGPU_WDDM_STDIO_H */
