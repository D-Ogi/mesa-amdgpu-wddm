/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Resource.cpp --
 *    Functions that manipulate GPU resources.
 */


#include "Resource.h"
#include "Format.h"
#include "State.h"
#include "Query.h"

#include "Debug.h"

#include "util/u_math.h"
#include "util/u_rect.h"
#include "util/u_surface.h"
#include "frontend/winsys_handle.h"

#include "util/os_time.h"

class Bc250DdiMapAudit {
   bool enabled;
   unsigned long long id;
public:
   Bc250DdiMapAudit(const char *origin, struct pipe_context *pipe,
                   struct pipe_resource *resource, unsigned level,
                   unsigned usage, const struct pipe_box &box) {
      static const bool active = bc250_diag_enabled && debug_get_bool_option("BC250_HOST_AUDIT", false);
      enabled = active;
      id = 0;
      if (!enabled)
         return;
      alignas(8) static LONG64 sequence = 0;
      id = (unsigned long long)InterlockedIncrement64(&sequence);
      BC250_DIAG("BC250 audit ddi event=begin id=%llu tid=%lu time_ns=%llu origin=%s ctx=%p resource=%p level=%u usage=%x x=%d y=%d z=%d box_width=%d box_height=%d box_depth=%d\n",
              id, GetCurrentThreadId(), (unsigned long long)os_time_get_nano(),
              origin, (void *)pipe, (void *)resource, level, usage,
              box.x, box.y, box.z, box.width, box.height, box.depth);
   }
   void Copied() {
      if (enabled)
         BC250_DIAG("BC250 audit ddi event=copy_complete id=%llu tid=%lu time_ns=%llu\n",
                 id, GetCurrentThreadId(), (unsigned long long)os_time_get_nano());
   }
   ~Bc250DdiMapAudit() {
      if (enabled)
         BC250_DIAG("BC250 audit ddi event=end id=%llu tid=%lu time_ns=%llu\n",
                 id, GetCurrentThreadId(), (unsigned long long)os_time_get_nano());
   }
};


// Byte-identical copy of bc250-win driver/contract/amdgpu_wddm_surface_format.h.
#include "amdgpu_wddm_surface_format.h"

static_assert(AMDGPU_WDDM_D3DDDI_A8R8G8B8 == D3DDDIFMT_A8R8G8B8 &&
              AMDGPU_WDDM_D3DDDI_X8R8G8B8 == D3DDDIFMT_X8R8G8B8 &&
              AMDGPU_WDDM_D3DDDI_A2B10G10R10 == D3DDDIFMT_A2B10G10R10 &&
              AMDGPU_WDDM_D3DDDI_A8B8G8R8 == D3DDDIFMT_A8B8G8R8 &&
              AMDGPU_WDDM_D3DDDI_A16B16G16R16F == D3DDDIFMT_A16B16G16R16F,
              "surface format table: D3DDDIFORMAT values");
static_assert(AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM == DXGI_FORMAT_R10G10B10A2_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM == DXGI_FORMAT_R8G8B8A8_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM == DXGI_FORMAT_B8G8R8A8_UNORM &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              "surface format table: DXGI_FORMAT values");


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateResourceSize --
 *
 *    The CalcPrivateResourceSize function determines the size of
 *    the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the
 *    size of the resource video memory).
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,                                // IN
                        __in const D3D10DDIARG_CREATERESOURCE *pCreateResource)  // IN
{
   LOG_ENTRYPOINT();
   return sizeof(Resource);
}


static pipe_resource_usage
translate_resource_usage(UINT usage)
{
   pipe_resource_usage resource_usage = PIPE_USAGE_DEFAULT;

   switch (usage) {
   case D3D10_DDI_USAGE_DEFAULT:
      resource_usage = PIPE_USAGE_DEFAULT;
      break;
   case D3D10_DDI_USAGE_IMMUTABLE:
      resource_usage = PIPE_USAGE_IMMUTABLE;
      break;
   case D3D10_DDI_USAGE_DYNAMIC:
      resource_usage = PIPE_USAGE_DYNAMIC;
      break;
   case D3D10_DDI_USAGE_STAGING:
      resource_usage = PIPE_USAGE_STAGING;
      break;
   default:
      assert(0);
      break;
   }

   return resource_usage;
}


static unsigned
translate_resource_flags(UINT flags)
{
   unsigned bind = 0;

   if (flags & D3D10_DDI_BIND_VERTEX_BUFFER)
      bind |= PIPE_BIND_VERTEX_BUFFER;

   if (flags & D3D10_DDI_BIND_INDEX_BUFFER)
      bind |= PIPE_BIND_INDEX_BUFFER;

   if (flags & D3D10_DDI_BIND_CONSTANT_BUFFER)
      bind |= PIPE_BIND_CONSTANT_BUFFER;

   if (flags & D3D10_DDI_BIND_SHADER_RESOURCE)
      bind |= PIPE_BIND_SAMPLER_VIEW;

   if (flags & D3D10_DDI_BIND_RENDER_TARGET)
      bind |= PIPE_BIND_RENDER_TARGET;

   if (flags & D3D10_DDI_BIND_DEPTH_STENCIL)
      bind |= PIPE_BIND_DEPTH_STENCIL;

   if (flags & D3D10_DDI_BIND_STREAM_OUTPUT)
      bind |= PIPE_BIND_STREAM_OUTPUT;

   if (flags & D3D10_DDI_BIND_PRESENT)
      bind |= PIPE_BIND_DISPLAY_TARGET;

   return bind;
}


static enum pipe_texture_target
translate_texture_target( D3D10DDIRESOURCE_TYPE ResourceDimension,
                             UINT ArraySize)
{
   assert(ArraySize >= 1);
   switch(ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      assert(ArraySize == 1);
      return PIPE_BUFFER;
   case D3D10DDIRESOURCE_TEXTURE1D:
      return ArraySize > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
   case D3D10DDIRESOURCE_TEXTURE2D:
      return ArraySize > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
   case D3D10DDIRESOURCE_TEXTURE3D:
      assert(ArraySize == 1);
      return PIPE_TEXTURE_3D;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      assert(ArraySize % 6 == 0);
      return ArraySize > 6 ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
   default:
      assert(0);
      return PIPE_TEXTURE_1D;
   }
}


static void
subResourceBox(struct pipe_resource *resource, // IN
                 UINT SubResource,  // IN
                 unsigned *pLevel, // OUT
                 struct pipe_box *pBox)   // OUT
{
   UINT MipLevels = resource->last_level + 1;
   unsigned layer;
   unsigned width;
   unsigned height;
   unsigned depth;

   *pLevel = SubResource % MipLevels;
   layer = SubResource / MipLevels;

   width  = u_minify(resource->width0,  *pLevel);
   height = u_minify(resource->height0, *pLevel);
   depth  = u_minify(resource->depth0,  *pLevel);

   pBox->x = 0;
   pBox->y = 0;
   pBox->z = 0 + layer;
   pBox->width  = width;
   pBox->height = height;
   pBox->depth  = depth;
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateResource --
 *
 *    The CreateResource function creates a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateResource(D3D10DDI_HDEVICE hDevice,                                // IN
               __in const D3D10DDIARG_CREATERESOURCE *pCreateResource,  // IN
               D3D10DDI_HRESOURCE hResource,                            // IN
               D3D10DDI_HRTRESOURCE hRTResource)                        // IN
{
   LOG_ENTRYPOINT();

   if ((pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       (pCreateResource->pPrimaryDesc &&
        pCreateResource->pPrimaryDesc->DriverFlags & DXGI_DDI_PRIMARY_OPTIONAL)) {

      DebugPrintf("%s(%dx%dx%d hResource=%p)\n",
	       __func__,
	       pCreateResource->pMipInfoList[0].TexelWidth,
	       pCreateResource->pMipInfoList[0].TexelHeight,
	       pCreateResource->pMipInfoList[0].TexelDepth,
	       hResource.pDrvPrivate);
      DebugPrintf("  ResourceDimension = %u\n",
	       pCreateResource->ResourceDimension);
      DebugPrintf("  Usage = %u\n",
	       pCreateResource->Usage);
      DebugPrintf("  BindFlags = 0x%x\n",
	       pCreateResource->BindFlags);
      DebugPrintf("  MapFlags = 0x%x\n",
	       pCreateResource->MapFlags);
      DebugPrintf("  MiscFlags = 0x%x\n",
	       pCreateResource->MiscFlags);
      DebugPrintf("  Format = %s\n",
	       FormatToName(pCreateResource->Format));
      DebugPrintf("  SampleDesc.Count = %u\n", pCreateResource->SampleDesc.Count);
      DebugPrintf("  SampleDesc.Quality = %u\n", pCreateResource->SampleDesc.Quality);
      DebugPrintf("  MipLevels = %u\n", pCreateResource->MipLevels);
      DebugPrintf("  ArraySize = %u\n", pCreateResource->ArraySize);
      DebugPrintf("  pPrimaryDesc = %p\n", pCreateResource->pPrimaryDesc);
      if (pCreateResource->pPrimaryDesc) {
	 DebugPrintf("    Flags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->Flags);
	 DebugPrintf("    VidPnSourceId = %u\n", pCreateResource->pPrimaryDesc->VidPnSourceId);
	 DebugPrintf("    ModeDesc.Width = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Width);
	 DebugPrintf("    ModeDesc.Height = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Height);
	 DebugPrintf("    ModeDesc.Format = %u)\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Format);
	 DebugPrintf("    ModeDesc.RefreshRate.Numerator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Numerator);
	 DebugPrintf("    ModeDesc.RefreshRate.Denominator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Denominator);
	 DebugPrintf("    ModeDesc.ScanlineOrdering = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.ScanlineOrdering);
	 DebugPrintf("    ModeDesc.Rotation = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Rotation);
	 DebugPrintf("    ModeDesc.Scaling = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Scaling);
	 DebugPrintf("    DriverFlags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->DriverFlags);
      }

   }

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   Resource *pResource = CastResource(hResource);

   memset(pResource, 0, sizeof *pResource);
   pResource->hRTResource = (HANDLE)hRTResource.handle;
   pResource->primary = pCreateResource->pPrimaryDesc != NULL;
   pResource->shared = (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0;
   pResource->vidpn = pCreateResource->pPrimaryDesc ? pCreateResource->pPrimaryDesc->VidPnSourceId : 0;

#if 0
   if (pCreateResource->pPrimaryDesc) {
      pCreateResource->pPrimaryDesc->DriverFlags = DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT;
      if (!(pCreateResource->pPrimaryDesc->DriverFlags & DXGI_DDI_PRIMARY_OPTIONAL)) {
         // http://msdn.microsoft.com/en-us/library/windows/hardware/ff568846.aspx
         SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
         return;
      }
   }
#endif

   pResource->Format = pCreateResource->Format;
   pResource->MipLevels = pCreateResource->MipLevels;

   struct pipe_resource templat;

   memset(&templat, 0, sizeof templat);

   templat.target     = translate_texture_target( pCreateResource->ResourceDimension,
                                                  pCreateResource->ArraySize );
   pResource->buffer = templat.target == PIPE_BUFFER;

   if (pCreateResource->Format == DXGI_FORMAT_UNKNOWN) {
      assert(pCreateResource->ResourceDimension == D3D10DDIRESOURCE_BUFFER);
      templat.format = PIPE_FORMAT_R8_UINT;
   } else {
      BOOL bindDepthStencil = !!(pCreateResource->BindFlags & D3D10_DDI_BIND_DEPTH_STENCIL);
      templat.format = FormatTranslate(pCreateResource->Format, bindDepthStencil);
   }

   templat.width0     = pCreateResource->pMipInfoList[0].TexelWidth;
   templat.height0    = pCreateResource->pMipInfoList[0].TexelHeight;
   templat.depth0     = pCreateResource->pMipInfoList[0].TexelDepth;
   templat.array_size = pCreateResource->ArraySize;
   templat.last_level = pCreateResource->MipLevels - 1;
   templat.nr_samples = pCreateResource->SampleDesc.Count;
   templat.nr_storage_samples = pCreateResource->SampleDesc.Count;
   templat.bind       = translate_resource_flags(pCreateResource->BindFlags);
   templat.usage      = translate_resource_usage(pCreateResource->Usage);

   if (templat.target != PIPE_BUFFER) {
      if (!screen->is_format_supported(screen,
                                       templat.format,
                                       templat.target,
                                       templat.nr_samples,
                                       templat.nr_storage_samples,
                                       templat.bind)) {
         debug_printf("%s: unsupported format %s\n",
                     __func__, util_format_name(templat.format));
         // Not an allocation failure: E_OUTOFMEMORY would end DWM (BD-058,
         // see OpenResource).
         SetError(hDevice, D3DDDIERR_APPLICATIONERROR);
         return;
      }
   }

   // E34 diagnostic import only: the app owns this NT handle. This bypasses
   // runtime allocation creation and cannot establish the DWM sharing contract.
   char probeHandle[32] = {};
   DWORD probeLength = GetEnvironmentVariableA("BC250_D3D_IMPORT_PROBE_HANDLE", probeHandle, sizeof(probeHandle));
   if (probeLength && probeLength < sizeof(probeHandle) && (templat.bind & PIPE_BIND_RENDER_TARGET)) {
      struct winsys_handle handle = {};
      handle.type = WINSYS_HANDLE_TYPE_WIN32_HANDLE;
      handle.handle = (HANDLE)(uintptr_t)_strtoui64(probeHandle, NULL, 16);
      handle.stride = templat.width0 * 4;
      handle.size = UINT64(handle.stride) * templat.height0;
      templat.bind |= PIPE_BIND_LINEAR;
      if (!screen->memobj_create_from_handle || !screen->resource_from_memobj) {
         SetError(hDevice, E_NOTIMPL);
         return;
      }
      struct pipe_memory_object *memory = screen->memobj_create_from_handle(screen, &handle, true);
      if (!memory) { SetError(hDevice, E_OUTOFMEMORY); return; }
      pResource->resource = screen->resource_from_memobj(screen, &templat, memory, 0);
      screen->memobj_destroy(screen, memory);
      BC250_DIAG("BC250 native shared probe import %s\n", pResource->resource ? "ok" : "failed");
   } else {
      pResource->resource = screen->resource_create(screen, &templat);
   }
   if (!pResource->resource) {
      DebugPrintf("%s: failed to create resource\n", __func__);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }

   pResource->NumSubResources = pCreateResource->MipLevels * pCreateResource->ArraySize;
   pResource->transfers = (struct pipe_transfer **)calloc(pResource->NumSubResources,
                                                          sizeof *pResource->transfers);

   if (GetEnvironmentVariableA("BC250_D3D_RUNTIME_INVENTORY", NULL, 0))
      BC250_DIAG("BC250 runtime resource misc=%x bind=%x primary=%u\n",
              pCreateResource->MiscFlags, pCreateResource->BindFlags, pResource->primary);
   if (GetEnvironmentVariableA("BC250_D3D_RUNTIME_INVENTORY", NULL, 0) ||
       pResource->primary || (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT) ||
       (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED)) {
      HRESULT hr = Bc250EnsureSurface(CastDevice(hDevice), pResource);
      if (FAILED(hr)) { SetError(hDevice, hr); return; }
   }

   if (pCreateResource->pInitialDataUP) {
      if (pResource->buffer) {
         assert(pResource->NumSubResources == 1);
         const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
               &pCreateResource->pInitialDataUP[0];

         unsigned level;
         struct pipe_box box;
         subResourceBox(pResource->resource, 0, &level, &box);

         Bc250DdiMapAudit audit("initial_data", pipe, pResource->resource, level,
                                PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED, box);
         struct pipe_transfer *transfer;
         void *map;
         map = pipe->buffer_map(pipe,
                                pResource->resource,
                                level,
                                PIPE_MAP_WRITE |
                                PIPE_MAP_UNSYNCHRONIZED,
                                &box,
                                &transfer);
         assert(map);
         if (map) {
            memcpy(map, pInitialDataUP->pSysMem, box.width);
            audit.Copied();
            pipe_buffer_unmap(pipe, transfer);
         }
      } else {
         for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {
            const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
                  &pCreateResource->pInitialDataUP[SubResource];

            unsigned level;
            struct pipe_box box;
            subResourceBox(pResource->resource, SubResource, &level, &box);

            Bc250DdiMapAudit audit("initial_data", pipe, pResource->resource, level,
                                   PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED, box);
            struct pipe_transfer *transfer;
            void *map;
            map = pipe->texture_map(pipe,
                                    pResource->resource,
                                    level,
                                    PIPE_MAP_WRITE |
                                    PIPE_MAP_UNSYNCHRONIZED,
                                    &box,
                                    &transfer);
            assert(map);
            if (map) {
               for (int z = 0; z < box.depth; ++z) {
                  uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
                  const uint8_t *src = (const uint8_t*)pInitialDataUP->pSysMem + z*pInitialDataUP->SysMemSlicePitch;
                  util_copy_rect(dst,
                                 templat.format,
                                 transfer->stride,
                                 0, 0, box.width, box.height,
                                 src,
                                 pInitialDataUP->SysMemPitch,
                                 0, 0);
               }
               audit.Copied();
               pipe_texture_unmap(pipe, transfer);
            }
         }
      }
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateOpenedResourceSize --
 *
 *    The CalcPrivateOpenedResourceSize function determines the size
 *    of the user-mode display driver's private shared region of memory
 *    (that is, the size of internal driver structures, not the size
 *    of the resource video memory) for an opened resource.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE hDevice,                             // IN
                              __in const D3D10DDIARG_OPENRESOURCE *pOpenResource)   // IN
{
   return sizeof(Resource);
}


struct OpenedSurface { UINT magic, version, width, height, pitch, format; UINT64 size; };   // LB7A v1

/*
 * A refused or failed open is reported as D3DDDIERR_APPLICATIONERROR, which
 * the runtime hands to the caller as DXGI_ERROR_INVALID_CALL. The other
 * codes end the compositor: E_OUTOFMEMORY is on dwmcore's memory exhaustion
 * list (fail-fast 0xC00001AD, BD-058), and any code outside the runtime's
 * short list (E_NOTIMPL, E_INVALIDARG) becomes
 * DXGI_ERROR_DRIVER_INTERNAL_ERROR, which removes the device. E_OUTOFMEMORY
 * and D3DDDIERR_DEVICEREMOVED pass through only from a kernel callback.
 *
 * The runtime never calls DestroyResource for a handle whose open failed
 * (pfnOpenResource remarks): release here what CreateResource made. The GPU
 * VA and the residency reference belong to the opened allocation, which the
 * runtime closes.
 */
static void
RefuseOpen(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, D3DKMT_HANDLE allocation,
           const OpenedSurface *data, UINT bpp, HRESULT hr, const char *why)
{
   if (hr != E_OUTOFMEMORY && hr != D3DDDIERR_DEVICEREMOVED)
      hr = D3DDDIERR_APPLICATIONERROR;
   const OpenedSurface none = {};
   if (!data) data = &none;
   BC250_ERROR("BC250 OpenResource refused allocation=%x %ux%u pitch=%u (importable %u) size=%llu format=%u: %s, "
               "%08lx\n", allocation, data->width, data->height, data->pitch, (data->width * bpp + 255u) & ~255u,
               (unsigned long long)data->size, data->format, why, hr);
   Resource *resource = CastResource(hResource);
   free(resource->transfers);
   resource->transfers = NULL;
   pipe_resource_reference(&resource->resource, NULL);
   resource->allocation = 0;
   resource->gpuVa = 0;
   resource->presentReady = FALSE;
   SetError(hDevice, hr);
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenResource --
 *
 *    The OpenResource function opens a shared resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
OpenResource(D3D10DDI_HDEVICE hDevice,                            // IN
             __in const D3D10DDIARG_OPENRESOURCE *pOpenResource,  // IN
             D3D10DDI_HRESOURCE hResource,                        // IN
             D3D10DDI_HRTRESOURCE hRTResource)                    // IN
{
   LOG_ENTRYPOINT();
   memset(CastResource(hResource), 0, sizeof(Resource));
   if (pOpenResource->NumAllocations != 1 || !pOpenResource->pOpenAllocationInfo2) {
      RefuseOpen(hDevice, hResource, 0, NULL, 4, E_NOTIMPL, "not one allocation"); return;
   }
   const D3DDDI_OPENALLOCATIONINFO2 *info = pOpenResource->pOpenAllocationInfo2;
   if (!info->pPrivateDriverData || info->PrivateDriverDataSize < sizeof(OpenedSurface)) {
      RefuseOpen(hDevice, hResource, info->hAllocation, NULL, 4, E_INVALIDARG, "no LB7A private data"); return;
   }
   OpenedSurface data;
   memcpy(&data, info->pPrivateDriverData, sizeof(data));
   // The compositor opens and samples the formats the shared table marks
   // COMPOSED. X8R8G8B8 is a kernel/GDI row there; this UMD has always
   // opened it as B8G8R8X8. A refused format keeps the 4-byte geometry check,
   // so its error codes are the ones it had before the table.
   const AMDGPU_WDDM_SURFACE_FORMAT *row = amdgpu_wddm_surface_admit(
      amdgpu_wddm_surface_format_by_d3dddi(data.format), AMDGPU_WDDM_SURFACE_COMPOSED);
   const DXGI_FORMAT format = row ? (DXGI_FORMAT)row->dxgi :
      data.format == D3DDDIFMT_X8R8G8B8 ? DXGI_FORMAT_B8G8R8X8_UNORM : DXGI_FORMAT_UNKNOWN;
   const UINT bpp = row ? row->bytes_per_pixel : 4;
   if (data.magic != 0x4137424c || data.version != 1 || !data.width || !data.height ||
       data.width > 8192 || data.height > 8192 || (data.pitch & 15u) ||
       data.pitch < ((data.width + 3u) & ~3u) * bpp ||
       data.size < UINT64(data.pitch) * ((data.height + 3u) & ~3u)) {
      RefuseOpen(hDevice, hResource, info->hAllocation, &data, bpp, E_INVALIDARG, "LB7A geometry"); return;
   }
   if (format == DXGI_FORMAT_UNKNOWN) {
      RefuseOpen(hDevice, hResource, info->hAllocation, &data, bpp, E_NOTIMPL, "format not composed"); return;
   }
   // The hosted branch imports the allocation as a linear image with the LB7A
   // pitch as its row pitch. RADV on GFX10 takes only its own linear pitch,
   // the row rounded up to 256 bytes (ac_surface_override_offset_stride),
   // which is also the pitch of every allocation this UMD makes
   // (Bc250EnsureSurface). Refuse any other pitch before anything is created.
   if (CastDevice(hDevice)->hosted_state && data.pitch != ((data.width * bpp + 255u) & ~255u)) {
      RefuseOpen(hDevice, hResource, info->hAllocation, &data, bpp, E_INVALIDARG, "pitch not importable"); return;
   }
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = mip.PhysicalWidth = data.width;
   mip.TexelHeight = mip.PhysicalHeight = data.height;
   mip.TexelDepth = mip.PhysicalDepth = 1;
   D3D10DDIARG_CREATERESOURCE create = {};
   create.pMipInfoList = &mip; create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   create.Usage = D3D10_DDI_USAGE_DEFAULT;
   create.BindFlags = D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET;
   create.Format = format; create.SampleDesc.Count = 1;
   create.MipLevels = 1; create.ArraySize = 1;
   CreateResource(hDevice, &create, hResource, hRTResource);
   Resource *resource = CastResource(hResource);
   if (!resource->resource) {
      // CreateResource has set the error.
      BC250_ERROR("BC250 OpenResource allocation=%x %ux%u format=%u: CreateResource failed\n", info->hAllocation,
                  data.width, data.height, data.format);
      free(resource->transfers);
      resource->transfers = NULL;
      return;
   }
   resource->allocation = info->hAllocation;
   resource->surfacePitch = data.pitch;
   resource->surfaceBytes = data.size;
   HRESULT hr = Bc250EnsureSurface(CastDevice(hDevice), resource);
   DebugPrintf("BC250 OpenResource %08lx handle %x\n", hr, resource->allocation);
   if (FAILED(hr))
      RefuseOpen(hDevice, hResource, info->hAllocation, &data, bpp, hr, "surface setup failed");
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyResource --
 *
 *    The DestroyResource function destroys the specified resource
 *    object. The resource object can be destoyed only if it is not
 *    currently bound to a display device, and if all views that
 *    refer to the resource are also destroyed.
 *
 * ----------------------------------------------------------------------
 */


extern "C" int d3d10_release_hosted_resource(struct pipe_context *, struct pipe_resource **);

void APIENTRY
DestroyResource(D3D10DDI_HDEVICE hDevice,       // IN
                D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);

   if (pResource->so_target) {
      pipe_so_target_reference(&pResource->so_target, NULL);
   }

   for (UINT SubResource = 0; pResource->transfers && SubResource < pResource->NumSubResources; ++SubResource) {
      if (pResource->transfers[SubResource]) {
         if (pResource->buffer) {
            pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
         } else {
            pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
         }
         pResource->transfers[SubResource] = NULL;
      }
   }
   Device *device = CastDevice(hDevice);
   if (pResource->allocation && device->hosted_state) {
      HRESULT idle=Bc250WaitPresentIdle(device);
      if (FAILED(idle) || !d3d10_release_hosted_resource(pipe,&pResource->resource)) {
         Bc250StopHostedSubmission(device);
         SetError(hDevice,FAILED(idle) ? idle : D3DDDIERR_DEVICEREMOVED);
      }
   }
   if (pResource->allocation) {
      if (pResource->cpuMapping) {
         D3DDDICB_UNLOCK2 unlock = {};
         unlock.hAllocation = pResource->allocation;
         device->KTCallbacks.pfnUnlock2Cb(device->hDevice, &unlock);
      }
      D3DDDICB_DEALLOCATE2 free = {};
      // Shared resources must be closed atomically by their runtime resource
      // handle (D3DDDICB_DEALLOCATE2 / pfnDeallocateCb). Closing only the
      // allocation leaves the resource and its device bindings alive.
      if (pResource->hRTResource) {
         free.hResource = pResource->hRTResource;
      } else {
         free.NumAllocations = 1; free.HandleList = &pResource->allocation;
      }
      HRESULT hr = device->KTCallbacks.pfnDeallocate2Cb(device->hDevice, &free);
      BC250_REPORT(FAILED(hr), "BC250 Deallocate %08lx flags=%u\n", hr, free.Flags.Value);
      if (FAILED(hr)) {
         Bc250StopHostedSubmission(device);
         SetError(hDevice,hr);
      }
      pResource->allocation=0;
      pResource->gpuVa=0;
      pResource->cpuMapping=NULL;
      pResource->surfaceResident=FALSE;
      pResource->surfaceFence=0;
   }
   free(pResource->transfers);
   pResource->transfers=NULL;

   if (pResource->buffer && pResource->resource) {
      struct pipe_resource *released = pResource->resource;
      pResource->resource = NULL;
      pipe_resource_release(pipe, released);
   } else {
      pipe_resource_reference(&pResource->resource, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceMap --
 *
 *    The ResourceMap function maps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceMap(D3D10DDI_HDEVICE hDevice,                                // IN
            D3D10DDI_HRESOURCE hResource,                            // IN
            UINT SubResource,                                        // IN
            D3D10_DDI_MAP DDIMap,                                    // IN
            UINT Flags,                                              // IN
            __out D3D10DDI_MAPPED_SUBRESOURCE *pMappedSubResource)   // OUT
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);
   struct pipe_resource *resource = pResource->resource;

   unsigned usage;
   switch (DDIMap) {
   case D3D10_DDI_MAP_READ:
      usage = PIPE_MAP_READ;
      break;
   case D3D10_DDI_MAP_READWRITE:
      usage = PIPE_MAP_READ | PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE:
      usage = PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE_DISCARD:
      usage = PIPE_MAP_WRITE;
      if (resource->last_level == 0 && resource->array_size == 1) {
         usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
      } else {
         usage |= PIPE_MAP_DISCARD_RANGE;
      }
      break;
   case D3D10_DDI_MAP_WRITE_NOOVERWRITE:
      usage = PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED;
      break;
   default:
      assert(0);
      return;
   }

   assert(SubResource < pResource->NumSubResources);

   unsigned level;
   struct pipe_box box;
   subResourceBox(resource, SubResource, &level, &box);

   assert(!pResource->transfers[SubResource]);
   Bc250DdiMapAudit audit("resource_map", pipe, resource, level, usage, box);

   void *map;
   if (pResource->buffer) {
      map = pipe->buffer_map(pipe,
                             resource,
                             level,
                             usage,
                             &box,
                             &pResource->transfers[SubResource]);
   } else {
      map = pipe->texture_map(pipe,
                              resource,
                              level,
                              usage,
                              &box,
                              &pResource->transfers[SubResource]);
   }
   if (!map) {
      DebugPrintf("%s: failed to map resource\n", __func__);
      SetError(hDevice, E_FAIL);
      return;
   }

   pMappedSubResource->pData = map;
   pMappedSubResource->RowPitch = pResource->transfers[SubResource]->stride;
   pMappedSubResource->DepthPitch = pResource->transfers[SubResource]->layer_stride;
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUnmap --
 *
 *    The ResourceUnmap function unmaps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUnmap(D3D10DDI_HDEVICE hDevice,      // IN
              D3D10DDI_HRESOURCE hResource,  // IN
              UINT SubResource)              // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);

   assert(SubResource < pResource->NumSubResources);

   if (pResource->transfers[SubResource]) {
      if (pResource->buffer) {
         pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
      } else {
         pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
      }
      pResource->transfers[SubResource] = NULL;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * areResourcesCompatible --
 *
 *      Check whether two resources can be safely passed to
 *      pipe_context::resource_copy_region method.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
areResourcesCompatible(const struct pipe_resource *src_resource, // IN
                       const struct pipe_resource *dst_resource) // IN
{
   if (src_resource->format == dst_resource->format) {
      /*
       * Trivial.
       */

      return true;
   } else if (src_resource->target == PIPE_BUFFER &&
              dst_resource->target == PIPE_BUFFER) {
      /*
       * Buffer resources are merely a collection of bytes.
       */

      return true;
   } else {
      /*
       * Check whether the formats are supported by
       * the resource_copy_region method.
       */

      const struct util_format_description *src_format_desc;
      const struct util_format_description *dst_format_desc;

      src_format_desc = util_format_description(src_resource->format);
      dst_format_desc = util_format_description(dst_resource->format);

      assert(src_format_desc->block.width  == dst_format_desc->block.width);
      assert(src_format_desc->block.height == dst_format_desc->block.height);
      assert(src_format_desc->block.bits   == dst_format_desc->block.bits);

      return util_is_format_compatible(src_format_desc, dst_format_desc);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopy --
 *
 *    The ResourceCopy function copies an entire source
 *    resource to a destination resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopy(D3D10DDI_HDEVICE hDevice,          // IN
             D3D10DDI_HRESOURCE hDstResource,   // IN
             D3D10DDI_HRESOURCE hSrcResource)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;
   bool compatible;

   assert(dst_resource->target == src_resource->target);
   assert(dst_resource->width0 == src_resource->width0);
   assert(dst_resource->height0 == src_resource->height0);
   assert(dst_resource->depth0 == src_resource->depth0);
   assert(dst_resource->last_level == src_resource->last_level);
   assert(dst_resource->array_size == src_resource->array_size);

   compatible = areResourcesCompatible(src_resource, dst_resource);

   /* could also use one 3d copy for arrays */
   for (unsigned layer = 0; layer < dst_resource->array_size; ++layer) {
      for (unsigned level = 0; level <= dst_resource->last_level; ++level) {
         struct pipe_box box;
         box.x = 0;
         box.y = 0;
         box.z = 0 + layer;
         box.width  = u_minify(dst_resource->width0,  level);
         box.height = u_minify(dst_resource->height0, level);
         box.depth  = u_minify(dst_resource->depth0,  level);

	 if (compatible) {
            pipe->resource_copy_region(pipe,
                                       dst_resource, level,
                                       0, 0, layer,
                                       src_resource, level,
                                       &box);
         } else {
            util_resource_copy_region(pipe,
                                      dst_resource, level,
                                      0, 0, layer,
                                      src_resource, level,
                                      &box);
         }
      }
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopyRegion --
 *
 *    The ResourceCopyRegion function copies a source subresource
 *    region to a location on a destination subresource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopyRegion(D3D10DDI_HDEVICE hDevice,                // IN
                   D3D10DDI_HRESOURCE hDstResource,         // IN
                   UINT DstSubResource,                     // IN
                   UINT DstX,                               // IN
                   UINT DstY,                               // IN
                   UINT DstZ,                               // IN
                   D3D10DDI_HRESOURCE hSrcResource,         // IN
                   UINT SrcSubResource,                     // IN
                   __in_opt const D3D10_DDI_BOX *pSrcBox)   // IN (optional)
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;

   unsigned dst_level = DstSubResource % (dst_resource->last_level + 1);
   unsigned dst_layer = DstSubResource / (dst_resource->last_level + 1);
   unsigned src_level = SrcSubResource % (src_resource->last_level + 1);
   unsigned src_layer = SrcSubResource / (src_resource->last_level + 1);

   struct pipe_box src_box;
   if (pSrcBox) {
      src_box.x = pSrcBox->left;
      src_box.y = pSrcBox->top;
      src_box.z = pSrcBox->front + src_layer;
      src_box.width  = pSrcBox->right  - pSrcBox->left;
      src_box.height = pSrcBox->bottom - pSrcBox->top;
      src_box.depth  = pSrcBox->back   - pSrcBox->front;
   } else {
      src_box.x = 0;
      src_box.y = 0;
      src_box.z = 0 + src_layer;
      src_box.width  = u_minify(src_resource->width0,  src_level);
      src_box.height = u_minify(src_resource->height0, src_level);
      src_box.depth  = u_minify(src_resource->depth0,  src_level);
   }

   if (areResourcesCompatible(src_resource, dst_resource)) {
      pipe->resource_copy_region(pipe,
                                 dst_resource, dst_level,
                                 DstX, DstY, DstZ + dst_layer,
                                 src_resource, src_level,
                                 &src_box);
   } else {
      util_resource_copy_region(pipe,
                                dst_resource, dst_level,
                                DstX, DstY, DstZ + dst_layer,
                                src_resource, src_level,
                                &src_box);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceResolveSubResource --
 *
 *    The ResourceResolveSubResource function resolves
 *    multiple samples to one pixel.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceResolveSubResource(D3D10DDI_HDEVICE hDevice,        // IN
                           D3D10DDI_HRESOURCE hDstResource, // IN
                           UINT DstSubResource,             // IN
                           D3D10DDI_HRESOURCE hSrcResource, // IN
                           UINT SrcSubResource,             // IN
                           DXGI_FORMAT ResolveFormat)       // IN
{
   LOG_UNSUPPORTED_ENTRYPOINT();
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceIsStagingBusy --
 *
 *    The ResourceIsStagingBusy function determines whether a
 *    resource is currently being used by the graphics pipeline.
 *
 * ----------------------------------------------------------------------
 */

BOOL APIENTRY
ResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice,       // IN
                      D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   /* ignore */

   return false;
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceReadAfterWriteHazard --
 *
 *    The ResourceReadAfterWriteHazard function informs the user-mode
 *    display driver that the specified resource was used as an output
 *    from the graphics processing unit (GPU) and that the resource
 *    will be used as an input to the GPU.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE hDevice,      // IN
                             D3D10DDI_HRESOURCE hResource)  // IN
{
   LOG_ENTRYPOINT();

   /* Not actually necessary */
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUpdateSubResourceUP --
 *
 *    The ResourceUpdateSubresourceUP function updates a
 *    destination subresource region from a source
 *    system memory region.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE hDevice,                // IN
                            D3D10DDI_HRESOURCE hDstResource,         // IN
                            UINT DstSubResource,                     // IN
                            __in_opt const D3D10_DDI_BOX *pDstBox,   // IN
                            __in const void *pSysMemUP,              // IN
                            UINT RowPitch,                           // IN
                            UINT DepthPitch)                         // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   struct pipe_resource *dst_resource = pDstResource->resource;

   unsigned level;
   struct pipe_box box;

   if (pDstBox) {
      UINT DstMipLevels = dst_resource->last_level + 1;
      level = DstSubResource % DstMipLevels;
      unsigned dst_layer = DstSubResource / DstMipLevels;
      box.x = pDstBox->left;
      box.y = pDstBox->top;
      box.z = pDstBox->front + dst_layer;
      box.width  = pDstBox->right  - pDstBox->left;
      box.height = pDstBox->bottom - pDstBox->top;
      box.depth  = pDstBox->back   - pDstBox->front;
   } else {
      subResourceBox(dst_resource, DstSubResource, &level, &box);
   }

   Bc250DdiMapAudit audit("update_subresource", pipe, dst_resource, level,
                          PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE, box);
   struct pipe_transfer *transfer;
   void *map;
   if (pDstResource->buffer) {
      map = pipe->buffer_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   } else {
      map = pipe->texture_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   }
   assert(map);
   if (map) {
      for (int z = 0; z < box.depth; ++z) {
         uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
         const uint8_t *src = (const uint8_t*)pSysMemUP + z*DepthPitch;
         util_copy_rect(dst,
                        dst_resource->format,
                        transfer->stride,
                        0, 0, box.width, box.height,
                        src,
                        RowPitch,
                        0, 0);
      }
      audit.Copied();
      if (pDstResource->buffer) {
         pipe_buffer_unmap(pipe, transfer);
      } else {
         pipe_texture_unmap(pipe, transfer);
      }
   }
}

