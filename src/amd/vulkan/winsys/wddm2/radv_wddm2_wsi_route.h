/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * radv_wddm2_wsi_route.h - which present route a BC-250 Vulkan swapchain takes, and the checks that
 * decide it. Plain C with no Windows or Vulkan header, so a host test drives the production rules.
 *
 * Routes:
 *   GDI               the CPU-image path of wsi_common_win32.cpp: a CPU copy into a DIB, BitBlt, DwmFlush.
 *                     It is the fallback and, until the lab passes the DXGI route, the default.
 *   DXGI              a D3D12 device on the same adapter (our D3D12 shell), a flip-model swap chain for the
 *                     window (CreateSwapChainForHwnd), one GPU copy per present on the D3D12 queue, Present1.
 *                     This is the shape the native D3D12 games present through.
 *   DXGI_COMPOSITION  the same, but the swap chain is created for composition and bound to the window
 *                     through a DirectComposition visual (upstream Mesa's shape).
 *
 * The switch: AMDGPU_WDDM_VK_WSI in the environment, else the REG_SZ value WsiRoute under
 * HKLM\SOFTWARE\amdgpu-wddm\Vulkan, else the default below. Values: "gdi", "dxgi", "dxgi-composition"
 * (case does not matter). A value that is not one of them selects GDI and is reported as invalid.
 */
#ifndef RADV_WDDM2_WSI_ROUTE_H
#define RADV_WDDM2_WSI_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum radv_wddm2_wsi_route {
   RADV_WDDM2_WSI_ROUTE_GDI = 0,
   RADV_WDDM2_WSI_ROUTE_DXGI = 1,
   RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION = 2,
};

/* The default route. It changes to DXGI only after the lab passes the DXGI route
 * (docs/design/vulkan-wsi-dxgi.md in bc250-win, section "The switch").
 */
#define RADV_WDDM2_WSI_ROUTE_DEFAULT RADV_WDDM2_WSI_ROUTE_GDI

enum radv_wddm2_wsi_route_source {
   RADV_WDDM2_WSI_SOURCE_DEFAULT = 0,
   RADV_WDDM2_WSI_SOURCE_ENV = 1,
   RADV_WDDM2_WSI_SOURCE_REGISTRY = 2,
};

struct radv_wddm2_wsi_route_choice {
   enum radv_wddm2_wsi_route route;
   enum radv_wddm2_wsi_route_source source;
   bool invalid; /* the source named a value that is not a route; route is GDI */
};

static inline const char *
radv_wddm2_wsi_route_name(enum radv_wddm2_wsi_route route)
{
   switch (route) {
   case RADV_WDDM2_WSI_ROUTE_GDI: return "gdi";
   case RADV_WDDM2_WSI_ROUTE_DXGI: return "dxgi";
   case RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION: return "dxgi-composition";
   default: return "unknown";
   }
}

static inline const char *
radv_wddm2_wsi_route_source_name(enum radv_wddm2_wsi_route_source source)
{
   switch (source) {
   case RADV_WDDM2_WSI_SOURCE_DEFAULT: return "default";
   case RADV_WDDM2_WSI_SOURCE_ENV: return "env";
   case RADV_WDDM2_WSI_SOURCE_REGISTRY: return "registry";
   default: return "unknown";
   }
}

static inline bool
radv_wddm2_wsi_ascii_ieq(const char *a, const char *b)
{
   for (;; a++, b++) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z')
         ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z')
         cb = (char)(cb - 'A' + 'a');
      if (ca != cb)
         return false;
      if (!ca)
         return true;
   }
}

/* Returns true and sets *out when text names a route. */
static inline bool
radv_wddm2_wsi_route_parse(const char *text, enum radv_wddm2_wsi_route *out)
{
   if (!text)
      return false;
   if (radv_wddm2_wsi_ascii_ieq(text, "gdi")) {
      *out = RADV_WDDM2_WSI_ROUTE_GDI;
      return true;
   }
   if (radv_wddm2_wsi_ascii_ieq(text, "dxgi")) {
      *out = RADV_WDDM2_WSI_ROUTE_DXGI;
      return true;
   }
   if (radv_wddm2_wsi_ascii_ieq(text, "dxgi-composition")) {
      *out = RADV_WDDM2_WSI_ROUTE_DXGI_COMPOSITION;
      return true;
   }
   return false;
}

/* env and reg are the raw values, NULL when absent. An empty string counts as absent, so that
 * "set AMDGPU_WDDM_VK_WSI=" in a shell clears the override.
 */
static inline struct radv_wddm2_wsi_route_choice
radv_wddm2_wsi_route_choose(const char *env, const char *reg)
{
   struct radv_wddm2_wsi_route_choice choice = {
      RADV_WDDM2_WSI_ROUTE_DEFAULT, RADV_WDDM2_WSI_SOURCE_DEFAULT, false,
   };
   const char *text = NULL;

   if (env && env[0]) {
      text = env;
      choice.source = RADV_WDDM2_WSI_SOURCE_ENV;
   } else if (reg && reg[0]) {
      text = reg;
      choice.source = RADV_WDDM2_WSI_SOURCE_REGISTRY;
   }
   if (!text)
      return choice;
   if (!radv_wddm2_wsi_route_parse(text, &choice.route)) {
      choice.route = RADV_WDDM2_WSI_ROUTE_GDI;
      choice.invalid = true;
   }
   return choice;
}

/* The module gate. The DXGI route loads System32 dxgi.dll and d3d12.dll by full path and creates a
 * D3D12 device on an explicit adapter. A process that already holds an application-local dxgi.dll
 * (DXVK), d3d12.dll or d3d12core.dll (vkd3d-proton) would see System32 modules bind their imports to
 * those by name (facts M792, M794; the vkd3d-proton case is the loader rule of M792 applied to
 * d3d12core). The route therefore stands down for such a process.
 *
 * Each path is the full path of the module that the process has loaded under that name, or NULL
 * when nothing is loaded under it. system_dir is the System32 directory without a trailing
 * separator. The comparison is ASCII case-insensitive and needs the module to sit directly in
 * system_dir.
 */
enum radv_wddm2_wsi_gate {
   RADV_WDDM2_WSI_GATE_OK = 0,
   RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR,
   RADV_WDDM2_WSI_GATE_FOREIGN_DXGI,
   RADV_WDDM2_WSI_GATE_FOREIGN_D3D12,
   RADV_WDDM2_WSI_GATE_FOREIGN_D3D12CORE,
};

static inline const char *
radv_wddm2_wsi_gate_name(enum radv_wddm2_wsi_gate gate)
{
   switch (gate) {
   case RADV_WDDM2_WSI_GATE_OK: return "ok";
   case RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR: return "no-system-dir";
   case RADV_WDDM2_WSI_GATE_FOREIGN_DXGI: return "foreign-dxgi";
   case RADV_WDDM2_WSI_GATE_FOREIGN_D3D12: return "foreign-d3d12";
   case RADV_WDDM2_WSI_GATE_FOREIGN_D3D12CORE: return "foreign-d3d12core";
   default: return "unknown";
   }
}

static inline wchar_t
radv_wddm2_wsi_wlower(wchar_t c)
{
   return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

/* True when path names a file directly inside dir. */
static inline bool
radv_wddm2_wsi_path_in_dir(const wchar_t *path, const wchar_t *dir)
{
   size_t i = 0;
   if (!path || !dir || !dir[0])
      return false;
   for (; dir[i]; i++) {
      wchar_t a = radv_wddm2_wsi_wlower(path[i]);
      wchar_t b = radv_wddm2_wsi_wlower(dir[i]);
      if (a == L'/')
         a = L'\\';
      if (b == L'/')
         b = L'\\';
      if (a != b)
         return false;
   }
   if (path[i] != L'\\' && path[i] != L'/')
      return false;
   i++;
   if (!path[i])
      return false;
   for (; path[i]; i++) {
      if (path[i] == L'\\' || path[i] == L'/')
         return false;
   }
   return true;
}

static inline enum radv_wddm2_wsi_gate
radv_wddm2_wsi_module_gate(const wchar_t *system_dir, const wchar_t *dxgi_path,
                           const wchar_t *d3d12_path, const wchar_t *d3d12core_path)
{
   if (!system_dir || !system_dir[0])
      return RADV_WDDM2_WSI_GATE_NO_SYSTEM_DIR;
   if (dxgi_path && !radv_wddm2_wsi_path_in_dir(dxgi_path, system_dir))
      return RADV_WDDM2_WSI_GATE_FOREIGN_DXGI;
   if (d3d12_path && !radv_wddm2_wsi_path_in_dir(d3d12_path, system_dir))
      return RADV_WDDM2_WSI_GATE_FOREIGN_D3D12;
   if (d3d12core_path && !radv_wddm2_wsi_path_in_dir(d3d12core_path, system_dir))
      return RADV_WDDM2_WSI_GATE_FOREIGN_D3D12CORE;
   return RADV_WDDM2_WSI_GATE_OK;
}

/* LB7A v1, the linear surface description the kernel driver, the D3D shells and this winsys share
 * (bc250-win driver/kmd/gdi_private.h, driver/contract/amdgpu_wddm_surface_format.h). The import
 * accepts the formats that the contract table enables for composition. bytes_per_pixel comes from
 * the format, never from a constant 4: an FP16 row is 8 bytes a pixel.
 */
struct radv_wddm2_lb7a {
   uint32_t magic, version, width, height, pitch, format;
   uint64_t size;
};

#define RADV_WDDM2_LB7A_MAGIC 0x4137424cu /* "LB7A" */

/* D3DDDIFORMAT numbers (d3dukmdt.h), as in the contract table. */
#define RADV_WDDM2_D3DDDIFMT_A8R8G8B8      21u
#define RADV_WDDM2_D3DDDIFMT_X8R8G8B8      22u
#define RADV_WDDM2_D3DDDIFMT_A2B10G10R10   31u
#define RADV_WDDM2_D3DDDIFMT_A8B8G8R8      32u
#define RADV_WDDM2_D3DDDIFMT_A16B16G16R16F 113u

static inline unsigned
radv_wddm2_lb7a_bytes_per_pixel(uint32_t d3dddi_format)
{
   switch (d3dddi_format) {
   case RADV_WDDM2_D3DDDIFMT_A8R8G8B8:
   case RADV_WDDM2_D3DDDIFMT_X8R8G8B8:
   case RADV_WDDM2_D3DDDIFMT_A8B8G8R8:
   case RADV_WDDM2_D3DDDIFMT_A2B10G10R10:
      return 4;
   case RADV_WDDM2_D3DDDIFMT_A16B16G16R16F:
      return 8;
   default:
      return 0;
   }
}

/* The checks the import applies to an LB7A block, in one place. */
static inline bool
radv_wddm2_lb7a_valid(const struct radv_wddm2_lb7a *s)
{
   const unsigned bpp = radv_wddm2_lb7a_bytes_per_pixel(s->format);
   if (s->magic != RADV_WDDM2_LB7A_MAGIC || s->version != 1 || !bpp)
      return false;
   if (!s->width || !s->height || s->width > 8192 || s->height > 8192)
      return false;
   if ((uint64_t)s->pitch < (uint64_t)s->width * bpp || (s->pitch & 15))
      return false;
   if (s->size < (uint64_t)s->pitch * s->height || s->size > UINT64_MAX - 4095)
      return false;
   return true;
}

#ifdef __cplusplus
}
#endif

#endif /* RADV_WDDM2_WSI_ROUTE_H */
