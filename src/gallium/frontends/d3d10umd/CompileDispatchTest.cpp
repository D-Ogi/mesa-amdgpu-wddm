/* SPDX-License-Identifier: MIT */
/* Standalone host test. No adapter or GPU is opened. Uses production DDI
 * wrappers with WDK-typed deterministic bodies, not a model of the dispatcher. */
#include <windows.h>
#include <cstdio>
#include <cstdlib>
static volatile LONG allocations, fail_at = -1;
static bool allocation_allowed() {
   LONG n = InterlockedIncrement(&allocations);
   LONG fail = InterlockedCompareExchange(&fail_at, 0, 0);
   return fail < 0 || n < fail;
}
#define BC250_DISPATCH_ALLOCATION_ALLOWED() allocation_allowed()
#include "CompileDispatch.h"

#define require(ok) do { if (!(ok)) { std::fprintf(stderr, "FAIL line %d\n", __LINE__); std::abort(); } } while (0)
static DWORD fls;
static thread_local unsigned callback_depth;
static thread_local unsigned runtime_marker;
static volatile LONG callbacks, destroyed, bodies;
struct Device {
   Bc250CompileDispatch *compile_dispatch = nullptr;
   D3D10DDI_CORELAYER_DEVICECALLBACKS um{};
   D3DDDI_DEVICECALLBACKS kt{};
   DXGI_DDI_BASE_CALLBACKS *dxgi = nullptr;
   DWORD caller = 0;
   HRESULT error = S_OK;
   unsigned seen = 0;
};
static Device *device(D3D10DDI_HDEVICE h) { return static_cast<Device *>(h.pDrvPrivate); }
struct Traits {
   static Device *device(D3D10DDI_HDEVICE h) { return ::device(h); }
   struct Scope { explicit Scope(Device *) {} };
};
static __declspec(noinline) void deep(unsigned n) {
   volatile unsigned char bytes[16384];
   for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<unsigned char>(i + n);
   if (n) deep(n - 1);
   require(bytes[n] == static_cast<unsigned char>(2 * n));
}
static void APIENTRY set_shader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
using SetShader = Bc250DdiEntry<set_shader, Traits, true>;
static void APIENTRY error_cb(D3D10DDI_HRTCORELAYER h, HRESULT hr) {
   auto *d = reinterpret_cast<Device *>(h.handle);
   require(GetCurrentThreadId() == d->caller);
   require(runtime_marker == 0x5678 && FlsGetValue(fls) == d);
   require(fegetround() == FE_DOWNWARD);
   d->error = hr;
   InterlockedIncrement(&callbacks);
   // A runtime callback can synchronously reenter a device entry.
   if (!callback_depth++) SetShader::Call({d}, {});
   --callback_depth;
}
static HRESULT APIENTRY context_cb(HANDLE h, D3DDDICB_CREATECONTEXTVIRTUAL *c) {
   auto *d = static_cast<Device *>(h);
   error_cb({d}, S_OK);
   c->hContext = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(123));
   return S_OK;
}
static HRESULT APIENTRY present_cb(HANDLE h, DXGIDDICB_PRESENT *) {
   auto *d = static_cast<Device *>(h);
   require(GetCurrentThreadId() == d->caller && FlsGetValue(fls) == d);
   return S_FALSE;
}
static void common(D3D10DDI_HDEVICE h) {
   auto *d = device(h);
   require(GetCurrentThreadId() != d->caller && fegetround() == FE_DOWNWARD);
   require(Bc250CompileDispatch::active == d->compile_dispatch);
   deep(5);
   InterlockedIncrement(&bodies);
}
static void APIENTRY set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER) {
   common(h);
   device(h)->seen |= 1;
}
static void APIENTRY map_resource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE, UINT,
                                 D3D10_DDI_MAP, UINT, D3D10DDI_MAPPED_SUBRESOURCE *out) {
   common(h);
   auto *d = device(h);
   out->pData = d;
   d->seen |= 2;
   d->um.pfnSetErrorCb({d}, S_OK);
}
static void APIENTRY get_query(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY, void *out, UINT n, UINT) {
   common(h);
   require(n == sizeof(UINT));
   *static_cast<UINT *>(out) = 42;
   device(h)->seen |= 4;
}
static void APIENTRY destroy_device(D3D10DDI_HDEVICE h) {
   common(h);
   auto *d = device(h);
   d->um.pfnSetErrorCb({d}, S_OK);
   require(d->dxgi->pfnPresentCb(d, nullptr) == S_FALSE);
   d->seen |= 8;
   InterlockedIncrement(&destroyed);
}
static HRESULT APIENTRY create_device(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE *info) {
   auto *d = device(info->hDrvDevice);
   d->compile_dispatch = Bc250CompileDispatch::active;
   d->um = *info->pUMCallbacks;
   d->kt = *info->pKTCallbacks;
   d->dxgi = info->DXGIBaseDDI.pDXGIBaseCallbacks;
   common(info->hDrvDevice);
   require(d->um.pfnStateVsShaderCb == nullptr && d->kt.pfnLockCb == nullptr);
   D3DDDICB_CREATECONTEXTVIRTUAL context{};
   require(SUCCEEDED(d->kt.pfnCreateContextVirtualCb(d, &context)));
   return S_OK;
}
using CreateDevice = Bc250CreateDeviceEntry<create_device>;
using Map = Bc250DdiEntry<map_resource, Traits, true>;
using Query = Bc250DdiEntry<get_query, Traits, true>;
using Destroy = Bc250DdiEntry<destroy_device, Traits, true, true>;
static void runtime_thread(Device &d) {
   d.caller = GetCurrentThreadId();
   runtime_marker = 0x5678;
   require(FlsSetValue(fls, &d) != FALSE && fesetround(FE_DOWNWARD) == 0);
}
static void create(Device &d) {
   runtime_thread(d);
   D3DDDI_DEVICECALLBACKS kt{};
   D3D10DDI_CORELAYER_DEVICECALLBACKS um{};
   DXGI_DDI_BASE_CALLBACKS dxgi{};
   kt.pfnCreateContextVirtualCb = context_cb;
   um.pfnSetErrorCb = error_cb;
   dxgi.pfnPresentCb = present_cb;
   D3D10DDIARG_CREATEDEVICE info{};
   info.hDrvDevice = {&d}; info.pKTCallbacks = &kt; info.pUMCallbacks = &um;
   info.DXGIBaseDDI.pDXGIBaseCallbacks = &dxgi;
   require(CreateDevice::Call({}, &info) == S_OK);
}
static unsigned __stdcall caller(void *) {
   Device d;
   create(d);
   // Allocation failures AFTER CreateDevice must not bypass any DDI body.
   InterlockedExchange(&fail_at, 0);
   LONG before = allocations;
   D3D10DDI_DEVICEFUNCS table{};
   table.pfnPsSetShader = SetShader::Call;
   table.pfnResourceMap = Map::Call;
   table.pfnQueryGetData = Query::Call;
   table.pfnDestroyDevice = Destroy::Call;
   for (unsigned i = 0; i < 16; ++i) {
      table.pfnPsSetShader({&d}, {});
      D3D10DDI_MAPPED_SUBRESOURCE mapped{};
      table.pfnResourceMap({&d}, {}, 0, D3D10_DDI_MAP_READ, 0, &mapped);
      require(mapped.pData == &d);
      UINT value = 0;
      table.pfnQueryGetData({&d}, {}, &value, sizeof(value), 0);
      require(value == 42 && d.error == S_OK);
   }
   table.pfnDestroyDevice({&d});
   require(d.seen == 15 && d.compile_dispatch == nullptr && allocations == before);
   require(FlsGetValue(fls) == &d && runtime_marker == 0x5678 && fegetround() == FE_DOWNWARD);
   FlsSetValue(fls, nullptr);
   return 0;
}
static unsigned __stdcall migrated_caller(void *arg) {
   auto &d = *static_cast<Device *>(arg);
   runtime_thread(d);
   for (unsigned i = 0; i < 16; ++i) {
      D3D10DDI_MAPPED_SUBRESOURCE mapped{};
      Map::Call({&d}, {}, 0, D3D10_DDI_MAP_READ, 0, &mapped);
      require(mapped.pData == &d && d.error == S_OK);
   }
   FlsSetValue(fls, nullptr);
   return 0;
}
int main() {
   SetErrorMode(SEM_NOGPFAULTERRORBOX);
   fls = FlsAlloc(nullptr);
   require(fls != FLS_OUT_OF_INDEXES);
   D3D10DDIARG_CREATEDEVICE info{};
   // All four setup allocations fail through the actual CreateDevice wrapper.
   // The DDI implementation must not run and no worker/event handles may leak.
   DWORD baseline = 0, after = 0;
   require(GetProcessHandleCount(GetCurrentProcess(), &baseline) != FALSE);
   for (LONG point = 1; point <= 4; ++point) {
      InterlockedExchange(&allocations, 0); InterlockedExchange(&fail_at, point);
      require(CreateDevice::Call({}, &info) == E_OUTOFMEMORY);
      require(bodies == 0);
   }
   require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after == baseline);
   // Each new 64 KiB runtime thread has no prior fiber conversion or setup.
   for (unsigned i = 0; i < 8; ++i) {
      InterlockedExchange(&fail_at, -1);
      HANDLE h = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 65536, caller, nullptr,
                                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
      require(h != nullptr && WaitForSingleObject(h, 10000) == WAIT_OBJECT_0);
      DWORD result = 1;
      require(GetExitCodeThread(h, &result) != FALSE && result == 0);
      CloseHandle(h);
   }
   require(destroyed == 8 && callbacks >= 8 * 18);
   // Devices made on this thread, then used on entirely new runtime threads,
   // with two devices in flight independently. No stack setup may be retried.
   Device devices[2];
   InterlockedExchange(&fail_at, -1);
   for (auto &d : devices) create(d);
   InterlockedExchange(&fail_at, 0);
   LONG before = allocations;
   for (unsigned iteration = 0; iteration < 4; ++iteration) {
      HANDLE threads[2];
      for (unsigned i = 0; i < 2; ++i) {
         threads[i] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 65536, migrated_caller, &devices[i],
                                                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
         require(threads[i] != nullptr);
      }
      require(WaitForMultipleObjects(2, threads, TRUE, 10000) == WAIT_OBJECT_0);
      for (HANDLE h : threads) {
         DWORD result = 1;
         require(GetExitCodeThread(h, &result) != FALSE && result == 0);
         CloseHandle(h);
      }
   }
   for (auto &d : devices) { runtime_thread(d); Destroy::Call({&d}); }
   require(allocations == before && destroyed == 10);
   FlsFree(fls);
   require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after == baseline);
   std::printf("PASS: real DDI wrappers; CreateDevice OOM x4, 8 fresh 64KiB callers; SetShader/Map/Query/Destroy under OOM; callback thread/FLS/TLS/FP, reentry, full teardown; 2 concurrent devices migrate across 8 new callers\n");
   return 0;
}
