/*
 * Copyright 2026 the amdgpu-wddm authors
 * SPDX-License-Identifier: MIT
 */

#ifndef WSI_WIN32_CPU_COPY_H
#define WSI_WIN32_CPU_COPY_H

/* The CPU copy of the GDI route: one frame from the mapped blit buffer of a swapchain image into the
 * DIB section that BitBlt reads. GDI takes 32-bit BGRX only, so an R8G8B8A8 chain changes R and B on
 * the way, and a B8G8R8A8 chain copies each row unchanged. The functions need no Mesa header and no
 * Windows runtime, so that a host test drives them (src/vulkan/wsi/tests/wsi_win32_cpu_copy_test.cpp).
 * The caller selects the SSSE3 path from util_get_cpu_caps(); no build flag is needed, because only
 * the one function below is compiled for SSSE3.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define WSI_WIN32_CPU_COPY_SSSE3 1
#include <tmmintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#define WSI_WIN32_TARGET_SSSE3 __attribute__((target("ssse3")))
#else
#define WSI_WIN32_TARGET_SSSE3
#endif
#endif

/* One R8G8B8A8 pixel as B8G8R8A8. On this little-endian target R, G, B and A are bits 0-7, 8-15,
 * 16-23 and 24-31 of the 32-bit load: G and A stay, R and B change places.
 */
static inline uint32_t
wsi_win32_rgba_to_bgra(uint32_t v)
{
   return (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
}

static inline void
wsi_win32_swizzle_row_scalar(uint8_t *dst, const uint8_t *src, unsigned width)
{
   for (unsigned x = 0; x < width; x++) {
      uint32_t v;
      memcpy(&v, src + (size_t)x * 4, 4);
      v = wsi_win32_rgba_to_bgra(v);
      memcpy(dst + (size_t)x * 4, &v, 4);
   }
}

#ifdef WSI_WIN32_CPU_COPY_SSSE3
/* The byte order of one 16-byte step (four pixels) after the shuffle: bytes 0 and 2 of each pixel
 * change places.
 */
static const uint8_t wsi_win32_rgba_to_bgra_shuffle[16] = {
   2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
};

/* Four pixels per step with one PSHUFB; the remaining zero to three pixels of the row go through
 * the scalar swizzle. Loads and stores are unaligned: neither the DIB section nor the blit buffer
 * gives a 16-byte row start. The shuffle is a parameter so that the host test can give a wrong one
 * and see its comparison fail; the frame copy below always gives wsi_win32_rgba_to_bgra_shuffle.
 */
WSI_WIN32_TARGET_SSSE3 static inline void
wsi_win32_shuffle_row_ssse3(uint8_t *dst, const uint8_t *src, unsigned width,
                            const uint8_t shuffle[16])
{
   const __m128i mask = _mm_loadu_si128((const __m128i *)shuffle);
   unsigned x = 0;
   for (; x + 4 <= width; x += 4) {
      const __m128i pixels = _mm_loadu_si128((const __m128i *)(src + (size_t)x * 4));
      _mm_storeu_si128((__m128i *)(dst + (size_t)x * 4), _mm_shuffle_epi8(pixels, mask));
   }
   wsi_win32_swizzle_row_scalar(dst + (size_t)x * 4, src + (size_t)x * 4, width - x);
}
#endif

/* Copies width x height 32-bit pixels from src to dst, each with its own row pitch in bytes. With
 * swizzle, R and B change places; ssse3 says the CPU has SSSE3 and is ignored where the target has
 * no SSE.
 */
static inline void
wsi_win32_copy_frame(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch,
                     unsigned width, unsigned height, bool swizzle, bool ssse3)
{
   (void)ssse3;
   for (unsigned y = 0; y < height; y++, dst += dst_pitch, src += src_pitch) {
      if (!swizzle)
         memcpy(dst, src, (size_t)width * 4);
#ifdef WSI_WIN32_CPU_COPY_SSSE3
      else if (ssse3)
         wsi_win32_shuffle_row_ssse3(dst, src, width, wsi_win32_rgba_to_bgra_shuffle);
#endif
      else
         wsi_win32_swizzle_row_scalar(dst, src, width);
   }
}

#endif /* WSI_WIN32_CPU_COPY_H */
