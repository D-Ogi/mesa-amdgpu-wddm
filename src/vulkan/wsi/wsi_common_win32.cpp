/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/cnd_monotonic.h"
#include "util/timespec.h"
#include "util/u_thread.h"
#include "util/os_time.h"
#include <inttypes.h>
#include "vk_format.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "wsi_win32_deadline.h"

#include <dxgi1_6.h>
#include "util/u_win32_library.h"
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <dcomp.h>
#include <dwmapi.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"      // warning: cast to pointer from integer of different size
#endif

struct wsi_win32;

struct wsi_win32 {
   struct wsi_interface                     base;

   struct wsi_device *wsi;

   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
   struct {
      IDXGIFactory4 *factory;
      IDCompositionDevice *dcomp;
   } dxgi;
   /* What the DXGI present route has shown and whether it is still allowed (wsi_win32_deadline.h).
    * Written by the first present that succeeds and by a wait of the route that expires.
    */
   struct wsi_win32_route_state route;
   /* The first D3D12 call of this instance's route that failed, with its HRESULT and the presenter's
    * removal reason read right after it (V2). Always on, written once, logged once.
    */
   struct wsi_win32_route_error error;
};

enum wsi_win32_image_state {
   WSI_IMAGE_IDLE,
   WSI_IMAGE_DRAWING,
   WSI_IMAGE_QUEUED,
};

struct wsi_win32_image {
   struct wsi_image base;
   enum wsi_win32_image_state state;
   struct wsi_win32_swapchain *chain;
   /* Whether the presenter's D3D12 Signal of the value now in base.blit.timeline_values[this image]
    * was accepted, which is what makes that value the presenter's and not the application's own
    * pending signal (wsi_win32_image_debt in wsi_win32_deadline.h). The retirement may host-signal a
    * shared blit timeline only for an image the presenter owes a value for.
    */
   struct wsi_win32_image_debt debt;
   struct {
      ID3D12Resource *swapchain_res;
      ID3D12Resource *blit_res;
      ID3D12CommandAllocator *cmd_alloc;
      ID3D12GraphicsCommandList *cmd_list;
   } dxgi;
   struct {
      HBITMAP bmp;
      int bmp_row_pitch;
      void *ppvBits;
   } sw;
};

struct wsi_win32_surface {
   VkIcdSurfaceWin32 base;

   /* The first time a swapchain is created against this surface, a DComp
    * target/visual will be created for it and that swapchain will be bound.
    * When a new swapchain is created, we delay changing the visual's content
    * until that swapchain has completed its first present once, otherwise the
    * window will flash white. When the currently-bound swapchain is destroyed,
    * the visual's content is unset.
    */
   IDCompositionTarget *target;
   IDCompositionVisual *visual;
   struct wsi_win32_swapchain *current_swapchain;
};

struct wsi_win32_swapchain {
   struct wsi_swapchain         base;
   IDXGISwapChain3            *dxgi;
   struct wsi_win32           *wsi;
   wsi_win32_surface          *surface;
   uint32_t                   next_cpu_image;
   mtx_t                      acquire_mutex;
   struct u_cnd_monotonic     acquire_cond;
   uint64_t                     flip_sequence;
   uint64_t                     completed_present_id;
   VkResult                     status;
   VkExtent2D                 extent;
   HWND wnd;
   VkFormat format;
   bool retired;
   /* The route of this chain. is_dxgi: the chain presents through a DXGI swap chain. dxgi holds that
    * swap chain until a newer chain on the same window takes it (detached). hwnd_target: the swap
    * chain belongs to the window (CreateSwapChainForHwnd); otherwise it is a composition swap chain
    * shown through the surface's DirectComposition visual.
    */
   bool is_dxgi;
   bool hwnd_target;
   bool detached;
   /* A bounded drain of the presenter's queue could not prove that the queue stopped referencing
    * this chain's resources (WSI_WIN32_FLUSH_UNPROVEN, V5). Nothing the queue may still name is
    * released while this is set: not the back buffers, not the copy command lists, not the imported
    * image memory, not the D3D12 resources, not the shared blit fences and not the DXGI chain. The
    * chain's own allocation is kept too, because every one of those references lives in it. It is a
    * deliberate leak of one swapchain, bounded by the route being retired at the same moment, and
    * the alternative is a release of memory a live GPU queue is reading.
    */
   bool resources_pinned;
   UINT swap_chain_flags;           /* DXGI_SWAP_CHAIN_FLAG_*, for ResizeBuffers on a steal */
   DXGI_FORMAT buffer_format;       /* the DXGI swap chain's buffer format */
   DXGI_COLOR_SPACE_TYPE color_space;
   /* The last present id handed to Present1. DXGI chains complete it in wait_for_present. */
   uint64_t                     submitted_present_id;
   const char                *route_reason;
   ID3D12Fence              **d3d12_blit_fences;
   /* The present this chain queued last. pending_present_value is the value wsi_dxgi_blit signalled
    * on d3d12_blit_fences[pending_present_image] after that copy; 0 means no present has been
    * queued. These two are diagnostic: the expired-acquire line prints them. What lifts the route's
    * deadlines is the ACQUIRED image's own timeline value and not this pair, because on a two-image
    * chain the last present always belongs to the other image (wsi_win32_deadline.h).
    */
   uint32_t                     pending_present_image;
   uint64_t                     pending_present_value;
   /* One bit per enum wsi_win32_present_stage: the stage lines this chain has already written. */
   uint32_t                     stage_log_bits;
   /* BC250_WSI_PRESENT_LOG=<file>: one CSV line per queued present with the
    * time spent in the CPU copy, BitBlt/Present1 and DwmFlush. Diagnostics
    * only; NULL unless the variable is set. */
   FILE                      *present_log;
   uint64_t                   present_log_lines;
   struct wsi_win32_image     images[0];
};

static void
wsi_win32_present_log(struct wsi_win32_swapchain *chain, const char *path,
                      uint64_t present_id, uint64_t t_enter, uint64_t t_copy,
                      uint64_t t_blit, uint64_t t_done, VkResult result)
{
   if (!chain->present_log)
      return;
   fprintf(chain->present_log,
           "%p,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d\n",
           (void *)chain, path, present_id, t_enter / 1000,
           (t_copy - t_enter) / 1000, (t_blit - t_copy) / 1000,
           (t_done - t_blit) / 1000, (int)result);
   chain->present_log_lines++;
   /* A lab arm is killed, not asked to quit, so a buffered row never reaches the file: the one row
    * the route's first present wrote on 2026-10-09 was lost exactly that way, and the arm reported
    * rows 0 for a present it had made. Flush at once while this chain is on a route that has not
    * proved itself, and for every one of the first 64 rows, whatever the path; after that the flush
    * is every 64 rows again, which is what a timedemo of thousands of frames wants.
    */
   if (chain->present_log_lines <= 64 ||
       (chain->is_dxgi && !wsi_win32_route_presented(&chain->wsi->route)) ||
       (chain->present_log_lines & 63) == 0)
      fflush(chain->present_log);
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                 uint32_t queueFamilyIndex)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;
   return (wsi_device->queue_supports_blit & BITFIELD64_BIT(queueFamilyIndex)) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateWin32SurfaceKHR(VkInstance _instance,
                          const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR);

   surface = (wsi_win32_surface *)vk_zalloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.base.platform = VK_ICD_WSI_PLATFORM_WIN32;

   surface->base.hinstance = pCreateInfo->hinstance;
   surface->base.hwnd = pCreateInfo->hwnd;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base.base);

   return VK_SUCCESS;
}

void
wsi_win32_surface_destroy(VkIcdSurfaceBase *icd_surface, VkInstance _instance,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   if (surface->visual)
      surface->visual->Release();
   if (surface->target)
      surface->target->Release();
   vk_free2(&instance->alloc, pAllocator, icd_surface);
}

static VkResult
wsi_win32_surface_get_support(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t queueFamilyIndex,
                              VkBool32* pSupported)
{
   *pSupported = true;

   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities(VkIcdSurfaceBase *surf,
                                   struct wsi_device *wsi_device,
                                   VkSurfaceCapabilities2KHR* caps)
{
   VkIcdSurfaceWin32 *surface = (VkIcdSurfaceWin32 *)surf;

   RECT win_rect;
   if (!GetClientRect(surface->hwnd, &win_rect))
      return VK_ERROR_SURFACE_LOST_KHR;

   caps->surfaceCapabilities.minImageCount = 1;

   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {
      /* DXGI doesn't support random presenting order (images need to
       * be presented in the order they were acquired), so we can't
       * expose more than two image per swapchain.
       */
      caps->surfaceCapabilities.minImageCount = caps->surfaceCapabilities.maxImageCount = 2;
   } else {
      caps->surfaceCapabilities.minImageCount = 1;
      /* Software callbacke, there is no real maximum */
      caps->surfaceCapabilities.maxImageCount = 0;
   }

   caps->surfaceCapabilities.currentExtent = {
      (uint32_t)win_rect.right - (uint32_t)win_rect.left,
      (uint32_t)win_rect.bottom - (uint32_t)win_rect.top
   };
   caps->surfaceCapabilities.minImageExtent =
      caps->surfaceCapabilities.currentExtent;
   caps->surfaceCapabilities.maxImageExtent =
      caps->surfaceCapabilities.currentExtent;

   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;

   caps->surfaceCapabilities.supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   /* A swap chain created for a window takes no alpha mode but IGNORE: only composition swap
    * chains blend with the desktop.
    */
   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue &&
       !wsi_device->win32.hwnd_target)
      caps->surfaceCapabilities.supportedCompositeAlpha |=
         VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
         VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;

   VkImageUsageFlags image_usage = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      image_usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   VkImageUsageFlags2CreateInfoKHR *usage2 =
      vk_find_struct(caps->pNext, IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2) {
      usage2->usage = image_usage;
   } else {
      caps->surfaceCapabilities.supportedUsageFlags = image_usage;
   }

   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    const void *info_next,
                                    VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeKHR *present_mode =
      (const VkSurfacePresentModeKHR *)vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);

   VkResult result =
      wsi_win32_surface_get_capabilities(surface, wsi_device,
                                         caps);

   vk_foreach_struct(sType, ext, caps->pNext) {
      switch (sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected_cap = (VkSurfaceProtectedCapabilitiesKHR *)ext;
         protected_cap->supportsProtected = VK_FALSE;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         /* Unsupported. */
         VkSurfacePresentScalingCapabilitiesEXT *scaling =
            (VkSurfacePresentScalingCapabilitiesEXT *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         /* Unsupported, just report the input present mode. */
         VkSurfacePresentModeCompatibilityKHR *compat =
            (VkSurfacePresentModeCompatibilityKHR *)ext;
         if (compat->pPresentModes) {
            if (compat->presentModeCount) {
               assert(present_mode);
               compat->pPresentModes[0] = present_mode->presentMode;
               compat->presentModeCount = 1;
            }
         } else {
            if (!present_mode)
               wsi_common_vk_warn_once("Use of VkSurfacePresentModeCompatibilityKHR "
                                       "without a VkSurfacePresentModeKHR set. This is an "
                                       "application bug.\n");
            compat->presentModeCount = 1;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR: {
         VkSurfaceCapabilitiesPresentId2KHR *caps = (VkSurfaceCapabilitiesPresentId2KHR *)ext;
         caps->presentId2Supported = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR: {
         VkSurfaceCapabilitiesPresentWait2KHR *caps = (VkSurfaceCapabilitiesPresentWait2KHR *)ext;
         caps->presentWait2Supported = VK_TRUE;
         break;
      }

      case VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT: {
         VkPresentTimingSurfaceCapabilitiesEXT *wait = (VkPresentTimingSurfaceCapabilitiesEXT *)ext;

         wait->presentStageQueries = 0;
         wait->presentTimingSupported = VK_FALSE;
         wait->presentAtAbsoluteTimeSupported = VK_FALSE;
         wait->presentAtRelativeTimeSupported = VK_FALSE;
         break;
      }

      case VK_STRUCTURE_TYPE_SWAPCHAIN_FLAGS_SURFACE_CAPABILITIES_EXT: {
         VkSwapchainFlagsSurfaceCapabilitiesEXT *surface_caps = (VkSwapchainFlagsSurfaceCapabilitiesEXT*)ext;
         VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);

         if (pdevice->supported_extensions.EXT_multisampled_render_to_swapchain)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;
         if (pdevice->supported_extensions.KHR_bind_memory2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR;
         if (pdevice->supported_extensions.KHR_present_id2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
         if (pdevice->supported_extensions.KHR_present_wait2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
         if (pdevice->supported_extensions.KHR_swapchain_mutable_format)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
         if (pdevice->supported_extensions.EXT_present_timing)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
         if (pdevice->supported_extensions.KHR_swapchain_maintenance1 ||
             pdevice->supported_extensions.EXT_swapchain_maintenance1)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT;
      }
      break;

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}


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

static const struct wsi_win32_format *
wsi_win32_find_format(VkFormat format)
{
   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      if (available_surface_formats[i].format == format)
         return &available_surface_formats[i];
   }
   return NULL;
}

static bool
wsi_win32_device_uses_dxgi(const struct wsi_device *wsi_device)
{
   return !wsi_device->sw && wsi_device->win32.get_d3d12_command_queue;
}

/* True when the output that shows most of the window runs in HDR (ST.2084, BT.2020). The HDR10
 * color space is only offered then, as other Windows drivers do; an application that asks for it
 * on an SDR output would get a swap chain that DXGI refuses to tag.
 */
static bool
wsi_win32_output_is_hdr(IDXGIFactory4 *factory, HWND hwnd)
{
   if (!factory)
      return false;
   HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
   bool hdr = false;
   IDXGIAdapter1 *adapter = NULL;
   for (UINT a = 0; !hdr && SUCCEEDED(factory->EnumAdapters1(a, &adapter)); a++) {
      IDXGIOutput *output = NULL;
      for (UINT o = 0; !hdr && SUCCEEDED(adapter->EnumOutputs(o, &output)); o++) {
         DXGI_OUTPUT_DESC desc;
         IDXGIOutput6 *output6 = NULL;
         if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor &&
             SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
            DXGI_OUTPUT_DESC1 desc1;
            if (SUCCEEDED(output6->GetDesc1(&desc1)))
               hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            output6->Release();
         }
         output->Release();
      }
      adapter->Release();
   }
   return hdr;
}

struct wsi_win32_surface_format {
   VkFormat format;
   VkColorSpaceKHR color_space;
};

/* The (format, color space) pairs of a surface, in the order they are reported. Returns the count;
 * out holds room for ARRAY_SIZE(available_surface_formats) + 2.
 */
static unsigned
wsi_win32_list_surface_formats(VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device,
                               struct wsi_win32_surface_format *out)
{
   /* The formats without a CPU path are offered only when this device can take the DXGI route
    * now. A driver that cannot make its D3D12 presenter says so here, before the application
    * picks a format, so that the swapchain can still fall back to CPU images.
    *
    * A route a deadline already retired counts as unavailable. Without that, an application that
    * was given its window back went on being offered a format with no CPU path, and the swapchain
    * it then created on that format answered VK_ERROR_INITIALIZATION_FAILED instead of taking CPU
    * images: the fallback the retired route exists for would not have been reachable.
    */
   const char *reason = NULL;
   struct wsi_win32 *win32 = (struct wsi_win32 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];
   const bool dxgi = wsi_win32_device_uses_dxgi(wsi_device) &&
                     (!win32 || wsi_win32_route_usable(&win32->route)) &&
                     (!wsi_device->win32.route_allowed ||
                      wsi_device->win32.route_allowed(wsi_device->pdevice, &reason));
   unsigned count = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      if (available_surface_formats[i].dxgi_only && !dxgi)
         continue;
      /* FP16 is scRGB (linear) in DXGI; it is only offered with the extended linear color space. */
      if (available_surface_formats[i].format == VK_FORMAT_R16G16B16A16_SFLOAT)
         continue;
      out[count++] = { available_surface_formats[i].format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
   }

   if (wsi_device->force_bgra8_unorm_first) {
      for (unsigned i = 0; i < count; i++) {
         if (out[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            out[i] = out[0];
            out[0] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
            break;
         }
      }
   }

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (dxgi && pdevice->instance->enabled_extensions.EXT_swapchain_colorspace) {
      struct wsi_win32 *wsi = (struct wsi_win32 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];
      out[count++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT };
      if (wsi_win32_output_is_hdr(wsi->dxgi.factory, ((VkIcdSurfaceWin32 *)icd_surface)->hwnd))
         out[count++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT };
   }
   return count;
}

static VkResult
wsi_win32_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                              struct wsi_device *wsi_device,
                              uint32_t* pSurfaceFormatCount,
                              VkSurfaceFormatKHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   struct wsi_win32_surface_format formats[ARRAY_SIZE(available_surface_formats) + 2];
   const unsigned count = wsi_win32_list_surface_formats(icd_surface, wsi_device, formats);

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = formats[i].format;
         f->colorSpace = formats[i].color_space;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_win32_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               const void *info_next,
                               uint32_t* pSurfaceFormatCount,
                               VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   struct wsi_win32_surface_format formats[ARRAY_SIZE(available_surface_formats) + 2];
   const unsigned count = wsi_win32_list_surface_formats(icd_surface, wsi_device, formats);

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = formats[i].format;
         f->surfaceFormat.colorSpace = formats[i].color_space;
      }
   }

   return vk_outarray_status(&out);
}

static DXGI_COLOR_SPACE_TYPE
wsi_win32_dxgi_color_space(VkColorSpaceKHR color_space)
{
   switch (color_space) {
   case VK_COLOR_SPACE_HDR10_ST2084_EXT:
      return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
   case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
      return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
   default:
      return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
   }
}

/* The GDI path composes through DWM. FIFO waits for the next composition
 * (DwmFlush) before the image is released; IMMEDIATE releases the image as
 * soon as the BitBlt into the redirection surface has completed, so an
 * application with vsync off (DXGI SyncInterval 0) is no longer quantised
 * to the compositor's 60 Hz. DWM still shows at most one frame per refresh.
 */
static const VkPresentModeKHR present_modes_gdi[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};
static const VkPresentModeKHR present_modes_dxgi[] = {
   VK_PRESENT_MODE_IMMEDIATE_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_win32_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    uint32_t* pPresentModeCount,
                                    VkPresentModeKHR* pPresentModes)
{
   const VkPresentModeKHR *array;
   size_t array_size;
   if (wsi_device->sw || !wsi_device->win32.get_d3d12_command_queue) {
      array = present_modes_gdi;
      array_size = ARRAY_SIZE(present_modes_gdi);
   } else {
      array = present_modes_dxgi;
      array_size = ARRAY_SIZE(present_modes_dxgi);
   }

   if (pPresentModes == NULL) {
      *pPresentModeCount = array_size;
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, array_size);
   typed_memcpy(pPresentModes, array, *pPresentModeCount);

   if (*pPresentModeCount < array_size)
      return VK_INCOMPLETE;
   else
      return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                      struct wsi_device *wsi_device,
                                      uint32_t* pRectCount,
                                      VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      /* We don't know a size so just return the usual "I don't know." */
      *rect = {
         { 0, 0 },
         { UINT32_MAX, UINT32_MAX },
      };
   }

   return vk_outarray_status(&out);
}

static DXGI_FORMAT
convert_to_dxgi_format(VkFormat vk_format)
{
   const struct wsi_win32_format *format = wsi_win32_find_format(vk_format);
   return format ? format->resource : DXGI_FORMAT_UNKNOWN;
}

static void
wsi_win32_route_log(struct wsi_win32_swapchain *chain, const char *fmt, ...)
{
   const struct wsi_device *wsi = chain->base.wsi;
   if (!wsi->win32.route_log)
      return;
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   wsi->win32.route_log(chain->base.device, line);
}

/* One line per stage per chain, while the route has not proved itself, so that a freeze names the
 * call that did not return without a debugger on the machine (BD-105). Silent for the GDI path and
 * for a route that has completed a present and an acquire.
 *
 * The gate is a bit per stage of this chain, not the route's presented flag: on 2026-10-09 the flag
 * went true when Present1 returned and silenced the acquire of the next frame, which is the call
 * that froze.
 */
static void
wsi_win32_stage_log(struct wsi_win32_swapchain *chain, enum wsi_win32_present_stage stage)
{
   if (!chain->is_dxgi || wsi_win32_route_presented(&chain->wsi->route))
      return;
   if (!wsi_win32_stage_first(&chain->stage_log_bits, stage))
      return;
   wsi_win32_route_log(chain, "chain %p: first present stage %s", (void *)chain,
                       wsi_win32_present_stage_name(stage));
}

/* The presenter's removal reason, and whether it could be read at all.
 *
 * S_OK from ID3D12Device::GetDeviceRemovedReason means a live device, so "could not be read" and
 * "alive" must not come out as the same value: round 4 returned S_OK for a driver that gave the WSI
 * no get_d3d12_device hook, which reads as a live presenter on evidence nobody has. The answer is
 * therefore a bool and an out parameter (V1, wsi_win32_presenter_state in wsi_win32_deadline.h).
 */
static bool
wsi_win32_device_removed_reason(struct wsi_win32_swapchain *chain, HRESULT *out_reason)
{
   const struct wsi_device *wsi = chain->base.wsi;
   *out_reason = S_OK;
   if (!wsi->win32.get_d3d12_device)
      return false;
   ID3D12Device *device = (ID3D12Device *)wsi->win32.get_d3d12_device(chain->base.device);
   if (!device)
      return false;
   *out_reason = device->GetDeviceRemovedReason();
   return true;
}

/* What is established about the presenter, from the two readings there are. image is the image whose
 * shared blit fence is read for the UINT64_MAX removal sentinel, or UINT32_MAX for no fence reading.
 * The log line names the state and both readings, so a lab record says which of the three it was.
 */
static enum wsi_win32_presenter_state
wsi_win32_presenter_read(struct wsi_win32_swapchain *chain, uint32_t image)
{
   HRESULT reason = S_OK;
   const bool reason_read = wsi_win32_device_removed_reason(chain, &reason);
   bool fence_read = false;
   uint64_t fence_value = 0;

   if (image != WSI_WIN32_ROUTE_ERROR_NO_IMAGE && chain->d3d12_blit_fences &&
       image < chain->base.image_count && chain->d3d12_blit_fences[image]) {
      fence_value = chain->d3d12_blit_fences[image]->GetCompletedValue();
      fence_read = true;
   }

   const enum wsi_win32_presenter_state state =
      wsi_win32_presenter_state(reason_read, (uint32_t)reason, fence_read, fence_value);
   wsi_win32_route_log(chain, "chain %p: presenter %s: removed reason %s0x%08lx, blit fence of "
                       "image %u %s%" PRIu64, (void *)chain,
                       wsi_win32_presenter_state_name(state), reason_read ? "" : "unread ",
                       (unsigned long)reason, image, fence_read ? "" : "unread ", fence_value);
   return state;
}

/* The removal reason as a bare number, for the error ledger. 0 when it could not be read, which the
 * ledger's own line says.
 */
static uint32_t
wsi_win32_removed_reason_value(struct wsi_win32_swapchain *chain)
{
   HRESULT reason = S_OK;
   if (!wsi_win32_device_removed_reason(chain, &reason))
      return 0u;
   return (uint32_t)reason;
}

/* V2. One D3D12 call of the route failed. The first such call of the process is kept and logged with
 * its name, its HRESULT, the presenter's removal reason read immediately afterwards and the image it
 * was for; later ones are counted by nothing and written by nothing, because the first one caused
 * them. Always on: no switch, one line, and the budget is one record per process.
 */
static void
wsi_win32_route_failed(struct wsi_win32_swapchain *chain, const char *call, HRESULT hr,
                       uint32_t image)
{
   const uint32_t reason = wsi_win32_removed_reason_value(chain);
   if (!wsi_win32_route_note_error(&chain->wsi->error, call, (uint32_t)hr, reason, image))
      return;
   wsi_win32_route_log(chain, "chain %p: FIRST route failure: %s hr=0x%08lx image %u, presenter "
                       "removed reason 0x%08lx", (void *)chain, call, (unsigned long)hr, image,
                       (unsigned long)reason);
}

/* Releases the queue work a dead route left waiting on its shared blit timelines, as far as it can
 * PROVE it is allowed to.
 *
 * The rule and the reason are wsi_win32_route_retire_action in wsi_win32_deadline.h: the
 * application's second submission per presented image waits for the value only the presenter's D3D12
 * Signal can reach, and a presenter that is gone leaves that wait outstanding for the life of the
 * process. The application then freezes not in a call of ours but in its own recovery -
 * vkDeviceWaitIdle of a resize path, which has no timeout - and no deadline of this route reaches it
 * (BD-105, the lab round of 2026-10-09).
 *
 * Every image of the chain is considered, not only the one whose wait expired: a two-image chain had
 * both presented, and the one that is not being acquired carries the same outstanding wait.
 *
 * What round 4b adds is the proof the signal needs (V1). A host signal of a timeline may only raise
 * it above its current value and must stay below any PENDING signal
 * (VUID-VkSemaphoreSignalInfo-value-03258/03259), and the application's own first submission signals
 * the value one below ours. So each image is released only when the presenter is proved REMOVED and
 * the semaphore reads exactly that one-below value; when it reads lower, the first submission is
 * still pending and this function waits for it, bounded, once, and asks again. An image it still
 * cannot release is reported as outstanding, and the caller answers the application VK_ERROR_DEVICE_LOST
 * instead of pretending a frame completed.
 *
 * What the review of round 4b adds is the second half of that proof. The semaphore reading "one
 * below the timeline value" only means the presenter owes one value when the timeline value IS the
 * presenter's; after a blit that failed it is the application's own pending signal, and signalling
 * past it is the very thing 03259 forbids. So each image is asked whether the presenter's Signal for
 * the value it carries was accepted (wsi_win32_image_debt, written in wsi_dxgi_blit), and an image
 * the presenter owes nothing for has nothing of ours outstanding behind it at all - the second
 * submission was never made.
 *
 * Returns true when at least one image is left with an outstanding wait of ours.
 */
static bool
wsi_win32_retire_blit_waits(struct wsi_win32_swapchain *chain,
                            enum wsi_win32_presenter_state presenter)
{
   const struct wsi_device *wsi = chain->base.wsi;
   bool outstanding = false;

   if (!chain->is_dxgi || !chain->base.blit.semaphores || !chain->base.blit.timeline_values ||
       !wsi->SignalSemaphore || !wsi->GetSemaphoreCounterValue)
      return false;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      const VkSemaphore semaphore = chain->base.blit.semaphores[i];
      const uint64_t want_present = chain->base.blit.timeline_values[i];
      /* Whether that value is the presenter's at all. A blit that failed left the application's own
       * pending value there, and nothing of ours waits for it.
       */
      const bool owes = wsi_win32_image_debt_owed(&chain->images[i].debt);
      uint64_t want = 0, have = 0;

      if (semaphore == VK_NULL_HANDLE)
         continue;
      if (wsi->GetSemaphoreCounterValue(chain->base.device, semaphore, &have) != VK_SUCCESS) {
         outstanding = outstanding || (owes && want_present != 0);
         continue;
      }

      enum wsi_win32_retire_action action =
         wsi_win32_route_retire_action(presenter, owes, want_present, have, &want);

      /* The application's own signal of the value below ours has not completed yet. It runs on the
       * application's own device, which is not the one that died, so it is expected to be satisfied
       * at once or never. Wait for it once, inside a bound of its own, and read the semaphore again.
       */
      if (action == WSI_WIN32_RETIRE_REFUSE && presenter == WSI_WIN32_PRESENTER_REMOVED &&
          wsi->WaitSemaphores) {
         const uint64_t wait_value = wsi_win32_route_retire_wait_value(want_present, have);
         if (wait_value) {
            const VkSemaphoreWaitInfo wait_info = {
               VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
               NULL,
               0,
               1,
               &semaphore,
               &wait_value,
            };
            const VkResult waited = wsi->WaitSemaphores(chain->base.device, &wait_info,
                                                        WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS);
            if (waited == VK_SUCCESS &&
                wsi->GetSemaphoreCounterValue(chain->base.device, semaphore, &have) == VK_SUCCESS)
               action = wsi_win32_route_retire_action(presenter, owes, want_present, have, &want);
            wsi_win32_route_log(chain, "chain %p: image %u: waited %" PRIu64 " ms for the "
                                "application's own signal %" PRIu64 " (%d), semaphore now %" PRIu64,
                                (void *)chain, i,
                                WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS / 1000000ull, wait_value,
                                (int)waited, have);
         }
      }

      if (action == WSI_WIN32_RETIRE_NOTHING) {
         /* The aliased reading the review of round 4b caught: a value below the timeline value, and
          * the presenter owes nothing for it. Said once per image, so a lab record shows that the
          * host declined to signal because the blit failed and not because nothing was presented.
          */
         if (want_present && !owes && have < want_present)
            wsi_win32_route_log(chain, "chain %p: image %u: nothing of ours is outstanding: the "
                                "presenter accepted no signal for %" PRIu64 ", so that value is the "
                                "application's own pending signal (semaphore %" PRIu64 ")",
                                (void *)chain, i, want_present, have);
         continue;
      }

      if (action == WSI_WIN32_RETIRE_REFUSE) {
         outstanding = true;
         wsi_win32_route_log(chain, "chain %p: image %u: the shared blit timeline is NOT signalled "
                             "from the CPU: presenter %s, semaphore %" PRIu64 ", the presenter's "
                             "value %" PRIu64 ". Signalling it would pass a pending signal "
                             "(VUID-VkSemaphoreSignalInfo-value-03259)", (void *)chain, i,
                             wsi_win32_presenter_state_name(presenter), have, want_present);
         continue;
      }

      const VkSemaphoreSignalInfo signal_info = {
         VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
         NULL,
         semaphore,
         want,
      };
      const VkResult result = wsi->SignalSemaphore(chain->base.device, &signal_info);
      if (result != VK_SUCCESS)
         outstanding = true;
      wsi_win32_route_log(chain, "chain %p: image %u: the shared blit timeline signalled from the "
                          "CPU, %" PRIu64 " -> %" PRIu64 " (%d), so the submission the dead route "
                          "left waiting can complete", (void *)chain, i, have, want, (int)result);
   }

   return outstanding;
}

static VkResult
wsi_dxgi_create_d3d12_resource(struct wsi_win32_swapchain *chain,
                               struct wsi_win32_image *win32_image,
                               HANDLE *out_handle)
{
   struct wsi_device *wsi_device = chain->wsi->wsi;
   ID3D12Device *d3d12_device;
   HRESULT hr;

   D3D12_HEAP_PROPERTIES heap_props = { 0 };
   heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
   heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
   heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
   heap_props.CreationNodeMask = 1;
   heap_props.VisibleNodeMask = 1;

   D3D12_RESOURCE_DESC desc = { 0 };
   desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   desc.Alignment = 0;
   desc.Width = chain->extent.width;
   desc.Height = chain->extent.height;
   desc.DepthOrArraySize = 1;
   desc.MipLevels = 1;
   desc.Format = convert_to_dxgi_format(chain->base.image_info.create.format);
   desc.SampleDesc = {1, 0};
   desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
   desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

   d3d12_device = (ID3D12Device *)wsi_device->win32.get_d3d12_device(chain->base.device);
   hr = d3d12_device->CreateCommittedResource(&heap_props,
                                              D3D12_HEAP_FLAG_SHARED,
                                              &desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              NULL,
                                              IID_PPV_ARGS(&win32_image->dxgi.blit_res));

   if (hr != S_OK) {
      wsi_win32_route_failed(chain, "ID3D12Device::CreateCommittedResource", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   if (out_handle) {
      hr = d3d12_device->CreateSharedHandle((ID3D12DeviceChild *)win32_image->dxgi.blit_res,
                                            NULL,
                                            GENERIC_ALL,
                                            NULL,
                                            out_handle);
      if (hr != S_OK) {
         wsi_win32_route_failed(chain, "ID3D12Device::CreateSharedHandle", hr,
                                WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
         win32_image->dxgi.blit_res->Release();
         win32_image->dxgi.blit_res = NULL;
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   return VK_SUCCESS;
}

static VkResult
wsi_dxgi_create_blit_context(struct wsi_win32_swapchain *chain,
                             struct wsi_win32_image *win32_image)
{
   struct wsi_device *wsi_device = chain->wsi->wsi;
   ID3D12Device *d3d12_device;
   ID3D12Resource *src = win32_image->dxgi.blit_res;
   ID3D12Resource *dst = win32_image->dxgi.swapchain_res;
   HRESULT hr;

   d3d12_device = (ID3D12Device *)wsi_device->win32.get_d3d12_device(chain->base.device);
   hr = d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&win32_image->dxgi.cmd_alloc));
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12Device::CreateCommandAllocator", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   hr = d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        win32_image->dxgi.cmd_alloc, NULL,
                                        IID_PPV_ARGS(&win32_image->dxgi.cmd_list));
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12Device::CreateCommandList", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      /* The pointer goes with the reference: wsi_win32_image_release_dxgi_buffers releases whatever
       * is still in the image, so a released allocator left in place would be released twice.
       */
      win32_image->dxgi.cmd_alloc->Release();
      win32_image->dxgi.cmd_alloc = NULL;
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   ID3D12GraphicsCommandList *cmd_list = win32_image->dxgi.cmd_list;
   D3D12_RESOURCE_BARRIER barrier = { 0 };
   barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

   barrier.Transition.pResource = dst;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
   cmd_list->ResourceBarrier(1, &barrier);

   barrier.Transition.pResource = src;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
   cmd_list->ResourceBarrier(1, &barrier);

   cmd_list->CopyResource(dst, src);

   barrier.Transition.pResource = dst;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
   cmd_list->ResourceBarrier(1, &barrier);

   barrier.Transition.pResource = src;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
   cmd_list->ResourceBarrier(1, &barrier);

   /* V2. Close is where a recording error is reported (local sdk-api
    * nf-d3d12-id3d12graphicscommandlist-close.md:59-75), and round 4 dropped its HRESULT. A list
    * that did not close must never be executed: ExecuteCommandLists of an invalid list is one of
    * the documented reasons the runtime removes the device (:71-79), and that removal would then be
    * the thing a later timeout could not explain. The chain's creation fails instead, which the
    * caller turns into CPU images.
    */
   hr = cmd_list->Close();
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12GraphicsCommandList::Close", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      win32_image->dxgi.cmd_list->Release();
      win32_image->dxgi.cmd_list = NULL;
      win32_image->dxgi.cmd_alloc->Release();
      win32_image->dxgi.cmd_alloc = NULL;
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static void
wsi_dxgi_destroy_blit_context(struct wsi_win32_swapchain *chain,
                              struct wsi_win32_image *win32_image)
{
   if (win32_image->dxgi.cmd_list)
      win32_image->dxgi.cmd_list->Release();
   if (win32_image->dxgi.cmd_alloc)
      win32_image->dxgi.cmd_alloc->Release();
}

static VkResult
wsi_dxgi_finish_create_image(const struct wsi_swapchain *chain,
                             const struct wsi_image_info *info,
                             struct wsi_image *image)
{
   struct wsi_win32_swapchain *win32_chain =
      container_of(chain, struct wsi_win32_swapchain, base);
   struct wsi_win32_image *win32_image =
      container_of(image, struct wsi_win32_image, base);
   const struct wsi_device *wsi = chain->wsi;

   /* The Vulkan image aliases the D3D12 resource; the driver confirms that both describe the
    * memory the same way (for a linear image: the same row pitch) before the chain uses it.
    */
   if (wsi->win32.check_blit_image) {
      VkResult result = wsi->win32.check_blit_image(chain->device, image->image, image->memory);
      if (result != VK_SUCCESS)
         return result;
   }

   return wsi_dxgi_create_blit_context(win32_chain, win32_image);
}

static VkResult
wsi_dxgi_blit(struct wsi_swapchain *drv_chain, uint32_t image_index)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   struct wsi_win32_image *win32_image = &chain->images[image_index];
   struct wsi_device *wsi_device = chain->wsi->wsi;

   /* The presenter owes this image nothing until its Signal below is accepted. Everything that
    * returns from here on leaves the application's own pending value in timeline_values[image_index],
    * and the retirement must not read that as a value the presenter still has to reach
    * (VUID-VkSemaphoreSignalInfo-value-03259; wsi_win32_image_debt in wsi_win32_deadline.h). The
    * previous cycle's debt is not lost by this: the application acquired this image to get here, and
    * that acquire waited for the fence the previous cycle's second submission signals.
    */
   wsi_win32_image_debt_reset(&win32_image->debt);

   ID3D12CommandQueue *queue = (ID3D12CommandQueue *)
      wsi_device->win32.get_d3d12_command_queue(chain->base.device);
   if (!queue)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* V2. Every HRESULT on this path is read, and a failure stops the enqueueing that depends on it.
    * Why it matters more here than anywhere else in the file: wsi_common_queue_present skips the
    * application's SECOND submission and the present itself when this function does not return
    * VK_SUCCESS (wsi_common.c:2831-2832, 2887-2888), so a reported failure leaves nothing waiting on
    * the shared timeline, while a swallowed one left the application waiting for a copy that was
    * never enqueued and the first diagnostic came two seconds later from a timeout that could name
    * nothing.
    *
    * The queue's Wait is GPU-side and returns before the GPU does anything (local sdk-api
    * nf-d3d12-id3d12commandqueue-wait.md:50), so its HRESULT is the only thing it tells us; the
    * acquire that waits for this copy is the bounded end of it (wsi_win32_deadline.h).
    */
   uint64_t wait_value = chain->base.blit.timeline_values[image_index];
   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_FENCE_WAIT);
   HRESULT hr = queue->Wait(chain->d3d12_blit_fences[image_index], wait_value);
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12CommandQueue::Wait", hr, image_index);
      return VK_ERROR_DEVICE_LOST;
   }

   /* A detached chain gave its DXGI buffers to a newer chain: keep the fence order, skip the copy. */
   if (!chain->detached && win32_image->dxgi.cmd_list) {
      ID3D12CommandList *cmd_lists[] = {(ID3D12CommandList *)win32_image->dxgi.cmd_list};
      wsi_win32_stage_log(chain, WSI_WIN32_STAGE_EXECUTE);
      queue->ExecuteCommandLists(1, cmd_lists);
      /* ExecuteCommandLists returns void, and an invalid list or state transition is one of the
       * documented reasons the runtime removes the device (nf-d3d12-id3d12commandqueue-
       * executecommandlists.md:71-79). The removal reason read right here is the only thing that
       * names this call as the one that did it.
       */
      const uint32_t removed = wsi_win32_removed_reason_value(chain);
      if (removed) {
         wsi_win32_route_failed(chain, "ID3D12CommandQueue::ExecuteCommandLists",
                                (HRESULT)removed, image_index);
         return VK_ERROR_DEVICE_LOST;
      }
   }

   /* The timeline value is raised only for a Signal that was accepted. A Signal that failed with the
    * value already raised would leave the chain claiming the presenter owes a value nothing waits
    * for, and the retirement rule would then read an outstanding wait that does not exist.
    */
   const uint64_t signal_value = chain->base.blit.timeline_values[image_index] + 1;
   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_SIGNAL);
   hr = queue->Signal(chain->d3d12_blit_fences[image_index], signal_value);
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12CommandQueue::Signal", hr, image_index);
      return VK_ERROR_DEVICE_LOST;
   }
   chain->base.blit.timeline_values[image_index] = signal_value;
   /* After the value, never before it: a retirement that saw the debt against the application's own
    * value would host-signal past a pending signal, which is the defect this order prevents.
    */
   wsi_win32_image_debt_note_signalled(&win32_image->debt);

   return VK_SUCCESS;
}

static VkResult
wsi_create_dxgi_image_mem(const struct wsi_swapchain *drv_chain,
                          const struct wsi_image_info *info,
                          struct wsi_image *image)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   const struct wsi_device *wsi = chain->base.wsi;

   VkImportMemoryWin32HandleInfoKHR import_memory_info = {
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      NULL,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
      NULL,
      NULL,
   };

   assert(chain->base.blit.type != WSI_SWAPCHAIN_BUFFER_BLIT);

   struct wsi_win32_image *win32_image =
      container_of(image, struct wsi_win32_image, base);
   uint32_t image_idx =
      ((uintptr_t)win32_image - (uintptr_t)chain->images) /
      sizeof(*win32_image);
   const HRESULT buffer_hr = chain->dxgi->GetBuffer(image_idx,
                                                    IID_PPV_ARGS(&win32_image->dxgi.swapchain_res));
   if (FAILED(buffer_hr)) {
      wsi_win32_route_failed(chain, "IDXGISwapChain::GetBuffer", buffer_hr, image_idx);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   if (wsi->win32.create_image_memory) {
      VkResult result =
         wsi->win32.create_image_memory(chain->base.device,
                                        win32_image->dxgi.swapchain_res,
                                        &chain->base.alloc,
                                        chain->base.blit.type == WSI_SWAPCHAIN_NO_BLIT ?
                                        &image->memory : &image->blit.memory);
      if (result != VK_SUCCESS)
         return result;

      if (chain->base.blit.type == WSI_SWAPCHAIN_NO_BLIT)
         return VK_SUCCESS;

      VkImageCreateInfo create = info->create;

      create.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
      create.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

      result = wsi->CreateImage(chain->base.device, &create,
                                &chain->base.alloc, &image->blit.image);
      if (result != VK_SUCCESS)
         return result;

      result = wsi->BindImageMemory(chain->base.device, image->blit.image,
                                    image->blit.memory, 0);
      if (result != VK_SUCCESS)
         return result;
   } else {
      VkResult result = wsi_dxgi_create_d3d12_resource(chain, win32_image,
                                                       &import_memory_info.handle);
      if (result != VK_SUCCESS)
         return result;
   }

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->base.device, image->image, &reqs);

   VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      nullptr,
      image->image,
      VK_NULL_HANDLE,
   };
   VkMemoryAllocateInfo memory_info = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      &memory_dedicated_info,
      reqs.size,
      info->select_image_memory_type(wsi, reqs.memoryTypeBits),
   };

   if (!wsi->win32.create_image_memory)
      __vk_append_struct(&memory_info, &import_memory_info);

   const VkResult result = wsi->AllocateMemory(chain->base.device, &memory_info,
                                               &chain->base.alloc, &image->memory);

   /* V6. The NT handle wsi_dxgi_create_d3d12_resource made with CreateSharedHandle is OURS.
    * ref/Vulkan-Docs/chapters/memory.adoc:2466-2472: importing memory through
    * VkImportMemoryWin32HandleInfoKHR does not transfer ownership of the handle, and the application
    * must close it when it no longer needs it. RADV deliberately keeps the payload without consuming
    * the handle (radv_device_memory.c:198-205), so nothing below this line closes it either, and the
    * import path inherited from 2026-09-26 leaked one handle per swapchain image - every swapchain
    * recreation of a resizing or mode-switching game. The CloseHandle at the end of
    * wsi_win32_surface_create_swapchain_dxgi is a different handle: the exported SEMAPHORE.
    *
    * Closed on both outcomes. The imported VkDeviceMemory keeps the resource alive through its own
    * reference, and a failed import has nothing to keep the handle for.
    */
   if (import_memory_info.handle) {
      CloseHandle(import_memory_info.handle);
      import_memory_info.handle = NULL;
   }

   return result;
}

enum wsi_swapchain_blit_type
wsi_dxgi_image_needs_blit(const struct wsi_device *wsi,
                          const struct wsi_dxgi_image_params *params,
                          VkDevice device)
{
   if (wsi->win32.requires_blits && wsi->win32.requires_blits(device))
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   else if (params->storage_image)
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   return WSI_SWAPCHAIN_NO_BLIT;
}

VkResult
wsi_dxgi_configure_image(const struct wsi_swapchain *chain,
                         const VkSwapchainCreateInfoKHR *pCreateInfo,
                         const struct wsi_dxgi_image_params *params,
                         struct wsi_image_info *info)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result =
      wsi_configure_image(chain, pCreateInfo, 0, info);
   if (result != VK_SUCCESS)
      return result;

   info->image_type = WSI_IMAGE_TYPE_DXGI;
   info->create_mem = wsi_create_dxgi_image_mem;

   if (chain->blit.type != WSI_SWAPCHAIN_NO_BLIT) {
      wsi_configure_image_blit_image(chain, info);
      info->select_image_memory_type = wsi_select_device_memory_type;
      info->select_blit_dst_memory_type = wsi_select_device_memory_type;
      if (!wsi->win32.create_image_memory)
         info->finish_create = wsi_dxgi_finish_create_image;
   }

   /* The application renders straight into the D3D12 shared resource. A driver whose D3D12 side
    * shares such a resource as a linear surface asks for a linear Vulkan image over it.
    */
   if (wsi->win32.linear_blit_image && !wsi->win32.create_image_memory)
      info->create.tiling = VK_IMAGE_TILING_LINEAR;

   return VK_SUCCESS;
}

static VkResult
wsi_win32_image_init(VkDevice device_h,
                     struct wsi_win32_swapchain *chain,
                     const VkSwapchainCreateInfoKHR *create_info,
                     const VkAllocationCallbacks *allocator,
                     struct wsi_win32_image *image)
{
   VkResult result = wsi_create_image(&chain->base, &chain->base.image_info,
                                      &image->base);
   if (result != VK_SUCCESS)
      return result;

   VkIcdSurfaceWin32 *win32_surface = (VkIcdSurfaceWin32 *)create_info->surface;
   chain->wnd = win32_surface->hwnd;
   image->chain = chain;

   if (chain->is_dxgi)
      return VK_SUCCESS;

   BITMAPINFO info = { 0 };
   info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   info.bmiHeader.biWidth = create_info->imageExtent.width;
   info.bmiHeader.biHeight = -(LONG)create_info->imageExtent.height;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;

   image->sw.bmp = CreateDIBSection(NULL, &info, DIB_RGB_COLORS,
                                    &image->sw.ppvBits, NULL, 0);
   if (!image->sw.bmp || !image->sw.ppvBits)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   BITMAP header;
   if (GetObject(image->sw.bmp, sizeof(header), &header) != sizeof(header))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   image->sw.bmp_row_pitch = header.bmWidthBytes;

   return VK_SUCCESS;
}

/* Waits until the D3D12 present queue has finished everything submitted so far: the copies of this
 * chain and of any chain that shares the queue. The caller needs this before it releases a back
 * buffer, a copy command list or the memory behind them.
 *
 * The wait has a deadline (WSI_WIN32_ROUTE_DEADLINE_NS). It was SetEventOnCompletion(1, NULL), which
 * blocks the calling thread with no deadline at all, and a queue whose own wait never completed then
 * froze the process with the GPU idle (BD-105).
 *
 * V5: what this function ANSWERS is now the point of it. Round 4 returned void, and returned early
 * on a fence it could not create and on a Signal that failed, after which every caller released the
 * back buffers, the command lists, the imported memory and the D3D12 resources, or handed the DXGI
 * chain to a new swapchain and resized it. ref/win32-docs/desktop-src/direct3d12/binding-model.md:35
 * is the rule it broke: the application must make sure the GPU has finished referencing a resource
 * before it frees it, and a route marked dead is not a statement about a queue. So:
 *
 *   DRAINED  the drain fence reached 1: everything submitted before it has retired.
 *   REMOVED  the presenter's device is proved removed, which is the other state in which the queue
 *            will not read those objects again (see the enum's comment for what that assumes).
 *   UNPROVEN anything else - no fence, a failed Signal, an expired wait - and then the caller keeps
 *            every dependent object alive.
 *
 * On expiry the route is still retired, so the next swapchain of this instance takes CPU images. The
 * drain fence of an expired wait is not released either, because the queue's Signal of it is still
 * outstanding.
 */
static enum wsi_win32_flush_result
wsi_win32_flush_d3d12_queue(struct wsi_win32_swapchain *chain)
{
   const struct wsi_device *wsi = chain->base.wsi;
   if (!wsi->win32.get_d3d12_command_queue || !wsi->win32.get_d3d12_device)
      return WSI_WIN32_FLUSH_UNPROVEN;
   ID3D12CommandQueue *queue =
      (ID3D12CommandQueue *)wsi->win32.get_d3d12_command_queue(chain->base.device);
   ID3D12Device *device = (ID3D12Device *)wsi->win32.get_d3d12_device(chain->base.device);
   ID3D12Fence *fence = NULL;
   bool queue_keeps_fence = false;
   enum wsi_win32_flush_result result = WSI_WIN32_FLUSH_UNPROVEN;

   if (!queue || !device)
      return WSI_WIN32_FLUSH_UNPROVEN;

   HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12Device::CreateFence (queue drain)", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      wsi_win32_route_log(chain, "chain %p: no drain fence (hr=0x%08lx): the queue's use of this "
                          "chain's resources is UNPROVEN", (void *)chain, (unsigned long)hr);
      return wsi_win32_presenter_read(chain, WSI_WIN32_ROUTE_ERROR_NO_IMAGE) ==
             WSI_WIN32_PRESENTER_REMOVED ? WSI_WIN32_FLUSH_REMOVED : WSI_WIN32_FLUSH_UNPROVEN;
   }

   hr = queue->Signal(fence, 1);
   if (FAILED(hr)) {
      wsi_win32_route_failed(chain, "ID3D12CommandQueue::Signal (queue drain)", hr,
                             WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      wsi_win32_route_log(chain, "chain %p: the drain Signal failed (hr=0x%08lx): the queue's use "
                          "of this chain's resources is UNPROVEN", (void *)chain,
                          (unsigned long)hr);
      result = wsi_win32_presenter_read(chain, WSI_WIN32_ROUTE_ERROR_NO_IMAGE) ==
               WSI_WIN32_PRESENTER_REMOVED ? WSI_WIN32_FLUSH_REMOVED : WSI_WIN32_FLUSH_UNPROVEN;
      fence->Release();
      return result;
   }

   const uint32_t deadline_ms = wsi_win32_wait_ms(WSI_WIN32_ROUTE_DEADLINE_NS);
   bool done = false;
   HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
   if (event) {
      hr = fence->SetEventOnCompletion(1, event);
      if (FAILED(hr))
         wsi_win32_route_failed(chain, "ID3D12Fence::SetEventOnCompletion (queue drain)", hr,
                                WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
   }
   if (event && SUCCEEDED(hr)) {
      done = WaitForSingleObject(event, deadline_ms) == WAIT_OBJECT_0;
   } else {
      /* No event to wait on: poll the fence to the same deadline instead of blocking. UINT64_MAX is
       * the removal sentinel and not a completed value, so it is not read as a drain.
       */
      const uint64_t end = os_time_get_nano() + WSI_WIN32_ROUTE_DEADLINE_NS;
      do {
         const uint64_t value = fence->GetCompletedValue();
         done = value != UINT64_MAX && value >= 1;
         if (done)
            break;
         Sleep(1);
      } while (os_time_get_nano() < end);
   }
   if (event)
      CloseHandle(event);

   if (done) {
      result = WSI_WIN32_FLUSH_DRAINED;
   } else {
      const bool dead = wsi_win32_route_wait_expired(&chain->wsi->route);
      wsi_win32_route_log(chain, "chain %p: the D3D12 queue did not drain in %u ms%s",
                          (void *)chain, deadline_ms,
                          dead ? ", route off for this process (CPU images from now on)" : "");
      result = wsi_win32_presenter_read(chain, WSI_WIN32_ROUTE_ERROR_NO_IMAGE) ==
               WSI_WIN32_PRESENTER_REMOVED ? WSI_WIN32_FLUSH_REMOVED : WSI_WIN32_FLUSH_UNPROVEN;
      /* The Signal(fence, 1) is still outstanding: the queue may write this fence after this
       * call returns, so the last reference stays with it instead of being dropped here. One
       * fence per expiry is the price of not freeing an object the GPU scheduler still names,
       * and an expiry means the route is being abandoned anyway.
       */
      queue_keeps_fence = true;
   }
   if (!queue_keeps_fence)
      fence->Release();

   wsi_win32_route_log(chain, "chain %p: queue drain %s", (void *)chain,
                       wsi_win32_flush_result_name(result));
   return result;
}

/* Releases what ties an image to the DXGI swap chain's buffers: the back buffer and the command
 * list that copies into it. The D3D12 queue must be idle.
 */
static void
wsi_win32_image_release_dxgi_buffers(struct wsi_win32_image *image)
{
   if (image->dxgi.cmd_list) {
      image->dxgi.cmd_list->Release();
      image->dxgi.cmd_list = NULL;
   }
   if (image->dxgi.cmd_alloc) {
      image->dxgi.cmd_alloc->Release();
      image->dxgi.cmd_alloc = NULL;
   }
   if (image->dxgi.swapchain_res) {
      image->dxgi.swapchain_res->Release();
      image->dxgi.swapchain_res = NULL;
   }
}

static void
wsi_win32_image_finish(struct wsi_win32_swapchain *chain,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_win32_image *image)
{
   wsi_win32_image_release_dxgi_buffers(image);

   if(image->sw.bmp)
      DeleteObject(image->sw.bmp);
   wsi_destroy_image(&chain->base, &image->base);

   /* The Vulkan memory that aliased it is gone; the D3D12 resource can go too. */
   if (image->dxgi.blit_res) {
      image->dxgi.blit_res->Release();
      image->dxgi.blit_res = NULL;
   }
}

static VkResult
wsi_win32_swapchain_destroy(struct wsi_swapchain *drv_chain,
                            const VkAllocationCallbacks *allocator)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *) drv_chain;

   /* V5. The drain decides what this teardown may touch. UNPROVEN means the presenter's queue may
    * still be reading this chain's back buffers, its copy command lists and the memory behind them,
    * and nothing below releases any of it: the chain and every reference in it are pinned and the
    * function returns. One leaked swapchain against a release of memory a live queue is reading.
    */
   const enum wsi_win32_flush_result flushed =
      chain->is_dxgi ? wsi_win32_flush_d3d12_queue(chain) : WSI_WIN32_FLUSH_DRAINED;
   if (!wsi_win32_flush_releases(flushed)) {
      chain->resources_pinned = true;
      /* The application's own waits are still released where that is proved safe: they are waits on
       * the application's own device and they do not depend on this chain's D3D12 objects.
       */
      const enum wsi_win32_presenter_state presenter =
         wsi_win32_presenter_read(chain, WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      const bool outstanding = wsi_win32_retire_blit_waits(chain, presenter);
      if (chain->surface->current_swapchain == chain)
         chain->surface->current_swapchain = NULL;
      if (chain->present_log) {
         fclose(chain->present_log);
         chain->present_log = NULL;
      }
      wsi_win32_route_log(chain, "chain %p: destroyed with the queue's use of its resources %s: the "
                          "chain, its %u images, its D3D12 resources, its shared blit fences and its "
                          "swap chain are kept (work still outstanding: %s)", (void *)chain,
                          wsi_win32_flush_result_name(flushed), chain->base.image_count,
                          outstanding ? "yes" : "no");
      return VK_SUCCESS;
   }

   /* A dead route's chain is torn down with the application's second submission per presented image
    * still waiting on a shared blit timeline the presenter will never signal. Release those waits
    * before the semaphores they wait on are destroyed, so that the teardown does not leave a thread
    * of the application blocked for the life of the process. A route that is still alive is left
    * alone: the value is the GPU's to signal, and signalling it here would call a frame presented
    * before the copy ran. What proves the host may signal at all is the presenter's state, not the
    * route's flag (V1).
    */
   if (chain->is_dxgi && !wsi_win32_route_usable(&chain->wsi->route)) {
      const enum wsi_win32_presenter_state presenter =
         wsi_win32_presenter_read(chain, WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
      if (wsi_win32_retire_blit_waits(chain, presenter))
         wsi_win32_route_log(chain, "chain %p: destroyed with a submission of this device still "
                             "waiting on a shared blit timeline: presenter %s", (void *)chain,
                             wsi_win32_presenter_state_name(presenter));
   }

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);


   if (chain->surface->current_swapchain == chain)
      chain->surface->current_swapchain = NULL;

   if (chain->dxgi)
      chain->dxgi->Release();

   if (chain->d3d12_blit_fences) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (chain->d3d12_blit_fences[i])
            chain->d3d12_blit_fences[i]->Release();
      }
      vk_free(allocator, chain->d3d12_blit_fences);
   }

   if (chain->present_log) {
      fclose(chain->present_log);
      chain->present_log = NULL;
   }

   wsi_swapchain_finish(&chain->base);

   u_cnd_monotonic_destroy(&chain->acquire_cond);
   mtx_destroy(&chain->acquire_mutex);

   vk_free(allocator, chain);
   return VK_SUCCESS;
}

static struct wsi_image *
wsi_win32_get_wsi_image(struct wsi_swapchain *drv_chain,
                        uint32_t image_index)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *) drv_chain;

   return &chain->images[image_index].base;
}

static void
wsi_win32_set_image_idle(struct wsi_win32_swapchain *chain,
                         struct wsi_win32_image *image)
{
   if (!chain->is_dxgi)
      mtx_lock(&chain->acquire_mutex);

   image->state = WSI_IMAGE_IDLE;

   if (!chain->is_dxgi) {
      u_cnd_monotonic_broadcast(&chain->acquire_cond);
      mtx_unlock(&chain->acquire_mutex);
   }
}

static VkResult
wsi_win32_release_images(struct wsi_swapchain *drv_chain,
                         uint32_t count, const uint32_t *indices)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *)drv_chain;

   if (chain->status == VK_ERROR_SURFACE_LOST_KHR)
      return chain->status;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);
      assert(chain->images[index].state == WSI_IMAGE_DRAWING);
      wsi_win32_set_image_idle(chain, &chain->images[index]);
   }

   return VK_SUCCESS;
}

static bool
wsi_win32_find_idle_image(struct wsi_win32_swapchain *chain,
                          uint32_t *out_image_index)
{
   /* CPU presentation releases images immediately. Always starting at zero
    * can starve other idle images while Zink cycles presents for readback.
    * Rotate this path under acquire_mutex; preserve DXGI's selection order. */
   const uint32_t first = chain->is_dxgi ? 0 : chain->next_cpu_image;
   for (uint32_t n = 0; n < chain->base.image_count; n++) {
      const uint32_t i = (first + n) % chain->base.image_count;
      if (chain->images[i].state == WSI_IMAGE_IDLE) {
         *out_image_index = i;
         chain->images[i].state = WSI_IMAGE_DRAWING;
         if (!chain->is_dxgi)
            chain->next_cpu_image = (i + 1) % chain->base.image_count;
         return true;
      }
   }
   return false;
}

static VkResult
wsi_win32_acquire_idle_cpu_image_locked(struct wsi_win32_swapchain *chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *out_image_index)
{
   if (chain->retired)
      return VK_ERROR_OUT_OF_DATE_KHR;
   if (chain->status != VK_SUCCESS)
      return chain->status;
   if (wsi_win32_find_idle_image(chain, out_image_index))
      return VK_SUCCESS;

   if (info->timeout == 0)
      return VK_NOT_READY;

   const uint64_t abs_timeout = os_time_get_absolute_timeout(info->timeout);
   struct timespec abs_timespec;
   timespec_from_nsec(&abs_timespec, abs_timeout);
   do {
      int ret = u_cnd_monotonic_timedwait(
         &chain->acquire_cond, &chain->acquire_mutex, &abs_timespec);
      if (chain->retired)
         return VK_ERROR_OUT_OF_DATE_KHR;
      if (chain->status != VK_SUCCESS)
         return chain->status;
      if (ret == thrd_timedout)
         return VK_TIMEOUT;
      else if (ret != thrd_success)
         return VK_ERROR_OUT_OF_DATE_KHR;
   } while (!wsi_win32_find_idle_image(chain, out_image_index));

   return VK_SUCCESS;
}

static inline VkResult
wsi_win32_acquire_idle_cpu_image(struct wsi_win32_swapchain *chain,
                                 const VkAcquireNextImageInfoKHR *info,
                                 uint32_t *out_image_index)
{
   mtx_lock(&chain->acquire_mutex);
   VkResult result = wsi_win32_acquire_idle_cpu_image_locked(chain, info,
                                                             out_image_index);
   mtx_unlock(&chain->acquire_mutex);
   return result;
}

static VkResult
wsi_win32_acquire_next_image(struct wsi_swapchain *drv_chain,
                             const VkAcquireNextImageInfoKHR *info,
                             uint32_t *image_index)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *)drv_chain;

   /* acquire timeout has to be explicitly handled for sw wsi */
   if (!chain->is_dxgi)
      return wsi_win32_acquire_idle_cpu_image(chain, info, image_index);

   /* Bail early if the swapchain is broken */
   if (chain->status != VK_SUCCESS)
      return chain->status;
   if (chain->detached)
      return VK_ERROR_OUT_OF_DATE_KHR;

   if (wsi_win32_find_idle_image(chain, image_index))
      return VK_SUCCESS;

   assert(chain->dxgi);
   uint32_t index = chain->dxgi->GetCurrentBackBufferIndex();
   if (chain->images[index].state == WSI_IMAGE_DRAWING) {
      index = (index + 1) % chain->base.image_count;
      assert(chain->images[index].state == WSI_IMAGE_QUEUED);
   }
   /* The wait is bounded while the route has never completed a present: the image becomes free when
    * the D3D12 queue has copied it and signalled the shared fence, so an application that asks for
    * UINT64_MAX would otherwise wait here for ever if that copy never runs (BD-105). On expiry the
    * route is retired for this instance and the swapchain goes out of date, which is the answer
    * that sends the application back through swapchain creation and onto CPU images.
    *
    * This is the call the lab found frozen on 2026-10-09, one frame after a present that ran to the
    * end: the flag that took its deadline away was set by Present1 returning. It is now set below,
    * after this wait has returned and the blit fence of the present before it has completed.
    */
   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_ACQUIRE);
   const bool presented = wsi_win32_route_presented(&chain->wsi->route);
   const uint64_t timeout = wsi_win32_acquire_timeout_ns(info->timeout, presented);
   if (chain->wsi->wsi->WaitForFences(chain->base.device, 1,
                                      &chain->base.fences[index],
                                      false, timeout) != VK_SUCCESS) {
      if (!wsi_win32_acquire_timeout_capped(info->timeout, presented))
         return VK_TIMEOUT;
      const bool dead = wsi_win32_route_wait_expired(&chain->wsi->route);
      wsi_win32_route_log(chain, "chain %p: image %u was still busy after %" PRIu64 " ms and the "
                          "route has never completed a present%s", (void *)chain, index,
                          timeout / 1000000ull,
                          dead ? ", route off for this process (CPU images from now on)" : "");
      /* The one measurement BD-105 still needs, written where the freeze happens instead of asking
       * for a debugger on the lab. want is the value the presenter's Signal of the copy should have
       * reached on this image's shared blit fence (one past the application's own signal), have is
       * where the fence really is, and the three cases decide between the candidates:
       *
       *   have <  want - 1   the application's own signal never reached the GPU: our submission
       *                      path, not the presenter (and the kernel driver's log says whether a
       *                      packet of that context is outstanding at all).
       *   have == want - 1   the application signalled and the presenter's Wait was satisfied, but
       *                      the Signal after ExecuteCommandLists did not retire: the copy or the
       *                      D3D12 queue behind it is stuck. This is the mutual-wait shape.
       *   have >= want       the copy completed and the fence is done, yet the Vulkan fence of the
       *                      image did not signal: the wait on our side is the defect.
       */
      if (chain->d3d12_blit_fences && chain->base.blit.timeline_values) {
         ID3D12Fence *fence = chain->d3d12_blit_fences[index];
         wsi_win32_route_log(chain, "chain %p: image %u blit fence have %" PRIu64 " want %" PRIu64
                             ", queued present image %u value %" PRIu64, (void *)chain, index,
                             fence ? fence->GetCompletedValue() : 0,
                             chain->base.blit.timeline_values[index],
                             chain->pending_present_image, chain->pending_present_value);
      }
      /* have == UINT64_MAX above is the documented answer of GetCompletedValue for a REMOVED device
       * and not a fence value, which is the fourth case of the round-3 decision table and the one
       * the lab read. This reading says which of the three presenter states holds, and a timeout on
       * its own says none of them (V1).
       */
      const enum wsi_win32_presenter_state presenter = wsi_win32_presenter_read(chain, index);
      /* The route is retired; now retire the work it queued, as far as the presenter's state proves
       * the host may, or the application freezes in its own recovery instead
       * (wsi_win32_retire_blit_waits). What cannot be proved is reported, not forced.
       */
      const bool outstanding = wsi_win32_retire_blit_waits(chain, presenter);
      if (wsi_win32_route_report(outstanding) == WSI_WIN32_REPORT_DEVICE_LOST) {
         wsi_win32_route_log(chain, "chain %p: VK_ERROR_DEVICE_LOST: a submission of this device "
                             "waits on a shared blit timeline that only a presenter this round "
                             "could not prove removed (%s) can signal", (void *)chain,
                             wsi_win32_presenter_state_name(presenter));
         chain->status = VK_ERROR_DEVICE_LOST;
         return VK_ERROR_DEVICE_LOST;
      }
      return VK_ERROR_OUT_OF_DATE_KHR;
   }

   /* The wait returned. If the copy of THIS image also completed - its shared blit fence has reached
    * the value wsi_dxgi_blit signalled for it after ExecuteCommandLists - then the route has shown
    * one full present-acquire cycle and its deadlines are lifted, once, with a line that says what
    * proved it. Until then every acquire of this instance keeps the bounded deadline.
    *
    * The value to compare against is base.blit.timeline_values[index] and not the one the chain's
    * last present recorded. An image cannot be presented again before it is acquired, so at this
    * point timeline_values[index] is exactly the value the last blit of this image signalled, and it
    * is 0 for an image that was never presented. The chain's last present belongs to the OTHER image
    * of a two-image swapchain, so a route proved against it would stay one frame behind for ever and
    * might never lift the deadlines on a route that works.
    */
   if (!presented && chain->d3d12_blit_fences && chain->base.blit.timeline_values) {
      const uint64_t want = chain->base.blit.timeline_values[index];
      ID3D12Fence *fence = chain->d3d12_blit_fences[index];
      const uint64_t done = fence ? fence->GetCompletedValue() : 0;
      if (wsi_win32_route_note_acquired(&chain->wsi->route, want, done))
         wsi_win32_route_log(chain, "chain %p: route presented: blit fence of image %u reached %"
                             PRIu64 " (wanted %" PRIu64 ") and the acquire of that image returned; "
                             "deadlines off", (void *)chain, index, done, want);
   }

   *image_index = index;
   chain->images[index].state = WSI_IMAGE_DRAWING;
   return VK_SUCCESS;
}

static VkResult
wsi_win32_queue_present_dxgi(struct wsi_win32_swapchain *chain,
                             uint32_t image_index,
                             struct wsi_win32_image *image,
                             const VkPresentRegionKHR *damage)
{
   /* FLIP_DISCARD takes no dirty rectangles, and the copy writes the whole buffer: damage is
    * accepted and not passed on.
    */
   (void)damage;
   DXGI_PRESENT_PARAMETERS params = {};

   if (chain->detached) {
      /* A newer chain owns the window's swap chain (oldSwapchain); this image cannot be shown. */
      wsi_win32_set_image_idle(chain, image);
      return VK_ERROR_OUT_OF_DATE_KHR;
   }

   image->state = WSI_IMAGE_QUEUED;
   /* FIFO: one present per vertical blank. MAILBOX: interval 0, and the newest frame replaces a
    * queued one at composition or flip. IMMEDIATE: interval 0 and tearing when the system allows it
    * (the swap chain was then created with ALLOW_TEARING); without it, IMMEDIATE acts as MAILBOX.
    */
   UINT sync_interval = chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR ? 1 : 0;
   UINT present_flags = 0;
   if (chain->base.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR &&
       (chain->swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
      present_flags |= DXGI_PRESENT_ALLOW_TEARING;

   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_PRESENT);
   HRESULT hres = chain->dxgi->Present1(sync_interval, present_flags, &params);
   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_DONE);
   if (FAILED(hres))
      wsi_win32_route_failed(chain, "IDXGISwapChain3::Present1", hres, image_index);
   switch (hres) {
   case DXGI_ERROR_DEVICE_REMOVED: return VK_ERROR_DEVICE_LOST;
   case DXGI_ERROR_DEVICE_RESET: return VK_ERROR_DEVICE_LOST;
   case E_OUTOFMEMORY: return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   default:
      if (FAILED(hres)) {
         wsi_win32_route_log(chain, "Present1 failed hr=0x%08lx interval=%u flags=0x%x",
                             (unsigned long)hres, sync_interval, present_flags);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      break;
   }

   if (!chain->hwnd_target && chain->surface->current_swapchain != chain) {
      chain->surface->visual->SetContent(chain->dxgi);
      chain->wsi->dxgi.dcomp->Commit();
      chain->surface->current_swapchain = chain;
   }

   /* Present1 took the frame. That is a QUEUED present and nothing more: neither the GPU copy this
    * route asked for before it nor the flip need have run, so the route is not proved here and the
    * deadlines stay on (BD-105, the lab arm of 2026-10-09, which froze one frame after this point).
    * What is recorded is the fence value that will say this image's copy completed, for the
    * expired-acquire line to print. The proof of the route is read from the acquired image's own
    * timeline value, not from this pair.
    */
   chain->pending_present_image = image_index;
   chain->pending_present_value = chain->base.blit.timeline_values ?
      chain->base.blit.timeline_values[image_index] : 0;

   /* The common completion path publishes status under acquire_mutex. */
   return VK_SUCCESS;
}

/* A DXGI present is queued, not complete: record its id for wait_for_present. */
static VkResult
wsi_win32_submit_present(struct wsi_win32_swapchain *chain,
                         uint64_t present_id, VkResult result)
{
   mtx_lock(&chain->acquire_mutex);
   chain->status = result;
   if (result == VK_SUCCESS && present_id > chain->submitted_present_id)
      chain->submitted_present_id = present_id;
   u_cnd_monotonic_broadcast(&chain->acquire_cond);
   mtx_unlock(&chain->acquire_mutex);
   return result;
}

/* DwmFlush is the completion boundary for this composed Win32 path, not
 * GPU submission or the return from BitBlt/Present1. Publish only afterwards.
 */
static VkResult
wsi_win32_complete_present(struct wsi_win32_swapchain *chain,
                           uint64_t present_id, VkResult result)
{
   mtx_lock(&chain->acquire_mutex);
   chain->status = result;
   if (result == VK_SUCCESS && present_id > chain->completed_present_id)
      chain->completed_present_id = present_id;
   u_cnd_monotonic_broadcast(&chain->acquire_cond);
   mtx_unlock(&chain->acquire_mutex);
   return result;
}

static VkResult
wsi_win32_wait_for_present(struct wsi_swapchain *base,
                           uint64_t present_id, uint64_t timeout)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)base;
   const uint64_t deadline = os_time_get_absolute_timeout(timeout);
   struct timespec abs_time;
   timespec_from_nsec(&abs_time, deadline);

   /* Completion must also consume the application's present wait semaphores.
    * Share one absolute deadline with the presentation-engine wait below.
    */
   VkResult result = wsi_swapchain_wait_for_present_semaphore(base, present_id, timeout);
   if (result != VK_SUCCESS)
      return result;

   mtx_lock(&chain->acquire_mutex);
   while (chain->completed_present_id < present_id) {
      if (chain->status != VK_SUCCESS) {
         result = chain->status;
         break;
      }
      if (chain->retired) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         break;
      }
      if (!timeout) {
         result = VK_TIMEOUT;
         break;
      }
      /* DXGI: the present is queued. Wait here, not in queue_present, for the next composition
       * (DwmFlush), so that MAILBOX and IMMEDIATE never block in vkQueuePresentKHR. In
       * independent flip DwmFlush still returns once per vertical blank, so this stays a bound of
       * one refresh, not a measure of the flip. Present ids are monotonic: this completes them all.
       */
      if (chain->is_dxgi && chain->submitted_present_id >= present_id) {
         const uint64_t submitted = chain->submitted_present_id;
         mtx_unlock(&chain->acquire_mutex);
         const HRESULT hr = DwmFlush();
         mtx_lock(&chain->acquire_mutex);
         (void)hr; /* DWM off (a failure) leaves nothing to wait for */
         if (submitted > chain->completed_present_id)
            chain->completed_present_id = submitted;
         u_cnd_monotonic_broadcast(&chain->acquire_cond);
         continue;
      }
      int ret = u_cnd_monotonic_timedwait(&chain->acquire_cond,
                                         &chain->acquire_mutex, &abs_time);
      if (ret == thrd_timedout) {
         result = chain->completed_present_id >= present_id ? VK_SUCCESS : VK_TIMEOUT;
         break;
      }
      if (ret != thrd_success) {
         result = VK_ERROR_DEVICE_LOST;
         break;
      }
   }
   mtx_unlock(&chain->acquire_mutex);
   return result;
}

static VkResult
wsi_win32_queue_present(struct wsi_swapchain *drv_chain,
                        uint32_t image_index,
                        uint64_t present_id,
                        const VkPresentRegionKHR *damage)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *) drv_chain;
   assert(image_index < chain->base.image_count);
   struct wsi_win32_image *image = &chain->images[image_index];

   assert(image->state == WSI_IMAGE_DRAWING);

   const uint64_t t_enter = chain->present_log ? os_time_get_nano() : 0;

   if (chain->is_dxgi) {
      VkResult result = wsi_win32_queue_present_dxgi(chain, image_index, image, damage);
      const uint64_t t_blit = chain->present_log ? os_time_get_nano() : 0;
      /* copy_us is 0 on this path: the copy runs on the GPU (the D3D12 queue). dwmflush_us is 0
       * as well: completion waits in wait_for_present.
       */
      if (chain->present_log)
         wsi_win32_present_log(chain, chain->hwnd_target ? "dxgi" : "dxgi-composition",
                               present_id, t_enter, t_enter, t_blit, t_blit, result);
      return wsi_win32_submit_present(chain, present_id, result);
   }

   RECT rect;
   VkResult result = VK_SUCCESS;
   if (!GetClientRect(chain->wnd, &rect))
      result = VK_ERROR_SURFACE_LOST_KHR;
   else if ((uint32_t)(rect.right - rect.left) != chain->extent.width ||
            (uint32_t)(rect.bottom - rect.top) != chain->extent.height)
      result = VK_ERROR_OUT_OF_DATE_KHR;
   if (result != VK_SUCCESS) {
      wsi_win32_set_image_idle(chain, image);
      return wsi_win32_complete_present(chain, present_id, result);
   }

   const uint8_t *ptr = (const uint8_t *)image->base.cpu_map;
   uint8_t *dptr = (uint8_t *)image->sw.ppvBits;
   for (unsigned h = 0; h < chain->extent.height; h++) {
      if (chain->format == VK_FORMAT_R8G8B8A8_UNORM) {
         for (unsigned x = 0; x < chain->extent.width; x++) {
            dptr[x * 4 + 0] = ptr[x * 4 + 2];
            dptr[x * 4 + 1] = ptr[x * 4 + 1];
            dptr[x * 4 + 2] = ptr[x * 4 + 0];
            dptr[x * 4 + 3] = ptr[x * 4 + 3];
         }
      } else {
         memcpy(dptr, ptr, chain->extent.width * 4);
      }
      dptr += image->sw.bmp_row_pitch;
      ptr += image->base.row_pitches[0];
   }

   const uint64_t t_copy = chain->present_log ? os_time_get_nano() : 0;
   HDC window_dc = GetDC(chain->wnd);
   HDC memory_dc = window_dc ? CreateCompatibleDC(window_dc) : NULL;
   HGDIOBJ previous = memory_dc ? SelectObject(memory_dc, image->sw.bmp) : NULL;
   if (!window_dc)
      result = VK_ERROR_SURFACE_LOST_KHR;
   else if (!memory_dc || !previous || previous == HGDI_ERROR)
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
   else if (!BitBlt(window_dc, 0, 0, chain->extent.width, chain->extent.height,
                    memory_dc, 0, 0, SRCCOPY) || !GdiFlush())
      result = VK_ERROR_SURFACE_LOST_KHR;
   if (previous && previous != HGDI_ERROR)
      SelectObject(memory_dc, previous);
   if (memory_dc)
      DeleteDC(memory_dc);
   if (window_dc)
      ReleaseDC(chain->wnd, window_dc);
   const uint64_t t_blit = chain->present_log ? os_time_get_nano() : 0;
   /* IMMEDIATE: the BitBlt (with GdiFlush) has already copied the frame into
    * the window's redirection surface, so the swapchain image is free; do not
    * wait for DWM's next composition. The log's dwmflush_us then stays near 0.
    */
   if (result == VK_SUCCESS &&
       chain->base.present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR &&
       FAILED(DwmFlush()))
      result = VK_ERROR_SURFACE_LOST_KHR;
   if (chain->present_log)
      wsi_win32_present_log(chain, "gdi", present_id, t_enter, t_copy, t_blit,
                            os_time_get_nano(), result);

   wsi_win32_set_image_idle(chain, image);
   return wsi_win32_complete_present(chain, present_id, result);
}

static void
wsi_win32_set_present_mode(struct wsi_swapchain *drv_chain, VkPresentModeKHR mode)
{
   /* Every DXGI chain is created with ALLOW_TEARING when the system has it, so a switch between
    * FIFO, MAILBOX and IMMEDIATE only changes the arguments of the next Present1.
    */
   drv_chain->present_mode = mode;
}

static void
wsi_win32_set_hdr_metadata(struct wsi_swapchain *drv_chain, const VkHdrMetadataEXT *metadata)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   if (!chain->is_dxgi || chain->detached || !chain->dxgi || !metadata)
      return;

   IDXGISwapChain4 *swapchain4 = NULL;
   if (FAILED(chain->dxgi->QueryInterface(IID_PPV_ARGS(&swapchain4))))
      return;

   /* DXGI_HDR_METADATA_HDR10: chromaticities in units of 0.00002, luminance in nits (max) and in
    * units of 0.0001 nits (min), content and frame-average light levels in nits.
    */
   DXGI_HDR_METADATA_HDR10 hdr10 = {};
   hdr10.RedPrimary[0] = (UINT16)(metadata->displayPrimaryRed.x * 50000.0f);
   hdr10.RedPrimary[1] = (UINT16)(metadata->displayPrimaryRed.y * 50000.0f);
   hdr10.GreenPrimary[0] = (UINT16)(metadata->displayPrimaryGreen.x * 50000.0f);
   hdr10.GreenPrimary[1] = (UINT16)(metadata->displayPrimaryGreen.y * 50000.0f);
   hdr10.BluePrimary[0] = (UINT16)(metadata->displayPrimaryBlue.x * 50000.0f);
   hdr10.BluePrimary[1] = (UINT16)(metadata->displayPrimaryBlue.y * 50000.0f);
   hdr10.WhitePoint[0] = (UINT16)(metadata->whitePoint.x * 50000.0f);
   hdr10.WhitePoint[1] = (UINT16)(metadata->whitePoint.y * 50000.0f);
   hdr10.MaxMasteringLuminance = (UINT)metadata->maxLuminance;
   hdr10.MinMasteringLuminance = (UINT)(metadata->minLuminance * 10000.0f);
   hdr10.MaxContentLightLevel = (UINT16)metadata->maxContentLightLevel;
   hdr10.MaxFrameAverageLightLevel = (UINT16)metadata->maxFrameAverageLightLevel;

   HRESULT hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr10), &hdr10);
   if (FAILED(hr))
      wsi_win32_route_log(chain, "SetHDRMetaData failed hr=0x%08lx", (unsigned long)hr);
   swapchain4->Release();
}

/* Takes the window's DXGI swap chain from the chain this one replaces. Only one flip-model swap
 * chain may belong to a window, so a new swap chain for the same window can only be made after
 * the old one is gone. The old chain is detached: its back buffers and their copy command lists
 * are released (after the D3D12 queue is idle), it keeps its Vulkan images, fences and D3D12
 * resources until the application destroys it, and its presents return VK_ERROR_OUT_OF_DATE_KHR.
 */
static IDXGISwapChain3 *
wsi_win32_take_old_dxgi(struct wsi_win32_swapchain *old_chain, HWND hwnd)
{
   if (!old_chain || !old_chain->is_dxgi || !old_chain->hwnd_target || old_chain->detached ||
       !old_chain->dxgi || old_chain->wnd != hwnd)
      return NULL;

   /* V5. The steal releases the old chain's back buffers and copy command lists and then RESIZES the
    * swap chain under them. Both are forbidden while the presenter's queue cannot be proved to have
    * stopped referencing them, and a route that was retired is not that proof. The old chain keeps
    * everything, nothing is handed over, and the caller's chain fails to take the DXGI route: the
    * swapchain the application asked for then takes CPU images, which is the fallback this route has
    * for every other failure too.
    */
   const enum wsi_win32_flush_result flushed = wsi_win32_flush_d3d12_queue(old_chain);
   if (!wsi_win32_flush_releases(flushed)) {
      old_chain->resources_pinned = true;
      wsi_win32_route_wait_expired(&old_chain->wsi->route);
      wsi_win32_route_log(old_chain, "chain %p: its swap chain is NOT handed to the new chain: the "
                          "queue's use of its buffers is %s, so they are neither released nor "
                          "resized", (void *)old_chain, wsi_win32_flush_result_name(flushed));
      return NULL;
   }

   for (uint32_t i = 0; i < old_chain->base.image_count; i++)
      wsi_win32_image_release_dxgi_buffers(&old_chain->images[i]);

   mtx_lock(&old_chain->acquire_mutex);
   IDXGISwapChain3 *dxgi = old_chain->dxgi;
   old_chain->dxgi = NULL;
   old_chain->detached = true;
   u_cnd_monotonic_broadcast(&old_chain->acquire_cond);
   mtx_unlock(&old_chain->acquire_mutex);
   return dxgi;
}

static VkResult
wsi_win32_surface_create_swapchain_dxgi(
   wsi_win32_surface *surface,
   VkDevice device,
   struct wsi_win32 *wsi,
   const VkSwapchainCreateInfoKHR *create_info,
   struct wsi_win32_swapchain *chain,
   struct wsi_win32_swapchain *old_chain)
{
   IDXGIFactory4 *factory = wsi->dxgi.factory;
   ID3D12CommandQueue *queue =
      (ID3D12CommandQueue *)wsi->wsi->win32.get_d3d12_command_queue(device);
   const struct wsi_win32_format *format = wsi_win32_find_format(create_info->imageFormat);
   HWND hwnd = surface->base.hwnd;
   HRESULT hr;

   if (!factory || !queue || !format)
      return VK_ERROR_INITIALIZATION_FAILED;

   chain->buffer_format = format->buffer;
   chain->color_space = wsi_win32_dxgi_color_space(create_info->imageColorSpace);

   DXGI_ALPHA_MODE alpha_mode;
   switch (create_info->compositeAlpha) {
   case VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR:
      alpha_mode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      break;
   case VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR:
      alpha_mode = DXGI_ALPHA_MODE_STRAIGHT;
      break;
   default:
      alpha_mode = DXGI_ALPHA_MODE_IGNORE;
      break;
   }
   if (chain->hwnd_target)
      alpha_mode = DXGI_ALPHA_MODE_IGNORE;

   /* ALLOW_TEARING goes on every chain the system allows it on, whatever the present mode: the
    * mode can change per present (VK_KHR_swapchain_maintenance1), and a chain that takes over the
    * window's swap chain must keep its flags for ResizeBuffers.
    */
   UINT flags = 0;
   IDXGIFactory5 *factory5 = NULL;
   if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
      BOOL tearing = FALSE;
      if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                  &tearing, sizeof(tearing))) && tearing)
         flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      factory5->Release();
   }
   chain->swap_chain_flags = flags;

   /* The buffers receive a GPU copy and are shown; RENDER_TARGET_OUTPUT is the usage the native
    * D3D12 games create theirs with, and the one the scan-out path of our D3D12 shell sees.
    */
   DXGI_SWAP_CHAIN_DESC1 desc = {};
   desc.Width = create_info->imageExtent.width;
   desc.Height = create_info->imageExtent.height;
   desc.Format = chain->buffer_format;
   desc.Stereo = FALSE;
   desc.SampleDesc.Count = 1;
   desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   /* A driver that renders straight into the buffers (win32.create_image_memory) needs them
    * readable by shaders when the application samples the swapchain images.
    */
   if (vk_swapchain_usage_flags(create_info) &
       (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      desc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
   desc.BufferCount = create_info->minImageCount;
   desc.Scaling = DXGI_SCALING_STRETCH;
   desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   desc.AlphaMode = alpha_mode;
   desc.Flags = flags;

   bool reused = false;
   IDXGISwapChain3 *stolen = chain->hwnd_target ? wsi_win32_take_old_dxgi(old_chain, hwnd) : NULL;
   /* The old chain kept its swap chain because its queue could not be proved idle (V5). The window
    * already has a flip-model swap chain, so creating another one for it cannot succeed; say so here
    * instead of letting CreateSwapChainForHwnd answer DXGI_ERROR_INVALID_CALL. The caller takes CPU
    * images.
    */
   if (!stolen && chain->hwnd_target && old_chain && old_chain->resources_pinned &&
       old_chain->wnd == hwnd) {
      wsi_win32_route_log(chain, "chain %p: no DXGI route: the window's swap chain is pinned to "
                          "chain %p, whose presenter queue could not be proved idle",
                          (void *)chain, (void *)old_chain);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (stolen) {
      DXGI_SWAP_CHAIN_DESC1 old_desc = {};
      if (SUCCEEDED(stolen->GetDesc1(&old_desc)) && old_desc.Flags == desc.Flags &&
          old_desc.BufferCount == desc.BufferCount && old_desc.AlphaMode == desc.AlphaMode &&
          old_desc.BufferUsage == desc.BufferUsage && old_desc.SwapEffect == desc.SwapEffect &&
          SUCCEEDED(hr = stolen->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height,
                                               desc.Format, desc.Flags))) {
         chain->dxgi = stolen;
         reused = true;
      } else {
         /* Releasing the last reference frees the window for a new swap chain. */
         stolen->Release();
      }
   }

   if (!chain->dxgi) {
      IDXGISwapChain1 *swapchain1 = NULL;
      if (chain->hwnd_target)
         hr = factory->CreateSwapChainForHwnd(queue, hwnd, &desc, NULL, NULL, &swapchain1);
      else
         hr = factory->CreateSwapChainForComposition(queue, &desc, NULL, &swapchain1);
      if (FAILED(hr)) {
         wsi_win32_route_failed(chain, chain->hwnd_target ?
                                "IDXGIFactory2::CreateSwapChainForHwnd" :
                                "IDXGIFactory2::CreateSwapChainForComposition", hr,
                                WSI_WIN32_ROUTE_ERROR_NO_IMAGE);
         wsi_win32_route_log(chain, "%s failed hr=0x%08lx %ux%u format %u flags 0x%x",
                             chain->hwnd_target ? "CreateSwapChainForHwnd" :
                                                  "CreateSwapChainForComposition",
                             (unsigned long)hr, desc.Width, desc.Height, (unsigned)desc.Format,
                             desc.Flags);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      hr = swapchain1->QueryInterface(IID_PPV_ARGS(&chain->dxgi));
      swapchain1->Release();
      if (FAILED(hr))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (chain->hwnd_target) {
      /* Vulkan owns the window mode: DXGI must not answer Alt+Enter or watch the message queue. */
      factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
   }

   /* The color space is set on every chain, also to sRGB, so that a chain that takes over the
    * swap chain of an HDR chain does not keep its tag.
    */
   UINT support = 0;
   if (SUCCEEDED(chain->dxgi->CheckColorSpaceSupport(chain->color_space, &support)) &&
       (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      hr = chain->dxgi->SetColorSpace1(chain->color_space);
      if (FAILED(hr))
         wsi_win32_route_log(chain, "SetColorSpace1(%u) failed hr=0x%08lx",
                             (unsigned)chain->color_space, (unsigned long)hr);
   } else if (chain->color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      wsi_win32_route_log(chain, "color space %u not supported for presentation",
                          (unsigned)chain->color_space);
   }

   if (!chain->hwnd_target) {
      if (!surface->target &&
          FAILED(wsi->dxgi.dcomp->CreateTargetForHwnd(hwnd, false, &surface->target)))
         return VK_ERROR_INITIALIZATION_FAILED;

      if (!surface->visual) {
         if (FAILED(wsi->dxgi.dcomp->CreateVisual(&surface->visual)) ||
             FAILED(surface->target->SetRoot(surface->visual)) ||
             FAILED(surface->visual->SetContent(chain->dxgi)) ||
             FAILED(wsi->dxgi.dcomp->Commit()))
            return VK_ERROR_INITIALIZATION_FAILED;

         surface->current_swapchain = chain;
      }
   }

   ID3D12Device *d3d12_device = (ID3D12Device *)wsi->wsi->win32.get_d3d12_device(device);
   if (!d3d12_device)
      return VK_ERROR_INITIALIZATION_FAILED;

   chain->d3d12_blit_fences = (ID3D12Fence**)vk_zalloc(&chain->base.alloc,
         create_info->minImageCount * sizeof(ID3D12Fence *),
         8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chain->d3d12_blit_fences)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   for (unsigned i = 0; i < create_info->minImageCount; i++) {
      const VkSemaphoreTypeCreateInfo type_info = {
         VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
         NULL,
         VK_SEMAPHORE_TYPE_TIMELINE,
      };
      const VkExportSemaphoreCreateInfo export_info = {
         VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
         &type_info,
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
      };
      const VkSemaphoreCreateInfo sem_info = {
         VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         &export_info,
         0,
      };

      VkResult result = wsi->wsi->CreateSemaphore(device, &sem_info, &chain->base.alloc,
                                                  &chain->base.blit.semaphores[i]);
      if (result != VK_SUCCESS)
         return result;

      HANDLE handle = nullptr;
      const VkSemaphoreGetWin32HandleInfoKHR get_info = {
         VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
         NULL,
         chain->base.blit.semaphores[i],
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
      };
      result = wsi->wsi->GetSemaphoreWin32HandleKHR(device, &get_info, &handle);
      if (result != VK_SUCCESS || !handle) {
         wsi_win32_route_log(chain, "blit fence %u: no D3D12 fence handle (%d)", i, (int)result);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      hr = d3d12_device->OpenSharedHandle(handle,
                                          IID_PPV_ARGS(&chain->d3d12_blit_fences[i]));
      /* This handle is the EXPORTED SEMAPHORE's, and closing it here is right: ownership of a
       * semaphore handle obtained from vkGetSemaphoreWin32HandleKHR is the application's. The
       * resource handle of the image import is a different one and is closed where it is made
       * (wsi_create_dxgi_image_mem, V6).
       */
      CloseHandle(handle);
      if (FAILED(hr)) {
         wsi_win32_route_failed(chain, "ID3D12Device::OpenSharedHandle (blit fence)", hr, i);
         wsi_win32_route_log(chain, "blit fence %u: OpenSharedHandle failed hr=0x%08lx", i,
                             (unsigned long)hr);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   }

   wsi_win32_route_log(chain, "chain %p: %s swap chain %ux%u format %u images %u flags 0x%x "
                       "color space %u%s", (void *)chain,
                       chain->hwnd_target ? "window" : "composition", desc.Width, desc.Height,
                       (unsigned)desc.Format, desc.BufferCount, desc.Flags,
                       (unsigned)chain->color_space, reused ? " (taken over from oldSwapchain)" : "");
   return VK_SUCCESS;
}

/* Creates one chain on the route given. On failure the chain is gone and *swapchain_out is unset. */
static VkResult
wsi_win32_create_chain(wsi_win32_surface *surface,
                       VkDevice device,
                       struct wsi_device *wsi_device,
                       const VkSwapchainCreateInfoKHR *create_info,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_win32_swapchain *old_chain,
                       bool use_dxgi,
                       const char *route_reason,
                       struct wsi_swapchain **swapchain_out)
{
   struct wsi_win32 *wsi =
      (struct wsi_win32 *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];

   const unsigned num_images = create_info->minImageCount;
   struct wsi_win32_swapchain *chain;
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);

   chain = (wsi_win32_swapchain *)vk_zalloc(allocator, size,
                     8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   int ret = mtx_init(&chain->acquire_mutex, mtx_plain);
   if (ret != thrd_success) {
      vk_free(allocator, chain);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   ret = u_cnd_monotonic_init(&chain->acquire_cond);
   if (ret != thrd_success) {
      mtx_destroy(&chain->acquire_mutex);
      vk_free(allocator, chain);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   const VkImageUsageFlags2KHR image_usage = vk_swapchain_usage_flags(create_info);

   struct wsi_dxgi_image_params dxgi_image_params = {
      { WSI_IMAGE_TYPE_DXGI },
   };
   dxgi_image_params.storage_image = (image_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;

   struct wsi_cpu_image_params cpu_image_params = {
      { WSI_IMAGE_TYPE_CPU },
   };

   struct wsi_base_image_params *image_params = use_dxgi ?
      &dxgi_image_params.base : &cpu_image_params.base;

   /* Set before wsi_swapchain_init: the image configuration reads them through the chain. */
   chain->is_dxgi = use_dxgi;
   chain->hwnd_target = use_dxgi && wsi_device->win32.hwnd_target;
   chain->route_reason = route_reason;
   chain->wsi = wsi;
   chain->surface = surface;

   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, device, create_info,
                                        num_images, image_params, allocator);
   if (result != VK_SUCCESS) {
      u_cnd_monotonic_destroy(&chain->acquire_cond);
      mtx_destroy(&chain->acquire_mutex);
      vk_free(allocator, chain);
      return result;
   }

   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, create_info);

   chain->present_log = NULL;
   chain->present_log_lines = 0;
   chain->pending_present_image = 0;
   chain->pending_present_value = 0;
   chain->stage_log_bits = 0;
   const char *present_log_path = getenv("BC250_WSI_PRESENT_LOG");
   if (present_log_path && present_log_path[0]) {
      chain->present_log = fopen(present_log_path, "a");
      if (chain->present_log) {
         fprintf(chain->present_log,
                 "# chain %p %ux%u format %u images %u path %s mode %u target %s reason %s\n"
                 "chain,path,present_id,enter_us,copy_us,blit_us,dwmflush_us,result\n",
                 (void *)chain, create_info->imageExtent.width,
                 create_info->imageExtent.height, (unsigned)create_info->imageFormat,
                 num_images, use_dxgi ? "dxgi" : "gdi",
                 (unsigned)chain->base.present_mode,
                 !use_dxgi ? "gdi" : chain->hwnd_target ? "window" : "composition",
                 route_reason ? route_reason : "-");
         fflush(chain->present_log);
      }
   }

   chain->base.destroy = wsi_win32_swapchain_destroy;
   chain->base.get_wsi_image = wsi_win32_get_wsi_image;
   chain->base.acquire_next_image = wsi_win32_acquire_next_image;
   chain->base.release_images = wsi_win32_release_images;
   chain->base.queue_present = wsi_win32_queue_present;
   chain->base.wait_for_present = wsi_win32_wait_for_present;
   chain->base.wait_for_present2 = wsi_win32_wait_for_present;
   chain->base.set_present_mode = wsi_win32_set_present_mode;
   if (use_dxgi)
      chain->base.set_hdr_metadata = wsi_win32_set_hdr_metadata;
   chain->extent = create_info->imageExtent;
   chain->format = create_info->imageFormat;
   chain->status = VK_SUCCESS;
   chain->wnd = surface->base.hwnd;

   if (use_dxgi) {
      result = wsi_win32_surface_create_swapchain_dxgi(surface, device, wsi, create_info, chain,
                                                       old_chain);
      if (result != VK_SUCCESS)
         goto fail;
   }

   for (uint32_t image = 0; image < num_images; image++) {
      result = wsi_win32_image_init(device, chain,
                                    create_info, allocator,
                                    &chain->images[image]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* The images and their D3D12 blit contexts exist. Before this line the chain was only the swap
    * chain; after it the next route line comes from the first present (BD-105: the evidence of the
    * freeze ended at the swap chain line, which left both halves open).
    */
   wsi_win32_stage_log(chain, WSI_WIN32_STAGE_IMAGES);

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail:
   if (!chain->hwnd_target && surface->visual && surface->current_swapchain == chain) {
      surface->visual->SetContent(NULL);
      surface->current_swapchain = NULL;
      wsi->dxgi.dcomp->Commit();
   }
   wsi_win32_swapchain_destroy(&chain->base, allocator);
   return result;
}

static VkResult
wsi_win32_surface_create_swapchain(
   VkIcdSurfaceBase *icd_surface,
   VkDevice device,
   struct wsi_device *wsi_device,
   const VkSwapchainCreateInfoKHR *create_info,
   const VkAllocationCallbacks *allocator,
   struct wsi_swapchain **swapchain_out)
{
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   struct wsi_win32 *wsi =
      (struct wsi_win32 *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];

   assert(create_info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   struct wsi_win32_swapchain *old_chain = NULL;
   if (create_info->oldSwapchain) {
      old_chain = (struct wsi_win32_swapchain *)create_info->oldSwapchain;
      mtx_lock(&old_chain->acquire_mutex);
      old_chain->retired = true;
      u_cnd_monotonic_broadcast(&old_chain->acquire_cond);
      mtx_unlock(&old_chain->acquire_mutex);
   }

   /* The route of this chain. The device offers DXGI when it gave the hooks; the driver may still
    * refuse it for this chain (route_allowed), and a DXGI chain that cannot be made falls back to
    * CPU images, unless its format has no CPU path.
    */
   const struct wsi_win32_format *format = wsi_win32_find_format(create_info->imageFormat);
   const bool dxgi_only = format && format->dxgi_only;
   const char *reason = "device";
   bool use_dxgi = wsi->dxgi.factory && wsi_win32_device_uses_dxgi(wsi_device) &&
                   (wsi_device->win32.hwnd_target || wsi->dxgi.dcomp);
   if (!wsi_win32_device_uses_dxgi(wsi_device))
      reason = "gdi-device";
   else if (!wsi->dxgi.factory)
      reason = "no-dxgi-factory";
   else if (!use_dxgi)
      reason = "no-dcomp";
   if (use_dxgi && wsi_device->win32.route_allowed &&
       !wsi_device->win32.route_allowed(wsi_device->pdevice, &reason))
      use_dxgi = false;
   /* A wait of the route expired before it had ever presented: this instance does not try the DXGI
    * route again (wsi_win32_deadline.h, BD-105).
    */
   if (use_dxgi && !wsi_win32_route_usable(&wsi->route)) {
      use_dxgi = false;
      reason = "route-deadline";
   }
   if (use_dxgi && !format) {
      use_dxgi = false;
      reason = "format";
   }

   VkResult result = VK_ERROR_INITIALIZATION_FAILED;
   if (use_dxgi) {
      result = wsi_win32_create_chain(surface, device, wsi_device, create_info, allocator,
                                      old_chain, true, "ok", swapchain_out);
      if (result == VK_SUCCESS)
         return result;
      if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_DEVICE_LOST)
         return result;
      reason = "dxgi-create-failed";
   }

   if (wsi_win32_device_uses_dxgi(wsi_device) && wsi_device->win32.route_log) {
      char line[160];
      snprintf(line, sizeof(line), "swapchain %ux%u format %u: CPU images (GDI), reason %s%s",
               create_info->imageExtent.width, create_info->imageExtent.height,
               (unsigned)create_info->imageFormat, reason,
               dxgi_only ? ", format has no CPU path: refused" : "");
      wsi_device->win32.route_log(device, line);
   }
   if (dxgi_only || !format)
      return use_dxgi ? result : VK_ERROR_INITIALIZATION_FAILED;

   return wsi_win32_create_chain(surface, device, wsi_device, create_info, allocator,
                                 old_chain, false, reason, swapchain_out);
}

static IDXGIFactory4 *
dxgi_get_factory(bool debug, const char **failure, HRESULT *failure_hr)
{
   HMODULE dxgi_mod = util_load_system_library(L"DXGI.DLL");
   *failure = "dxgi-load";
   if (!dxgi_mod) {
      *failure_hr = HRESULT_FROM_WIN32(GetLastError());
      return NULL;
   }

   typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT flags, REFIID riid, void **ppFactory);
   PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2;

   CreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(dxgi_mod, "CreateDXGIFactory2");
   if (!CreateDXGIFactory2) {
      *failure_hr = HRESULT_FROM_WIN32(GetLastError());
      return NULL;
   }

   UINT flags = 0;
   if (debug)
      flags |= DXGI_CREATE_FACTORY_DEBUG;

   IDXGIFactory4 *factory;
   HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));
   if (FAILED(hr)) {
      *failure = "dxgi-factory";
      *failure_hr = hr;
      return NULL;
   }

   return factory;
}

static IDCompositionDevice *
dcomp_get_device(const char **failure, HRESULT *failure_hr)
{
   HMODULE dcomp_mod = util_load_system_library(L"DComp.DLL");
   *failure = "dcomp-load";
   if (!dcomp_mod) {
      *failure_hr = HRESULT_FROM_WIN32(GetLastError());
      return NULL;
   }

   typedef HRESULT (STDAPICALLTYPE *PFN_DCOMP_CREATE_DEVICE)(IDXGIDevice *, REFIID, void **);
   PFN_DCOMP_CREATE_DEVICE DCompositionCreateDevice;

   DCompositionCreateDevice = (PFN_DCOMP_CREATE_DEVICE)GetProcAddress(dcomp_mod, "DCompositionCreateDevice");
   if (!DCompositionCreateDevice) {
      *failure_hr = HRESULT_FROM_WIN32(GetLastError());
      return NULL;
   }

   IDCompositionDevice *device;
   HRESULT hr = DCompositionCreateDevice(NULL, IID_PPV_ARGS(&device));
   if (FAILED(hr)) {
      *failure = "dcomp-device";
      *failure_hr = hr;
      return NULL;
   }

   return device;
}

VkResult
wsi_win32_init_wsi(struct wsi_device *wsi_device,
                   const VkAllocationCallbacks *alloc,
                   VkPhysicalDevice physical_device)
{
   struct wsi_win32 *wsi;
   VkResult result;

   wsi = (wsi_win32 *)vk_zalloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   if (!wsi_device->sw) {
      const char *failure = NULL;
      HRESULT failure_hr = S_OK;
      wsi->dxgi.factory = dxgi_get_factory(WSI_DEBUG & WSI_DEBUG_DXGI, &failure, &failure_hr);
      if (!wsi->dxgi.factory) {
         wsi_device->win32.init_failure = failure;
         wsi_device->win32.init_hr = (long)failure_hr;
         wsi_device->sw = true;
         goto sw_fallback;
      }
      wsi->dxgi.dcomp = dcomp_get_device(&failure, &failure_hr);
      if (!wsi->dxgi.dcomp) {
         wsi_device->win32.init_failure = failure;
         wsi_device->win32.init_hr = (long)failure_hr;
         wsi->dxgi.factory->Release();
         wsi->dxgi.factory = NULL;
         wsi_device->sw = true;
      }

      if (!wsi->wsi->win32.create_image_memory)
         wsi_device->blit = wsi_dxgi_blit;
   }

sw_fallback:
   wsi->base.get_support = wsi_win32_surface_get_support;
   wsi->base.get_capabilities2 = wsi_win32_surface_get_capabilities2;
   wsi->base.get_formats = wsi_win32_surface_get_formats;
   wsi->base.get_formats2 = wsi_win32_surface_get_formats2;
   wsi->base.get_present_modes = wsi_win32_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_win32_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_win32_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32] = &wsi->base;

   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32] = NULL;

   return result;
}

void
wsi_win32_finish_wsi(struct wsi_device *wsi_device,
                  const VkAllocationCallbacks *alloc)
{
   struct wsi_win32 *wsi =
      (struct wsi_win32 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];
   if (!wsi)
      return;

   if (wsi->dxgi.factory)
      wsi->dxgi.factory->Release();
   if (wsi->dxgi.dcomp)
      wsi->dxgi.dcomp->Release();

   vk_free(alloc, wsi);
}
