/*
 * Copyright © FIXME
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_WINSYS_BO_H
#define RADV_WINSYS_BO_H

#include "radv_radeon_winsys.h"
#include "tools/radv_debug.h"

#include "util/list.h"
#include "util/os_time.h"
#include "util/rwlock.h"

struct radv_winsys_bo_list {
   struct radeon_winsys_bo **bos;
   uint32_t count;
   uint32_t capacity;
   struct u_rwlock lock;
};

struct radv_winsys_bo_log_entry {
   struct list_head list;
   uint64_t va;
   uint64_t size;
   uint64_t timestamp; /* CPU timestamp */
   uint64_t mapped_va;
   uint8_t is_virtual : 1;
   uint8_t destroyed : 1;
   uint8_t virtual_mapping : 1;
};

struct radv_winsys_bo_log {
   struct u_rwlock lock;
   struct list_head list;
   bool keep_log;
   FILE *history_logfile;
};

static int
radv_winsys_bo_va_compare(const void *a, const void *b)
{
   const struct radeon_winsys_bo *bo_a = *(const struct radeon_winsys_bo *const *)a;
   const struct radeon_winsys_bo *bo_b = *(const struct radeon_winsys_bo *const *)b;
   return bo_a->va < bo_b->va ? -1 : bo_a->va > bo_b->va ? 1 : 0;
}

static uint64_t
radv_winsys_canonicalize_va(uint64_t va)
{
   /* Would be less hardcoded to use addr32_hi (0xffff8000) to generate a mask,
    * but there are confusing differences between page fault reports from kernel where
    * it seems to report the top 48 bits, where addr32_hi has 47-bits. */
   return va & ((1ull << 48) - 1);
}

static void
radv_winsys_bo_list_init(struct radv_winsys_bo_list *list)
{
   u_rwlock_init(&list->lock);
}

static void
radv_winsys_bo_list_destroy(struct radv_winsys_bo_list *list)
{
   u_rwlock_destroy(&list->lock);
   free(list->bos);
}

static int
radv_winsys_bo_list_add(struct radv_winsys_bo_list *list, struct radeon_winsys_bo *bo)
{
   u_rwlock_wrlock(&list->lock);
   if (list->count == list->capacity) {
      unsigned capacity = MAX2(4, list->capacity * 2);
      void *data = realloc(list->bos, capacity * sizeof(struct radeon_winsys_bo *));
      if (!data) {
         u_rwlock_wrunlock(&list->lock);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      list->bos = (struct radeon_winsys_bo **)data;
      list->capacity = capacity;
   }

   list->bos[list->count++] = bo;
   bo->use_global_list = true;
   u_rwlock_wrunlock(&list->lock);
   return VK_SUCCESS;
}

static void
radv_winsys_bo_list_del(struct radv_winsys_bo_list *list, struct radeon_winsys_bo *bo)
{
   u_rwlock_wrlock(&list->lock);
   for (unsigned i = list->count; i-- > 0;) {
      if (list->bos[i] == bo) {
         list->bos[i] = list->bos[list->count - 1];
         --list->count;
         bo->use_global_list = false;
         break;
      }
   }
   u_rwlock_wrunlock(&list->lock);
}

static bool
radv_winsys_bo_list_get_cpu_addr(struct radv_winsys_bo_list *list, struct radeon_winsys *ws,
                                 uint64_t addr, void **cpu_addr)
{
   u_rwlock_rdlock(&list->lock);
   for (uint32_t i = 0; i < list->count; i++) {
      struct radeon_winsys_bo *bo = list->bos[i];
      if (addr >= bo->va && addr - bo->va < bo->size) {
         void *map = radv_buffer_map(ws, bo);
         if (map) {
            u_rwlock_rdunlock(&list->lock);
            *cpu_addr = (char *)map + (addr - bo->va);
            return true;
         }
      }
   }
   u_rwlock_rdunlock(&list->lock);
   return false;
}

static void
radv_winsys_dump_bo_ranges(struct radv_winsys_bo_list *list, FILE *file)
{
   struct radeon_winsys_bo **bos = NULL;
   int i = 0;

   u_rwlock_rdlock(&list->lock);
   bos = malloc(sizeof(*bos) * list->count);
   if (!bos) {
      u_rwlock_rdunlock(&list->lock);
      fprintf(file, "  Failed to allocate memory to sort VA ranges for dumping\n");
      return;
   }

   for (i = 0; i < list->count; i++) {
      bos[i] = list->bos[i];
   }
   qsort(bos, list->count, sizeof(bos[0]), radv_winsys_bo_va_compare);

   for (i = 0; i < list->count; ++i) {
      fprintf(file, "  VA=%.16llx-%.16llx, handle=%d\n", (long long)radv_winsys_canonicalize_va(bos[i]->va),
              (long long)radv_winsys_canonicalize_va(bos[i]->va + bos[i]->size), bos[i]->handle);
   }
   free(bos);
   u_rwlock_rdunlock(&list->lock);
}

static void
radv_winsys_bo_log_init(struct radv_winsys_bo_log *log, const BITSET_WORD *debug_flags)
{
   list_inithead(&log->list);
   u_rwlock_init(&log->lock);

   log->keep_log = !!(BITSET_TEST(debug_flags, RADV_DEBUG_HANG));

#ifndef _WIN32
   if (BITSET_TEST(debug_flags, RADV_DEBUG_DUMP_BO_HISTORY)) {
      log->history_logfile = fopen("/tmp/radv_bo_history.log", "w+");
      if (!log->history_logfile)
         fprintf(stderr, "radv: Failed to create /tmp/radv_bo_history.log.\n");
   }
#endif
}

static void
radv_winsys_bo_log_destroy(struct radv_winsys_bo_log *log)
{
   u_rwlock_destroy(&log->lock);

   if (log->history_logfile)
      fclose(log->history_logfile);

}

static void
radv_winsys_log_bo(struct radv_winsys_bo_log *log, struct radeon_winsys_bo *bo, bool destroyed)
{
   struct radv_winsys_bo_log_entry *entry = NULL;
   const uint64_t timestamp = os_time_get_nano();

   if (log->keep_log) {
      entry = calloc(1, sizeof(*entry));
      if (!entry)
         return;

      entry->va = bo->va;
      entry->size = bo->size;
      entry->timestamp = timestamp;
      entry->is_virtual = bo->is_virtual;
      entry->destroyed = destroyed;

      u_rwlock_wrlock(&log->lock);
      list_addtail(&entry->list, &log->list);
      u_rwlock_wrunlock(&log->lock);
   }

   if (log->history_logfile) {
      fprintf(log->history_logfile, "timestamp=%llu, VA=%.16llx-%.16llx, destroyed=%d, is_virtual=%d\n",
              (long long)timestamp, (long long)radv_winsys_canonicalize_va(bo->va),
              (long long)radv_winsys_canonicalize_va(bo->va + bo->size), destroyed, bo->is_virtual);
      fflush(log->history_logfile);
   }
}

static void
radv_winsys_log_va_op(struct radv_winsys_bo_log *log, struct radeon_winsys_bo *bo,
                      uint64_t offset, uint64_t size, uint64_t virtual_va)
{
   struct radv_winsys_bo_log_entry *entry = NULL;
   uint64_t mapped_va = bo ? (bo->va + offset) : 0;
   const uint64_t timestamp = os_time_get_nano();

   if (log->keep_log) {
      entry = calloc(1, sizeof(*entry));
      if (!entry)
         return;

      entry->va = virtual_va;
      entry->size = size;
      entry->timestamp = timestamp;
      entry->virtual_mapping = 1;
      entry->mapped_va = mapped_va;

      u_rwlock_wrlock(&log->lock);
      list_addtail(&entry->list, &log->list);
      u_rwlock_wrunlock(&log->lock);
   }

   if (log->history_logfile) {
      fprintf(log->history_logfile, "timestamp=%llu, VA=%.16llx-%.16llx, mapped_to=%.16llx\n", (long long)timestamp,
              (long long)radv_winsys_canonicalize_va(virtual_va),
              (long long)radv_winsys_canonicalize_va(virtual_va + size),
              (long long)radv_winsys_canonicalize_va(mapped_va));
      fflush(log->history_logfile);
   }
}

static bool
radv_winsys_bo_log_find(struct radv_winsys_bo_log *log, uint64_t addr, bool *destroyed)
{
   bool found = false;
          
   u_rwlock_rdlock(&log->lock);
   list_for_each_entry_rev (struct radv_winsys_bo_log_entry, entry, &log->list, list) {
      if (addr >= entry->va && addr - entry->va < entry->size) {
         found = true;
         if (destroyed)
            *destroyed = entry->destroyed;
         break;
      }
   }
   u_rwlock_rdunlock(&log->lock);

   return found;
}

static void
radv_winsys_dump_bo_log(struct radv_winsys_bo_log *log, FILE *file)
{
   struct radv_winsys_bo_log_entry *entry = NULL;

   u_rwlock_rdlock(&log->lock);
   LIST_FOR_EACH_ENTRY (entry, &log->list, list) {
      if (entry->virtual_mapping) {
         fprintf(file, "timestamp=%llu, VA=%.16llx-%.16llx, mapped_to=%.16llx\n",
                 (long long)entry->timestamp,
                 (long long)radv_winsys_canonicalize_va(entry->va),
                 (long long)radv_winsys_canonicalize_va(entry->va + entry->size),
                 (long long)radv_winsys_canonicalize_va(entry->mapped_va));
      } else {
         fprintf(file, "timestamp=%llu, VA=%.16llx-%.16llx, destroyed=%d, is_virtual=%d\n",
                 (long long)entry->timestamp,
                 (long long)radv_winsys_canonicalize_va(entry->va),
                 (long long)radv_winsys_canonicalize_va(entry->va + entry->size), entry->destroyed,
                 entry->is_virtual);
      }
   }
   u_rwlock_rdunlock(&log->lock);
}

#endif /* RADV_WINSYS_BO_H */