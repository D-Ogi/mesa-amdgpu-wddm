/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: the log stream of the amdgpu-wddm lines, for the system Vulkan ICD line.
 *
 * The D3D ICD line (amdgpu-wddm/b23-icd) has a larger file of this name. There, meson.build forces this header
 * into every C and C++ file of an MSVC build and sends stdout, stderr, printf, vprintf, puts and putchar to NUL
 * unless AMDGPU_WDDM_LOG asks for them, because the ICD writes inside someone else's process (3DMark's helper
 * blocked in WriteFile on a full stderr pipe, lab session 283). **This file does none of that.** It declares the
 * named log of the WDDM2 winsys and nothing else, so the system ICD keeps the stdio behaviour of the registered
 * build. A rebase of this line onto the D3D ICD line drops this file for that one.
 *
 * AMDGPU_WDDM_LOG decides where amdgpu_wddm_log() writes, with the values the D3D12 shell and the vkd3d-proton
 * engine read too:
 *   unset, empty, "0" or any other value: nothing is written.
 *   "stderr": the process's own stderr.
 *   "file:<path>": appended to <path>, opened for append only and shared, so the shell, the engine and other
 *     processes may write to the same file and each line stays whole.
 * An executable built from this tree keeps its stderr, because the policy is for a DLL in a foreign process.
 * Outside an MSVC build amdgpu_wddm_log() is plain fprintf(stderr).
 */
#ifndef AMDGPU_WDDM_STDIO_H
#define AMDGPU_WDDM_STDIO_H

#include <stdio.h>

#ifdef _MSC_VER

#ifdef __cplusplus
extern "C" {
#endif

/* The stream of the amdgpu-wddm lines: the process's stderr, the AMDGPU_WDDM_LOG file, or NULL (no output). */
FILE *amdgpu_wddm_log_stream(void);
int amdgpu_wddm_log(const char *format, ...);

#ifdef __cplusplus
}
#endif

#else

#define amdgpu_wddm_log_stream() (stderr)
#define amdgpu_wddm_log(...) fprintf(stderr, __VA_ARGS__)

#endif /* _MSC_VER */

#endif /* AMDGPU_WDDM_STDIO_H */
