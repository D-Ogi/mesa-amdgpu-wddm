#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile production cache paths with fake adapter and hash-table transports.

Run from a C compiler environment: python winsys_cache_test.py --output <dir>.
No GPU or KMT calls. The backend records which adapter each new object opened.
The source boundaries deliberately fail closed when creation/destruction changes.
"""
import argparse
from pathlib import Path
import subprocess


def production(source):
    start = source.index("static simple_mtx_t winsys_creation_mutex")
    globals_ = source[start:source.index("static NTSTATUS\nquery_adapter_info", start)]
    start = source.index("VkResult\nradv_wddm2_winsys_create(")
    create = source[start:source.index("   ws->chain_ib =", start)]
    end = source.index("   _mesa_hash_table_insert", start)
    tail = source[end:source.index("error_create_device:", end)]
    create += "   (void)debug_flags; (void)status; ws->opened_luid = ws->adapter_luid; opens++;\n"
    create += tail + "fail:\n   simple_mtx_unlock(&winsys_creation_mutex); return result;\n}\n"
    start = source.index("static void\nradv_wddm2_winsys_destroy(")
    destroy = source[start:source.index("   if (ws->null_prt.bo)", start)]
    destroy += "   (void)status; closes++; free(ws);\n}\n"
    return globals_ + create + destroy


FAKE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef struct { uint32_t LowPart; int32_t HighPart; } LUID;
typedef int simple_mtx_t, NTSTATUS, VkResult, BITSET_WORD;
#define SIMPLE_MTX_INITIALIZER 0
#define ASSERTED
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_INCOMPATIBLE_DRIVER -9
static void simple_mtx_lock(simple_mtx_t *m) { assert(!*m); *m = 1; }
static void simple_mtx_unlock(simple_mtx_t *m) { assert(*m); *m = 0; }
struct radeon_winsys { int unused; };
struct bc250_host { const void *identity; LUID adapter_luid; };
struct vk_dx_adapter_info { LUID adapter_luid; };
struct radv_wddm2_winsys {
   struct radeon_winsys base;
   unsigned refcount;
   const void *cache_key;
   bool cache_hosted;
   struct bc250_host host;
   LUID adapter_luid, opened_luid;
};
static struct radv_wddm2_winsys *radv_wddm2_winsys(struct radeon_winsys *w) { return (void*)w; }
struct hash_entry { const void *key; void *data; };
struct hash_table {
   uint32_t (*hash)(const void*);
   bool (*equal)(const void*, const void*);
   unsigned count;
   struct hash_entry e[16];
};
static unsigned opens, closes, table_count;
static bool fail_table;
static uint32_t _mesa_hash_data(const void *p, size_t n) {
   const unsigned char *s = p; uint32_t h=0;
   while (n--) h = h*33 + *s++;
   return h;
}
static uint32_t pointer_hash(const void *p) { return (uint32_t)(uintptr_t)p; }
static bool pointer_equal(const void *a, const void *b) { return a == b; }
static struct hash_table *_mesa_hash_table_create(void *ctx, uint32_t (*hash)(const void*), bool (*equal)(const void*,const void*)) {
   (void)ctx;
   if (fail_table) return NULL;
   struct hash_table *t = calloc(1, sizeof(*t)); assert(t); t->hash=hash; t->equal=equal; table_count++; return t;
}
static struct hash_table *_mesa_pointer_hash_table_create(void *ctx) { return _mesa_hash_table_create(ctx,pointer_hash,pointer_equal); }
static struct hash_entry *_mesa_hash_table_search(struct hash_table *t, const void *key) {
   uint32_t hash=t->hash(key);
   for (unsigned i=0;i<t->count;i++) if (t->hash(t->e[i].key)==hash && t->equal(t->e[i].key,key)) return t->e+i;
   return NULL;
}
static void _mesa_hash_table_insert(struct hash_table *t, const void *key, void *data) { assert(t->count<16); t->e[t->count++]=(struct hash_entry){key,data}; }
static void _mesa_hash_table_remove_key(struct hash_table *t, const void *key) {
   struct hash_entry *e=_mesa_hash_table_search(t,key); assert(e); *e=t->e[--t->count];
}
static unsigned _mesa_hash_table_num_entries(struct hash_table *t) { return t->count; }
static void _mesa_hash_table_destroy(struct hash_table *t, void *d) { (void)d; assert(!t->count); table_count--; free(t); }
'''

TEST = r'''
static unsigned checks, failures;
#define CHECK(expr) do { checks++; if (!(expr)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); failures++; } } while(0)
static struct radeon_winsys *get(uint32_t lo, int32_t hi, const struct bc250_host *host) {
   struct vk_dx_adapter_info a={{lo,hi}};
   struct radeon_winsys *w=NULL;
   CHECK(radv_wddm2_winsys_create(&a,NULL,host,&w)==VK_SUCCESS);
   if (!w) exit(2);
   CHECK(radv_wddm2_winsys(w)->opened_luid.LowPart==lo);
   CHECK(radv_wddm2_winsys(w)->opened_luid.HighPart==hi);
   if (!host && radv_wddm2_winsys(w)->cache_key != &radv_wddm2_winsys(w)->adapter_luid) {
      CHECK(!"standalone key is not owned by its cached object");
      exit(1); /* Do not dereference a deliberately broken key in a negative control. */
   }
   memset(&a,0xCC,sizeof(a)); /* A cache key must not retain this caller's stack. */
   return w;
}
int main(void) {
   struct radeon_winsys *a=get(7,1,NULL), *b=get(7,2,NULL), *c=get(8,1,NULL);
   CHECK(a!=b && a!=c && b!=c); CHECK(opens==3);
   struct radeon_winsys *a2=get(7,1,NULL);
   CHECK(a==a2); CHECK(opens==3);
   radv_wddm2_winsys_destroy(a); CHECK(closes==0);
   radv_wddm2_winsys_destroy(b); CHECK(closes==1);
   struct radeon_winsys *c2=get(8,1,NULL); CHECK(c==c2);
   radv_wddm2_winsys_destroy(a2); CHECK(closes==2);
   radv_wddm2_winsys_destroy(c); radv_wddm2_winsys_destroy(c2); CHECK(closes==3 && table_count==0);
   a=get(0,0,NULL); b=get(0,-1,NULL); c=get(UINT32_MAX,-1,NULL);
   CHECK(a!=b && b!=c); radv_wddm2_winsys_destroy(b); radv_wddm2_winsys_destroy(a); radv_wddm2_winsys_destroy(c);
   CHECK(table_count==0);
   struct bc250_host h={(void*)(uintptr_t)1,{1,0}}, h2={(void*)(uintptr_t)2,{1,0}};
   a=get(1,0,NULL); b=get(1,0,&h); c=get(1,0,&h2); a2=get(1,0,&h);
   CHECK(a!=b && a!=c && b!=c && b==a2);
   struct vk_dx_adapter_info bad={{2,0}}; struct radeon_winsys *out=NULL;
   unsigned before=opens;
   CHECK(radv_wddm2_winsys_create(&bad,NULL,&h,&out)==VK_ERROR_INCOMPATIBLE_DRIVER);
   CHECK(opens==before && out==NULL);
   radv_wddm2_winsys_destroy(a); radv_wddm2_winsys_destroy(b);
   c2=get(1,0,&h2); CHECK(c==c2);
   radv_wddm2_winsys_destroy(a2); radv_wddm2_winsys_destroy(c); radv_wddm2_winsys_destroy(c2);
   CHECK(table_count==0 && opens==closes);
   fail_table=true;
   CHECK(radv_wddm2_winsys_create(&bad,NULL,NULL,&out)==VK_ERROR_OUT_OF_HOST_MEMORY);
   CHECK(radv_wddm2_winsys_create(&h.adapter_luid /* replaced below */,NULL,&h,&out)==VK_ERROR_OUT_OF_HOST_MEMORY);
   CHECK(opens==closes && table_count==0 && winsys_creation_mutex==0);
   printf("%u checks, %u failures\n",checks,failures);
   return failures ? 1 : 0;
}
'''.replace('&h.adapter_luid /* replaced below */', '&(struct vk_dx_adapter_info){{1,0}}')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--compiler', default='cl')
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'radv_wddm2_winsys.c')
    p.add_argument('--negative', choices=['high-half', 'borrowed-key'])
    args = p.parse_args()
    source = args.source.read_text(encoding='utf-8')
    if args.negative == 'high-half':
        source = source.replace('_mesa_hash_data(key, sizeof(LUID))', '_mesa_hash_data(key, sizeof(uint32_t))')
        source = source.replace(' && left->HighPart == right->HighPart', '')
    if args.negative == 'borrowed-key':
        source = source.replace('ws->cache_key = host ? key : &ws->adapter_luid;', 'ws->cache_key = key;')
    args.output.mkdir(parents=True, exist_ok=True)
    c = (args.output / 'cache.c').resolve()
    exe = c.with_suffix('.exe')
    c.write_text(FAKE + production(source) + TEST, encoding='utf-8')
    if Path(args.compiler).stem.lower() == 'cl':
        command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', str(c), '/Fe:' + str(exe), '/Fo:' + str(c.with_suffix('.obj'))]
    else:
        command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)]
    subprocess.run(command, check=True)
    return subprocess.run([str(exe)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
