/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 *
 * The host gate of the GDI route's CPU copy (wsi_win32_cpu_copy.h). It drives the production
 * functions, never a copy of them; it needs no Mesa header, no Vulkan runtime and no lab. The
 * reference is the per-byte loop that the SSSE3 copy replaces, kept here and nowhere else.
 *
 *   wsi_win32_cpu_copy_test.exe                   every case must pass
 *   wsi_win32_cpu_copy_test.exe --swapped-mask    the SSSE3 cases with a shuffle that leaves R and B
 *                                                 of the fourth pixel of each step in place; it must fail
 *   wsi_win32_cpu_copy_test.exe --bench           also print the median time of each copy of a
 *                                                 1920x1200 and a 256x256 frame (a measurement, not
 *                                                 a gate)
 *
 * The cases: widths 1, 3, 4, 5, 15, 16, 17 and 1920, each with tight rows and with row pitches larger
 * than the width (a different one on each side), and with row starts that are 4-aligned but not
 * 16-aligned and not 4-aligned at all. For each: the SSSE3 swizzle and the scalar swizzle give the
 * bytes of the reference, the copy never writes into the row padding or past the last row, and a
 * B8G8R8A8 copy gives the source bytes unchanged. Then one full 1920x1200 frame through
 * wsi_win32_copy_frame on both paths, and a known value: R8G8B8A8 red (FF 00 00 FF) comes out as
 * B8G8R8A8 red (00 00 FF FF).
 */

#include "../wsi_win32_cpu_copy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

static unsigned checks;
static unsigned failures;

static void
check(bool ok, const char *what, unsigned width, size_t src_pitch, size_t dst_pitch, unsigned misalign)
{
   checks++;
   if (!ok) {
      failures++;
      if (failures <= 40)
         std::printf("FAIL %s: width %u, src pitch %zu, dst pitch %zu, misalign %u\n", what, width,
                     src_pitch, dst_pitch, misalign);
   }
}

static bool
host_has_ssse3()
{
#ifdef _MSC_VER
   int r[4];
   __cpuid(r, 1);
   return ((r[2] >> 9) & 1) != 0;
#else
   unsigned a, b, c, d;
   return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 9) & 1);
#endif
}

/* The per-byte loop of wsi_win32_queue_present before this test's header existed. */
static void
reference_frame(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch, unsigned width,
                unsigned height, bool swizzle)
{
   for (unsigned h = 0; h < height; h++) {
      if (swizzle) {
         for (unsigned x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
         }
      } else {
         memcpy(dst, src, width * 4);
      }
      dst += dst_pitch;
      src += src_pitch;
   }
}

static const uint8_t *shuffle = wsi_win32_rgba_to_bgra_shuffle;
static const uint8_t swapped_shuffle[16] = {
   2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 12, 13, 14, 15,
};

/* The SSSE3 row function over a frame, with the shuffle of this run. */
static void
ssse3_frame(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch, unsigned width,
            unsigned height)
{
   for (unsigned y = 0; y < height; y++)
      wsi_win32_shuffle_row_ssse3(dst + y * dst_pitch, src + y * src_pitch, width, shuffle);
}

static uint32_t rng = 0x2545F491u;

static uint8_t
next_byte()
{
   rng ^= rng << 13;
   rng ^= rng >> 17;
   rng ^= rng << 5;
   return (uint8_t)(rng >> 11);
}

enum path { PATH_SSSE3_ROW, PATH_FRAME_SSSE3, PATH_FRAME_SCALAR, PATH_FRAME_COPY };

static const char *const path_names[] = {
   "SSSE3 row swizzle", "frame swizzle, SSSE3", "frame swizzle, scalar", "frame BGRA8 copy",
};

/* One case: a source with random pixels, a destination full of a guard byte, the copy, and the
 * comparison with the reference over every byte of the destination buffer, padding and guard
 * included.
 */
static void
run_case(enum path p, unsigned width, unsigned height, size_t src_pitch, size_t dst_pitch,
         unsigned misalign)
{
   const size_t guard = 64;
   std::vector<uint8_t> src_buf(misalign + src_pitch * height + guard);
   std::vector<uint8_t> dst_buf(misalign + dst_pitch * height + guard, 0xCD);
   std::vector<uint8_t> ref_buf(dst_buf.size(), 0xCD);
   for (auto &b : src_buf)
      b = next_byte();
   const uint8_t *src = src_buf.data() + misalign;
   uint8_t *dst = dst_buf.data() + misalign;
   const bool swizzle = p != PATH_FRAME_COPY;

   reference_frame(ref_buf.data() + misalign, dst_pitch, src, src_pitch, width, height, swizzle);
   switch (p) {
   case PATH_SSSE3_ROW:
      ssse3_frame(dst, dst_pitch, src, src_pitch, width, height);
      break;
   case PATH_FRAME_SSSE3:
      wsi_win32_copy_frame(dst, dst_pitch, src, src_pitch, width, height, true, true);
      break;
   case PATH_FRAME_SCALAR:
      wsi_win32_copy_frame(dst, dst_pitch, src, src_pitch, width, height, true, false);
      break;
   case PATH_FRAME_COPY:
      wsi_win32_copy_frame(dst, dst_pitch, src, src_pitch, width, height, false, true);
      break;
   }
   check(dst_buf == ref_buf, path_names[p], width, src_pitch, dst_pitch, misalign);
   if (p == PATH_FRAME_COPY) {
      bool same = true;
      for (unsigned y = 0; y < height; y++)
         same = same && !memcmp(dst + y * dst_pitch, src + y * src_pitch, (size_t)width * 4);
      check(same, "BGRA8 rows equal the source rows", width, src_pitch, dst_pitch, misalign);
   }
}

static void
known_value()
{
   const uint8_t red_rgba[4] = { 0xFF, 0x00, 0x00, 0xFF };
   const uint8_t red_bgra[4] = { 0x00, 0x00, 0xFF, 0xFF };
   uint8_t src[8 * 4], dst_simd[8 * 4], dst_scalar[8 * 4];
   for (unsigned x = 0; x < 8; x++)
      memcpy(src + x * 4, red_rgba, 4);
   wsi_win32_copy_frame(dst_simd, sizeof(dst_simd), src, sizeof(src), 8, 1, true, true);
   wsi_win32_copy_frame(dst_scalar, sizeof(dst_scalar), src, sizeof(src), 8, 1, true, false);
   bool ok_simd = true, ok_scalar = true;
   for (unsigned x = 0; x < 8; x++) {
      ok_simd = ok_simd && !memcmp(dst_simd + x * 4, red_bgra, 4);
      ok_scalar = ok_scalar && !memcmp(dst_scalar + x * 4, red_bgra, 4);
   }
   check(ok_simd, "RGBA8 red is BGRA8 red, SSSE3", 8, sizeof(src), sizeof(dst_simd), 0);
   check(ok_scalar, "RGBA8 red is BGRA8 red, scalar", 8, sizeof(src), sizeof(dst_scalar), 0);
   check(wsi_win32_rgba_to_bgra(0xFF0000FFu) == 0xFFFF0000u, "scalar of 0xFF0000FF", 1, 4, 4, 0);
   check(wsi_win32_rgba_to_bgra(0x80402010u) == 0x80102040u, "scalar of 0x80402010", 1, 4, 4, 0);
}

/* The four copies of one frame, interleaved: each round times every copy once, so that a change of
 * clock or load during the run moves all four alike. The median of the rounds is printed per copy.
 */
static void
bench(unsigned w, unsigned h, unsigned rounds)
{
   const size_t pitch = (size_t)w * 4;
   std::vector<uint8_t> src(pitch * h), dst(pitch * h, 0);
   for (auto &b : src)
      b = next_byte();
   std::vector<double> t[4];
   for (unsigned r = 0; r < rounds; r++) {
      for (int k = 0; k < 4; k++) {
         auto a = std::chrono::steady_clock::now();
         if (k == 0)
            reference_frame(dst.data(), pitch, src.data(), pitch, w, h, true);
         else
            wsi_win32_copy_frame(dst.data(), pitch, src.data(), pitch, w, h, k != 3, k == 2);
         t[k].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
      }
   }
   double med[4];
   for (int k = 0; k < 4; k++) {
      std::sort(t[k].begin(), t[k].end());
      med[k] = t[k][rounds / 2];
   }
   std::printf("bench %ux%u, median of %u interleaved rounds, ms: per-byte reference %.3f, scalar %.3f, "
               "SSSE3 %.3f, memcpy %.3f\n",
               w, h, rounds, med[0], med[1], med[2], med[3]);
}

int
main(int argc, char **argv)
{
   bool control = false, want_bench = false;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--swapped-mask"))
         control = true;
      else if (!strcmp(argv[i], "--bench"))
         want_bench = true;
      else {
         std::printf("usage: %s [--swapped-mask] [--bench]\n", argv[0]);
         return 2;
      }
   }
   if (control)
      shuffle = swapped_shuffle;

   checks++;
   if (!host_has_ssse3()) {
      failures++;
      std::printf("FAIL this host has no SSSE3, so the SSSE3 path is not tested\n");
   }

   static const unsigned widths[] = { 1, 3, 4, 5, 15, 16, 17, 1920 };
   static const unsigned misaligns[] = { 0, 4, 1 };
   for (unsigned width : widths) {
      const size_t tight = (size_t)width * 4;
      const size_t pitches[][2] = { { tight, tight }, { tight + 64, tight + 12 }, { tight + 4, tight + 256 } };
      for (const auto &pp : pitches)
         for (unsigned m : misaligns)
            for (int p = PATH_SSSE3_ROW; p <= PATH_FRAME_COPY; p++)
               run_case((enum path)p, width, 5, pp[0], pp[1], m);
   }
   /* One frame at the Q2RTX size, with the pitch rounded up as a linear image's would be. */
   for (int p = PATH_SSSE3_ROW; p <= PATH_FRAME_COPY; p++)
      run_case((enum path)p, 1920, 1200, 1920 * 4 + 256, 1920 * 4, 0);
   known_value();

   if (want_bench) {
      bench(1920, 1200, 41);   /* 9.2 MB each way: the copy as GDI presents it, mostly memory time */
      bench(256, 256, 401);    /* 256 KiB each way, inside the L2: the instructions' own time */
   }
   std::printf("cpu copy%s: %u checks, %u failures\n", control ? " (swapped-mask control)" : "", checks,
               failures);
   return failures ? 1 : 0;
}
