/* SPDX-License-Identifier: MIT
 *
 * Configuration of the hosted D3D UMD (bc250d3d_zink.dll) as a registered UMD.
 *
 * The trial router of DWM050 handed this DLL its adapter LUID, its Vulkan ICD and its logging through the
 * process environment. A registered UMD has no such environment. Here:
 *  - the LUID comes from the runtime's adapter: the KMD's UMDRIVERPRIVATE data carries it in the
 *    bc250_adapter_identity trailer (KMD 0.7.169.1 and later, driver/kmd/wddm.c at DXGKQAITYPE_UMDRIVERPRIVATE);
 *  - the ICD is amdgpu_wddm_radv.dll in this DLL's own directory, unless the registry or the environment
 *    names another file;
 *  - diagnostics are off unless the registry or the environment turns them on.
 *
 * Precedence for every setting: process environment (trial tooling), then
 * HKLM\SOFTWARE\amdgpu-wddm\HostedUmd, then the default.
 */
#pragma once

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC250_HOSTED_UMD_KEY L"SOFTWARE\\amdgpu-wddm\\HostedUmd"
#define BC250_HOSTED_ICD_FILE L"amdgpu_wddm_radv.dll"

struct Bc250Config {
   /* Hosted rendering through the runtime's callbacks; the only mode a registered UMD has. Environment
    * BC250_HOSTED_RENDER=0 selects the old loader-based diagnostic screen instead. */
   bool hosted_render;
   /* Create the runtime virtual context at device creation (E26: DWM asks for broadcast synchronization
    * before its first Present). Environment BC250_D3D_RUNTIME_PROBE=0 turns it off. */
   bool runtime_probe;
   bool diagnostics;
   /* Where the ICD path came from: "environment", "registry" or "module-directory". */
   const char *icd_source;
   /* ANSI path of the ICD for util_dl_open; empty and icd_status failed when it cannot be formed. */
   char icd_path[MAX_PATH];
   HRESULT icd_status;
   wchar_t module_directory[MAX_PATH];
   wchar_t log_path[MAX_PATH];
};

/* Resolved once per process (init-once); never NULL. */
const struct Bc250Config *Bc250GetConfig(void);

/* The adapter LUID of an OpenAdapter call. The identity trailer is authoritative; BC250_D3D_ZINK_LUID is
 * accepted only when the KMD has no trailer, and refused when it disagrees with one. */
HRESULT Bc250QueryAdapterLuid(const void *open_adapter_args, UINT64 *luid);

/* Cheap check for the per-entrypoint trace of DebugPrintf (BC250_UMD_VERBOSE=1 with diagnostics on).
 * Declared in Debug.h as well, for the C sources. */
int Bc250UmdVerbose(void);

#ifdef __cplusplus
}
#endif
