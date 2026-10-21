/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 */

#ifndef WSI_WIN32_FORMATS_H
#define WSI_WIN32_FORMATS_H

/* The surface formats of the Win32 WSI: the table and the rule that lists (format, color space)
 * pairs from it. They are here, with no Mesa header and no Windows runtime, so that a host test
 * drives the production list (src/vulkan/wsi/tests/wsi_win32_formats_test.cpp). The only headers
 * are the Vulkan core header and dxgiformat.h, both plain C enums.
 */

#include <stdbool.h>
#include <stddef.h>

#include <dxgiformat.h>
#include <vulkan/vulkan_core.h>

/* Surface formats. resource is the format of the D3D12 resource the Vulkan image aliases, buffer is
 * the format of the DXGI swap chain's buffers; CopyResource needs both in one cast family. A flip
 * model swap chain takes no sRGB buffer format, so the sRGB formats use the UNORM layout on both
 * sides: the bytes are the same, and the Vulkan view does the encoding. dxgi_only formats have no CPU
 * path (the GDI copy handles 8-bit BGRA and RGBA only) and are listed only on the DXGI route.
 */
struct wsi_win32_format {
   VkFormat     format;
   DXGI_FORMAT  resource;
   DXGI_FORMAT  buffer;
   bool         dxgi_only;
};

static const struct wsi_win32_format available_surface_formats[] = {
   { VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false },
   { VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false },
   { VK_FORMAT_B8G8R8A8_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false },
   { VK_FORMAT_R8G8B8A8_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, true },
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM,
     DXGI_FORMAT_R10G10B10A2_UNORM, true },
   { VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, true },
};

#define WSI_WIN32_FORMAT_COUNT (sizeof(available_surface_formats) / sizeof(available_surface_formats[0]))
/* The table's rows in the sRGB color space, plus FP16 scRGB and HDR10. */
#define WSI_WIN32_SURFACE_FORMATS_MAX (WSI_WIN32_FORMAT_COUNT + 2)

/* The 8-bit RGBA layouts, whose BGRA8 twins are the firmware plane's own format. */
static inline bool
wsi_win32_format_is_rgba8(VkFormat format)
{
   return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
}

struct wsi_win32_surface_format {
   VkFormat format;
   VkColorSpaceKHR color_space;
};

/* What the listing depends on, read by the caller at each listing. */
struct wsi_win32_format_inputs {
   bool dxgi;                      /* the DXGI route can be taken now (route usable and allowed) */
   bool force_bgra8_unorm_first;   /* the driconf option of that name */
   bool colorspace_ext;            /* the instance enabled VK_EXT_swapchain_colorspace */
   bool output_hdr;                /* the window's output runs in HDR10 (asked only with the two above) */
   /* wsi_device.win32.scanout_plane_formats and its device. Asked at each listing on the DXGI
    * route, never on the GDI route, and never kept from one listing to the next (BD-197).
    */
   bool (*scanout_plane_formats)(VkPhysicalDevice pdevice, const char **reason);
   VkPhysicalDevice pdevice;
};

/* The (format, color space) pairs of a surface, in the order they are reported. Returns the count;
 * out holds room for WSI_WIN32_SURFACE_FORMATS_MAX.
 *
 * BD-197. On the DXGI route a chain that covers its output is scanned out only in a layout the plane
 * takes. Where that is BGRA8 alone, an RGBA8 chain stays composed (the D3D12 shell's FORMAT clause),
 * and an application that prefers RGBA8 - Quake II RTX asks for R8G8B8A8_SRGB first - would never
 * reach an independent flip. The RGBA8 formats hold the same pixels as their BGRA8 twins, which stay
 * listed, so nothing an application can show is lost. That is the one change the plane reading
 * makes: the 10-bit and FP16 formats and the extended color spaces have no twin and stay listed
 * whatever it says, and such a chain is composed.
 */
static inline unsigned
wsi_win32_surface_formats_of(const struct wsi_win32_format_inputs *in,
                             struct wsi_win32_surface_format *out)
{
   const char *reason = NULL;
   const bool rgba8_listed = !in->dxgi || !in->scanout_plane_formats ||
                             in->scanout_plane_formats(in->pdevice, &reason);
   unsigned count = 0;

   for (unsigned i = 0; i < WSI_WIN32_FORMAT_COUNT; i++) {
      if (available_surface_formats[i].dxgi_only && !in->dxgi)
         continue;
      if (!rgba8_listed && wsi_win32_format_is_rgba8(available_surface_formats[i].format))
         continue;
      /* FP16 is scRGB (linear) in DXGI; it is only offered with the extended linear color space. */
      if (available_surface_formats[i].format == VK_FORMAT_R16G16B16A16_SFLOAT)
         continue;
      out[count].format = available_surface_formats[i].format;
      out[count].color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      count++;
   }

   if (in->force_bgra8_unorm_first) {
      for (unsigned i = 0; i < count; i++) {
         if (out[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            out[i] = out[0];
            out[0].format = VK_FORMAT_B8G8R8A8_UNORM;
            out[0].color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            break;
         }
      }
   }

   if (in->dxgi && in->colorspace_ext) {
      out[count].format = VK_FORMAT_R16G16B16A16_SFLOAT;
      out[count].color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
      count++;
      if (in->output_hdr) {
         out[count].format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
         out[count].color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
         count++;
      }
   }
   return count;
}

#endif /* WSI_WIN32_FORMATS_H */
