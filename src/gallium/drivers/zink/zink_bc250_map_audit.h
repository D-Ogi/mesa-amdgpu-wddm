/* SPDX-License-Identifier: MIT */
#ifndef ZINK_BC250_MAP_AUDIT_H
#define ZINK_BC250_MAP_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

struct zink_bc250_map_bucket {
   unsigned target, width, height, depth, format, bind, usage;
   unsigned box_width, box_height, box_depth, runtime, user_ptr;
   uint64_t calls, bytes;
};

/* Caller serializes the table and its aggregate counters. A failed growth
 * preserves every existing bucket; callers must count the missing request.
 * Keys are zero-initialized so the existing padding-aware comparison is stable.
 */
static inline bool
zink_bc250_map_record(struct zink_bc250_map_bucket **buckets,
                     unsigned *count, unsigned *capacity,
                     const struct zink_bc250_map_bucket *key, uint64_t bytes,
                     void *(*grow)(void *, size_t))
{
   for (unsigned i = 0; i < *count; ++i) {
      struct zink_bc250_map_bucket *b = &(*buckets)[i];
      if (!memcmp(b, key, offsetof(struct zink_bc250_map_bucket, calls))) {
         b->calls++;
         b->bytes += bytes;
         return true;
      }
   }
   if (*count == *capacity) {
      if (*capacity > UINT_MAX / 2)
         return false;
      unsigned next = *capacity ? *capacity * 2 : 128;
      if ((size_t)next > SIZE_MAX / sizeof(**buckets))
         return false;
      void *storage = grow(*buckets, (size_t)next * sizeof(**buckets));
      if (!storage)
         return false;
      *buckets = (struct zink_bc250_map_bucket *)storage;
      *capacity = next;
   }
   struct zink_bc250_map_bucket *b = &(*buckets)[(*count)++];
   *b = *key;
   b->calls = 1;
   b->bytes = bytes;
   return true;
}
#endif
