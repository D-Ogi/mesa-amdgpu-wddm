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
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
 */


#include "Draw.h"
#include "DxgiFns.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "Format.h"

#include "Debug.h"

#include "util/u_sampler.h"
#include "util/u_framebuffer.h"


#include "CompileDispatch.h"
#include <tuple>
#include <type_traits>
static Device *Bc250CompilerEntryDevice(D3D10DDI_HDEVICE h) { return CastDevice(h); }
template <typename A> static Device *Bc250CompilerEntryDevice(A *a) { return CastDevice(a->hDevice); }
static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
struct Bc250CompilerTraits {
   template<typename A> static Device *device(A a) { return Bc250CompilerEntryDevice(a); }
   struct Scope { explicit Scope(Device *) {} };
};
template<auto F, bool Compiler = true>
using Bc250CompilerEntry = Bc250DdiEntry<F, Bc250CompilerTraits, Compiler,
   std::is_same_v<std::integral_constant<decltype(F), F>,
                  std::integral_constant<decltype(&DestroyDevice), &DestroyDevice>>>;

static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   return sizeof(Device);
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

struct Bc250CreateGuard {
   Device *device;
   bool complete = false;
   explicit Bc250CreateGuard(Device *value) : device(value) {}
   ~Bc250CreateGuard() {
      if (complete) return;
      auto *pipe = device->pipe;
      if (pipe) {
         pipe->bind_fs_state(pipe, NULL);
         pipe->bind_vs_state(pipe, NULL);
         if (device->cso) {
            cso_unbind_context(device->cso);
            cso_destroy_context(device->cso);
         }
         if (device->empty_fs) DeleteEmptyShader(device, MESA_SHADER_FRAGMENT, device->empty_fs);
         if (device->empty_vs) DeleteEmptyShader(device, MESA_SHADER_VERTEX, device->empty_vs);
         pipe->destroy(pipe);
      }
      if (device->hContext) {
         D3DDDICB_DESTROYCONTEXT destroy = {};
         destroy.hContext = device->hContext;
         device->KTCallbacks.pfnDestroyContextCb(device->hDevice, &destroy);
      }
   }
};

static ATTRIBUTE_NOINLINE HRESULT APIENTRY
CreateDeviceOnCompilerStack(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();

   if (0) {
      DebugPrintf("hAdapter = %p\n", hAdapter);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("p10_1DeviceFuncs = %p\n", pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice);
      DebugPrintf("DXGIBaseDDI = %p\n", pCreateData->DXGIBaseDDI);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer);
      DebugPrintf("pUMCallbacks = %p\n", pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __func__, pCreateData->Interface);
      return E_FAIL;
   }

   Adapter *pAdapter = CastAdapter(hAdapter);

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);
   pDevice->compile_dispatch = Bc250CompileDispatch::active;
   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;
   Bc250CreateGuard createGuard(pDevice);

   // E26: DWM requests runtime synchronization while creating its primary,
   // before the first Present. Register a virtual context at device creation
   // so the runtime has a context for its broadcast synchronization callbacks.
   if (!pCreateData->pKTCallbacks->pfnCreateContextVirtualCb) return E_NOTIMPL;
   D3DDDICB_CREATECONTEXTVIRTUAL context = {};
   context.EngineAffinity = 1;
   HRESULT contextResult = pCreateData->pKTCallbacks->pfnCreateContextVirtualCb(
       (HANDLE)pCreateData->hRTDevice.handle, &context);
   DebugPrintf("BC250 initial CreateContextVirtual %08lx\n", contextResult);
   if (FAILED(contextResult)) return contextResult;
   pDevice->hContext = context.hContext;

   struct pipe_screen *screen = pAdapter->screen;
   DebugPrintf("BC250 Renderer: %s\n", screen->get_name(screen));
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) return E_OUTOFMEMORY;
   pDevice->pipe = pipe;
   pDevice->cso = cso_create_context(pipe, CSO_NO_VBUF);
   if (!pDevice->cso) return E_OUTOFMEMORY;

   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   if (!pDevice->empty_vs) return E_OUTOFMEMORY;
   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);
   if (!pDevice->empty_fs) return E_OUTOFMEMORY;

   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   pDevice->draw_so_target = NULL;

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
   D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = Bc250CompilerEntry<ResourceUpdateSubResourceUP>::Call;
   pDeviceFuncs->pfnVsSetConstantBuffers = Bc250CompilerEntry<VsSetConstantBuffers, false>::Call;
   pDeviceFuncs->pfnPsSetShaderResources = Bc250CompilerEntry<PsSetShaderResources, false>::Call;
   pDeviceFuncs->pfnPsSetShader = Bc250CompilerEntry<PsSetShader>::Call;
   pDeviceFuncs->pfnPsSetSamplers = Bc250CompilerEntry<PsSetSamplers, false>::Call;
   pDeviceFuncs->pfnVsSetShader = Bc250CompilerEntry<VsSetShader>::Call;
   pDeviceFuncs->pfnDrawIndexed = Bc250CompilerEntry<DrawIndexed>::Call;
   pDeviceFuncs->pfnDraw = Bc250CompilerEntry<Draw>::Call;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicIABufferUnmap = Bc250CompilerEntry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = Bc250CompilerEntry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnPsSetConstantBuffers = Bc250CompilerEntry<PsSetConstantBuffers, false>::Call;
   pDeviceFuncs->pfnIaSetInputLayout = Bc250CompilerEntry<IaSetInputLayout, false>::Call;
   pDeviceFuncs->pfnIaSetVertexBuffers = Bc250CompilerEntry<IaSetVertexBuffers, false>::Call;
   pDeviceFuncs->pfnIaSetIndexBuffer = Bc250CompilerEntry<IaSetIndexBuffer, false>::Call;
   pDeviceFuncs->pfnDrawIndexedInstanced = Bc250CompilerEntry<DrawIndexedInstanced>::Call;
   pDeviceFuncs->pfnDrawInstanced = Bc250CompilerEntry<DrawInstanced>::Call;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicResourceUnmap = Bc250CompilerEntry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnGsSetConstantBuffers = Bc250CompilerEntry<GsSetConstantBuffers, false>::Call;
   pDeviceFuncs->pfnGsSetShader = Bc250CompilerEntry<GsSetShader>::Call;
   pDeviceFuncs->pfnIaSetTopology = Bc250CompilerEntry<IaSetTopology, false>::Call;
   pDeviceFuncs->pfnStagingResourceMap = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnStagingResourceUnmap = Bc250CompilerEntry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnVsSetShaderResources = Bc250CompilerEntry<VsSetShaderResources, false>::Call;
   pDeviceFuncs->pfnVsSetSamplers = Bc250CompilerEntry<VsSetSamplers, false>::Call;
   pDeviceFuncs->pfnGsSetShaderResources = Bc250CompilerEntry<GsSetShaderResources, false>::Call;
   pDeviceFuncs->pfnGsSetSamplers = Bc250CompilerEntry<GsSetSamplers, false>::Call;
   pDeviceFuncs->pfnSetRenderTargets = Bc250CompilerEntry<SetRenderTargets, false>::Call;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = Bc250CompilerEntry<ShaderResourceViewReadAfterWriteHazard, false>::Call;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = Bc250CompilerEntry<ResourceReadAfterWriteHazard, false>::Call;
   pDeviceFuncs->pfnSetBlendState = Bc250CompilerEntry<SetBlendState, false>::Call;
   pDeviceFuncs->pfnSetDepthStencilState = Bc250CompilerEntry<SetDepthStencilState, false>::Call;
   pDeviceFuncs->pfnSetRasterizerState = Bc250CompilerEntry<SetRasterizerState, false>::Call;
   pDeviceFuncs->pfnQueryEnd = Bc250CompilerEntry<QueryEnd, false>::Call;
   pDeviceFuncs->pfnQueryBegin = Bc250CompilerEntry<QueryBegin, false>::Call;
   pDeviceFuncs->pfnResourceCopyRegion = Bc250CompilerEntry<ResourceCopyRegion>::Call;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = Bc250CompilerEntry<ResourceUpdateSubResourceUP>::Call;
   pDeviceFuncs->pfnSoSetTargets = Bc250CompilerEntry<SoSetTargets, false>::Call;
   pDeviceFuncs->pfnDrawAuto = Bc250CompilerEntry<DrawAuto>::Call;
   pDeviceFuncs->pfnSetViewports = Bc250CompilerEntry<SetViewports, false>::Call;
   pDeviceFuncs->pfnSetScissorRects = Bc250CompilerEntry<SetScissorRects, false>::Call;
   pDeviceFuncs->pfnClearRenderTargetView = Bc250CompilerEntry<ClearRenderTargetView>::Call;
   pDeviceFuncs->pfnClearDepthStencilView = Bc250CompilerEntry<ClearDepthStencilView>::Call;
   pDeviceFuncs->pfnSetPredication = Bc250CompilerEntry<SetPredication, false>::Call;
   pDeviceFuncs->pfnQueryGetData = Bc250CompilerEntry<QueryGetData>::Call;
   pDeviceFuncs->pfnFlush = Bc250CompilerEntry<Flush>::Call;
   pDeviceFuncs->pfnGenMips = Bc250CompilerEntry<GenMips>::Call;
   pDeviceFuncs->pfnResourceCopy = Bc250CompilerEntry<ResourceCopy>::Call;
   pDeviceFuncs->pfnResourceResolveSubresource = Bc250CompilerEntry<ResourceResolveSubResource>::Call;
   pDeviceFuncs->pfnResourceMap = Bc250CompilerEntry<ResourceMap>::Call;
   pDeviceFuncs->pfnResourceUnmap = Bc250CompilerEntry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnResourceIsStagingBusy = Bc250CompilerEntry<ResourceIsStagingBusy, false>::Call;
   pDeviceFuncs->pfnRelocateDeviceFuncs = Bc250CompilerEntry<RelocateDeviceFuncs, false>::Call;
   pDeviceFuncs->pfnCalcPrivateResourceSize = Bc250CompilerEntry<CalcPrivateResourceSize, false>::Call;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = Bc250CompilerEntry<CalcPrivateOpenedResourceSize, false>::Call;
   pDeviceFuncs->pfnCreateResource = Bc250CompilerEntry<CreateResource>::Call;
   pDeviceFuncs->pfnOpenResource = Bc250CompilerEntry<OpenResource>::Call;
   pDeviceFuncs->pfnDestroyResource = Bc250CompilerEntry<DestroyResource, false>::Call;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = Bc250CompilerEntry<CalcPrivateShaderResourceViewSize, false>::Call;
   pDeviceFuncs->pfnCreateShaderResourceView = Bc250CompilerEntry<CreateShaderResourceView, false>::Call;
   pDeviceFuncs->pfnDestroyShaderResourceView = Bc250CompilerEntry<DestroyShaderResourceView, false>::Call;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = Bc250CompilerEntry<CalcPrivateRenderTargetViewSize, false>::Call;
   pDeviceFuncs->pfnCreateRenderTargetView = Bc250CompilerEntry<CreateRenderTargetView, false>::Call;
   pDeviceFuncs->pfnDestroyRenderTargetView = Bc250CompilerEntry<DestroyRenderTargetView, false>::Call;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = Bc250CompilerEntry<CalcPrivateDepthStencilViewSize, false>::Call;
   pDeviceFuncs->pfnCreateDepthStencilView = Bc250CompilerEntry<CreateDepthStencilView, false>::Call;
   pDeviceFuncs->pfnDestroyDepthStencilView = Bc250CompilerEntry<DestroyDepthStencilView, false>::Call;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = Bc250CompilerEntry<CalcPrivateElementLayoutSize, false>::Call;
   pDeviceFuncs->pfnCreateElementLayout = Bc250CompilerEntry<CreateElementLayout, false>::Call;
   pDeviceFuncs->pfnDestroyElementLayout = Bc250CompilerEntry<DestroyElementLayout, false>::Call;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = Bc250CompilerEntry<CalcPrivateBlendStateSize, false>::Call;
   pDeviceFuncs->pfnCreateBlendState = Bc250CompilerEntry<CreateBlendState, false>::Call;
   pDeviceFuncs->pfnDestroyBlendState = Bc250CompilerEntry<DestroyBlendState, false>::Call;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = Bc250CompilerEntry<CalcPrivateDepthStencilStateSize, false>::Call;
   pDeviceFuncs->pfnCreateDepthStencilState = Bc250CompilerEntry<CreateDepthStencilState, false>::Call;
   pDeviceFuncs->pfnDestroyDepthStencilState = Bc250CompilerEntry<DestroyDepthStencilState, false>::Call;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = Bc250CompilerEntry<CalcPrivateRasterizerStateSize, false>::Call;
   pDeviceFuncs->pfnCreateRasterizerState = Bc250CompilerEntry<CreateRasterizerState, false>::Call;
   pDeviceFuncs->pfnDestroyRasterizerState = Bc250CompilerEntry<DestroyRasterizerState, false>::Call;
   pDeviceFuncs->pfnCalcPrivateShaderSize = Bc250CompilerEntry<CalcPrivateShaderSize, false>::Call;
   pDeviceFuncs->pfnCreateVertexShader = Bc250CompilerEntry<CreateVertexShader>::Call;
   pDeviceFuncs->pfnCreateGeometryShader = Bc250CompilerEntry<CreateGeometryShader>::Call;
   pDeviceFuncs->pfnCreatePixelShader = Bc250CompilerEntry<CreatePixelShader>::Call;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = Bc250CompilerEntry<CalcPrivateGeometryShaderWithStreamOutput, false>::Call;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = Bc250CompilerEntry<CreateGeometryShaderWithStreamOutput>::Call;
   pDeviceFuncs->pfnDestroyShader = Bc250CompilerEntry<DestroyShader, false>::Call;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = Bc250CompilerEntry<CalcPrivateSamplerSize, false>::Call;
   pDeviceFuncs->pfnCreateSampler = Bc250CompilerEntry<CreateSampler, false>::Call;
   pDeviceFuncs->pfnDestroySampler = Bc250CompilerEntry<DestroySampler, false>::Call;
   pDeviceFuncs->pfnCalcPrivateQuerySize = Bc250CompilerEntry<CalcPrivateQuerySize, false>::Call;
   pDeviceFuncs->pfnCreateQuery = Bc250CompilerEntry<CreateQuery, false>::Call;
   pDeviceFuncs->pfnDestroyQuery = Bc250CompilerEntry<DestroyQuery, false>::Call;
   pDeviceFuncs->pfnCheckFormatSupport = Bc250CompilerEntry<CheckFormatSupport, false>::Call;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = Bc250CompilerEntry<CheckMultisampleQualityLevels, false>::Call;
   pDeviceFuncs->pfnCheckCounterInfo = Bc250CompilerEntry<CheckCounterInfo, false>::Call;
   pDeviceFuncs->pfnCheckCounter = Bc250CompilerEntry<CheckCounter, false>::Call;
   pDeviceFuncs->pfnDestroyDevice = Bc250CompilerEntry<DestroyDevice>::Call;
   pDeviceFuncs->pfnSetTextFilterSize = Bc250CompilerEntry<SetTextFilterSize, false>::Call;
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = Bc250CompilerEntry<RelocateDeviceFuncs1, false>::Call;
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = Bc250CompilerEntry<CalcPrivateShaderResourceViewSize1, false>::Call;
      p10_1DeviceFuncs->pfnCreateShaderResourceView = Bc250CompilerEntry<CreateShaderResourceView1, false>::Call;
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = Bc250CompilerEntry<CalcPrivateBlendStateSize1, false>::Call;
      p10_1DeviceFuncs->pfnCreateBlendState = Bc250CompilerEntry<CreateBlendState1, false>::Call;
      p10_1DeviceFuncs->pfnResourceConvert = Bc250CompilerEntry<ResourceCopy>::Call;
      p10_1DeviceFuncs->pfnResourceConvertRegion = Bc250CompilerEntry<ResourceCopyRegion>::Call;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      Bc250CompilerEntry<_Present>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      Bc250CompilerEntry<_GetGammaCaps, false>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      Bc250CompilerEntry<_SetDisplayMode, false>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      Bc250CompilerEntry<_SetResourcePriority, false>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      Bc250CompilerEntry<_QueryResourceResidency, false>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      Bc250CompilerEntry<_RotateResourceIdentities, false>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      Bc250CompilerEntry<_Blt>::Call;

   // E26: the linear shared-resource path is implemented for the tested formats.
   createGuard.complete = true;
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   pipe->flush(pipe, NULL, 0);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   cso_unbind_context(pDevice->cso);
   cso_destroy_context(pDevice->cso);

   DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);

   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   memset(sampler_views, 0, sizeof sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

   if (pDevice->hContext) {
      D3DDDICB_DESTROYCONTEXT destroy = {};
      destroy.hContext = pDevice->hContext;
      pDevice->KTCallbacks.pfnDestroyContextCb(pDevice->hDevice, &destroy);
   }
   if (pDevice->pagingQueue) {
      D3DDDI_DESTROYPAGINGQUEUE destroy = {};
      destroy.hPagingQueue = pDevice->pagingQueue;
      pDevice->KTCallbacks.pfnDestroyPagingQueueCb(pDevice->hDevice, &destroy);
   }
   pipe->destroy(pipe);
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs1 function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);

   pipe->flush(pipe, NULL, 0);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   //LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, false);
   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      }
#endif
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
#endif
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   /* XXX: Disable MSAA */
   *pNumQualityLevels = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE *info)
{
   return Bc250CreateDeviceEntry<CreateDeviceOnCompilerStack>::Call(adapter, info);
}
