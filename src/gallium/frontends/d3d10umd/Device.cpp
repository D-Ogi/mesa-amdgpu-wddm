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

#include "util/bc250_host_bootstrap.h"
#include "util/u_sampler.h"
#include "util/u_framebuffer.h"


extern "C" struct pipe_screen *d3d10_create_screen(void);


#include <d3dkmthk.h>

struct Bc250HostProbeState {
   Device *device;
   DWORD thread;
   HANDLE contexts[16];
   unsigned calls[64];
};

static HANDLE Bc250HostContext(Bc250HostProbeState *s, UINT token)
{
   return token && token <= 16 ? s->contexts[token - 1] : NULL;
}

static HRESULT Bc250HostOperation(Bc250HostProbeState *s, uint32_t op, void *argument)
{
   auto &cb = s->device->KTCallbacks;
   HANDLE rt = s->device->hDevice;
   if (!argument) return E_INVALIDARG;
#define HOST_CALL(name, arg) (cb.pfn##name##Cb ? cb.pfn##name##Cb(rt, arg) : E_NOTIMPL)
   switch (op) {
   case BC250_HOST_CREATE_PAGING: {
      auto *a = (bc250_host_paging *)argument;
      D3DDDICB_CREATEPAGINGQUEUE b = {};
      HRESULT hr = HOST_CALL(CreatePagingQueue, &b);
      a->queue=b.hPagingQueue; a->sync=b.hSyncObject; a->cpu_address=b.FenceValueCPUVirtualAddress;
      return hr;
   }
   case BC250_HOST_DESTROY_PAGING: {
      D3DDDI_DESTROYPAGINGQUEUE b = {};
      b.hPagingQueue=((bc250_host_paging *)argument)->queue;
      return HOST_CALL(DestroyPagingQueue, &b);
   }
   case BC250_HOST_CreateAllocation2: {
      auto *a=(D3DKMT_CREATEALLOCATION *)argument;
      D3DDDICB_ALLOCATE b = {};
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      b.NumAllocations=a->NumAllocations; b.pAllocationInfo2=a->pAllocationInfo2;
      HRESULT hr=HOST_CALL(Allocate, &b);
      a->hResource=b.hKMResource;
      return hr;
   }
   case BC250_HOST_DestroyAllocation2: {
      auto *a=(D3DKMT_DESTROYALLOCATION2 *)argument;
      if (a->hResource || a->Flags.Value) return E_NOTIMPL;
      D3DDDICB_DEALLOCATE b = {};
      b.NumAllocations=a->AllocationCount; b.HandleList=a->phAllocationList;
      return HOST_CALL(Deallocate, &b);
   }
   case BC250_HOST_ReserveGpuVirtualAddress:
      return HOST_CALL(ReserveGpuVirtualAddress, (D3DDDI_RESERVEGPUVIRTUALADDRESS *)argument);
   case BC250_HOST_MapGpuVirtualAddress:
      return HOST_CALL(MapGpuVirtualAddress, (D3DDDI_MAPGPUVIRTUALADDRESS *)argument);
   case BC250_HOST_MakeResident:
      return HOST_CALL(MakeResident, (D3DDDI_MAKERESIDENT *)argument);
   case BC250_HOST_FreeGpuVirtualAddress: {
      auto *a=(D3DKMT_FREEGPUVIRTUALADDRESS *)argument;
      D3DDDICB_FREEGPUVIRTUALADDRESS b = {};
      b.BaseAddress=a->BaseAddress; b.Size=a->Size;
      return HOST_CALL(FreeGpuVirtualAddress, &b);
   }
   case BC250_HOST_Evict: {
      auto *a=(D3DKMT_EVICT *)argument;
      D3DDDICB_EVICT b = {};
      b.NumAllocations=a->NumAllocations; b.AllocationList=a->AllocationList; b.Flags=a->Flags;
      HRESULT hr=HOST_CALL(Evict, &b); a->NumBytesToTrim=b.NumBytesToTrim; return hr;
   }
   case BC250_HOST_Lock2: {
      auto *a=(D3DKMT_LOCK2 *)argument;
      D3DDDICB_LOCK2 b = {};
      b.hAllocation=a->hAllocation; b.Flags.Value=a->Flags.Value;
      HRESULT hr=HOST_CALL(Lock2, &b); a->pData=b.pData; return hr;
   }
   case BC250_HOST_Unlock2: {
      D3DDDICB_UNLOCK2 b = {};
      b.hAllocation=((D3DKMT_UNLOCK2 *)argument)->hAllocation;
      return HOST_CALL(Unlock2, &b);
   }
   case BC250_HOST_CreateContextVirtual: {
      auto *a=(D3DKMT_CREATECONTEXTVIRTUAL *)argument;
      unsigned slot=0; while (slot<16 && s->contexts[slot]) ++slot;
      if (slot==16) return E_OUTOFMEMORY;
      D3DDDICB_CREATECONTEXTVIRTUAL b = {};
      b.NodeOrdinal=a->NodeOrdinal; b.EngineAffinity=a->EngineAffinity; b.Flags=a->Flags;
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      HRESULT hr=HOST_CALL(CreateContextVirtual, &b);
      if (SUCCEEDED(hr)) { s->contexts[slot]=b.hContext; a->hContext=slot+1; }
      return hr;
   }
   case BC250_HOST_DestroyContext: {
      auto *a=(D3DKMT_DESTROYCONTEXT *)argument;
      D3DDDICB_DESTROYCONTEXT b = {};
      b.hContext=Bc250HostContext(s,a->hContext);
      if (!b.hContext) return E_INVALIDARG;
      HRESULT hr=HOST_CALL(DestroyContext, &b);
      if (SUCCEEDED(hr)) s->contexts[a->hContext-1]=NULL;
      return hr;
   }
   case BC250_HOST_CreateSynchronizationObject2: {
      auto *a=(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *)argument;
      if (a->Info.Flags.Shared || a->Info.Flags.NtSecuritySharing) return E_NOTIMPL;
      D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 b = {};
      b.Info=a->Info;
      HRESULT hr=HOST_CALL(CreateSynchronizationObject2, &b);
      a->Info=b.Info; a->hSyncObject=b.hSyncObject; return hr;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT b = {};
      b.hSyncObject=((D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *)argument)->hSyncObject;
      return HOST_CALL(DestroySynchronizationObject, &b);
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      auto *a=(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *)argument;
      D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.FenceValueArray=a->FenceValueArray; b.hAsyncEvent=a->hAsyncEvent; b.Flags=a->Flags;
      return HOST_CALL(WaitForSynchronizationObjectFromCpu, &b);
   }
   case BC250_HOST_SignalSynchronizationObjectFromCpu: {
      auto *a=(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *)argument;
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray; b.FenceValueArray=a->FenceValueArray;
      return HOST_CALL(SignalSynchronizationObjectFromCpu, &b);
   }
   case BC250_HOST_WaitForSynchronizationObjectFromGpu: {
      auto *a=(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *)argument;
      D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU b = {};
      b.hContext=Bc250HostContext(s,a->hContext); if (!b.hContext) return E_INVALIDARG;
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.MonitoredFenceValueArray=a->MonitoredFenceValueArray;
      return HOST_CALL(WaitForSynchronizationObjectFromGpu, &b);
   }
   case BC250_HOST_SignalSynchronizationObjectFromGpu2: {
      auto *a=(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *)argument;
      if (a->BroadcastContextCount > D3DDDI_MAX_BROADCAST_CONTEXT || a->Flags.Value) return E_NOTIMPL;
      HANDLE contexts[D3DDDI_MAX_BROADCAST_CONTEXT] = {};
      for (UINT i=0;i<a->BroadcastContextCount;++i) {
         contexts[i]=Bc250HostContext(s,a->BroadcastContextArray[i]); if (!contexts[i]) return E_INVALIDARG;
      }
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.BroadcastContextCount=a->BroadcastContextCount; b.BroadcastContextArray=contexts;
      b.MonitoredFenceValueArray=a->MonitoredFenceValueArray;
      return HOST_CALL(SignalSynchronizationObjectFromGpu2, &b);
   }
   case BC250_HOST_SubmitCommand: {
      auto *a=(D3DKMT_SUBMITCOMMAND *)argument;
      if (a->BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a->NumPrimaries>D3DDDI_MAX_WRITTEN_PRIMARIES || a->Flags.NullRendering || a->Flags.PresentRedirected || a->Flags.NoKmdAccess || a->Flags.Reserved || a->PresentHistoryToken || a->NumHistoryBuffers) return E_NOTIMPL;
      D3DDDICB_SUBMITCOMMAND b = {};
      b.Commands=a->Commands; b.CommandLength=a->CommandLength;
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      b.BroadcastContextCount=a->BroadcastContextCount;
      for (UINT i=0;i<a->BroadcastContextCount;++i) {
         b.BroadcastContext[i]=Bc250HostContext(s,a->BroadcastContext[i]); if (!b.BroadcastContext[i]) return E_INVALIDARG;
      }
      b.NumPrimaries=a->NumPrimaries;
      for (UINT i=0;i<a->NumPrimaries;++i) b.WrittenPrimaries[i]=a->WrittenPrimaries[i];
      return HOST_CALL(SubmitCommand, &b);
   }
   default: return E_NOTIMPL;
   }
#undef HOST_CALL
}

static int32_t Bc250HostDispatch(void *userdata, uint32_t operation, void *argument)
{
   auto *s=(Bc250HostProbeState *)userdata;
   if (GetCurrentThreadId()!=s->thread) {
      fprintf(stderr,"BC250 hosted wrong-thread op=%u\n",operation);
      return (int32_t)0xc000000d;
   }
   HRESULT hr=Bc250HostOperation(s,operation,argument);
   unsigned count=operation<64 ? ++s->calls[operation] : 0;
   if (count<=2 || FAILED(hr)) fprintf(stderr,"BC250 hosted op=%u count=%u hr=%08lx\n",operation,count,hr);
   if (hr==E_PENDING && (operation==BC250_HOST_MapGpuVirtualAddress || operation==BC250_HOST_MakeResident)) return 0x103;
   return SUCCEEDED(hr) ? 0 : hr==E_NOTIMPL ? (int32_t)0xc00000bb : (int32_t)0xc0000001;
}

extern "C" bool d3d10_hosted_bootstrap(struct bc250_host *host);

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

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
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



   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);
   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;

   // This Zink-only diagnostic DLL is loaded through D3D_DRIVER_TYPE_SOFTWARE.
   // That runtime cannot service native WDDM presentation callbacks. RADV owns
   // its rendering context; native primary sharing/presentation is a separate
   // prerequisite before this DLL may replace the system DWM UMD.
   if (GetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE", NULL, 0)) {
   // E26: DWM requests runtime synchronization while creating its primary,
   // before the first Present. Register a virtual context at device creation
   // so the runtime has a context for its broadcast synchronization callbacks.
   fprintf(stderr,"BC250 D3D device stage 0\n"); fflush(stderr);
   if (!pCreateData->pKTCallbacks->pfnCreateContextVirtualCb) return E_NOTIMPL;
   D3DDDICB_CREATECONTEXTVIRTUAL context = {};
   context.EngineAffinity = 1;
   fprintf(stderr,"BC250 D3D device stage 1\n"); fflush(stderr);
   HRESULT contextResult = pCreateData->pKTCallbacks->pfnCreateContextVirtualCb(
       (HANDLE)pCreateData->hRTDevice.handle, &context);
   DebugPrintf("BC250 initial CreateContextVirtual %08lx\n", contextResult);
   if (FAILED(contextResult)) return contextResult;
   pDevice->hContext = context.hContext;

   }

   fprintf(stderr,"BC250 D3D device stage 2\n"); fflush(stderr);
   // Adapter screen remains capability-only for this prototype. Rendering
   // screens belong to one runtime device so future hosted callbacks cannot
   // accidentally be inherited from a different D3D device.
   if (GetEnvironmentVariableA("BC250_HOSTED_ICD", NULL, 0)) {
      Bc250HostProbeState state = {pDevice, GetCurrentThreadId()};
      bc250_host host = {};
      host.sType = BC250_HOST_STYPE; host.version = BC250_HOST_VERSION;
      host.size = sizeof(host); host.identity = pDevice->hDevice;
      host.userdata = &state; host.dispatch = Bc250HostDispatch;
      if (!d3d10_hosted_bootstrap(&host)) return E_FAIL;
   }
   pDevice->owned_screen = d3d10_create_screen();
   if (!pDevice->owned_screen) return E_OUTOFMEMORY;
   struct pipe_screen *screen = pDevice->owned_screen;
   fprintf(stderr, "BC250 device screen=%p runtime=%p\n", screen, pDevice->hDevice);
   DebugPrintf("BC250 Renderer: %s\n", screen->get_name(screen));
   fprintf(stderr,"BC250 D3D device stage 3\n"); fflush(stderr);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      screen->destroy(screen);
      pDevice->owned_screen = NULL;
      return E_OUTOFMEMORY;
   }
   pDevice->pipe = pipe;
   static const float zero_vertex[4] = {0, 0, 0, 0};
   pDevice->zero_vertex_buffer = pipe_buffer_create_with_data(
       pipe, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_IMMUTABLE,
       sizeof(zero_vertex), zero_vertex);
   if (!pDevice->zero_vertex_buffer) {
      pipe->destroy(pipe);
      return E_OUTOFMEMORY;
   }
   fprintf(stderr,"BC250 D3D device stage 4\n"); fflush(stderr);
   pDevice->cso = cso_create_context(pipe, CSO_NO_VBUF);

   fprintf(stderr,"BC250 D3D device stage 5\n"); fflush(stderr);
   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   fprintf(stderr,"BC250 D3D device stage 6\n"); fflush(stderr);
   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);

   fprintf(stderr,"BC250 D3D device stage 7\n"); fflush(stderr);
   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   fprintf(stderr,"BC250 D3D device stage 8\n"); fflush(stderr);


   pDevice->draw_so_target = NULL;

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
   D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
   pDeviceFuncs->pfnVsSetShader = VsSetShader;
   pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
   pDeviceFuncs->pfnDraw = Draw;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
   pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   pDeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
   pDeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
   pDeviceFuncs->pfnDrawInstanced = DrawInstanced;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
   pDeviceFuncs->pfnGsSetShader = GsSetShader;
   pDeviceFuncs->pfnIaSetTopology = IaSetTopology;
   pDeviceFuncs->pfnStagingResourceMap = ResourceMap;
   pDeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
   pDeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
   pDeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
   pDeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
   pDeviceFuncs->pfnSetRenderTargets = SetRenderTargets;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = ShaderResourceViewReadAfterWriteHazard;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = ResourceReadAfterWriteHazard;
   pDeviceFuncs->pfnSetBlendState = SetBlendState;
   pDeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
   pDeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
   pDeviceFuncs->pfnQueryEnd = QueryEnd;
   pDeviceFuncs->pfnQueryBegin = QueryBegin;
   pDeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnSoSetTargets = SoSetTargets;
   pDeviceFuncs->pfnDrawAuto = DrawAuto;
   pDeviceFuncs->pfnSetViewports = SetViewports;
   pDeviceFuncs->pfnSetScissorRects = SetScissorRects;
   pDeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
   pDeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
   pDeviceFuncs->pfnSetPredication = SetPredication;
   pDeviceFuncs->pfnQueryGetData = QueryGetData;
   pDeviceFuncs->pfnFlush = Flush;
   pDeviceFuncs->pfnGenMips = GenMips;
   pDeviceFuncs->pfnResourceCopy = ResourceCopy;
   pDeviceFuncs->pfnResourceResolveSubresource = ResourceResolveSubResource;
   pDeviceFuncs->pfnResourceMap = ResourceMap;
   pDeviceFuncs->pfnResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
   pDeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
   pDeviceFuncs->pfnCalcPrivateResourceSize = CalcPrivateResourceSize;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = CalcPrivateOpenedResourceSize;
   pDeviceFuncs->pfnCreateResource = CreateResource;
   pDeviceFuncs->pfnOpenResource = OpenResource;
   pDeviceFuncs->pfnDestroyResource = DestroyResource;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize;
   pDeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView;
   pDeviceFuncs->pfnDestroyShaderResourceView = DestroyShaderResourceView;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = CalcPrivateRenderTargetViewSize;
   pDeviceFuncs->pfnCreateRenderTargetView = CreateRenderTargetView;
   pDeviceFuncs->pfnDestroyRenderTargetView = DestroyRenderTargetView;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize;
   pDeviceFuncs->pfnCreateDepthStencilView = CreateDepthStencilView;
   pDeviceFuncs->pfnDestroyDepthStencilView = DestroyDepthStencilView;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
   pDeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
   pDeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
   pDeviceFuncs->pfnCreateBlendState = CreateBlendState;
   pDeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
   pDeviceFuncs->pfnCreateDepthStencilState = CreateDepthStencilState;
   pDeviceFuncs->pfnDestroyDepthStencilState = DestroyDepthStencilState;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
   pDeviceFuncs->pfnCreateRasterizerState = CreateRasterizerState;
   pDeviceFuncs->pfnDestroyRasterizerState = DestroyRasterizerState;
   pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   pDeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
   pDeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
   pDeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnDestroyShader = DestroyShader;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
   pDeviceFuncs->pfnCreateSampler = CreateSampler;
   pDeviceFuncs->pfnDestroySampler = DestroySampler;
   pDeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   pDeviceFuncs->pfnCreateQuery = CreateQuery;
   pDeviceFuncs->pfnDestroyQuery = DestroyQuery;
   pDeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
   pDeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
   pDeviceFuncs->pfnCheckCounter = CheckCounter;
   pDeviceFuncs->pfnDestroyDevice = DestroyDevice;
   pDeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs1;
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize1;
      p10_1DeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView1;
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize1;
      p10_1DeviceFuncs->pfnCreateBlendState = CreateBlendState1;
      p10_1DeviceFuncs->pfnResourceConvert = ResourceCopy;
      p10_1DeviceFuncs->pfnResourceConvertRegion = ResourceCopyRegion;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      _Present;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      _GetGammaCaps;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      _SetDisplayMode;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      _SetResourcePriority;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      _QueryResourceResidency;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      _RotateResourceIdentities;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      _Blt;

   // E26: the linear shared-resource path is implemented for the tested formats.
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

   fprintf(stderr,"D3D destroy stage 0\n"); fflush(stderr);
   pipe->flush(pipe, NULL, 0);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   fprintf(stderr,"D3D destroy stage 1\n"); fflush(stderr);
   cso_unbind_context(pDevice->cso);
   fprintf(stderr,"D3D destroy stage 2\n"); fflush(stderr);
   cso_destroy_context(pDevice->cso);

   fprintf(stderr,"D3D destroy stage 3\n"); fflush(stderr);
   DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);

   fprintf(stderr,"D3D destroy stage 4\n"); fflush(stderr);
   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   fprintf(stderr,"D3D destroy stage 5\n"); fflush(stderr);
   pipe_resource_reference(&pDevice->zero_vertex_buffer, NULL);
   pipe_resource_reference(&pDevice->index_buffer, NULL);

   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   memset(sampler_views, 0, sizeof sampler_views);
   fprintf(stderr,"D3D destroy stage 6\n"); fflush(stderr);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

   fprintf(stderr,"D3D destroy stage 7\n"); fflush(stderr);
   pipe->destroy(pipe);
   pDevice->pipe = NULL;
   if (pDevice->owned_screen) {
      fprintf(stderr, "BC250 destroy device screen=%p runtime=%p\n", pDevice->owned_screen, pDevice->hDevice);
      pDevice->owned_screen->destroy(pDevice->owned_screen);
      pDevice->owned_screen = NULL;
   }
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
   fprintf(stderr,"D3D destroy stage 8\n"); fflush(stderr);
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
