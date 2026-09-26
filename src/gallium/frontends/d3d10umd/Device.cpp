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
extern "C" struct pipe_screen *d3d10_create_hosted_screen(struct bc250_host *host);


#include <d3dkmthk.h>

static thread_local Device *Bc250RuntimeDevice;
struct Bc250RuntimeScope {
   Device *previous;
   explicit Bc250RuntimeScope(Device *device) : previous(Bc250RuntimeDevice) { Bc250RuntimeDevice=device; }
   ~Bc250RuntimeScope() { Bc250RuntimeDevice=previous; }
};

static Device *Bc250EntryDevice(D3D10DDI_HDEVICE h) { return CastDevice(h); }
template <typename A> static Device *Bc250EntryDevice(A *a) { return CastDevice(a->hDevice); }
template <auto F> struct Bc250Entry;
template <typename R, typename A, typename... Rest, R (APIENTRY *F)(A, Rest...)>
struct Bc250Entry<F> {
   static R APIENTRY Call(A a, Rest... rest) {
      Bc250RuntimeScope scope(Bc250EntryDevice(a));
      return F(a, rest...);
   }
};

struct Bc250HostProbeState {
   Device *device;
   DWORD thread;
   HANDLE contexts[16];
   unsigned calls[64];
   bc250_host_progress progress[16];
   bool submission_failed;
   D3DKMT_HANDLE present_sync;
   UINT64 present_value, present_waited[16];
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
   case BC250_HOST_PUBLISH_PROGRESS: {
      auto *a=(bc250_host_progress *)argument;
      if (!Bc250HostContext(s,a->context) || !a->sync || !a->value) return E_INVALIDARG;
      auto &old=s->progress[a->context-1];
      if (old.sync && (old.sync!=a->sync || old.value>=a->value)) return E_INVALIDARG;
      old=*a;
      return S_OK;
   }
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
      if (SUCCEEDED(hr)) { s->contexts[slot]=b.hContext; s->present_waited[slot]=0; a->hContext=slot+1; }
      return hr;
   }
   case BC250_HOST_DestroyContext: {
      auto *a=(D3DKMT_DESTROYCONTEXT *)argument;
      D3DDDICB_DESTROYCONTEXT b = {};
      b.hContext=Bc250HostContext(s,a->hContext);
      if (!b.hContext) return E_INVALIDARG;
      HRESULT hr=HOST_CALL(DestroyContext, &b);
      if (SUCCEEDED(hr)) {
         s->contexts[a->hContext-1]=NULL;
         s->progress[a->hContext-1]={};
      }
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
      HRESULT hr=HOST_CALL(DestroySynchronizationObject, &b);
      if (SUCCEEDED(hr)) for (auto &p:s->progress) if (p.sync==b.hSyncObject) p={};
      return hr;
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
         UINT token=a->BroadcastContext[i];
         if (s->present_value>s->present_waited[token-1]) {
            D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait={};
            wait.hContext=b.BroadcastContext[i]; wait.ObjectCount=1;
            wait.ObjectHandleArray=&s->present_sync; wait.MonitoredFenceValueArray=&s->present_value;
            HRESULT hr=HOST_CALL(WaitForSynchronizationObjectFromGpu, &wait);
            if (s->present_value<=3 || FAILED(hr)) fprintf(stderr,"BC250 render waits Present value=%llu hr=%08lx\n",(unsigned long long)s->present_value,hr);
            if (FAILED(hr)) return hr;
            s->present_waited[token-1]=s->present_value;
         }
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
   if (Bc250RuntimeDevice!=s->device) {
      fprintf(stderr,"BC250 hosted wrong-thread op=%u\n",operation);
      return (int32_t)0xc000000d;
   }
   HRESULT hr=Bc250HostOperation(s,operation,argument);
   if (FAILED(hr) && (operation==BC250_HOST_SubmitCommand || operation==BC250_HOST_SignalSynchronizationObjectFromGpu2 || operation==BC250_HOST_PUBLISH_PROGRESS))
      s->submission_failed=true;
   unsigned count=operation<64 ? ++s->calls[operation] : 0;
   if (count<=2 || (FAILED(hr) && hr!=E_PENDING)) fprintf(stderr,"BC250 hosted op=%u count=%u hr=%08lx\n",operation,count,hr);
   if (hr==E_PENDING && (operation==BC250_HOST_MapGpuVirtualAddress || operation==BC250_HOST_MakeResident)) return 0x103;
   return SUCCEEDED(hr) ? 0 : hr==E_NOTIMPL ? (int32_t)0xc00000bb : (int32_t)0xc0000001;
}

HRESULT Bc250QueuePresentWait(Device *device)
{
   auto *s=(Bc250HostProbeState *)device->hosted_state;
   if (!s || Bc250RuntimeDevice!=device || !device->hContext) return E_INVALIDARG;
   if (s->submission_failed) return DXGI_ERROR_DEVICE_REMOVED;
   if (!device->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb ||
       !device->KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb ||
       !device->KTCallbacks.pfnCreateSynchronizationObject2Cb) return E_NOTIMPL;
   if (!s->present_sync) {
      D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 create={};
      create.Info.Type=D3DDDI_MONITORED_FENCE;
      create.Info.MonitoredFence.EngineAffinity=1;
      HRESULT hr=device->KTCallbacks.pfnCreateSynchronizationObject2Cb(device->hDevice,&create);
      if (FAILED(hr)) return hr;
      s->present_sync=create.hSyncObject;
   }
   D3DKMT_HANDLE objects[16]={};
   UINT64 values[16]={};
   UINT count=0;
   for (const auto &p:s->progress) {
      if (!p.sync) continue;
      if (!Bc250HostContext(s,p.context)) return E_FAIL;
      objects[count]=p.sync; values[count]=p.value; ++count;
   }
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait={};
   wait.hContext=device->hContext; wait.ObjectCount=count;
   wait.ObjectHandleArray=objects; wait.MonitoredFenceValueArray=values;
   HRESULT hr=count ? device->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb(device->hDevice,&wait) : S_OK;
   if (device->profilePresents<3 || FAILED(hr)) {
      fprintf(stderr,"BC250 Present GPU wait count=%u hr=%08lx cpu_render_wait=0\n",count,hr);
      for (UINT i=0;i<count;++i) fprintf(stderr,"BC250 Present fence=%x value=%llu\n",objects[i],(unsigned long long)values[i]);
   }
   return hr;
}

HRESULT Bc250SignalPresent(Device *device)
{
   auto *s=(Bc250HostProbeState *)device->hosted_state;
   if (!s || Bc250RuntimeDevice!=device || !s->present_sync) return E_INVALIDARG;
   UINT64 value=s->present_value+1;
   if (!value) return E_FAIL;
   D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal={};
   signal.ObjectCount=1; signal.ObjectHandleArray=&s->present_sync;
   signal.BroadcastContextCount=1; signal.BroadcastContextArray=&device->hContext;
   signal.MonitoredFenceValueArray=&value;
   HRESULT hr=device->KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb(device->hDevice,&signal);
   if (SUCCEEDED(hr)) s->present_value=value;
   else s->submission_failed=true;
   if (value<=3 || FAILED(hr)) fprintf(stderr,"BC250 Present signals value=%llu hr=%08lx\n",(unsigned long long)value,hr);
   return hr;
}

HRESULT Bc250WaitPresentIdle(Device *device)
{
   auto *s=(Bc250HostProbeState *)device->hosted_state;
   if (!s || !s->present_value) return S_OK;
   if (Bc250RuntimeDevice!=device || !device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb) return E_INVALIDARG;
   HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);
   if (!event) return HRESULT_FROM_WIN32(GetLastError());
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait={};
   wait.ObjectCount=1; wait.ObjectHandleArray=&s->present_sync; wait.FenceValueArray=&s->present_value;
   wait.hAsyncEvent=event;
   HRESULT hr=device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(device->hDevice,&wait);
   if (SUCCEEDED(hr) && WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) hr=DXGI_ERROR_DEVICE_HUNG;
   CloseHandle(event);
   return hr;
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
   Bc250RuntimeScope runtimeScope(pDevice);
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
   const bool hostedRender=GetEnvironmentVariableA("BC250_HOSTED_RENDER", NULL, 0)!=0;
   if (!hostedRender && GetEnvironmentVariableA("BC250_HOSTED_ICD", NULL, 0)) {
      Bc250HostProbeState state = {pDevice, GetCurrentThreadId()};
      bc250_host host = {};
      host.sType = BC250_HOST_STYPE; host.version = BC250_HOST_VERSION;
      host.size = sizeof(host); host.identity = pDevice->hDevice;
      host.userdata = &state; host.dispatch = Bc250HostDispatch;
      if (!d3d10_hosted_bootstrap(&host)) return E_FAIL;
   }
   if (hostedRender) {
      auto *state=new Bc250HostProbeState{};
      state->device=pDevice;
      state->thread=GetCurrentThreadId();
      pDevice->hosted_state=state;
      bc250_host host={};
      host.sType=BC250_HOST_STYPE; host.version=BC250_HOST_VERSION;
      host.size=sizeof(host); host.identity=pDevice->hDevice;
      host.userdata=state; host.dispatch=Bc250HostDispatch;
      pDevice->owned_screen=d3d10_create_hosted_screen(&host);
      if (!pDevice->owned_screen) { delete state; pDevice->hosted_state=NULL; }
   } else {
      pDevice->owned_screen = d3d10_create_screen();
   }
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
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = Bc250Entry<ResourceUpdateSubResourceUP>::Call;
   pDeviceFuncs->pfnVsSetConstantBuffers = Bc250Entry<VsSetConstantBuffers>::Call;
   pDeviceFuncs->pfnPsSetShaderResources = Bc250Entry<PsSetShaderResources>::Call;
   pDeviceFuncs->pfnPsSetShader = Bc250Entry<PsSetShader>::Call;
   pDeviceFuncs->pfnPsSetSamplers = Bc250Entry<PsSetSamplers>::Call;
   pDeviceFuncs->pfnVsSetShader = Bc250Entry<VsSetShader>::Call;
   pDeviceFuncs->pfnDrawIndexed = Bc250Entry<DrawIndexed>::Call;
   pDeviceFuncs->pfnDraw = Bc250Entry<Draw>::Call;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicIABufferUnmap = Bc250Entry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = Bc250Entry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnPsSetConstantBuffers = Bc250Entry<PsSetConstantBuffers>::Call;
   pDeviceFuncs->pfnIaSetInputLayout = Bc250Entry<IaSetInputLayout>::Call;
   pDeviceFuncs->pfnIaSetVertexBuffers = Bc250Entry<IaSetVertexBuffers>::Call;
   pDeviceFuncs->pfnIaSetIndexBuffer = Bc250Entry<IaSetIndexBuffer>::Call;
   pDeviceFuncs->pfnDrawIndexedInstanced = Bc250Entry<DrawIndexedInstanced>::Call;
   pDeviceFuncs->pfnDrawInstanced = Bc250Entry<DrawInstanced>::Call;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnDynamicResourceUnmap = Bc250Entry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnGsSetConstantBuffers = Bc250Entry<GsSetConstantBuffers>::Call;
   pDeviceFuncs->pfnGsSetShader = Bc250Entry<GsSetShader>::Call;
   pDeviceFuncs->pfnIaSetTopology = Bc250Entry<IaSetTopology>::Call;
   pDeviceFuncs->pfnStagingResourceMap = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnStagingResourceUnmap = Bc250Entry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnVsSetShaderResources = Bc250Entry<VsSetShaderResources>::Call;
   pDeviceFuncs->pfnVsSetSamplers = Bc250Entry<VsSetSamplers>::Call;
   pDeviceFuncs->pfnGsSetShaderResources = Bc250Entry<GsSetShaderResources>::Call;
   pDeviceFuncs->pfnGsSetSamplers = Bc250Entry<GsSetSamplers>::Call;
   pDeviceFuncs->pfnSetRenderTargets = Bc250Entry<SetRenderTargets>::Call;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = Bc250Entry<ShaderResourceViewReadAfterWriteHazard>::Call;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = Bc250Entry<ResourceReadAfterWriteHazard>::Call;
   pDeviceFuncs->pfnSetBlendState = Bc250Entry<SetBlendState>::Call;
   pDeviceFuncs->pfnSetDepthStencilState = Bc250Entry<SetDepthStencilState>::Call;
   pDeviceFuncs->pfnSetRasterizerState = Bc250Entry<SetRasterizerState>::Call;
   pDeviceFuncs->pfnQueryEnd = Bc250Entry<QueryEnd>::Call;
   pDeviceFuncs->pfnQueryBegin = Bc250Entry<QueryBegin>::Call;
   pDeviceFuncs->pfnResourceCopyRegion = Bc250Entry<ResourceCopyRegion>::Call;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = Bc250Entry<ResourceUpdateSubResourceUP>::Call;
   pDeviceFuncs->pfnSoSetTargets = Bc250Entry<SoSetTargets>::Call;
   pDeviceFuncs->pfnDrawAuto = Bc250Entry<DrawAuto>::Call;
   pDeviceFuncs->pfnSetViewports = Bc250Entry<SetViewports>::Call;
   pDeviceFuncs->pfnSetScissorRects = Bc250Entry<SetScissorRects>::Call;
   pDeviceFuncs->pfnClearRenderTargetView = Bc250Entry<ClearRenderTargetView>::Call;
   pDeviceFuncs->pfnClearDepthStencilView = Bc250Entry<ClearDepthStencilView>::Call;
   pDeviceFuncs->pfnSetPredication = Bc250Entry<SetPredication>::Call;
   pDeviceFuncs->pfnQueryGetData = Bc250Entry<QueryGetData>::Call;
   pDeviceFuncs->pfnFlush = Bc250Entry<Flush>::Call;
   pDeviceFuncs->pfnGenMips = Bc250Entry<GenMips>::Call;
   pDeviceFuncs->pfnResourceCopy = Bc250Entry<ResourceCopy>::Call;
   pDeviceFuncs->pfnResourceResolveSubresource = Bc250Entry<ResourceResolveSubResource>::Call;
   pDeviceFuncs->pfnResourceMap = Bc250Entry<ResourceMap>::Call;
   pDeviceFuncs->pfnResourceUnmap = Bc250Entry<ResourceUnmap>::Call;
   pDeviceFuncs->pfnResourceIsStagingBusy = Bc250Entry<ResourceIsStagingBusy>::Call;
   pDeviceFuncs->pfnRelocateDeviceFuncs = Bc250Entry<RelocateDeviceFuncs>::Call;
   pDeviceFuncs->pfnCalcPrivateResourceSize = Bc250Entry<CalcPrivateResourceSize>::Call;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = Bc250Entry<CalcPrivateOpenedResourceSize>::Call;
   pDeviceFuncs->pfnCreateResource = Bc250Entry<CreateResource>::Call;
   pDeviceFuncs->pfnOpenResource = Bc250Entry<OpenResource>::Call;
   pDeviceFuncs->pfnDestroyResource = Bc250Entry<DestroyResource>::Call;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = Bc250Entry<CalcPrivateShaderResourceViewSize>::Call;
   pDeviceFuncs->pfnCreateShaderResourceView = Bc250Entry<CreateShaderResourceView>::Call;
   pDeviceFuncs->pfnDestroyShaderResourceView = Bc250Entry<DestroyShaderResourceView>::Call;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = Bc250Entry<CalcPrivateRenderTargetViewSize>::Call;
   pDeviceFuncs->pfnCreateRenderTargetView = Bc250Entry<CreateRenderTargetView>::Call;
   pDeviceFuncs->pfnDestroyRenderTargetView = Bc250Entry<DestroyRenderTargetView>::Call;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = Bc250Entry<CalcPrivateDepthStencilViewSize>::Call;
   pDeviceFuncs->pfnCreateDepthStencilView = Bc250Entry<CreateDepthStencilView>::Call;
   pDeviceFuncs->pfnDestroyDepthStencilView = Bc250Entry<DestroyDepthStencilView>::Call;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = Bc250Entry<CalcPrivateElementLayoutSize>::Call;
   pDeviceFuncs->pfnCreateElementLayout = Bc250Entry<CreateElementLayout>::Call;
   pDeviceFuncs->pfnDestroyElementLayout = Bc250Entry<DestroyElementLayout>::Call;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = Bc250Entry<CalcPrivateBlendStateSize>::Call;
   pDeviceFuncs->pfnCreateBlendState = Bc250Entry<CreateBlendState>::Call;
   pDeviceFuncs->pfnDestroyBlendState = Bc250Entry<DestroyBlendState>::Call;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = Bc250Entry<CalcPrivateDepthStencilStateSize>::Call;
   pDeviceFuncs->pfnCreateDepthStencilState = Bc250Entry<CreateDepthStencilState>::Call;
   pDeviceFuncs->pfnDestroyDepthStencilState = Bc250Entry<DestroyDepthStencilState>::Call;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = Bc250Entry<CalcPrivateRasterizerStateSize>::Call;
   pDeviceFuncs->pfnCreateRasterizerState = Bc250Entry<CreateRasterizerState>::Call;
   pDeviceFuncs->pfnDestroyRasterizerState = Bc250Entry<DestroyRasterizerState>::Call;
   pDeviceFuncs->pfnCalcPrivateShaderSize = Bc250Entry<CalcPrivateShaderSize>::Call;
   pDeviceFuncs->pfnCreateVertexShader = Bc250Entry<CreateVertexShader>::Call;
   pDeviceFuncs->pfnCreateGeometryShader = Bc250Entry<CreateGeometryShader>::Call;
   pDeviceFuncs->pfnCreatePixelShader = Bc250Entry<CreatePixelShader>::Call;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = Bc250Entry<CalcPrivateGeometryShaderWithStreamOutput>::Call;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = Bc250Entry<CreateGeometryShaderWithStreamOutput>::Call;
   pDeviceFuncs->pfnDestroyShader = Bc250Entry<DestroyShader>::Call;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = Bc250Entry<CalcPrivateSamplerSize>::Call;
   pDeviceFuncs->pfnCreateSampler = Bc250Entry<CreateSampler>::Call;
   pDeviceFuncs->pfnDestroySampler = Bc250Entry<DestroySampler>::Call;
   pDeviceFuncs->pfnCalcPrivateQuerySize = Bc250Entry<CalcPrivateQuerySize>::Call;
   pDeviceFuncs->pfnCreateQuery = Bc250Entry<CreateQuery>::Call;
   pDeviceFuncs->pfnDestroyQuery = Bc250Entry<DestroyQuery>::Call;
   pDeviceFuncs->pfnCheckFormatSupport = Bc250Entry<CheckFormatSupport>::Call;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = Bc250Entry<CheckMultisampleQualityLevels>::Call;
   pDeviceFuncs->pfnCheckCounterInfo = Bc250Entry<CheckCounterInfo>::Call;
   pDeviceFuncs->pfnCheckCounter = Bc250Entry<CheckCounter>::Call;
   pDeviceFuncs->pfnDestroyDevice = Bc250Entry<DestroyDevice>::Call;
   pDeviceFuncs->pfnSetTextFilterSize = Bc250Entry<SetTextFilterSize>::Call;
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = Bc250Entry<RelocateDeviceFuncs1>::Call;
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = Bc250Entry<CalcPrivateShaderResourceViewSize1>::Call;
      p10_1DeviceFuncs->pfnCreateShaderResourceView = Bc250Entry<CreateShaderResourceView1>::Call;
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = Bc250Entry<CalcPrivateBlendStateSize1>::Call;
      p10_1DeviceFuncs->pfnCreateBlendState = Bc250Entry<CreateBlendState1>::Call;
      p10_1DeviceFuncs->pfnResourceConvert = Bc250Entry<ResourceCopy>::Call;
      p10_1DeviceFuncs->pfnResourceConvertRegion = Bc250Entry<ResourceCopyRegion>::Call;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      Bc250Entry<_Present>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      Bc250Entry<_GetGammaCaps>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      Bc250Entry<_SetDisplayMode>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      Bc250Entry<_SetResourcePriority>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      Bc250Entry<_QueryResourceResidency>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      Bc250Entry<_RotateResourceIdentities>::Call;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      Bc250Entry<_Blt>::Call;

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
   auto *hosted=(Bc250HostProbeState *)pDevice->hosted_state;
   if (hosted && hosted->present_sync) {
      HRESULT hr=Bc250WaitPresentIdle(pDevice);
      if (FAILED(hr)) { SetError(hDevice,hr); return; }
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT destroy={};
      destroy.hSyncObject=hosted->present_sync;
      hr=pDevice->KTCallbacks.pfnDestroySynchronizationObjectCb(pDevice->hDevice,&destroy);
      if (FAILED(hr)) { SetError(hDevice,hr); return; }
      hosted->present_sync=0;
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
   delete (Bc250HostProbeState *)pDevice->hosted_state;
   pDevice->hosted_state=NULL;
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
