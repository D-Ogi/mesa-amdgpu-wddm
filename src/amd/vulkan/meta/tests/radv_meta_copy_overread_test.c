/* SPDX-License-Identifier: MIT
 *
 * BD-290 host check of radv_copy_memory_overread_tail (radv_meta_copy_overread.h).
 *
 * The oracle is a model of the source loads of the compute copy shader
 * (ac_nir_meta_cs_clear_copy_buffer.c, copy path, addresses in user data):
 * thread t of n = DIV_ROUND_UP(dst_align + size, S) loads S bytes from
 * src_va - dst_align + t * S, the first thread starts at src_va & ~3 when
 * src_va % 4 < dst_align, and every load is widened to whole dwords. Only the
 * 4 KiB pages that hold a byte of the source range count as mapped. A load
 * outside them is a fault.
 *
 * For each copy the test checks:
 * - without a split (the code before BD-290) the model finds the faults that
 *   the function must remove; a run that finds none would prove nothing;
 * - with the split, the compute part (size - tail bytes, same S, same
 *   destination) loads no byte outside the mapped pages, and the CP DMA part
 *   copies exactly the remaining bytes;
 * - the function splits only copies that fault without the split.
 *
 * Built and run on the host: clang -std=c11 -I<src/amd/vulkan/meta>
 * radv_meta_copy_overread_test.c, exit 0 = pass.
 */
#include "radv_meta_copy_overread.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE 4096ull

static unsigned failures;

/* Whether the compute copy of size bytes from src_va (S bytes per thread, destination misalignment dst_align) loads a
 * byte outside the pages of [range_lo, range_hi). */
static bool
compute_copy_faults(uint64_t src_va, uint64_t size, unsigned s, unsigned dst_align, uint64_t range_lo,
                    uint64_t range_hi)
{
   if (!size)
      return false;
   const uint64_t threads = (dst_align + size + s - 1) / s;
   const uint64_t first_page = range_lo / PAGE, last_page = (range_hi - 1) / PAGE;
   const unsigned src_align = src_va % 4;
   for (uint64_t t = 0; t < threads; t++) {
      uint64_t lo = src_va - dst_align + t * s;
      uint64_t hi = lo + s;
      if (t == 0 && src_align < dst_align)
         lo = src_va - src_align; /* the first thread skips the bytes before the buffer */
      lo &= ~3ull;
      hi = (hi + 3) & ~3ull;
      if (lo / PAGE < first_page || (hi - 1) / PAGE > last_page)
         return true;
   }
   return false;
}

static void
check_copy(uint64_t src_va, uint64_t size, unsigned s, unsigned dst_align, unsigned *faults_before,
           unsigned *splits)
{
   const uint64_t lo = src_va, hi = src_va + size;
   const bool faults = compute_copy_faults(src_va, size, s, dst_align, lo, hi);
   const uint64_t tail = radv_copy_memory_overread_tail(src_va, size, s, dst_align);

   *faults_before += faults;
   *splits += tail != 0;

   if (tail > size || (tail && !faults) || (!tail && faults)) {
      if (failures++ < 20)
         printf("FAIL src 0x%llx size %llu S %u dst_align %u: faults %d tail %llu\n", (unsigned long long)src_va,
                (unsigned long long)size, s, dst_align, faults, (unsigned long long)tail);
      return;
   }
   if (tail) {
      const uint64_t head = size - tail;
      /* The compute part: same S, same destination, so the same dst_align. */
      if ((head && (dst_align + head) % s) || compute_copy_faults(src_va, head, s, dst_align, lo, hi)) {
         if (failures++ < 20)
            printf("FAIL split src 0x%llx size %llu S %u dst_align %u: head %llu still loads out of range\n",
                   (unsigned long long)src_va, (unsigned long long)size, s, dst_align, (unsigned long long)head);
      }
      /* The CP DMA part is [src_va + head, src_va + size): inside the range by construction. */
      if (head + tail != size && failures++ < 20)
         printf("FAIL split sizes\n");
   }
}

int
main(void)
{
   static const unsigned sizes_per_thread[] = {4, 8, 12, 16};
   unsigned cases = 0, faults_before = 0, splits = 0;

   for (unsigned si = 0; si < sizeof(sizes_per_thread) / sizeof(sizes_per_thread[0]); si++) {
      const unsigned s = sizes_per_thread[si];
      for (unsigned dst_align = 0; dst_align < s; dst_align++) {
         /* The source range ends near a page boundary: 40 bytes before it to 40 bytes after it. */
         for (int end_off = -40; end_off <= 40; end_off++) {
            const uint64_t end = 0x10000000ull + 7 * PAGE + (int64_t)end_off;
            for (uint64_t size = 1; size <= 300; size++) {
               check_copy(end - size, size, s, dst_align, &faults_before, &splits);
               cases++;
            }
            static const uint64_t big[] = {4093, 4096, 4097, 65532, 65536, 65537, 1u << 20};
            for (unsigned b = 0; b < sizeof(big) / sizeof(big[0]); b++) {
               check_copy(end - big[b], big[b], s, dst_align, &faults_before, &splits);
               cases++;
            }
         }
      }
   }

   /* BD-287's case: 65536 bytes to destination offset 4, S = 8, source range ends at a page (BO) end. */
   const uint64_t bd287_tail = radv_copy_memory_overread_tail(0x100000ull - 65536, 65536, 8, 4);
   printf("%s BD-287 case: tail %llu (want 4)\n", bd287_tail == 4 ? "PASS" : "FAIL",
          (unsigned long long)bd287_tail);
   failures += bd287_tail != 4;

   /* BD-290's first step: the source range at the BO end, dstOffset 4, a size that is not a multiple of S. */
   const uint64_t bd290_tail = radv_copy_memory_overread_tail(0x200000ull - 1000, 1000, 16, 4);
   printf("%s BD-290 case: tail %llu (want (4 + 1000) %% 16 = 12)\n", bd290_tail == 12 ? "PASS" : "FAIL",
          (unsigned long long)bd290_tail);
   failures += bd290_tail != 12;

   printf("%s negative control: %u of %u copies load outside the source pages without the split\n",
          faults_before ? "PASS" : "FAIL", faults_before, cases);
   failures += !faults_before;
   printf("%s %u copies, %u split, %u failures\n", failures ? "FAIL" : "PASS", cases, splits, failures);
   return failures != 0;
}
