/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_winsys_cache.h, the process-wide winsys cache. The D3D12 and D3D11 shells pin the ICD for
 * the process (BD-315), so the cache lives across a device loss, a TDR and a device removal. The contract: a device
 * destroyed after its loss releases its reference, the entry goes with the last reference, and the next open of the
 * same key (the same host identity, also a new host object at the address of the old one) gets a new winsys.
 *
 * open_device() and destroy_device() do what radv_wddm2_winsys_create and radv_wddm2_winsys_destroy do with the
 * cache (find and count, or make and insert; release, and destroy on the last reference), with a fake winsys. A device
 * loss changes nothing in the cache: radv_destroy_device releases the winsys of a lost device as of any other.
 *
 * Run by scratch\bd201-fix\tools\run-winsys-tests.ps1 -Test src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_winsys_cache_test.c
 * -Link util, one case per process: radv_wddm2_winsys_cache_test <case>.
 * Negative control: -Define BC250_TEST_SKIP_ONE_RELEASE skips the release of the first lost device that is destroyed
 * (a leaked reference). Then the cases lost_reopen, shared_lost and two_keys must fail.
 */
#include "../radv_wddm2_winsys_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
   do {                                                                                            \
      checks++;                                                                                    \
      if (!(cond)) {                                                                               \
         failures++;                                                                               \
         printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                    \
      } else {                                                                                     \
         printf("PASS %s:%d: %s\n", __func__, __LINE__, #cond);                                    \
      }                                                                                            \
   } while (0)

struct fake_ws {
   uint32_t refcount;
   const void *cache_key;
   unsigned generation; /* which make of a winsys this is: a reopen that gets a new winsys gets a new number */
};

struct fake_device {
   struct fake_ws *ws;
   bool lost;
};

static struct hash_table *cache;
static unsigned made;
#ifdef BC250_TEST_SKIP_ONE_RELEASE
static bool skipped;
#endif

/* radv_wddm2_winsys_create with the cache: a cached winsys of the key gets one more reference, else a new one. */
static struct fake_device
open_device(const void *key)
{
   struct fake_device d = {0};
   if (!radv_wddm2_cache_prepare(&cache))
      return d;
   d.ws = radv_wddm2_cache_find(cache, key);
   if (d.ws) {
      ++d.ws->refcount;
      return d;
   }
   d.ws = calloc(1, sizeof(*d.ws));
   if (!d.ws)
      return d;
   d.ws->refcount = 1;
   d.ws->cache_key = key;
   d.ws->generation = ++made;
   radv_wddm2_cache_insert(cache, key, d.ws);
   return d;
}

/* The device loss: the KMD reports it and radv marks the device lost. The cache is not touched. */
static void
lose(struct fake_device *d)
{
   d->lost = true;
}

/* radv_destroy_device -> radv_wddm2_winsys_destroy: one release, the winsys goes with the last reference. */
static void
destroy_device(struct fake_device *d)
{
#ifdef BC250_TEST_SKIP_ONE_RELEASE
   if (d->lost && !skipped) {
      skipped = true; /* negative control: the reference of a lost device leaks */
      d->ws = NULL;
      return;
   }
#endif
   if (radv_wddm2_cache_release(&cache, d->ws->cache_key, &d->ws->refcount))
      free(d->ws);
   d->ws = NULL;
}

/* One host identity: open, lose, destroy, open again. The reopen must get a new winsys. */
static void
test_lost_reopen(void)
{
   static int host;
   struct fake_device a = open_device(&host);
   CHECK(a.ws && a.ws->refcount == 1);
   const unsigned first = a.ws ? a.ws->generation : 0;
   lose(&a);
   destroy_device(&a);
   CHECK(cache == NULL);
   CHECK(radv_wddm2_cache_find(cache, &host) == NULL);
   struct fake_device b = open_device(&host);
   CHECK(b.ws && b.ws->generation != first);
   CHECK(b.ws && b.ws->refcount == 1);
   if (b.ws)
      destroy_device(&b);
   CHECK(cache == NULL);
}

/* Two references of one key (the physical device and the device): the entry stays while one is held, goes with the
 * second release, and a reopen gets a new winsys. */
static void
test_shared_lost(void)
{
   static int host;
   struct fake_device pdev = open_device(&host);
   struct fake_device dev = open_device(&host);
   CHECK(pdev.ws && pdev.ws == dev.ws && dev.ws->refcount == 2);
   const unsigned first = dev.ws ? dev.ws->generation : 0;
   lose(&dev);
   destroy_device(&dev);
   CHECK(pdev.ws && radv_wddm2_cache_find(cache, &host) == pdev.ws && pdev.ws->refcount == 1);
   destroy_device(&pdev);
   CHECK(cache == NULL);
   struct fake_device again = open_device(&host);
   CHECK(again.ws && again.ws->generation != first && again.ws->refcount == 1);
   if (again.ws)
      destroy_device(&again);
   CHECK(cache == NULL);
}

/* Two host identities: the loss and destroy of one leave the other cached; the lost key reopens new. */
static void
test_two_keys(void)
{
   static int host_a, host_b;
   struct fake_device a = open_device(&host_a);
   struct fake_device b = open_device(&host_b);
   CHECK(a.ws && b.ws && a.ws != b.ws);
   const unsigned first_a = a.ws ? a.ws->generation : 0;
   lose(&a);
   destroy_device(&a);
   CHECK(radv_wddm2_cache_find(cache, &host_a) == NULL);
   CHECK(b.ws && radv_wddm2_cache_find(cache, &host_b) == b.ws && b.ws->refcount == 1);
   struct fake_device a2 = open_device(&host_a);
   CHECK(a2.ws && a2.ws->generation != first_a && a2.ws->refcount == 1);
   if (a2.ws)
      destroy_device(&a2);
   destroy_device(&b);
   CHECK(cache == NULL);
}

/* A new host object at the address of the old one (heap reuse) is the same key: after the old device is destroyed it
 * must still get a new winsys. */
static void
test_address_reuse(void)
{
   void *host = malloc(64);
   struct fake_device a = open_device(host);
   const unsigned first = a.ws ? a.ws->generation : 0;
   destroy_device(&a);
   free(host);
   void *host2 = malloc(64); /* the same address or not: the case is the same key */
   struct fake_device b = open_device(host2);
   CHECK(b.ws && b.ws->generation != first && b.ws->refcount == 1);
   if (b.ws)
      destroy_device(&b);
   free(host2);
   CHECK(cache == NULL);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"lost_reopen", test_lost_reopen},
   {"shared_lost", test_shared_lost},
   {"two_keys", test_two_keys},
   {"address_reuse", test_address_reuse},
};

int
main(int argc, char **argv)
{
   unsigned run = 0;
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      tests[i].fn();
      run++;
   }
   if (!run) {
      printf("FAIL no case named %s\n", argc > 1 ? argv[1] : "(none)");
      return 2;
   }
   printf("tests=%u checks=%d failures=%d%s\n", run, checks, failures,
#ifdef BC250_TEST_SKIP_ONE_RELEASE
          " (negative control: one release skipped)"
#else
          ""
#endif
   );
   return failures ? 1 : 0;
}
