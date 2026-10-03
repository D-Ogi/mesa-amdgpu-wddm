/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of OpenResource in the hosted UMD, without a WDDM runtime or a
 * GPU: fake kernel callbacks and a pipe screen that creates and imports
 * descriptions only (BD-058). DWM opens the surfaces of other processes here.
 * Every refusal must reach the runtime as D3DDDIERR_APPLICATIONERROR: the
 * runtime passes E_OUTOFMEMORY through and dwmcore ends DWM on it, and turns
 * E_NOTIMPL or E_INVALIDARG into a removed device. E_OUTOFMEMORY passes only
 * from a kernel callback. A failed open releases what it created, since the
 * runtime never calls DestroyResource for it. The hosted branch imports only
 * the 256-byte linear pitch RADV takes on GFX10; the CPU branch keeps any.
 */

#include <malloc.h>
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

struct Lb7a { UINT magic, version, width, height, pitch, format; UINT64 size; };

struct FakeAllocation { D3DKMT_HANDLE handle; void *memory; UINT locks; };
static FakeAllocation allocations[16];
static UINT allocationCount, mapCalls, residentCalls, lockCalls, errorCalls;
static HRESULT residentFailure, lastError;
static volatile UINT64 pagingFence = 1;

static FakeAllocation *FindAllocation(D3DKMT_HANDLE handle)
{
   for (UINT i = 0; i < allocationCount; ++i)
      if (allocations[i].handle == handle) return &allocations[i];
   return NULL;
}

// An allocation another process made and this device opens.
static FakeAllocation *AddAllocation(UINT64 size)
{
   FakeAllocation *a = &allocations[allocationCount++];
   a->handle = 0x40000000u + allocationCount * 4;
   a->memory = _aligned_malloc((size_t)size, 4096);
   return a;
}

static HRESULT APIENTRY Deallocate2Cb(HANDLE, const D3DDDICB_DEALLOCATE2 *) { return S_OK; }

static HRESULT APIENTRY CreatePagingQueueCb(HANDLE, D3DDDICB_CREATEPAGINGQUEUE *queue)
{
   queue->hPagingQueue = 0x60000000u;
   queue->FenceValueCPUVirtualAddress = (VOID *)&pagingFence;
   return S_OK;
}

static HRESULT APIENTRY MapGpuVirtualAddressCb(HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *map)
{
   map->VirtualAddress = 0x100000000ull + 0x10000000ull * mapCalls++; map->PagingFenceValue = 1;
   return S_OK;
}

static HRESULT APIENTRY MakeResidentCb(HANDLE, D3DDDI_MAKERESIDENT *resident)
{
   residentCalls++;
   if (HRESULT hr = residentFailure) { residentFailure = S_OK; return hr; }
   resident->PagingFenceValue = 1;
   return S_OK;
}

static HRESULT APIENTRY Lock2Cb(HANDLE, D3DDDICB_LOCK2 *lock)
{
   lockCalls++;
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

static void APIENTRY SetErrorCb(D3D10DDI_HRTCORELAYER, HRESULT hr) { errorCalls++; lastError = hr; }

// A pipe screen whose textures are descriptions only: it counts the live
// ones, and imports a given number of failures, then succeeds.
static struct pipe_screen screen;
static struct pipe_context pipe;
static UINT importCalls, importFailures, liveTextures;
static struct winsys_handle lastImport;

static struct pipe_resource *NewTexture(const struct pipe_resource *templat)
{
   struct pipe_resource *r = (struct pipe_resource *)calloc(1, sizeof(*r));
   *r = *templat;
   pipe_reference_init(&r->reference, 1);
   r->screen = &screen;
   liveTextures++;
   return r;
}

static bool FakeFormatSupported(struct pipe_screen *, enum pipe_format, enum pipe_texture_target, unsigned,
                                unsigned, unsigned)
{
   return true;
}

static struct pipe_resource *FakeCreate(struct pipe_screen *, const struct pipe_resource *templat)
{
   return NewTexture(templat);
}

static struct pipe_resource *FakeFromHandle(struct pipe_screen *, const struct pipe_resource *templat,
                                            struct winsys_handle *handle, unsigned)
{
   importCalls++;
   lastImport = *handle;
   if (importFailures) { importFailures--; return NULL; }
   return NewTexture(templat);
}

static void FakeResourceDestroy(struct pipe_screen *, struct pipe_resource *resource)
{
   liveTextures--;
   free(resource);
}

static HRESULT Open(Device *device, Resource *r, D3DKMT_HANDLE allocation, const Lb7a &data,
                    UINT numAllocations = 1, UINT privateSize = sizeof(Lb7a))
{
   D3DDDI_OPENALLOCATIONINFO2 info = {};
   info.hAllocation = allocation;
   info.pPrivateDriverData = &data;
   info.PrivateDriverDataSize = privateSize;
   D3D10DDIARG_OPENRESOURCE open = {};
   open.NumAllocations = numAllocations;
   open.pOpenAllocationInfo2 = &info;
   D3D10DDI_HDEVICE hDevice = {device};
   D3D10DDI_HRESOURCE hResource = {r};
   D3D10DDI_HRTRESOURCE hRTResource = {};
   memset(r, 0xcd, sizeof(*r));   // the runtime's memory is not zeroed
   lastError = S_OK;
   errorCalls = 0;
   OpenResource(hDevice, &open, hResource, hRTResource);
   return lastError;
}

// The hosted branch: what is imported, what is refused before anything is
// created, and that a failed open leaves no texture behind.
static void TestHosted(Device *device)
{
   device->hosted_state = (void *)&screen;
   struct Case { UINT format, bpp, width, pitch; bool opens; const char *what; } cases[] = {
      {32, 4, 992, 3968, false, "RGBA8 992 wide at the CPU UMD's 64-byte pitch (BD-058)"},
      {32, 4, 992, 4096, true, "RGBA8 992 wide at the 256-byte pitch"},
      {21, 4, 1000, 4032, false, "BGRA8 1000 wide at a 64-byte pitch"},
      {21, 4, 1000, 4096, true, "BGRA8 1000 wide at the 256-byte pitch"},
      {21, 4, 1024, 4096, true, "BGRA8 1024 wide, both rules agree"},
      {21, 4, 992, 4352, false, "BGRA8 992 wide with a padded pitch"},
      {113, 8, 192, 1536, true, "RGBA16F 192 wide"},
      {113, 8, 100, 800, false, "RGBA16F 100 wide at a 32-byte pitch"},
      {113, 8, 100, 1024, true, "RGBA16F 100 wide at the 256-byte pitch"},
   };
   for (const Case &c : cases) {
      const UINT height = 960;
      Lb7a data = {0x4137424c, 1, c.width, height, c.pitch, c.format, UINT64(c.pitch) * height};
      const UINT imports = importCalls, maps = mapCalls, live = liveTextures;
      Resource r;
      HRESULT hr = Open(device, &r, 0x40001cc0, data);
      if (c.opens) {
         CHECK(hr == S_OK && !errorCalls && r.presentReady && r.resource && liveTextures == live + 1,
               "%s: %08lx, %u live textures", c.what, hr, liveTextures - live);
         CHECK(importCalls == imports + 1 && lastImport.type == WINSYS_HANDLE_TYPE_FD &&
               lastImport.stride == c.pitch && (D3DKMT_HANDLE)(uintptr_t)lastImport.handle == 0x40001cc0,
               "%s: import stride %u", c.what, lastImport.stride);
         // DestroyResource of a hosted resource needs the zink context.
         pipe_resource_reference(&r.resource, NULL);
         free(r.transfers);
      } else {
         CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "%s: %08lx, want APPLICATIONERROR", c.what, hr);
         CHECK(importCalls == imports && mapCalls == maps && liveTextures == live && !r.resource &&
               !r.transfers && !r.allocation, "%s: %u imports, %u maps, %u live textures after the refusal",
               c.what, importCalls - imports, mapCalls - maps, liveTextures - live);
      }
   }
   printf("hosted pitch: %u cases\n", (UINT)(sizeof(cases) / sizeof(cases[0])));

   // Failures after CreateResource release its texture. A refused import is
   // not an allocation failure; MakeResident running out of memory is one.
   const UINT width = 992, height = 960, pitch = 4096;
   Lb7a data = {0x4137424c, 1, width, height, pitch, 32, UINT64(pitch) * height};
   Resource r;
   UINT live = liveTextures;
   importFailures = 1;
   HRESULT hr = Open(device, &r, 0x40001cc0, data);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1 && liveTextures == live && !r.resource &&
         !r.transfers && !r.presentReady, "import fails: %08lx, %u live textures", hr, liveTextures - live);
   live = liveTextures;
   residentFailure = E_OUTOFMEMORY;
   hr = Open(device, &r, 0x40001cc0, data);
   CHECK(hr == E_OUTOFMEMORY && errorCalls == 1 && liveTextures == live && !r.resource,
         "MakeResident out of memory: %08lx, %u live textures", hr, liveTextures - live);

   // Refusals before CreateResource.
   live = liveTextures;
   hr = Open(device, &r, 0x40001cc0, data, 2);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "two allocations: %08lx", hr);
   hr = Open(device, &r, 0x40001cc0, data, 1, 16);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "short private data: %08lx", hr);
   Lb7a bad = data; bad.magic = 0x12345678;
   hr = Open(device, &r, 0x40001cc0, bad);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "not LB7A: %08lx", hr);
   bad = data; bad.size = UINT64(pitch) * (height - 4);
   hr = Open(device, &r, 0x40001cc0, bad);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "size below the footprint: %08lx", hr);
   bad = data; bad.format = 35;
   hr = Open(device, &r, 0x40001cc0, bad);
   CHECK(hr == D3DDDIERR_APPLICATIONERROR && errorCalls == 1, "A2R10G10B10, not composed: %08lx", hr);
   CHECK(liveTextures == live, "refusals left %u live textures", liveTextures - live);
   printf("hosted failures: 7 cases\n");
}

// The CPU branch keeps any pitch the LB7A checks admit: it reads the
// allocation through Lock2 at that pitch.
static void TestCpu(Device *device)
{
   device->hosted_state = NULL;
   const UINT width = 992, height = 960, pitch = 3968;
   FakeAllocation *a = AddAllocation(UINT64(pitch) * height);
   Lb7a data = {0x4137424c, 1, width, height, pitch, 32, UINT64(pitch) * height};
   Resource r;
   const UINT locks = lockCalls;
   HRESULT hr = Open(device, &r, a->handle, data);
   CHECK(hr == S_OK && !errorCalls && r.presentReady && lockCalls == locks + 1 && a->locks == 1 &&
         lastImport.type == WINSYS_HANDLE_TYPE_USER_MEMORY && lastImport.stride == pitch &&
         lastImport.user_memory == a->memory, "CPU, 992 wide at pitch 3968: %08lx, import stride %u", hr,
         lastImport.stride);
   D3D10DDI_HDEVICE hDevice = {device};
   D3D10DDI_HRESOURCE hResource = {&r};
   DestroyResource(hDevice, hResource);
   CHECK(!a->locks, "CPU: destroy leaves %u Lock2 held", a->locks);
   printf("CPU pitch: 1 case\n");
}

int main(void)
{
   _set_error_mode(_OUT_TO_STDERR);
   screen.is_format_supported = FakeFormatSupported;
   screen.resource_create = FakeCreate;
   screen.resource_from_handle = FakeFromHandle;
   screen.resource_destroy = FakeResourceDestroy;
   pipe.screen = &screen;
   Device *device = (Device *)calloc(1, sizeof(Device));
   device->pipe = &pipe;
   device->hDevice = (HANDLE)0x10;
   device->UMCallbacks.pfnSetErrorCb = SetErrorCb;
   device->KTCallbacks.pfnDeallocate2Cb = Deallocate2Cb;
   device->KTCallbacks.pfnCreatePagingQueueCb = CreatePagingQueueCb;
   device->KTCallbacks.pfnMapGpuVirtualAddressCb = MapGpuVirtualAddressCb;
   device->KTCallbacks.pfnMakeResidentCb = MakeResidentCb;
   device->KTCallbacks.pfnLock2Cb = Lock2Cb;
   device->KTCallbacks.pfnUnlock2Cb = Unlock2Cb;

   TestHosted(device);
   TestCpu(device);

   printf("%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
