/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of Bc250PresentMultiplaneOverlay in the hosted UMD, without a
 * WDDM runtime or a GPU: fake kernel and DXGI callbacks and a pipe context
 * that only counts flushes. The CPU branch runs; the hosted branch shares
 * the context and flush steps with _Present (Bc250PresentContext,
 * Bc250FlushForPresent). The checks: one callback per present and none for
 * a refused one, the flush before the callback, one entry per plane in the
 * order of pPresentPlanes, and the runtime's context pointer carried.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "State.h"
#include "Resource.h"
#include "DxgiFns.h"

#include "util/u_inlines.h"

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); \
   printf(__VA_ARGS__); printf("\n"); } } while (0)

// One sequence for every call, so that the order of the flush and the
// callback shows.
static UINT sequence, contextCalls, flushCalls, flushAt, presentCalls, presentAt;
static HRESULT presentResult = S_OK;
static DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY lastPresent;
static HANDLE lastPresentDevice;

static HRESULT APIENTRY CreateContextVirtualCb(HANDLE, D3DDDICB_CREATECONTEXTVIRTUAL *create)
{
   contextCalls++;
   create->hContext = (HANDLE)0x7C00;
   return S_OK;
}

static HRESULT APIENTRY Lock2Cb(HANDLE, D3DDDICB_LOCK2 *) { return E_FAIL; }
static HRESULT APIENTRY Unlock2Cb(HANDLE, const D3DDDICB_UNLOCK2 *) { return E_FAIL; }
static void APIENTRY SetErrorCb(D3D10DDI_HRTCORELAYER, HRESULT) {}

static HRESULT APIENTRY PresentMultiplaneOverlayCb(HANDLE hDevice, const DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY *present)
{
   presentCalls++;
   presentAt = ++sequence;
   lastPresentDevice = hDevice;
   lastPresent = *present;
   return presentResult;
}

static HRESULT APIENTRY PresentCb(HANDLE, DXGIDDICB_PRESENT *) { return E_FAIL; }

// pipe_screen has a const member (caps), so C++ gives it no default constructor: the double is zeroed
// storage.
static struct pipe_screen &screen = *(struct pipe_screen *)calloc(1, sizeof(struct pipe_screen));
static struct pipe_context pipe;

static void FakeFlush(struct pipe_context *, struct pipe_fence_handle **fence, unsigned)
{
   flushCalls++;
   flushAt = ++sequence;
   if (fence) *fence = NULL;
}

static void FakeFenceReference(struct pipe_screen *, struct pipe_fence_handle **ptr, struct pipe_fence_handle *fence)
{
   *ptr = fence;
}

// A surface the runtime opened: the allocation came with OpenResource, so
// Bc250EnsureSurface has nothing left to do.
static void OpenedSurface(Resource *r, struct pipe_resource *texture, D3DKMT_HANDLE allocation)
{
   memset(r, 0, sizeof(*r));
   r->resource = texture;
   r->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   r->allocation = allocation;
   r->presentReady = TRUE;
}

int main(void)
{
   _set_error_mode(_OUT_TO_STDERR);
   pipe.screen = &screen;
   pipe.flush = FakeFlush;
   screen.fence_reference = FakeFenceReference;
   DXGI_DDI_BASE_CALLBACKS dxgi = {};
   dxgi.pfnPresentCb = PresentCb;
   dxgi.pfnPresentMultiplaneOverlayCb = PresentMultiplaneOverlayCb;
   Device *device = (Device *)calloc(1, sizeof(Device));
   device->pipe = &pipe;
   device->hDevice = (HANDLE)0x10;
   device->UMCallbacks.pfnSetErrorCb = SetErrorCb;
   device->KTCallbacks.pfnCreateContextVirtualCb = CreateContextVirtualCb;
   device->KTCallbacks.pfnLock2Cb = Lock2Cb;
   device->KTCallbacks.pfnUnlock2Cb = Unlock2Cb;
   device->pDXGIBaseCallbacks = &dxgi;

   struct pipe_resource texture = {};
   texture.target = PIPE_TEXTURE_2D; texture.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   texture.width0 = 1920; texture.height0 = 1080; texture.depth0 = 1; texture.array_size = 1;
   pipe_reference_init(&texture.reference, 1);
   texture.screen = &screen;
   Resource game, desktop;
   OpenedSurface(&game, &texture, 0x40000104);
   OpenedSurface(&desktop, &texture, 0x40000208);

   DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY planes[17];
   memset(planes, 0, sizeof(planes));
   DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY args = {};
   args.hDevice = (DXGI_DDI_HDEVICE)device;
   args.pDXGIContext = (void *)0x5300;
   args.pPresentPlanes = planes;

   // Refusals that reach no callback and create no context.
   CHECK(Bc250PresentMultiplaneOverlay(NULL) == E_INVALIDARG, "a null argument was not refused");
   args.PresentPlaneCount = 0;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_INVALIDARG, "a present of no plane was not refused");
   args.PresentPlaneCount = 17;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_INVALIDARG, "17 planes were not refused (16 is the DDI maximum)");
   args.PresentPlaneCount = 1;
   args.pPresentPlanes = NULL;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_INVALIDARG, "a null plane list was not refused");
   args.pPresentPlanes = planes;
   dxgi.pfnPresentMultiplaneOverlayCb = NULL;
   planes[0].Enabled = TRUE; planes[0].hResource = (DXGI_DDI_HRESOURCE)&game;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == DXGI_ERROR_UNSUPPORTED,
         "a runtime with no overlay callback did not get DXGI_ERROR_UNSUPPORTED");
   dxgi.pfnPresentMultiplaneOverlayCb = PresentMultiplaneOverlayCb;
   device->KTCallbacks.pfnCreateContextVirtualCb = NULL;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_NOTIMPL, "a runtime with no context callback was not refused");
   device->KTCallbacks.pfnCreateContextVirtualCb = CreateContextVirtualCb;
   CHECK(presentCalls == 0 && contextCalls == 0 && flushCalls == 0,
         "a refused present made %u callbacks, %u contexts and %u flushes", presentCalls, contextCalls, flushCalls);

   // One plane: the context, the flush, then one callback with one entry.
   planes[0].SubResourceIndex = 2;
   HRESULT hr = Bc250PresentMultiplaneOverlay(&args);
   CHECK(hr == S_OK && presentCalls == 1, "a one-plane present: %08lx, %u callbacks", hr, presentCalls);
   CHECK(contextCalls == 1 && lastPresent.hContext == (HANDLE)0x7C00 && lastPresentDevice == (HANDLE)0x10,
         "the present is not on the device's context: %u creates, context %p, device %p", contextCalls,
         lastPresent.hContext, lastPresentDevice);
   CHECK(flushCalls == 1 && flushAt < presentAt, "the flush (%u at %u) does not come before the callback (at %u)",
         flushCalls, flushAt, presentAt);
   CHECK(lastPresent.pDXGIContext == (void *)0x5300, "the runtime's context pointer was not carried");
   CHECK(lastPresent.AllocationInfoCount == 1 && lastPresent.AllocationInfo[0].PresentAllocation == 0x40000104 &&
         lastPresent.AllocationInfo[0].SubResourceIndex == 2,
         "a one-plane present carried %lu entries, the first %x/%u", lastPresent.AllocationInfoCount,
         lastPresent.AllocationInfo[0].PresentAllocation, lastPresent.AllocationInfo[0].SubResourceIndex);

   // Three planes: the desktop, a disabled one with no resource, the game.
   // One entry per enabled plane in order, and the count is the number of
   // enabled planes; the context is not created again.
   planes[0].hResource = (DXGI_DDI_HRESOURCE)&desktop; planes[0].SubResourceIndex = 0;
   planes[1].Enabled = FALSE; planes[1].hResource = 0;
   planes[2].Enabled = TRUE; planes[2].hResource = (DXGI_DDI_HRESOURCE)&game; planes[2].SubResourceIndex = 1;
   args.PresentPlaneCount = 3;
   hr = Bc250PresentMultiplaneOverlay(&args);
   CHECK(hr == S_OK && presentCalls == 2 && contextCalls == 1, "a three-plane present: %08lx, %u callbacks, %u creates",
         hr, presentCalls, contextCalls);
   CHECK(lastPresent.AllocationInfoCount == 2 &&
         lastPresent.AllocationInfo[0].PresentAllocation == 0x40000208 &&
         lastPresent.AllocationInfo[1].PresentAllocation == 0x40000104 && lastPresent.AllocationInfo[1].SubResourceIndex == 1 &&
         lastPresent.AllocationInfo[2].PresentAllocation == 0 && lastPresent.AllocationInfo[2].SubResourceIndex == 0,
         "a three-plane present carried %lu entries: %x %x %x", lastPresent.AllocationInfoCount,
         lastPresent.AllocationInfo[0].PresentAllocation, lastPresent.AllocationInfo[1].PresentAllocation,
         lastPresent.AllocationInfo[2].PresentAllocation);
   // A disabled plane that names a resource has no allocation in the present
   // either: it gets no entry, and its resource is not asked for a surface.
   planes[1].hResource = (DXGI_DDI_HRESOURCE)&desktop; planes[1].SubResourceIndex = 3;
   hr = Bc250PresentMultiplaneOverlay(&args);
   CHECK(hr == S_OK && lastPresent.AllocationInfoCount == 2 &&
         lastPresent.AllocationInfo[1].PresentAllocation == 0x40000104 &&
         lastPresent.AllocationInfo[1].SubResourceIndex == 1 && lastPresent.AllocationInfo[2].PresentAllocation == 0,
         "a disabled plane with a resource got an entry: %lu entries, second %x/%u", lastPresent.AllocationInfoCount,
         lastPresent.AllocationInfo[1].PresentAllocation, lastPresent.AllocationInfo[1].SubResourceIndex);
   planes[1].hResource = 0; planes[1].SubResourceIndex = 0;

   // An enabled plane with no resource, and a present with no enabled plane:
   // refused, no callback.
   const UINT before = presentCalls;
   planes[1].Enabled = TRUE;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_INVALIDARG, "an enabled plane with no resource was not refused");
   planes[0].Enabled = planes[1].Enabled = planes[2].Enabled = FALSE;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_INVALIDARG, "a present with no enabled plane was not refused");
   // A surface this driver would allocate itself in a format it does not
   // allocate: the Bc250EnsureSurface answer, and no callback.
   Resource tenBit;
   OpenedSurface(&tenBit, &texture, 0);
   tenBit.presentReady = FALSE;
   tenBit.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
   planes[0].Enabled = TRUE; planes[0].hResource = (DXGI_DDI_HRESOURCE)&tenBit;
   args.PresentPlaneCount = 1;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == E_NOTIMPL, "a surface Bc250EnsureSurface refuses was presented");
   CHECK(presentCalls == before, "a refused present reached the callback %u times", presentCalls - before);

   // The callback's answer is the answer.
   planes[0].hResource = (DXGI_DDI_HRESOURCE)&game;
   presentResult = DXGI_ERROR_INVALID_CALL;
   CHECK(Bc250PresentMultiplaneOverlay(&args) == DXGI_ERROR_INVALID_CALL, "the callback's failure was not returned");

   printf("present mpo: %u callbacks, %u flushes\n", presentCalls, flushCalls);
   printf("%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
