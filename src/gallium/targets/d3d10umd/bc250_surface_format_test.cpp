/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of the LB7A surface formats of the desktop UMD, without a WDDM
 * runtime: the shared format table as this UMD consumes it, OpenResource and
 * shared CreateResource against fake kernel callbacks, and llvmpipe sampling
 * of an opened surface through the DXGI Blt DDI into a B8G8R8A8 target, the
 * way the compositor reads it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "State.h"
#include "Resource.h"
#include "Shader.h"
#include "DxgiFns.h"
#include "Format.h"
#include "amdgpu_wddm_surface_format.h"

#include "util/format/u_format.h"
#include "util/box.h"

EXTERN_C struct pipe_screen *d3d10_create_screen(void);

static_assert(D3DDDIFMT_A8R8G8B8 == 21 && D3DDDIFMT_X8R8G8B8 == 22 && D3DDDIFMT_A8B8G8R8 == 32 &&
              D3DDDIFMT_A2B10G10R10 == 31, "D3DDDIFORMAT values");
static_assert(DXGI_FORMAT_B8G8R8A8_UNORM == 87 && DXGI_FORMAT_B8G8R8X8_UNORM == 88 &&
              DXGI_FORMAT_R8G8B8A8_UNORM == 28 && DXGI_FORMAT_R10G10B10A2_UNORM == 24, "DXGI_FORMAT values");

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); \
   printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FakeAllocation { D3DKMT_HANDLE handle; void *memory; UINT64 size; bool locked; };
static FakeAllocation allocations[16];
static UINT allocationCount, allocateCalls, deallocateCalls;
static unsigned char lastSurfaceData[32], lastResourceData[16];
static UINT lastSurfaceDataSize, lastResourceDataSize;
static volatile UINT64 pagingFence = 1;
static HRESULT lastError;

static FakeAllocation *FindAllocation(D3DKMT_HANDLE handle)
{
   for (UINT i = 0; i < allocationCount; ++i)
      if (allocations[i].handle == handle) return &allocations[i];
   return NULL;
}

static FakeAllocation *AddAllocation(UINT64 size)
{
   FakeAllocation *a = &allocations[allocationCount++];
   a->handle = 0x40000000u + allocationCount * 4;
   a->memory = _aligned_malloc((size_t)size, 4096);
   a->size = size;
   return a;
}

static HRESULT APIENTRY AllocateCb(HANDLE, D3DDDICB_ALLOCATE *allocate)
{
   allocateCalls++;
   D3DDDI_ALLOCATIONINFO2 *info = &allocate->pAllocationInfo2[0];
   lastSurfaceDataSize = info->PrivateDriverDataSize;
   memcpy(lastSurfaceData, info->pPrivateDriverData, MIN2(info->PrivateDriverDataSize, 32u));
   lastResourceDataSize = allocate->PrivateDriverDataSize;
   memcpy(lastResourceData, allocate->pPrivateDriverData, MIN2(allocate->PrivateDriverDataSize, 16u));
   UINT64 size;
   memcpy(&size, lastSurfaceData + 24, sizeof(size));
   info->hAllocation = AddAllocation(size)->handle;
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

static HRESULT APIENTRY MapGpuVirtualAddressCb(HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *map)
{
   map->VirtualAddress = 0x100000000ull; map->PagingFenceValue = 1;
   return S_OK;
}

static HRESULT APIENTRY MakeResidentCb(HANDLE, D3DDDI_MAKERESIDENT *resident)
{
   resident->PagingFenceValue = 1;
   return S_OK;
}

static HRESULT APIENTRY Lock2Cb(HANDLE, D3DDDICB_LOCK2 *lock)
{
   FakeAllocation *a = FindAllocation(lock->hAllocation);
   if (!a) return E_INVALIDARG;
   a->locked = true; lock->pData = a->memory;
   return S_OK;
}

static HRESULT APIENTRY Unlock2Cb(HANDLE, const D3DDDICB_UNLOCK2 *unlock)
{
   FakeAllocation *a = FindAllocation(unlock->hAllocation);
   if (!a || !a->locked) return E_INVALIDARG;
   a->locked = false;
   return S_OK;
}

static void APIENTRY SetErrorCb(D3D10DDI_HRTCORELAYER, HRESULT hr) { lastError = hr; }

struct Lb7a { UINT magic, version, width, height, pitch, format; UINT64 size; };
static_assert(sizeof(Lb7a) == 32, "LB7A ABI");

// Opens a fake KMD allocation as the D3D11 runtime does for a shared surface.
static HRESULT Open(D3D10DDI_HDEVICE hDevice, Resource *resource, const Lb7a &data, D3DKMT_HANDLE handle)
{
   D3DDDI_OPENALLOCATIONINFO2 info = {};
   info.hAllocation = handle;
   info.pPrivateDriverData = &data; info.PrivateDriverDataSize = sizeof(data);
   D3D10DDIARG_OPENRESOURCE open = {};
   open.NumAllocations = 1; open.pOpenAllocationInfo2 = &info;
   D3D10DDI_HRESOURCE hResource = {resource};
   D3D10DDI_HRTRESOURCE hRTResource = {(HANDLE)0x70000000};
   memset(resource, 0, sizeof(*resource));
   lastError = S_OK;
   OpenResource(hDevice, &open, hResource, hRTResource);
   return lastError;
}

static HRESULT Create(D3D10DDI_HDEVICE hDevice, Resource *resource, DXGI_FORMAT format, UINT width, UINT height,
                      UINT miscFlags, DXGI_DDI_PRIMARY_DESC *primary)
{
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = mip.PhysicalWidth = width;
   mip.TexelHeight = mip.PhysicalHeight = height;
   mip.TexelDepth = mip.PhysicalDepth = 1;
   D3D10DDIARG_CREATERESOURCE create = {};
   create.pMipInfoList = &mip; create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   create.Usage = D3D10_DDI_USAGE_DEFAULT;
   create.BindFlags = D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET;
   create.MiscFlags = miscFlags; create.Format = format; create.SampleDesc.Count = 1;
   create.MipLevels = 1; create.ArraySize = 1; create.pPrimaryDesc = primary;
   D3D10DDI_HRESOURCE hResource = {resource};
   D3D10DDI_HRTRESOURCE hRTResource = {(HANDLE)0x70000010};
   lastError = S_OK;
   CreateResource(hDevice, &create, hResource, hRTResource);
   return lastError;
}

static void Destroy(D3D10DDI_HDEVICE hDevice, Resource *resource)
{
   D3D10DDI_HRESOURCE hResource = {resource};
   DestroyResource(hDevice, hResource);
}

// Every COMPOSED row of the shared table must be one this UMD can open and
// sample: a pipe format of the row's size that llvmpipe renders and samples.
static void TestTable(struct pipe_screen *screen)
{
   unsigned count, composed = 0;
   const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
   for (unsigned i = 0; i < count; ++i) {
      const AMDGPU_WDDM_SURFACE_FORMAT *row = &rows[i];
      if (!amdgpu_wddm_surface_admit(row, AMDGPU_WDDM_SURFACE_COMPOSED)) continue;
      composed++;
      enum pipe_format pipe = FormatTranslate((DXGI_FORMAT)row->dxgi, FALSE);
      CHECK(pipe != PIPE_FORMAT_NONE && util_format_get_blocksize(pipe) == row->bytes_per_pixel,
            "%s: pipe format %s", row->name, util_format_short_name(pipe));
      CHECK(screen->is_format_supported(screen, pipe, PIPE_TEXTURE_2D, 1, 1,
                                        PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET),
            "%s: llvmpipe support", row->name);
   }
   const AMDGPU_WDDM_SURFACE_FORMAT *rgb10a2 = amdgpu_wddm_surface_format_by_d3dddi(D3DDDIFMT_A2B10G10R10);
   CHECK(rgb10a2 && rgb10a2 == amdgpu_wddm_surface_format_by_dxgi(DXGI_FORMAT_R10G10B10A2_UNORM) &&
         FormatTranslate(DXGI_FORMAT_R10G10B10A2_UNORM, FALSE) == PIPE_FORMAT_R10G10B10A2_UNORM,
         "RGB10A2: 31 <-> 24 <-> R10G10B10A2_UNORM");
   printf("table: %u COMPOSED rows of %u, %s\n", composed, count, failures ? "with failures" : "pass");
}

// Fills a surface of the given format with a pattern and returns the B8G8R8A8
// value the compositor should read for pixel (x, y).
static UINT Pattern(UINT format, UINT x, UINT y, UINT width, UINT height, UINT *stored)
{
   if (format == 31) {
      UINT r = x * 1023 / (width - 1), g = y * 1023 / (height - 1);
      UINT b = ((x * 31 + y * 17) * 7) & 1023, a = (x + y) & 3;
      *stored = r | g << 10 | b << 20 | a << 30;
      auto unorm8 = [](UINT v) { return (v * 255 * 2 + 1023) / (1023 * 2); };
      return unorm8(b) | unorm8(g) << 8 | unorm8(r) << 16 | (a * 85) << 24;
   }
   UINT r = (x * 3) & 255, g = (y * 3) & 255, b = ((x + y) * 5) & 255, a = (x * 7 + y) & 255;
   UINT bgra = b | g << 8 | r << 16 | a << 24;
   *stored = format == 32 ? r | g << 8 | b << 16 | a << 24 : bgra;
   return bgra;
}

// Opens a surface of the given LB7A format and composes it into a B8G8R8A8
// target through _Blt; returns the largest per-channel difference.
static void TestOpenAndCompose(Device *device, UINT format, enum pipe_format pipeFormat)
{
   D3D10DDI_HDEVICE hDevice = {device};
   const UINT width = 67, height = 65, pitch = (width * 4 + 255) & ~255u;
   Lb7a data = {0x4137424c, 1, width, height, pitch, format, UINT64(pitch) * ((height + 3) & ~3u)};
   FakeAllocation *kmd = AddAllocation(data.size);
   memset(kmd->memory, 0xcd, (size_t)data.size);
   UINT *expected = (UINT *)calloc(width * height, sizeof(UINT));
   for (UINT y = 0; y < height; ++y)
      for (UINT x = 0; x < width; ++x)
         expected[y * width + x] = Pattern(format, x, y, width, height, (UINT *)((char *)kmd->memory + y * pitch) + x);
   void *before = malloc((size_t)data.size);
   memcpy(before, kmd->memory, (size_t)data.size);

   Resource src, dst;
   HRESULT hr = Open(hDevice, &src, data, kmd->handle);
   CHECK(hr == S_OK, "format %u: OpenResource %08lx", format, hr);
   if (FAILED(hr)) { free(expected); free(before); return; }
   CHECK(src.resource->format == pipeFormat && src.presentReady && src.cpuMapping == kmd->memory &&
         src.surfacePitch == pitch, "format %u: opened resource state", format);
   hr = Create(hDevice, &dst, DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 0, NULL);
   CHECK(hr == S_OK && !dst.allocation, "format %u: destination %08lx", format, hr);

   DXGI_DDI_ARG_BLT blt = {};
   blt.hDevice = (DXGI_DDI_HDEVICE)device;
   blt.hSrcResource = (DXGI_DDI_HRESOURCE)&src; blt.hDstResource = (DXGI_DDI_HRESOURCE)&dst;
   blt.DstRight = width; blt.DstBottom = height; blt.Rotate = DXGI_DDI_MODE_ROTATION_IDENTITY;
   hr = _Blt(&blt);
   CHECK(hr == S_OK, "format %u: _Blt %08lx", format, hr);

   struct pipe_box box;
   u_box_2d(0, 0, width, height, &box);
   struct pipe_transfer *transfer;
   const char *map = (const char *)device->pipe->texture_map(device->pipe, dst.resource, 0, PIPE_MAP_READ,
                                                             &box, &transfer);
   UINT maxDiff = 0, exact = 0, alphaMismatch = 0;
   UINT worst = 0, worstGot = 0, worstWant = 0;
   for (UINT y = 0; map && y < height; ++y)
      for (UINT x = 0; x < width; ++x) {
         UINT got = ((const UINT *)(map + y * transfer->stride))[x], want = expected[y * width + x];
         UINT diff = 0;
         for (UINT c = 0; c < 32; c += 8) {
            int d = abs(int((got >> c) & 255) - int((want >> c) & 255));
            if ((UINT)d > diff) diff = d;
         }
         if ((got >> 24) != (want >> 24)) alphaMismatch++;
         if (!diff) exact++;
         if (diff > maxDiff) { maxDiff = diff; worst = y * width + x; worstGot = got; worstWant = want; }
      }
   if (map) device->pipe->texture_unmap(device->pipe, transfer);
   CHECK(map != NULL, "format %u: destination map", format);
   // 10-bit to 8-bit through float may round differently from the oracle by
   // one step; 8-bit sources convert exactly. Alpha is exact for both.
   const UINT allowed = format == 31 ? 1 : 0;
   CHECK(maxDiff <= allowed && !alphaMismatch, "format %u: max diff %u (pixel %u got %08x want %08x), alpha %u",
         format, maxDiff, worst, worstGot, worstWant, alphaMismatch);
   CHECK(!memcmp(before, kmd->memory, (size_t)data.size), "format %u: source memory changed", format);
   printf("format %u -> %s: %u/%u pixels exact, max channel diff %u, alpha mismatches %u\n", format,
          util_format_short_name(pipeFormat), exact, width * height, maxDiff, alphaMismatch);

   UINT deallocations = deallocateCalls;
   Destroy(hDevice, &dst);
   Destroy(hDevice, &src);
   CHECK(deallocateCalls == deallocations + 1 && !kmd->locked, "format %u: close", format);
   free(expected); free(before);
}

static void TestOpenRefusals(Device *device)
{
   D3D10DDI_HDEVICE hDevice = {device};
   const UINT width = 64, height = 64, pitch = 256;
   FakeAllocation *kmd = AddAllocation(pitch * height);
   Resource r;
   struct Case { UINT format, pitch, magic; HRESULT want; } cases[] = {
      {35, pitch, 0x4137424c, E_NOTIMPL},        // A2R10G10B10, not R10G10B10A2
      {113, pitch, 0x4137424c, E_NOTIMPL},       // FP16: a row without COMPOSED
      {113, width * 6, 0x4137424c, E_NOTIMPL},   // refused rows keep the 4-byte geometry check
      {31, width * 4 - 16, 0x4137424c, E_INVALIDARG}, // pitch below width * 4
      {113, width * 4 - 16, 0x4137424c, E_INVALIDARG}, // refused format: geometry first, as before
      {31, pitch, 0x12345678, E_INVALIDARG},     // not LB7A
   };
   for (const Case &c : cases) {
      Lb7a data = {c.magic, 1, width, height, c.pitch, c.format, UINT64(c.pitch) * height};
      HRESULT hr = Open(hDevice, &r, data, kmd->handle);
      CHECK(hr == c.want && !kmd->locked, "format %u pitch %u: %08lx, want %08lx", c.format, c.pitch, hr, c.want);
   }
   // X8R8G8B8 is a kernel/GDI row in the shared table; this UMD opens it as
   // B8G8R8X8 as it always has.
   Lb7a x8 = {0x4137424c, 1, width, height, pitch, 22, UINT64(pitch) * height};
   HRESULT hr = Open(hDevice, &r, x8, kmd->handle);
   CHECK(hr == S_OK && r.resource && r.resource->format == PIPE_FORMAT_B8G8R8X8_UNORM && kmd->locked,
         "X8R8G8B8 open: %08lx", hr);
   if (SUCCEEDED(hr)) Destroy(hDevice, &r);
   printf("open refusals: %u cases, X8R8G8B8 open\n", (UINT)(sizeof(cases) / sizeof(cases[0])));
}

// The allocate path: 8-bit shared and primary surfaces send the same LB7A and
// E26R bytes as before; R10G10B10A2 is not created by this UMD.
static void TestCreateShared(Device *device)
{
   D3D10DDI_HDEVICE hDevice = {device};
   const UINT width = 67, height = 65;
   struct Case { DXGI_FORMAT format; UINT ddi; bool primary; } cases[] = {
      {DXGI_FORMAT_B8G8R8A8_UNORM, 21, false},
      {DXGI_FORMAT_R8G8B8A8_UNORM, 32, false},
      {DXGI_FORMAT_B8G8R8X8_UNORM, 22, false},
      {DXGI_FORMAT_B8G8R8A8_UNORM, 21, true},
   };
   for (const Case &c : cases) {
      DXGI_DDI_PRIMARY_DESC primary = {};
      primary.ModeDesc.Width = width; primary.ModeDesc.Height = height; primary.ModeDesc.Format = c.format;
      Resource r;
      UINT calls = allocateCalls;
      HRESULT hr = Create(hDevice, &r, c.format, width, height, D3D10_DDI_RESOURCE_MISC_SHARED,
                          c.primary ? &primary : NULL);
      // The formula of the code before 10-bit surfaces could be opened.
      const UINT alignment = c.primary ? 256u : 64u;
      const UINT pitch = (width * 4 + alignment - 1) & ~(alignment - 1);
      Lb7a want = {0x4137424c, 1, width, height, pitch, c.ddi, UINT64(pitch) * ((height + 3) & ~3u)};
      const UINT group[4] = {0x52363245, 2, 1, (c.primary ? 1u : 0u) | 2u};
      CHECK(hr == S_OK && allocateCalls == calls + 1, "format %u primary %u: %08lx", c.ddi, c.primary, hr);
      CHECK(lastSurfaceDataSize == 32 && !memcmp(lastSurfaceData, &want, 32), "format %u primary %u: LB7A bytes",
            c.ddi, c.primary);
      CHECK(lastResourceDataSize == 16 && !memcmp(lastResourceData, group, 16), "format %u primary %u: E26R bytes",
            c.ddi, c.primary);
      if (SUCCEEDED(hr)) Destroy(hDevice, &r);
   }
   Resource r;
   UINT calls = allocateCalls;
   HRESULT hr = Create(hDevice, &r, DXGI_FORMAT_R10G10B10A2_UNORM, width, height, D3D10_DDI_RESOURCE_MISC_SHARED, NULL);
   CHECK(hr == E_NOTIMPL && allocateCalls == calls, "R10G10B10A2 shared create: %08lx", hr);
   Destroy(hDevice, &r);
   printf("create shared: %u cases\n", (UINT)(sizeof(cases) / sizeof(cases[0])) + 1);
}

static FakeAllocation *AddWriteCombinedAllocation(UINT64 size)
{
   FakeAllocation *a = &allocations[allocationCount++];
   a->handle = 0x40000000u + allocationCount * 4;
   a->memory = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE);
   a->size = size;
   return a;
}

static void FillSurface(void *memory, UINT pitch, UINT width, UINT height, UINT seed)
{
   for (UINT y = 0; y < height; ++y)
      for (UINT x = 0; x < width; ++x)
         ((UINT *)((char *)memory + y * pitch))[x] = (x * 2654435761u) ^ (y * 40503u) ^ seed;
}

// Rows of the shadow equal the surface's rows (the pixels, not the padding).
static bool ShadowMatches(Device *device, Resource *r, UINT pitch, UINT width, UINT height)
{
   struct pipe_box box;
   u_box_2d(0, 0, width, height, &box);
   struct pipe_transfer *transfer;
   const char *map = (const char *)device->pipe->texture_map(device->pipe, r->shadow, 0, PIPE_MAP_READ, &box,
                                                             &transfer);
   bool same = map != NULL;
   for (UINT y = 0; same && y < height; ++y)
      same = !memcmp(map + y * transfer->stride, (const char *)r->cpuMapping + y * pitch, width * 4);
   if (map) device->pipe->texture_unmap(device->pipe, transfer);
   return same;
}

// Writes (set) or checks (!set) a byte pattern in the row padding of a texture;
// returns the stride, or 0 when it has no padding or the map failed.
static UINT Padding(Device *device, struct pipe_resource *texture, bool set, bool *intact)
{
   struct pipe_box box;
   u_box_2d(0, 0, texture->width0, texture->height0, &box);
   struct pipe_transfer *transfer;
   char *map = (char *)device->pipe->texture_map(device->pipe, texture, 0,
                                                 set ? PIPE_MAP_READ | PIPE_MAP_WRITE : PIPE_MAP_READ, &box, &transfer);
   if (!map) return 0;
   const UINT used = texture->width0 * 4, stride = transfer->stride;
   *intact = true;
   for (UINT y = 0; y < texture->height0; ++y)
      for (UINT b = used; b < stride; ++b) {
         char want = (char)(0xa5 ^ b ^ y);
         if (set) map[y * stride + b] = want;
         else if (map[y * stride + b] != want) *intact = false;
      }
   device->pipe->texture_unmap(device->pipe, transfer);
   return stride > used ? stride : 0;
}

// The R8G8B8A8 value a B8G8R8A8 texel converts to.
static UINT Rgba(UINT bgra)
{
   return ((bgra >> 16) & 0xff) | (bgra & 0xff00ff00u) | ((bgra & 0xff) << 16);
}

// A queued draw-based blit from the shadow into an R8G8B8A8 target (a format
// change, so llvmpipe cannot take the immediate copy path).
static void BlitShadow(Device *device, Resource *src, Resource *dst)
{
   struct pipe_blit_info info = {};
   info.src.resource = src->shadow; info.src.format = src->shadow->format;
   info.src.box.width = src->shadow->width0; info.src.box.height = src->shadow->height0; info.src.box.depth = 1;
   info.dst.resource = dst->resource; info.dst.format = dst->resource->format;
   info.dst.box = info.src.box;
   info.mask = PIPE_MASK_RGBA; info.filter = PIPE_TEX_FILTER_NEAREST;
   device->pipe->blit(device->pipe, &info);
}

// Every texel of dst is the R8G8B8A8 form of the surface pattern with seed.
static bool Holds(Device *device, Resource *dst, UINT width, UINT height, UINT seed)
{
   struct pipe_box box;
   u_box_2d(0, 0, width, height, &box);
   struct pipe_transfer *transfer;
   const char *map = (const char *)device->pipe->texture_map(device->pipe, dst->resource, 0, PIPE_MAP_READ, &box,
                                                             &transfer);
   bool same = map != NULL;
   for (UINT y = 0; same && y < height; ++y)
      for (UINT x = 0; same && x < width; ++x)
         same = ((const UINT *)(map + y * transfer->stride))[x] == Rgba((x * 2654435761u) ^ (y * 40503u) ^ seed);
   if (map) device->pipe->texture_unmap(device->pipe, transfer);
   return same;
}

// A llvmpipe map that fails for one texture, to exercise a failed copy.
static decltype(pipe_context::texture_map) realTextureMap;
static struct pipe_resource *failingTexture;
static void *FailingTextureMap(struct pipe_context *pipe, struct pipe_resource *resource, unsigned level,
                               unsigned usage, const struct pipe_box *box, struct pipe_transfer **transfer)
{
   if (resource == failingTexture) { *transfer = NULL; return NULL; }
   return realTextureMap(pipe, resource, level, usage, box, transfer);
}

// An opened write-combined surface is sampled through a cached shadow that
// every draw sampling it copies first; a surface in ordinary memory keeps
// sampling its own storage.
static void TestShadow(Device *device)
{
   D3D10DDI_HDEVICE hDevice = {device};
   const UINT width = 67, height = 65, pitch = (width * 4 + 255) & ~255u;
   Lb7a data = {0x4137424c, 1, width, height, pitch, 21, UINT64(pitch) * ((height + 3) & ~3u)};
   FakeAllocation *wc = AddWriteCombinedAllocation(data.size);
   FakeAllocation *plain = AddAllocation(data.size);
   CHECK(wc->memory != NULL, "write-combined test memory");
   if (!wc->memory) return;
   FillSurface(wc->memory, pitch, width, height, 1);

   Resource src, other, over;
   HRESULT hr = Open(hDevice, &src, data, wc->handle);
   CHECK(hr == S_OK && src.shadow && src.shadow->format == src.resource->format && src.shadow->width0 == width &&
         device->shadowedCount == 1 && device->shadowBytes == Bc250ShadowCharge(width, height),
         "shadow for the write-combined surface: %08lx", hr);
   hr = Open(hDevice, &other, data, plain->handle);
   CHECK(hr == S_OK && !other.shadow && device->shadowedCount == 1, "no shadow for ordinary memory: %08lx", hr);
   if (!src.shadow) return;
   // Over the byte budget the surface is sampled in place.
   const UINT64 charged = device->shadowBytes;
   device->shadowBytes = BC250_MAX_SHADOW_BYTES - 1;
   hr = Open(hDevice, &over, data, wc->handle);
   CHECK(hr == S_OK && !over.shadow && device->shadowedCount == 1, "no shadow over the budget: %08lx", hr);
   device->shadowBytes = charged;
   if (SUCCEEDED(hr)) Destroy(hDevice, &over);

   ShaderResourceView view = {}, second = {}, plainView = {};
   D3D10_1DDIARG_CREATESHADERRESOURCEVIEW create = {};
   create.Format = DXGI_FORMAT_B8G8R8A8_UNORM; create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   create.Tex2D.MipLevels = 1; create.Tex2D.ArraySize = 1;
   create.hDrvResource.pDrvPrivate = &src;
   D3D10DDI_HSHADERRESOURCEVIEW hView = {&view}, hSecond = {&second}, hPlainView = {&plainView};
   CreateShaderResourceView1(hDevice, &create, hView, {(HANDLE)0x70000020});
   CreateShaderResourceView1(hDevice, &create, hSecond, {(HANDLE)0x70000028});
   create.hDrvResource.pDrvPrivate = &other;
   CreateShaderResourceView1(hDevice, &create, hPlainView, {(HANDLE)0x70000030});
   CHECK(view.handle && view.handle->texture == src.shadow && view.shadowOf == &src, "view samples the shadow");
   CHECK(plainView.handle && plainView.handle->texture == other.resource && !plainView.shadowOf,
         "view of ordinary memory samples the resource");

   CHECK(Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 0, "unbound shadow is not copied");
   bool intact = false;
   const UINT stride = Padding(device, src.shadow, true, &intact);
   PsSetShaderResources(hDevice, 3, 1, &hView);
   CHECK(src.shadowBindings == 1, "bound once");
   CHECK(Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 1 &&
         ShadowMatches(device, &src, pitch, width, height), "a draw copies the surface");
   Padding(device, src.shadow, false, &intact);
   CHECK(stride && stride != pitch && intact, "rows copied at their own strides (%u and %u), padding untouched",
         stride, pitch);
   CHECK(Bc250ShadowCharge(width, height) >= UINT64(stride) * ((height + 3) & ~3u),
         "the budget charge covers the backing");

   // Producer writes B with no Present of this device in between: the next draw sees B.
   FillSurface(wc->memory, pitch, width, height, 2);
   CHECK(Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 2 &&
         ShadowMatches(device, &src, pitch, width, height), "the next draw copies the new contents");

   // Several views of one surface in several stages and slots: one copy per draw.
   PsSetShaderResources(hDevice, 4, 1, &hSecond);
   VsSetShaderResources(hDevice, 0, 1, &hView);
   CHECK(src.shadowBindings == 3 && Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 3,
         "three bindings, one copy");

   // A draw queued from copy B still reads B after the next copy writes C.
   Resource out1, out2;
   hr = Create(hDevice, &out1, DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 0, NULL);
   HRESULT hr2 = Create(hDevice, &out2, DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 0, NULL);
   CHECK(hr == S_OK && hr2 == S_OK, "blit targets %08lx %08lx", hr, hr2);
   if (SUCCEEDED(hr) && SUCCEEDED(hr2)) {
      BlitShadow(device, &src, &out1);
      FillSurface(wc->memory, pitch, width, height, 3);
      CHECK(Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 4, "copy C");
      BlitShadow(device, &src, &out2);
      device->pipe->flush(device->pipe, NULL, 0);
      CHECK(Holds(device, &out1, width, height, 2), "the queued draw read copy B");
      CHECK(Holds(device, &out2, width, height, 3), "the later draw read copy C");
      Destroy(hDevice, &out1);
      Destroy(hDevice, &out2);
   }
   CHECK(device->profileShadowBytes == 4ull * width * 4 * height, "bytes copied: %llu", device->profileShadowBytes);

   // A failed copy skips the draw and reports out of memory to the runtime.
   realTextureMap = device->pipe->texture_map;
   device->pipe->texture_map = FailingTextureMap;
   failingTexture = src.shadow;
   lastError = S_OK;
   bool prepared = Bc250ShadowPrepareDraw(device);
   device->pipe->texture_map = realTextureMap;
   failingTexture = NULL;
   CHECK(!prepared && lastError == E_OUTOFMEMORY && device->profileShadowRefreshes == 4,
         "failed copy: draw skipped, %08lx reported", lastError);
   lastError = S_OK;

   D3D10DDI_HSHADERRESOURCEVIEW none = {NULL};
   PsSetShaderResources(hDevice, 3, 1, &none);
   PsSetShaderResources(hDevice, 4, 1, &none);
   VsSetShaderResources(hDevice, 0, 1, &none);
   CHECK(src.shadowBindings == 0 && Bc250ShadowPrepareDraw(device) && device->profileShadowRefreshes == 4,
         "unbound again: no copy");

   PsSetShaderResources(hDevice, 5, 1, &hView);
   DestroyShaderResourceView(hDevice, hView);
   DestroyShaderResourceView(hDevice, hSecond);
   DestroyShaderResourceView(hDevice, hPlainView);
   Destroy(hDevice, &src);
   CHECK(!src.shadow && device->shadowedCount == 0 && device->shadowBytes == 0 &&
         !device->shadowSlots[MESA_SHADER_FRAGMENT][5], "destroy releases the shadow, its bytes and its slot");
   Destroy(hDevice, &other);
   printf("shadow: write-combined surface sampled through a cached copy, %llu copies, strides %u/%u\n",
          device->profileShadowRefreshes, stride, pitch);
   device->profileShadowRefreshes = device->profileShadowBytes = device->profileShadowTicks = 0;
}

int main(void)
{
   _set_error_mode(_OUT_TO_STDERR);
   struct pipe_screen *screen = d3d10_create_screen();
   if (!screen) { printf("FAIL: no llvmpipe screen\n"); return 1; }
   printf("screen: %s\n", screen->get_name(screen));
   TestTable(screen);
   Device *device = (Device *)calloc(1, sizeof(Device));
   device->pipe = screen->context_create(screen, NULL, 0);
   device->hDevice = (HANDLE)0x10;
   device->UMCallbacks.pfnSetErrorCb = SetErrorCb;
   device->KTCallbacks.pfnAllocateCb = AllocateCb;
   device->KTCallbacks.pfnDeallocate2Cb = Deallocate2Cb;
   device->KTCallbacks.pfnCreatePagingQueueCb = CreatePagingQueueCb;
   device->KTCallbacks.pfnMapGpuVirtualAddressCb = MapGpuVirtualAddressCb;
   device->KTCallbacks.pfnMakeResidentCb = MakeResidentCb;
   device->KTCallbacks.pfnLock2Cb = Lock2Cb;
   device->KTCallbacks.pfnUnlock2Cb = Unlock2Cb;

   TestOpenAndCompose(device, 21, PIPE_FORMAT_B8G8R8A8_UNORM);
   TestOpenAndCompose(device, 32, PIPE_FORMAT_R8G8B8A8_UNORM);
   TestOpenAndCompose(device, 31, PIPE_FORMAT_R10G10B10A2_UNORM);
   TestOpenRefusals(device);
   TestCreateShared(device);
   TestShadow(device);

   device->pipe->destroy(device->pipe);
   screen->destroy(screen);
   printf("%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
