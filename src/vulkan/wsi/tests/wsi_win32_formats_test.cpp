/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 *
 * The host gate of the Win32 WSI surface format list (wsi_win32_formats.h). It drives the
 * production table and rule, never a copy of them; it needs the Vulkan core header and
 * dxgiformat.h, and no Vulkan runtime, no DXGI and no lab.
 *
 *   wsi_win32_formats_test.exe                   every case must pass
 *   wsi_win32_formats_test.exe --pre-bd197       the same cases against a driver that does not
 *                                                answer the plane question and a WSI that keeps
 *                                                the first answer; it must fail
 *
 * BD-197. The DXGI route asks the driver whether the scan-out plane takes RGBA8 as well as BGRA8.
 * The cases: (1) a plane that takes BGRA8 alone withdraws R8G8B8A8_UNORM and R8G8B8A8_SRGB and
 * nothing else, in every combination of the other inputs; the 10-bit, FP16 and HDR pairs stay;
 * (2) a plane that takes all formats, and a driver without the hook, give the list of 042227dc;
 * (3) the GDI route never asks; (4) the reading is asked again at each listing: two listings with
 * two trailer states give two lists.
 */

#include "../wsi_win32_formats.h"

#include <cstdio>
#include <cstring>

static unsigned checks;
static unsigned failures;

static void
check(bool ok, const char *what)
{
   checks++;
   if (!ok) {
      failures++;
      std::printf("FAIL %s\n", what);
   }
}

/* The fake driver: the plane reading it gives now, and how often it was asked. */
struct fake_plane {
   bool all_formats;
   unsigned asked;
   /* --pre-bd197: keep the first answer, the shape this gate exists to refuse. */
   bool cache;
   bool cached;
   bool cached_answer;
};

static struct fake_plane plane;

static bool
fake_plane_formats(VkPhysicalDevice pdevice, const char **reason)
{
   (void)pdevice;
   plane.asked++;
   if (plane.cache) {
      if (!plane.cached) {
         plane.cached = true;
         plane.cached_answer = plane.all_formats;
      }
      *reason = "cached";
      return plane.cached_answer;
   }
   *reason = plane.all_formats ? "all-formats" : "bgra8-only";
   return plane.all_formats;
}

struct list {
   unsigned count;
   struct wsi_win32_surface_format f[WSI_WIN32_SURFACE_FORMATS_MAX];
};

static struct list
list_of(const struct wsi_win32_format_inputs *in)
{
   struct list l;
   std::memset(&l, 0, sizeof(l));
   l.count = wsi_win32_surface_formats_of(in, l.f);
   return l;
}

static bool
has(const struct list *l, VkFormat format, VkColorSpaceKHR color_space)
{
   for (unsigned i = 0; i < l->count; i++)
      if (l->f[i].format == format && l->f[i].color_space == color_space)
         return true;
   return false;
}

static bool
same(const struct list *a, const struct list *b)
{
   if (a->count != b->count)
      return false;
   for (unsigned i = 0; i < a->count; i++)
      if (a->f[i].format != b->f[i].format || a->f[i].color_space != b->f[i].color_space)
         return false;
   return true;
}

/* a without its RGBA8 pairs, in a's order. */
static struct list
without_rgba8(const struct list *a)
{
   struct list l;
   std::memset(&l, 0, sizeof(l));
   for (unsigned i = 0; i < a->count; i++)
      if (!wsi_win32_format_is_rgba8(a->f[i].format))
         l.f[l.count++] = a->f[i];
   return l;
}

static bool
has_rgba8(const struct list *l)
{
   for (unsigned i = 0; i < l->count; i++)
      if (wsi_win32_format_is_rgba8(l->f[i].format))
         return true;
   return false;
}

int
main(int argc, char **argv)
{
   const bool control = argc > 1 && !std::strcmp(argv[1], "--pre-bd197");
   /* The control's driver does not answer the plane question at all (the ICD of 042227dc). */
   bool (*const hook)(VkPhysicalDevice, const char **) = control ? NULL : fake_plane_formats;
   int device = 0;
   const VkPhysicalDevice pdevice = reinterpret_cast<VkPhysicalDevice>(&device);
   char what[160];

   /* 1-3. Every combination of the other inputs, three plane answers each. */
   for (unsigned bits = 0; bits < 16; bits++) {
      struct wsi_win32_format_inputs in = {};
      in.dxgi = (bits & 1u) != 0;
      in.colorspace_ext = (bits & 2u) != 0;
      in.output_hdr = (bits & 4u) != 0;
      in.force_bgra8_unorm_first = (bits & 8u) != 0;
      in.pdevice = pdevice;
      std::snprintf(what, sizeof(what), "dxgi %u colorspace %u hdr %u bgra8-first %u",
                    in.dxgi ? 1u : 0u, in.colorspace_ext ? 1u : 0u, in.output_hdr ? 1u : 0u,
                    in.force_bgra8_unorm_first ? 1u : 0u);

      /* The list of 042227dc: no hook. */
      in.scanout_plane_formats = NULL;
      const struct list before = list_of(&in);

      std::memset(&plane, 0, sizeof(plane));
      plane.all_formats = true;
      in.scanout_plane_formats = fake_plane_formats;
      const struct list all = list_of(&in);
      const unsigned asked_all = plane.asked;

      std::memset(&plane, 0, sizeof(plane));
      plane.all_formats = false;
      in.scanout_plane_formats = hook;
      const struct list bgra8 = list_of(&in);
      const unsigned asked_bgra8 = plane.asked;

      char line[256];
      std::snprintf(line, sizeof(line), "%s: all-formats plane gives the list of 042227dc", what);
      check(same(&all, &before), line);

      if (!in.dxgi) {
         std::snprintf(line, sizeof(line), "%s: the GDI route never asks the plane", what);
         check(asked_all == 0 && asked_bgra8 == 0, line);
         std::snprintf(line, sizeof(line), "%s: the GDI route list does not change", what);
         check(same(&bgra8, &before), line);
         continue;
      }

      std::snprintf(line, sizeof(line), "%s: the DXGI route asks the plane once per listing", what);
      check(asked_all == 1 && asked_bgra8 == 1, line);

      /* The one change: the RGBA8 pairs go, and every other pair stays in its order. */
      const struct list expect = without_rgba8(&before);
      std::snprintf(line, sizeof(line), "%s: bgra8-only withdraws exactly the RGBA8 pairs", what);
      check(same(&bgra8, &expect), line);
      std::snprintf(line, sizeof(line), "%s: bgra8-only lists no RGBA8", what);
      check(!has_rgba8(&bgra8), line);
      std::snprintf(line, sizeof(line), "%s: the list before had both RGBA8 formats", what);
      check(has(&before, VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) &&
            has(&before, VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR), line);
      std::snprintf(line, sizeof(line), "%s: BGRA8 UNORM and SRGB stay", what);
      check(has(&bgra8, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) &&
            has(&bgra8, VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR), line);
      std::snprintf(line, sizeof(line), "%s: A2B10G10R10 sRGB stays", what);
      check(has(&bgra8, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR),
            line);
      std::snprintf(line, sizeof(line), "%s: FP16 extended linear as before", what);
      check(has(&bgra8, VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) ==
            in.colorspace_ext, line);
      std::snprintf(line, sizeof(line), "%s: A2B10G10R10 HDR10 as before", what);
      check(has(&bgra8, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) ==
            (in.colorspace_ext && in.output_hdr), line);
      /* A2R10G10B10 has no row in the table (DXGI has no B-first 10-bit buffer): absent before and
       * after, so the plane reading does not touch it either.
       */
      std::snprintf(line, sizeof(line), "%s: A2R10G10B10 is no row, before or after", what);
      check(!has(&before, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) &&
            !has(&bgra8, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR),
            line);
      std::snprintf(line, sizeof(line), "%s: bgra8-first still puts B8G8R8A8_UNORM first", what);
      check(!in.force_bgra8_unorm_first ||
            (bgra8.count && bgra8.f[0].format == VK_FORMAT_B8G8R8A8_UNORM), line);
   }

   /* 4. Two listings, two trailer states, and back: each listing gets the reading of its moment.
    * The control keeps the first answer, as a WSI that cached the reading would.
    */
   {
      struct wsi_win32_format_inputs in = {};
      in.dxgi = true;
      in.pdevice = pdevice;
      in.scanout_plane_formats = fake_plane_formats;
      std::memset(&plane, 0, sizeof(plane));
      plane.cache = control;

      plane.all_formats = false;
      const struct list first = list_of(&in);
      plane.all_formats = true;    /* the kernel driver publishes PLANE_FORMATS now */
      const struct list second = list_of(&in);
      plane.all_formats = false;   /* and withdraws it */
      const struct list third = list_of(&in);

      check(!has_rgba8(&first), "listing 1, plane bgra8-only: no RGBA8");
      check(has(&second, VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) &&
            has(&second, VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR),
            "listing 2, plane all-formats: both RGBA8 formats");
      check(!has_rgba8(&third), "listing 3, plane bgra8-only again: no RGBA8");
      check(plane.asked == 3, "three listings asked the plane three times");
      check(second.count == first.count + 2 && third.count == first.count,
            "the two states differ by the two RGBA8 pairs alone");
   }

   std::printf("formats%s: %u checks, %u failures\n", control ? " (pre-BD-197 control)" : "",
               checks, failures);
   return failures ? 1 : 0;
}
