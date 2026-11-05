/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * The process-wide winsys cache of radv_wddm2_winsys.c: one winsys per key (the host identity of a hosted device,
 * one key for every system device), shared by the radv devices and physical devices of that key and counted by
 * them. The caller holds winsys_creation_mutex around each call.
 *
 * The D3D12 and D3D11 shells pin this ICD for the process (bc250-win BD-315), so the cache now lives as long as the
 * process, also across a device loss, a TDR or a device removal. Before, the last adapter close unloaded the ICD and
 * the cache with it. So an entry must go with its last reference: radv_destroy_device releases the reference of a
 * device on every path, a lost device included, and the next open of that key then makes a new winsys instead of
 * getting the winsys of the destroyed device. tests/radv_wddm2_winsys_cache_test.c holds that contract.
 */
#ifndef RADV_WDDM2_WINSYS_CACHE_H
#define RADV_WDDM2_WINSYS_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/hash_table.h"

/* The cached winsys of key, or NULL when the table has none (also when there is no table). */
static inline void *
radv_wddm2_cache_find(struct hash_table *table, const void *key)
{
   struct hash_entry *entry = table ? _mesa_hash_table_search(table, key) : NULL;
   return entry ? entry->data : NULL;
}

/* Makes the table when there is none. False when it cannot be made (no memory). */
static inline bool
radv_wddm2_cache_prepare(struct hash_table **table)
{
   if (!*table)
      *table = _mesa_pointer_hash_table_create(NULL);
   return *table != NULL;
}

/* Adds a new winsys that holds one reference. The table exists (radv_wddm2_cache_prepare). */
static inline void
radv_wddm2_cache_insert(struct hash_table *table, const void *key, void *ws)
{
   _mesa_hash_table_insert(table, key, ws);
}

/* Drops one reference. True when it was the last one: the entry is removed, the table too when it has no entry left,
 * and the caller destroys the winsys. */
static inline bool
radv_wddm2_cache_release(struct hash_table **table, const void *key, uint32_t *refcount)
{
   if (--*refcount)
      return false;
   _mesa_hash_table_remove_key(*table, key);
   /* Clean the hashtable up if empty, though there is no empty function. */
   if (_mesa_hash_table_num_entries(*table) == 0) {
      _mesa_hash_table_destroy(*table, NULL);
      *table = NULL;
   }
   return true;
}

#endif
