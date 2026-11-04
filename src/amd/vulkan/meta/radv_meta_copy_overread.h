/* SPDX-License-Identifier: MIT */

#ifndef RADV_META_COPY_OVERREAD_H
#define RADV_META_COPY_OVERREAD_H

#include <stdint.h>

/* bc250 (BD-290): the part at the end of a compute buffer copy that has to go another way.
 *
 * The compute copy shader (ac_nir_meta_cs_clear_copy_buffer.c) does not clamp the source. Each thread loads
 * S = dwords_per_thread * 4 bytes, and the window of the first thread starts dst_align_offset bytes before the
 * copy. So the last thread loads S - (dst_align_offset + size) % S bytes past the end of the source range, and a
 * load that is not dword aligned is made of whole dwords, which can add up to 3 bytes more. RADV gives the shader
 * addresses, not buffer descriptors with a size, so nothing stops that load at the end of the BO. When those
 * bytes are in the next 4 KiB page, that page can be outside every mapping, and on WDDM the GPU page fault ends
 * in a TDR. A load that ends in the page of the last source byte cannot fault, because a GPU mapping covers
 * whole pages.
 *
 * Returns 0 when the copy can run as it is. Otherwise returns the size of the tail: the bytes of the last thread,
 * MIN2((dst_align_offset + size) % S, size). The caller copies the first size - tail bytes with the compute
 * shader, with the same S and the same destination, so that copy ends on a thread boundary and loads no byte past
 * its range, and copies the tail with CP DMA, which reads only the bytes it copies.
 */
static inline uint64_t
radv_copy_memory_overread_tail(uint64_t src_va, uint64_t size, unsigned bytes_per_thread, unsigned dst_align_offset)
{
   const uint64_t page = 4096;

   if (!size || !bytes_per_thread)
      return 0;

   const uint64_t last_thread_bytes = (dst_align_offset + size) % bytes_per_thread;
   if (!last_thread_bytes)
      return 0;

   const uint64_t end = src_va + size; /* one past the last source byte */
   const uint64_t load_end = (end + (bytes_per_thread - last_thread_bytes) + 3) & ~(uint64_t)3;
   if ((end - 1) / page == (load_end - 1) / page)
      return 0;

   return last_thread_bytes < size ? last_thread_bytes : size;
}

#endif /* RADV_META_COPY_OVERREAD_H */
