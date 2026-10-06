/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of Bc250EnsureSurface in the hosted UMD, without a WDDM runtime
 * or a GPU: fake kernel callbacks and a pipe screen that only imports. A
 * failed surface setup leaves presentReady false, and the next Present or
 * SetDisplayMode calls it again. The retry must redo only the failed step and
 * the ones after it: one allocation, one VA range, one MakeResident and, on
 * the CPU branch, one Lock2 that DestroyResource balances (BD-037). Both
 * branches run: the hosted one imports the allocation at its GPU VA, the CPU
 * one imports the Lock2 mapping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "State.h"
#include "Resource.h"

#include "util/u_inlines.h"
#include "frontend/winsys_handle.h"

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); \
   printf(__VA_ARGS__); printf("\n"); } } while (0)

// Lock2 is counted per allocation, so that an unbalanced Lock2 shows.
struct FakeAllocation { D3DKMT_HANDLE handle; void *memory; UINT64 size; UINT locks; };
static FakeAllocation allocations[16];
static UINT allocationCount, allocateCalls, deallocateCalls, mapCalls, residentCalls, lockCalls;
// A failure the next MakeResident or Lock2 returns once, and the paging
// fence value MakeResident reports.
static HRESULT residentFailure, lockFailure;
static UINT64 residentFence = 1;
static volatile UINT64 pagingFence = 1;

static FakeAllocation *FindAllocation(D3DKMT_HANDLE handle)
{
   for (UINT i = 0; i < allocationCount; ++i)
      if (allocations[i].handle == handle) return &allocations[i];
   return NULL;
}

static HRESULT APIENTRY AllocateCb(HANDLE, D3DDDICB_ALLOCATE *allocate)
{
   allocateCalls++;
   D3DDDI_ALLOCATIONINFO2 *info = &allocate->pAllocationInfo2[0];
   UINT64 size;
   memcpy(&size, (const char *)info->pPrivateDriverData + 24, sizeof(size));
   FakeAllocation *a = &allocations[allocationCount++];
   a->handle = 0x40000000u + allocationCount * 4;
   a->memory = _aligned_malloc((size_t)size, 4096);
   a->size = size;
   info->hAllocation = a->handle;
   allocate->hKMResource = 0x50000000u;
   return S_OK;
}

static HRESULT APIENTRY Deallocate2Cb(HANDLE, const D3DDDICB_DEALLOCATE2 *) { deallocateCalls++; return S_OK; }

static HRESULT APIENTRY CreatePagingQueueCb(HANDLE, D3DDDICB_CREATEPAGINGQUEUE *queue)
{
   queue->hPagingQueue = 0x60000000u;
   queue->FenceValueCPUVirtualAddress = (VOID *)&pagingFence;
   return S_OK;
}

// Every map gets a new range, as from VidMm, so that a repeated map shows.
static HRESULT APIENTRY MapGpuVirtualAddressCb(HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *map)
{
   map->VirtualAddress = 0x100000000ull + 0x10000000ull * mapCalls++; map->PagingFenceValue = 1;
   return S_OK;
}

static HRESULT APIENTRY MakeResidentCb(HANDLE, D3DDDI_MAKERESIDENT *resident)
{
   residentCalls++;
   if (HRESULT hr = residentFailure) { residentFailure = S_OK; return hr; }
   resident->PagingFenceValue = residentFence;
   return S_OK;
}

static HRESULT APIENTRY Lock2Cb(HANDLE, D3DDDICB_LOCK2 *lock)
{
   lockCalls++;
   if (HRESULT hr = lockFailure) { lockFailure = S_OK; return hr; }
   FakeAllocation *a = FindAllocation(lock->hAllocation);
   if (!a) return E_INVALIDARG;
   a->locks++; lock->pData = a->memory;
   return S_OK;
}

static HRESULT APIENTRY Unlock2Cb(HANDLE, const D3DDDICB_UNLOCK2 *unlock)
{
   FakeAllocation *a = FindAllocation(unlock->hAllocation);
   if (!a || !a->locks) return E_INVALIDARG;
   a->locks--;
   return S_OK;
}

static void APIENTRY SetErrorCb(D3D10DDI_HRTCORELAYER, HRESULT) {}

// A pipe screen that imports a given number of failures, then succeeds, and
// keeps the last handle it was given.
static struct pipe_screen screen;
static struct pipe_context pipe;
static UINT importCalls, importFailures;
static struct winsys_handle lastImport;

static struct pipe_resource *NewTexture(const struct pipe_resource *templat)
{
   struct pipe_resource *r = (struct pipe_resource *)calloc(1, sizeof(*r));
   *r = *templat;
   pipe_reference_init(&r->reference, 1);
   r->screen = &screen;
   return r;
}

static struct pipe_resource *FakeFromHandle(struct pipe_screen *, const struct pipe_resource *templat,
                                            struct winsys_handle *handle, unsigned)
{
   importCalls++;
   lastImport = *handle;
   if (importFailures) { importFailures--; return NULL; }
   return NewTexture(templat);
}

static void FakeResourceDestroy(struct pipe_screen *, struct pipe_resource *resource) { free(resource); }

static void TestEnsureRetry(Device *device, bool hosted)
{
   const char *branch = hosted ? "hosted" : "CPU";
   device->hosted_state = hosted ? (void *)&screen : NULL;
   enum Fault { IMPORT, RESIDENT, PAGING, LOCK };
   const struct Case { Fault fault; const char *step; HRESULT want; } cases[] = {
      // A refused import is D3DDDIERR_APPLICATIONERROR, not E_OUTOFMEMORY: E_OUTOFMEMORY from here ends DWM
      // (BD-058, d4f23cb7adf7c3d8994aa41d4435d96bb13e95c1).
      {IMPORT, "resource_from_handle", D3DDDIERR_APPLICATIONERROR},
      {RESIDENT, "MakeResident", E_OUTOFMEMORY},
      {PAGING, "paging fence wait (5 s)", HRESULT_FROM_WIN32(WAIT_TIMEOUT)},
      {LOCK, "Lock2", E_FAIL},
   };
   UINT count = 0;
   for (const Case &c : cases) {
      if (hosted && c.fault == LOCK) continue;   // the hosted branch does not lock
      count++;
      // A resource CreateResource left to the first Present or SetDisplayMode.
      struct pipe_resource templat = {};
      templat.target = PIPE_TEXTURE_2D; templat.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      templat.width0 = 67; templat.height0 = 65; templat.depth0 = 1; templat.array_size = 1;
      templat.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
      Resource r;
      memset(&r, 0, sizeof(r));
      r.resource = NewTexture(&templat);
      r.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      r.hRTResource = (HANDLE)0x70000010;
      const UINT allocates = allocateCalls, maps = mapCalls, residents = residentCalls, locks = lockCalls,
                 imports = importCalls, deallocations = deallocateCalls;
      switch (c.fault) {
      case IMPORT: importFailures = 1; break;
      case RESIDENT: residentFailure = E_OUTOFMEMORY; break;
      case PAGING: residentFence = pagingFence + 1; break;
      case LOCK: lockFailure = E_FAIL; break;
      }
      HRESULT hr = Bc250EnsureSurface(device, &r);
      const UINT64 va = r.gpuVa;
      CHECK(hr == c.want && !r.presentReady && r.allocation && va, "%s, %s fails: %08lx, want %08lx", branch, c.step,
            hr, c.want);
      if (c.fault == PAGING) {
         CHECK(r.surfaceFence == residentFence, "%s, %s: fence %llu kept, want %llu", branch, c.step,
               r.surfaceFence, residentFence);
         pagingFence = residentFence;
         residentFence = 1;
      }
      hr = Bc250EnsureSurface(device, &r);
      FakeAllocation *a = FindAllocation(r.allocation);
      CHECK(hr == S_OK && r.presentReady && a, "%s, %s retried: %08lx", branch, c.step, hr);
      CHECK(allocateCalls == allocates + 1 && mapCalls == maps + 1 && r.gpuVa == va,
            "%s, %s retried: %u allocations, %u maps, VA %llx then %llx", branch, c.step, allocateCalls - allocates,
            mapCalls - maps, va, r.gpuVa);
      CHECK(residentCalls == residents + (c.fault == RESIDENT ? 2 : 1) &&
            importCalls == imports + (c.fault == IMPORT ? 2 : 1),
            "%s, %s retried: %u MakeResident, %u imports", branch, c.step, residentCalls - residents,
            importCalls - imports);
      if (hosted) {
         CHECK(lockCalls == locks && lastImport.type == WINSYS_HANDLE_TYPE_FD && lastImport.bc250_va == va &&
               (D3DKMT_HANDLE)(uintptr_t)lastImport.handle == r.allocation,
               "%s, %s retried: %u Lock2, import of %x at VA %llx", branch, c.step, lockCalls - locks,
               (D3DKMT_HANDLE)(uintptr_t)lastImport.handle, lastImport.bc250_va);
         // DestroyResource of a hosted resource needs the zink context.
         pipe_resource_reference(&r.resource, NULL);
         continue;
      }
      CHECK(lockCalls == locks + (c.fault == LOCK ? 2 : 1) && a && a->locks == 1 &&
            lastImport.type == WINSYS_HANDLE_TYPE_USER_MEMORY && lastImport.user_memory == a->memory,
            "%s, %s retried: %u Lock2, %u held", branch, c.step, lockCalls - locks, a ? a->locks : 0);
      D3D10DDI_HDEVICE hDevice = {device};
      D3D10DDI_HRESOURCE hResource = {&r};
      DestroyResource(hDevice, hResource);
      CHECK(a && !a->locks && deallocateCalls == deallocations + 1, "%s, %s: destroy leaves %u Lock2 held", branch,
            c.step, a ? a->locks : 0);
   }
   printf("ensure retry, %s branch: %u failed steps retried\n", branch, count);
}

int main(void)
{
   _set_error_mode(_OUT_TO_STDERR);
   screen.resource_from_handle = FakeFromHandle;
   screen.resource_destroy = FakeResourceDestroy;
   pipe.screen = &screen;
   Device *device = (Device *)calloc(1, sizeof(Device));
   device->pipe = &pipe;
   device->hDevice = (HANDLE)0x10;
   device->UMCallbacks.pfnSetErrorCb = SetErrorCb;
   device->KTCallbacks.pfnAllocateCb = AllocateCb;
   device->KTCallbacks.pfnDeallocate2Cb = Deallocate2Cb;
   device->KTCallbacks.pfnCreatePagingQueueCb = CreatePagingQueueCb;
   device->KTCallbacks.pfnMapGpuVirtualAddressCb = MapGpuVirtualAddressCb;
   device->KTCallbacks.pfnMakeResidentCb = MakeResidentCb;
   device->KTCallbacks.pfnLock2Cb = Lock2Cb;
   device->KTCallbacks.pfnUnlock2Cb = Unlock2Cb;

   TestEnsureRetry(device, true);
   TestEnsureRetry(device, false);

   printf("%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
