import argparse
import hashlib
import re
from pathlib import Path


def function(source, name):
    matches = list(re.finditer(r"^static (?:VkResult|void)\s*\n" + re.escape(name) + r"\(", source, re.M))
    if len(matches) != 1:
        raise SystemExit("Expected one definition of " + name)
    start = matches[0].start()
    begin = source.index("{", matches[0].end())
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


TEMPLATE = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
using VkDevice = uintptr_t;
using VkResult = int;
using HBITMAP = void *;
using LONG = int32_t;
enum { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1, BI_RGB = 0, DIB_RGB_COLORS = 0 };
struct VkAllocationCallbacks {};
struct VkIcdSurfaceWin32 { void *hwnd; };
struct VkSwapchainCreateInfoKHR { uintptr_t surface; struct { uint32_t width, height; } imageExtent; };
struct BITMAPINFOHEADER { unsigned biSize; int biWidth, biHeight; unsigned biPlanes, biBitCount, biCompression; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; };
struct BITMAP { int bmWidthBytes; };
static unsigned checks, failures, creates, destroys, doubles, bitmap_creates, bitmap_deletes;
static unsigned fail_image, fail_stage, gdi_failure;
static void check(bool ok, const char *what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
struct Resource { unsigned releases; void Release() { check(!releases++, "COM resource released once"); } };
struct wsi_image { unsigned id; bool destroyed; };
struct wsi_swapchain { unsigned image_info; };
struct wsi_win32_swapchain;
struct wsi_win32_image {
   wsi_image base;
   bool base_initialized;
   wsi_win32_swapchain *chain;
   struct { Resource *swapchain_res, *blit_res; } dxgi;
   struct { HBITMAP bmp; int bmp_row_pitch; void *ppvBits; } sw;
};
struct wsi_win32_swapchain { wsi_swapchain base; bool is_dxgi; void *wnd; };
static Resource resources[4];
static void wsi_destroy_image(const wsi_swapchain *, wsi_image *image)
{
   if (!image->id) return;
   ++destroys;
   if (image->destroyed) ++doubles;
   image->destroyed = true;
}
static VkResult wsi_create_image(const wsi_swapchain *chain, const unsigned *, wsi_image *image)
{
   auto wrapper = reinterpret_cast<wsi_win32_image *>(image);
   unsigned index = creates++;
   if (index == fail_image && fail_stage == 1) return -10;
   image->id = index + 1;
   wrapper->dxgi.blit_res = &resources[index];
   if (index == fail_image && fail_stage >= 2) {
      wsi_destroy_image(chain, image);
      return -int(10 + fail_stage);
   }
   return VK_SUCCESS;
}
static HBITMAP CreateDIBSection(void *, const BITMAPINFO *, unsigned, void **bits, void *, unsigned)
{
   if (gdi_failure == 1) return nullptr;
   ++bitmap_creates; *bits = gdi_failure == 2 ? nullptr : reinterpret_cast<void *>(1);
   return reinterpret_cast<HBITMAP>(1);
}
static int GetObject(HBITMAP, size_t bytes, BITMAP *out)
{ out->bmWidthBytes = 512; return gdi_failure == 3 ? 0 : int(bytes); }
static void DeleteObject(HBITMAP bitmap)
{ check(bitmap != nullptr, "delete live bitmap"); ++bitmap_deletes; }
static void wsi_win32_image_release_dxgi_buffers(wsi_win32_image *image)
{
   if (image->dxgi.swapchain_res) { image->dxgi.swapchain_res->Release(); image->dxgi.swapchain_res = nullptr; }
}
@ACTUAL_INIT@
@ACTUAL_FINISH@
static void reset()
{
   creates = destroys = doubles = bitmap_creates = bitmap_deletes = 0;
   fail_image = 99; fail_stage = gdi_failure = 0;
   std::memset(resources, 0, sizeof(resources));
}
int main()
{
   VkIcdSurfaceWin32 surface{};
   VkSwapchainCreateInfoKHR info{reinterpret_cast<uintptr_t>(&surface), {128, 64}};
   for (unsigned position = 0; position < 3; ++position) {
      for (unsigned stage = 0; stage < 5; ++stage) {
         reset(); fail_image = position; fail_stage = stage;
         wsi_win32_swapchain chain{}; chain.is_dxgi = true;
         wsi_win32_image images[3]{};
         for (unsigned i = 0; i < 3; ++i) {
            VkResult result = wsi_win32_image_init(1, &chain, &info, nullptr, &images[i]);
            if (result != VK_SUCCESS) { check(i == position && stage != 0, "expected failed image"); break; }
         }
         for (auto &image : images) wsi_win32_image_finish(&chain, nullptr, &image);
         check(!doubles, "failed common image is not destroyed twice");
         for (unsigned i = 0; i < creates; ++i)
            check(resources[i].releases == (i == position && stage == 1 ? 0u : 1u),
                  "D3D12 resource released even after Vulkan import failure");
         unsigned expected = stage == 0 ? 3 : position + (stage == 1 ? 0 : 1);
         check(destroys == expected, "each acquired Vulkan image destroyed once");
         for (auto &image : images) wsi_win32_image_finish(&chain, nullptr, &image);
         check(!doubles && destroys == expected, "finish does not repeat Vulkan destruction");
      }
   }
   for (unsigned fault = 0; fault < 4; ++fault) {
      reset(); gdi_failure = fault;
      wsi_win32_swapchain chain{}; wsi_win32_image image{};
      VkResult result = wsi_win32_image_init(1, &chain, &info, nullptr, &image);
      check((result == VK_SUCCESS) == (fault == 0), "GDI failure result retained");
      wsi_win32_image_finish(&chain, nullptr, &image);
      check(destroys == 1 && !doubles, "GDI failed wrapper releases its Vulkan image");
      check(bitmap_deletes == bitmap_creates, "partially created bitmap released");
      check(resources[0].releases == 1, "GDI wrapper releases memory callback resource");
      wsi_win32_image_finish(&chain, nullptr, &image);
      check(destroys == 1 && !doubles && bitmap_deletes == bitmap_creates, "GDI finish repeats safely");
   }
   std::printf("Win32 image lifetime: %u checks, %u failures; no GPU/desktop\n", checks, failures);
   return failures ? 1 : 0;
}
'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "wsi_common_win32.cpp")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    generated = TEMPLATE.replace("@ACTUAL_INIT@", function(source, "wsi_win32_image_init"))
    generated = generated.replace("@ACTUAL_FINISH@", function(source, "wsi_win32_image_finish"))
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    print("source sha256=" + hashlib.sha256(args.source.read_bytes()).hexdigest())
