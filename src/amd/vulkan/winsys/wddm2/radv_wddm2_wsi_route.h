/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * radv_wddm2_wsi_route.h - which present route a BC-250 Vulkan swapchain takes, and the checks that
 * decide it. Plain C with no Windows or Vulkan header, so a host test drives the production rules.
 *
 * Routes:
 *   GDI               the CPU-image path of wsi_common_win32.cpp: a CPU copy into a DIB, BitBlt, DwmFlush.
 *                     It is the explicit rollback ("gdi") and the automatic fallback of the DXGI routes.
 *   DXGI              a D3D12 device on the same adapter (our D3D12 shell), a flip-model swap chain for the
 *                     window (CreateSwapChainForHwnd), one GPU copy per present on the D3D12 queue, Present1.
 *                     This is the shape the native D3D12 games present through, and the default.
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

/* The default route (owner decision 2026-10-07: a GPU route that has to be switched on gets
 * forgotten). A real failure of it still ends on GDI for that swapchain: no DXGI factory, no D3D12
 * device or queue, a D3D12 device that a replacement runtime implements, a fence or resource import
 * that fails, a swap chain that cannot be made (docs/design/vulkan-wsi-dxgi.md in bc250-win,
 * section "The switch and the fallback").
 */
#define RADV_WDDM2_WSI_ROUTE_DEFAULT RADV_WDDM2_WSI_ROUTE_DXGI

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

static inline wchar_t
radv_wddm2_wsi_wlower(wchar_t c)
{
   return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

/* True when path names a file directly inside dir (no trailing separator on dir). ASCII
 * case-insensitive; '/' and '\\' are the same separator.
 */
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

/* True when the last component of path is name (ASCII case-insensitive). */
static inline bool
radv_wddm2_wsi_base_name_is(const wchar_t *path, const wchar_t *name)
{
   const wchar_t *base = path;
   if (!path || !name)
      return false;
   for (const wchar_t *p = path; *p; p++) {
      if (*p == L'\\' || *p == L'/')
         base = p + 1;
   }
   for (;; base++, name++) {
      if (radv_wddm2_wsi_wlower(*base) != radv_wddm2_wsi_wlower(*name))
         return false;
      if (!*base)
         return true;
   }
}

/* Application-local runtime modules. The DXGI route binds System32 dxgi.dll and d3d12.dll by full
 * path and takes CreateDXGIFactory2 and D3D12CreateDevice from those handles, so a copy of dxgi.dll
 * (DXVK), d3d12.dll or d3d12core.dll (vkd3d-proton, or the Agility SDK) next to the application does
 * not change the route (owner decision 2026-10-07; M793: the DXGI present route passes beside DXVK
 * and vkd3d-proton DLLs in all six E56 modes). The copies are only reported on the init line.
 *
 * Each path is the full path of a module or file under that name, or NULL. A path directly inside
 * system_dir is not application-local.
 */
#define RADV_WDDM2_WSI_LOCAL_DXGI      (1u << 0)
#define RADV_WDDM2_WSI_LOCAL_D3D12     (1u << 1)
#define RADV_WDDM2_WSI_LOCAL_D3D12CORE (1u << 2)

static inline unsigned
radv_wddm2_wsi_app_local(const wchar_t *system_dir, const wchar_t *dxgi_path,
                         const wchar_t *d3d12_path, const wchar_t *d3d12core_path)
{
   unsigned mask = 0;
   if (dxgi_path && !radv_wddm2_wsi_path_in_dir(dxgi_path, system_dir))
      mask |= RADV_WDDM2_WSI_LOCAL_DXGI;
   if (d3d12_path && !radv_wddm2_wsi_path_in_dir(d3d12_path, system_dir))
      mask |= RADV_WDDM2_WSI_LOCAL_D3D12;
   if (d3d12core_path && !radv_wddm2_wsi_path_in_dir(d3d12core_path, system_dir))
      mask |= RADV_WDDM2_WSI_LOCAL_D3D12CORE;
   return mask;
}

/* "none", or a comma list such as "dxgi.dll(loaded),d3d12core.dll(file)": loaded names a module in
 * the process, file a DLL in the application's directory that nothing has loaded yet. Returns buf.
 */
static inline const char *
radv_wddm2_wsi_app_local_text(unsigned loaded, unsigned files, char *buf, size_t size)
{
   static const char *const names[3] = {"dxgi.dll", "d3d12.dll", "d3d12core.dll"};
   size_t used = 0;
   if (!buf || !size)
      return "";
   buf[0] = 0;
   for (unsigned i = 0; i < 3; i++) {
      const unsigned bit = 1u << i;
      const char *kind = (loaded & bit) ? "loaded" : (files & bit) ? "file" : NULL;
      if (!kind)
         continue;
      const char *parts[4] = {used ? "," : "", names[i], "(", kind};
      for (unsigned p = 0; p < 4; p++) {
         for (const char *c = parts[p]; *c && used + 2 < size; c++)
            buf[used++] = *c;
      }
      if (used + 1 < size)
         buf[used++] = ')';
      buf[used] = 0;
   }
   if (!used) {
      const char *none = "none";
      for (; *none && used + 1 < size; none++)
         buf[used++] = *none;
      buf[used] = 0;
   }
   return buf;
}

/* The one narrow fallback that application-local modules can cause. System32 d3d12.dll loads its
 * core at run time ("d3d12core" is a string in d3d12.dll, not an import), and vkd3d-proton's
 * d3d12core.dll exports the same D3D12GetInterface and D3D12SDKVersion as Microsoft's. If the
 * System32 runtime binds to such a replacement core, the "D3D12 device" is vkd3d-proton running on
 * Vulkan, not our D3D12 shell: it has no LB7A shared resources and would re-enter this ICD. The
 * route checks the module that implements the device it got (the module of the device's vtable):
 *
 *   SYSTEM    d3d12core.dll or d3d12.dll in System32: the system runtime.
 *   AGILITY   d3d12core.dll in the directory the application names by its D3D12SDKPath export.
 *   WRAPPED   any other module name (the debug layer d3d12sdklayers.dll, a capture tool): it
 *             forwards to the runtime and is accepted.
 *   FOREIGN   d3d12core.dll or d3d12.dll anywhere else: a replacement runtime. GDI for the process.
 *   UNKNOWN   the module could not be found; the later checks (resource pitch, fences) decide.
 *
 * agility_dir is NULL when the executable exports no D3D12SDKPath.
 */
enum radv_wddm2_wsi_d3d12_impl {
   RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM = 0,
   RADV_WDDM2_WSI_D3D12_IMPL_AGILITY,
   RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED,
   RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN,
   RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN,
};

static inline const char *
radv_wddm2_wsi_d3d12_impl_name(enum radv_wddm2_wsi_d3d12_impl impl)
{
   switch (impl) {
   case RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM: return "system";
   case RADV_WDDM2_WSI_D3D12_IMPL_AGILITY: return "agility";
   case RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED: return "wrapped";
   case RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN: return "foreign";
   default: return "unknown";
   }
}

static inline enum radv_wddm2_wsi_d3d12_impl
radv_wddm2_wsi_d3d12_impl_class(const wchar_t *system_dir, const wchar_t *agility_dir,
                                const wchar_t *impl_path)
{
   if (!impl_path || !impl_path[0])
      return RADV_WDDM2_WSI_D3D12_IMPL_UNKNOWN;
   const bool core = radv_wddm2_wsi_base_name_is(impl_path, L"d3d12core.dll");
   const bool front = radv_wddm2_wsi_base_name_is(impl_path, L"d3d12.dll");
   if (!core && !front)
      return RADV_WDDM2_WSI_D3D12_IMPL_WRAPPED;
   if (radv_wddm2_wsi_path_in_dir(impl_path, system_dir))
      return RADV_WDDM2_WSI_D3D12_IMPL_SYSTEM;
   if (core && agility_dir && radv_wddm2_wsi_path_in_dir(impl_path, agility_dir))
      return RADV_WDDM2_WSI_D3D12_IMPL_AGILITY;
   return RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN;
}

static inline bool
radv_wddm2_wsi_d3d12_impl_usable(enum radv_wddm2_wsi_d3d12_impl impl)
{
   return impl != RADV_WDDM2_WSI_D3D12_IMPL_FOREIGN;
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
