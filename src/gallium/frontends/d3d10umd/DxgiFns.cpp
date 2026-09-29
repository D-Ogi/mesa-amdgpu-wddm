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
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>
#include <vector>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "util/box.h"
#include "tgsi/tgsi_scan.h"
#include "c99_compat.h"
extern "C" {
#include "util/streaming-load-memcpy.h"
}
#include "frontend/winsys_handle.h"


/*
 * ----------------------------------------------------------------------
 *
 * Streaming shadows of opened write-combined surfaces --
 *
 *    A surface another process presents (a D3D12 swap chain buffer is a
 *    primary, and primaries must not be Cached) reaches this UMD through a
 *    write-combined Lock2 mapping. llvmpipe sampling it there reads each
 *    texel uncached: about 25 MiB/s on unit A, where MOVNTDQA streaming
 *    loads copy the same mapping at about 3.3 GiB/s (lab bench wc-read 001).
 *    Such a surface gets an ordinary cached llvmpipe texture that its shader
 *    resource views sample, and every draw that can sample it copies the
 *    surface into it first: a draw whose bound VS, GS or PS declares a slot
 *    holding one of its views. A view left bound under shaders that do not
 *    declare its slot costs nothing (DWM keeps the game's view bound across
 *    many such draws; copying on those made 106 composite at ~7 frames/s).
 *    There is no cheaper boundary to key on: this device's own Present does
 *    not tell when the producer wrote the surface again. Render targets,
 *    copies, Blt, Map and Present keep using the imported surface itself.
 *
 *    Producer ordering is unchanged: the runtime orders the other process's
 *    GPU work before this device's use of the surface, exactly as it did
 *    when llvmpipe sampled the mapping directly. The maps are synchronized:
 *    the surface's map waits for this context's queued writes to it, the
 *    shadow's map for queued draws that still sample the previous copy.
 *
 * ----------------------------------------------------------------------
 */

void
Bc250ShadowCreate(Device *device, Resource *resource)
{
   struct pipe_resource *r = resource->resource;
   MEMORY_BASIC_INFORMATION m;
   // Only write-combined mappings: streaming loads are what makes the copy
   // cheap there, and nothing shows they help an uncached one.
   if (!r || !resource->cpuMapping || resource->shadow || r->target != PIPE_TEXTURE_2D ||
       r->last_level || r->array_size != 1 || r->depth0 != 1 || r->nr_samples > 1 ||
       util_format_get_blocksize(r->format) != 4 || device->shadowedCount >= BC250_MAX_SHADOWS ||
       VirtualQuery(resource->cpuMapping, &m, sizeof(m)) != sizeof(m) ||
       (m.Protect & PAGE_NOCACHE) || !(m.Protect & PAGE_WRITECOMBINE))
      return;
   const UINT64 bytes = Bc250ShadowCharge(r->width0, r->height0);
   if (device->shadowBytes + bytes > BC250_MAX_SHADOW_BYTES) {
      DebugPrintf("BC250 Shadow handle %x %ux%u over the budget, sampled in place\n", resource->allocation,
                  r->width0, r->height0);
      return;
   }
   struct pipe_resource templ = *r;
   templ.next = NULL;
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.flags = 0;
   templ.usage = PIPE_USAGE_DEFAULT;
   resource->shadow = device->pipe->screen->resource_create(device->pipe->screen, &templ);
   DebugPrintf("BC250 Shadow handle %x %ux%u %s protect %lx: %s\n", resource->allocation, r->width0,
               r->height0, util_format_short_name(r->format), m.Protect, resource->shadow ? "created" : "failed");
   if (!resource->shadow) return;
   resource->shadowBytes = bytes;
   resource->shadowBindings = 0;
   device->shadowBytes += bytes;
   device->shadowed[device->shadowedCount++] = resource;
}

void
Bc250ShadowRelease(Device *device, Resource *resource)
{
   if (!resource->shadow) return;
   for (UINT i = 0; i < device->shadowedCount; ++i)
      if (device->shadowed[i] == resource) {
         device->shadowed[i] = device->shadowed[--device->shadowedCount];
         break;
      }
   for (UINT sh = 0; sh < MESA_SHADER_STAGES; ++sh)
      for (UINT i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++i)
         if (device->shadowSlots[sh][i] == resource) device->shadowSlots[sh][i] = NULL;
   resource->shadowBindings = 0;
   device->shadowBytes -= resource->shadowBytes;
   resource->shadowBytes = 0;
   pipe_resource_reference(&resource->shadow, NULL);
}

void
Bc250ShadowBind(Device *device, UINT stage, UINT slot, ShaderResourceView *view)
{
   Resource *owner = view ? view->shadowOf : NULL;
   Resource **bound = &device->shadowSlots[stage][slot];
   if (*bound == owner) return;
   if (*bound) (*bound)->shadowBindings--;
   if (owner) owner->shadowBindings++;
   *bound = owner;
}

static bool
Bc250ShadowRefresh(Device *device, Resource *resource)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_resource *src = resource->resource;
   LARGE_INTEGER start, end;
   QueryPerformanceCounter(&start);
   struct pipe_box box;
   u_box_2d(0, 0, src->width0, src->height0, &box);
   struct pipe_transfer *from = NULL, *to = NULL;
   // Waits for this context's own pending writes to the surface.
   char *s = (char *)pipe->texture_map(pipe, src, 0, PIPE_MAP_READ, &box, &from);
   // Waits for queued draws that still sample the previous copy.
   char *d = s ? (char *)pipe->texture_map(pipe, resource->shadow, 0,
                                           PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, &box, &to) : NULL;
   if (d) {
      const size_t row = util_format_get_stride(src->format, src->width0);
      const unsigned rows = util_format_get_nblocksy(src->format, src->height0);
      // Visible bytes of each row, each side at its own stride.
      for (unsigned y = 0; y < rows; ++y)
         util_streaming_load_memcpy(d + (size_t)y * to->stride, s + (size_t)y * from->stride, row);
      device->profileShadowRefreshes++;
      device->profileShadowBytes += (UINT64)row * rows;
   } else {
      DebugPrintf("BC250 Shadow handle %x refresh map failed\n", resource->allocation);
   }
   if (to) pipe->texture_unmap(pipe, to);
   if (from) pipe->texture_unmap(pipe, from);
   QueryPerformanceCounter(&end);
   device->profileShadowTicks += end.QuadPart - start.QuadPart;
   return d != NULL;
}

// Whether a shader declares shader resource view `slot` (the translator
// declares a sampler view for every DCL_RESOURCE, so every slot it can read).
static bool
Bc250ShaderDeclaresView(Shader *shader, UINT slot)
{
   if (!shader || !shader->state.tokens) return false;
   if (!shader->viewsScanned) {
      struct tgsi_shader_info info;
      tgsi_scan_shader(shader->state.tokens, &info);
      shader->viewsDeclared = info.file_mask[TGSI_FILE_SAMPLER_VIEW];
      shader->viewsMax = info.file_max[TGSI_FILE_SAMPLER_VIEW];
      shader->viewsScanned = true;
   }
   return slot < 32 ? ((shader->viewsDeclared >> slot) & 1) != 0 : (int)slot <= shader->viewsMax;
}

static bool
Bc250ShadowSampled(Device *device, Resource *resource)
{
   for (UINT stage = 0; stage < MESA_SHADER_STAGES; ++stage) {
      Shader *shader = device->shadowShaders[stage];
      if (!shader || !shader->state.tokens) continue;
      for (UINT slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot)
         if (device->shadowSlots[stage][slot] == resource && Bc250ShaderDeclaresView(shader, slot))
            return true;
   }
   return false;
}

// Copies every shadow the bound shaders can sample once, however many stages
// and slots bind views of it. False when a copy failed: the draw must not
// sample an old copy, and the runtime hears of it (a failed llvmpipe map is
// out of memory).
bool
Bc250ShadowPrepareDraw(Device *device)
{
   bool ok = true;
   for (UINT i = 0; i < device->shadowedCount; ++i) {
      Resource *r = device->shadowed[i];
      if (r->shadowBindings && Bc250ShadowSampled(device, r) && !Bc250ShadowRefresh(device, r)) ok = false;
   }
   if (!ok) {
      DebugPrintf("BC250 SetError %08lx: draw skipped\n", E_OUTOFMEMORY);
      device->UMCallbacks.pfnSetErrorCb(device->hRTCoreLayer, E_OUTOFMEMORY);
   }
   return ok;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

HRESULT Bc250EnsureSurface(Device *device, Resource *resource)
{
   if (!resource || !resource->resource) return E_INVALIDARG;
   if (resource->presentReady) return S_OK;
   UINT width = resource->resource->width0, height = resource->resource->height0;
   if (!width || !height || width > 8192 || height > 8192) return E_INVALIDARG;
   if (!device->pagingQueue) {
      if (!device->KTCallbacks.pfnCreatePagingQueueCb) return E_NOTIMPL;
      D3DDDICB_CREATEPAGINGQUEUE queue = {};
      queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
      HRESULT hr = device->KTCallbacks.pfnCreatePagingQueueCb(device->hDevice, &queue);
      DebugPrintf("BC250 CreatePagingQueue %08lx\n", hr);
      if (FAILED(hr)) return hr;
      device->pagingQueue = queue.hPagingQueue;
      device->pagingFence = (volatile UINT64 *)queue.FenceValueCPUVirtualAddress;
   }
   // A surface this UMD allocates stays 8-bit: _Present and the KMD blit copy
   // 8-bit formats only. An opened surface (allocation set by OpenResource)
   // was admitted there by the shared surface format table.
   UINT format = 0;
   if (!resource->allocation) {
      switch (resource->Format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM: format = D3DDDIFMT_A8R8G8B8; break;
      case DXGI_FORMAT_B8G8R8X8_UNORM: format = D3DDDIFMT_X8R8G8B8; break;
      case DXGI_FORMAT_R8G8B8A8_UNORM: format = D3DDDIFMT_A8B8G8R8; break;
      default: return E_NOTIMPL;
      }
   }
   const UINT bpp = util_format_get_blocksize(resource->resource->format);
   struct SurfacePrivate { UINT magic, version, width, height, pitch, format; UINT64 size; };
   static_assert(sizeof(SurfacePrivate) == 32, "LB7A ABI");
   const UINT pitchAlignment = resource->primary ? 256u : 64u;
   const UINT pitch = resource->allocation ? resource->surfacePitch :
      (width * bpp + pitchAlignment - 1) & ~(pitchAlignment - 1);
   const UINT64 bytes = resource->allocation ? resource->surfaceBytes :
      UINT64(pitch) * ((height + 3u) & ~3u);
   if ((pitch & 15u) || pitch < ((width + 3u) & ~3u) * bpp ||
       bytes < UINT64(pitch) * ((height + 3u) & ~3u))
      return E_INVALIDARG;
   SurfacePrivate data = {0x4137424c, 1, width, height, pitch, format, bytes};
   HRESULT hr;
   if (!resource->allocation) {
      D3DDDI_ALLOCATIONINFO2 info = {};
      info.pPrivateDriverData = &data; info.PrivateDriverDataSize = sizeof(data);
      info.Flags.Primary = resource->primary; info.VidPnSourceId = resource->vidpn;
      D3DDDICB_ALLOCATE allocate = {};
      allocate.hResource = resource->hRTResource;
      // E26R v2 requires KMD 0.7.147.1 or later. CPU-read shared surfaces
      // use cached system backing; primary surfaces must never request Cached.
      // Match Primary to the same resource property used in ALLOCATIONINFO2.
      struct ResourcePrivate { UINT magic, version, shared, cpuAccessFlags; };
      static_assert(sizeof(ResourcePrivate) == 16, "E26R v2 ABI");
      const UINT primaryFlag = resource->primary ? 1u : 0u;
      const UINT cpuReadFlag = resource->shared ? 2u : 0u;
      ResourcePrivate group = {0x52363245, 2, resource->shared ? 1u : 0u,
                               primaryFlag | cpuReadFlag};
      allocate.pPrivateDriverData = &group; allocate.PrivateDriverDataSize = sizeof(group);
      DebugPrintf("BC250 Allocate input runtime %p size %llu primary %u\n", resource->hRTResource, data.size, resource->primary);
      allocate.NumAllocations = 1; allocate.pAllocationInfo2 = &info;
      hr = device->KTCallbacks.pfnAllocateCb(device->hDevice, &allocate);
      DebugPrintf("BC250 Allocate %08lx format %u primary %u handle %x resource %x\n", hr, format, resource->primary, info.hAllocation, allocate.hKMResource);
      if (FAILED(hr)) return hr;
      resource->allocation = info.hAllocation;
      resource->surfacePitch = pitch;
      resource->surfaceBytes = bytes;
   }
   resource->gpuBytes = (data.size + 4095) & ~UINT64(4095);
   D3DDDI_MAPGPUVIRTUALADDRESS map = {};
   map.hPagingQueue = device->pagingQueue;
   map.hAllocation = resource->allocation;
   map.SizeInPages = resource->gpuBytes / 4096;
   map.Protection.Write = 1;
   hr = device->KTCallbacks.pfnMapGpuVirtualAddressCb(device->hDevice, &map);
   DebugPrintf("BC250 MapGpuVa %08lx va %llx\n", hr, map.VirtualAddress);
   // WDDM callbacks report successful asynchronous paging as E_PENDING.
   if (FAILED(hr) && hr != E_PENDING) return hr;
   resource->gpuVa = map.VirtualAddress;
   UINT64 fence = map.PagingFenceValue;
   D3DDDI_MAKERESIDENT resident = {};
   resident.hPagingQueue = device->pagingQueue;
   resident.NumAllocations = 1; resident.AllocationList = &resource->allocation;
   hr = device->KTCallbacks.pfnMakeResidentCb(device->hDevice, &resident);
   DebugPrintf("BC250 MakeResident %08lx\n", hr);
   if (FAILED(hr) && hr != E_PENDING) return hr;
   if (resident.PagingFenceValue > fence) fence = resident.PagingFenceValue;
   ULONGLONG deadline = GetTickCount64() + 5000;
   while (device->pagingFence && *device->pagingFence < fence && GetTickCount64() < deadline) Sleep(1);
   if (!device->pagingFence || *device->pagingFence < fence) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
   D3DDDICB_LOCK2 lock = {};
   lock.hAllocation = resource->allocation;
   hr = device->KTCallbacks.pfnLock2Cb(device->hDevice, &lock);
   if (FAILED(hr)) return hr;
   resource->cpuMapping = lock.pData;
   struct winsys_handle handle = {};
   handle.type = WINSYS_HANDLE_TYPE_USER_MEMORY;
   handle.user_memory = lock.pData; handle.stride = data.pitch;
   handle.size = data.size;
   struct pipe_resource desc = *resource->resource;
   desc.bind |= PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED;
   struct pipe_resource *imported = device->pipe->screen->resource_from_handle(
       device->pipe->screen, &desc, &handle, 0);
   if (!imported) return E_OUTOFMEMORY;
   pipe_resource_reference(&resource->resource, NULL);
   resource->resource = imported;
   resource->presentReady = TRUE;
   return S_OK;
}

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   /* E26 software baseline: use the runtime's WDDM callbacks, not GetDC.
    * LB7A is the existing linear-surface contract in bc250kmd/wddm.c.
    * This path remains diagnostic until sharing, residency and flips pass. */
   if (!device->KTCallbacks.pfnCreateContextVirtualCb ||
       !device->KTCallbacks.pfnLock2Cb || !device->KTCallbacks.pfnUnlock2Cb)
      return E_NOTIMPL;
   if (!device->hContext) {
      D3DDDICB_CREATECONTEXTVIRTUAL create = {};
      create.EngineAffinity = 1;
      HRESULT hr = device->KTCallbacks.pfnCreateContextVirtualCb(device->hDevice, &create);
      DebugPrintf("BC250 CreateContextVirtual %08lx\n", hr);
      if (FAILED(hr)) return hr;
      device->hContext = create.hContext;
   }
   struct pipe_resource *resource = pSrcResource->resource;
   UINT width = resource->width0, height = resource->height0;
   if (!width || !height || width > 8192 || height > 8192) return E_INVALIDARG;
   if (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       resource->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
       resource->format != PIPE_FORMAT_R8G8B8A8_UNORM &&
       resource->format != PIPE_FORMAT_R8G8B8X8_UNORM) return E_NOTIMPL;
   HRESULT allocated = Bc250EnsureSurface(device, pSrcResource);
   if (FAILED(allocated)) return allocated;
   // llvmpipe flush queues CPU raster work. Publish the scanout allocation
   // only after its render fence completes; a CPU barrier alone cannot wait.
   LARGE_INTEGER renderStart, renderEnd;
   QueryPerformanceCounter(&renderStart);
   pipe_fence_handle *renderFence = NULL;
   device->pipe->flush(device->pipe, &renderFence, 0);
   bool rendered = !renderFence || device->pipe->screen->fence_finish(
      device->pipe->screen, device->pipe, renderFence, OS_TIMEOUT_INFINITE);
   device->pipe->screen->fence_reference(device->pipe->screen, &renderFence, NULL);
   QueryPerformanceCounter(&renderEnd);
   if (!rendered) return E_FAIL;
   MemoryBarrier();
   HRESULT hr;
   DXGIDDICB_PRESENT present = {};
   present.hSrcAllocation = pSrcResource->allocation;
   present.hContext = device->hContext;
   Resource *dstResource = CastResource(pPresentData->hDstResource);
   if (dstResource) {
      hr = Bc250EnsureSurface(device, dstResource);
      if (FAILED(hr)) return hr;
      present.hDstAllocation = dstResource->allocation;
   }
   DebugPrintf("BC250 Present flags %x src %x dst %x\n", pPresentData->Flags.Value,
               present.hSrcAllocation, present.hDstAllocation);
   present.pDXGIContext = pPresentData->pDXGIContext;
   LARGE_INTEGER presentStart, presentEnd, frequency;
   QueryPerformanceCounter(&presentStart);
   hr = device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice, &present);
   QueryPerformanceCounter(&presentEnd);QueryPerformanceFrequency(&frequency);
   if(++device->profilePresents<=120 || device->profilePresents%60==0)
      DebugPrintf("BC250 Perf frame %u gap_ms %.3f draws %llu draw_ms %.3f max_draw_ms %.3f present_ms %.3f render_wait_ms %.3f"
       " shadows %u refreshes %llu shadow_mib %.3f shadow_ms %.3f\n",
       device->profilePresents,
       device->profileLastPresent?1000.0*(presentEnd.QuadPart-device->profileLastPresent)/frequency.QuadPart:0.0,
       device->profileDrawCalls,1000.0*device->profileDrawTicks/frequency.QuadPart,
       1000.0*device->profileDrawMax/frequency.QuadPart,
       1000.0*(presentEnd.QuadPart-presentStart.QuadPart)/frequency.QuadPart,
       1000.0*(renderEnd.QuadPart-renderStart.QuadPart)/frequency.QuadPart,
       device->shadowedCount,device->profileShadowRefreshes,device->profileShadowBytes/1048576.0,
       1000.0*device->profileShadowTicks/frequency.QuadPart);
   device->profileLastPresent=presentEnd.QuadPart;
   device->profileDrawTicks=device->profileDrawMax=device->profileDrawCalls=0;
   device->profileShadowTicks=device->profileShadowBytes=device->profileShadowRefreshes=0;

   DebugPrintf("BC250 PresentCb %08lx\n", hr);
   return hr;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   Device *device = CastDevice(SetDisplayMode->hDevice);
   Resource *resource = CastResource(SetDisplayMode->hResource);
   HRESULT hr = Bc250EnsureSurface(device, resource);
   if (FAILED(hr)) return hr;
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = resource->allocation;
   hr = device->KTCallbacks.pfnSetDisplayModeCb(device->hDevice, &mode);
   DebugPrintf("BC250 SetDisplayMode %08lx\n", hr);
   return hr;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   LOG_ENTRYPOINT();
   if (args->Resources <= 1) return S_OK;
   Device *device = CastDevice(args->hDevice);
   pipe_context *pipe = device->pipe;
   // Microsoft DXGI_BASE_FUNCTIONS: rotate kernel identities, preserve RT
   // handles. Copying pixels writes into a buffer still scanned by DCN.
   std::vector<Resource> before(args->Resources);
   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *r = CastResource(args->pResources[i]);
      // An opened surface with a shadow is not one of this device's
      // swap chain buffers; its views would keep sampling the old shadow.
      if (!r || !r->presentReady || !r->resource || r->shadow ||
          r->resource->target != PIPE_TEXTURE_2D || r->resource->last_level ||
          (r->resource->bind & PIPE_BIND_DEPTH_STENCIL)) return E_NOTIMPL;
      before[i] = *r;
   }
   auto rotated = [&](pipe_resource *resource) -> pipe_resource * {
      for (UINT i = 0; i < args->Resources; ++i)
         if (resource == before[i].resource)
            return before[(i + 1) % args->Resources].resource;
      return resource;
   };
   pipe->flush(pipe, NULL, 0);
   // Prepare sampler views before publishing any changed resource identity.
   struct ViewChange { ShaderResourceView *view; pipe_sampler_view *next; };
   std::vector<ViewChange> changes;
   for (ShaderResourceView *v = device->shaderResourceViews; v; v = v->next) {
      if (!v->handle || rotated(v->handle->texture) == v->handle->texture) continue;
      pipe_sampler_view *next = pipe->create_sampler_view(pipe,
         rotated(v->handle->texture), v->handle);
      if (!next) {
         for (auto &c : changes) pipe->sampler_view_release(pipe, c.next);
         return E_OUTOFMEMORY;
      }
      changes.push_back({v, next});
   }
   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *r = CastResource(args->pResources[i]);
      const Resource &next = before[(i + 1) % args->Resources];
      r->resource = next.resource;
      r->allocation = next.allocation;
      r->gpuVa = next.gpuVa;
      r->gpuBytes = next.gpuBytes;
      r->surfacePitch = next.surfacePitch;
      r->surfaceBytes = next.surfaceBytes;
      r->cpuMapping = next.cpuMapping;
      r->presentReady = next.presentReady;
      // hRTResource and the logical resource/view descriptors stay in place.
      DebugPrintf("BC250 Rotate slot %u kernel %x -> %x va %llx\n",
                  i, before[i].allocation, r->allocation, r->gpuVa);
   }
   for (RenderTargetView *v = device->renderTargetViews; v; v = v->next)
      pipe_resource_reference(&v->surface.texture, rotated(v->surface.texture));
   for (UINT i = 0; i < device->fb.nr_cbufs; ++i)
      pipe_resource_reference(&device->fb.cbufs[i].texture,
                              rotated(device->fb.cbufs[i].texture));
   // A cached bound framebuffer and bound samplers also retain old storage.
   pipe->set_framebuffer_state(pipe, &device->fb);
   for (auto &c : changes) {
      pipe_sampler_view *old = c.view->handle;
      for (UINT sh = 0; sh < MESA_SHADER_STAGES; ++sh)
         for (UINT i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++i)
            if (device->sampler_views[sh][i] == old)
               device->sampler_views[sh][i] = c.next;
      c.view->handle = c.next;
      pipe->sampler_view_release(pipe, old);
   }
   if (!changes.empty())
      for (auto sh : {MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY})
         pipe->set_sampler_views(pipe, sh, 0, PIPE_MAX_SHADER_SAMPLER_VIEWS,
                                 0, device->sampler_views[sh]);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_ENTRYPOINT();
   struct pipe_context *pipe = CastPipeDevice(Blt->hDevice);
   struct pipe_resource *src = CastPipeResource(Blt->hSrcResource);
   struct pipe_resource *dst = CastPipeResource(Blt->hDstResource);
   if (!src || !dst || Blt->SrcSubresource || Blt->DstSubresource ||
       src->target != PIPE_TEXTURE_2D || dst->target != PIPE_TEXTURE_2D ||
       src->array_size != 1 || dst->array_size != 1 ||
       Blt->Rotate > DXGI_DDI_MODE_ROTATION_IDENTITY) return E_NOTIMPL;
   if (Blt->DstRight <= Blt->DstLeft || Blt->DstBottom <= Blt->DstTop ||
       Blt->DstRight > dst->width0 || Blt->DstBottom > dst->height0)
      return E_INVALIDARG;
   if (!pipe->screen->is_format_supported(pipe->screen, src->format, src->target,
        src->nr_samples, src->nr_storage_samples, PIPE_BIND_SAMPLER_VIEW) ||
       !pipe->screen->is_format_supported(pipe->screen, dst->format, dst->target,
        dst->nr_samples, dst->nr_storage_samples, PIPE_BIND_RENDER_TARGET)) return E_NOTIMPL;
   struct pipe_blit_info info = {};
   info.src.resource = src; info.src.format = src->format;
   info.src.box.width = src->width0; info.src.box.height = src->height0; info.src.box.depth = 1;
   info.dst.resource = dst; info.dst.format = dst->format;
   info.dst.box.x = Blt->DstLeft; info.dst.box.y = Blt->DstTop;
   info.dst.box.width = Blt->DstRight - Blt->DstLeft;
   info.dst.box.height = Blt->DstBottom - Blt->DstTop; info.dst.box.depth = 1;
   info.mask = PIPE_MASK_RGBA; info.filter = PIPE_TEX_FILTER_NEAREST;
   pipe->blit(pipe, &info);
   pipe->flush(pipe, NULL, 0);
   MemoryBarrier();
   return S_OK;
}
