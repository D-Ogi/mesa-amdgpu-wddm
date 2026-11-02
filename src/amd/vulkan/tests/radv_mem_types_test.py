# SPDX-License-Identifier: MIT
"""Host test of the RADV memory types on a Windows APU (BC-250), with no GPU.

It compiles the production radv_physical_device_init_mem_types (extracted from radv_physical_device.c with the
enum radv_heap and the helpers before it) against a model of the physical device fields that the function reads,
and checks the memory types for the split layout (two heaps) and the unified layout (one heap, BD-028):
- both layouts have a HOST_VISIBLE | HOST_CACHED type: the hosted D3D12 heap import requires one for a WRITE_BACK
  (readback) heap and has no fallback (icd-ckit3, test_queue_wait refused every readback buffer);
- every type names an existing heap, and the unified layout has exactly one heap, device-local;
- the cached types are GTT memory with CPU access, as in the split layout;
- a dedicated-VRAM device and a non-Windows build of the same function are unchanged by the Windows rule.

Usage: python radv_mem_types_test.py --source <mesa> --clang-cl <clang-cl.exe> --work <dir> [--baseline]
--baseline compiles the function without the Windows rule (negative control: the unified layout check fails).
"""
import argparse
import os
import re
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--source", required=True)
ap.add_argument("--clang-cl", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--baseline", action="store_true")
a = ap.parse_args()
src = os.path.abspath(a.source)
work = os.path.abspath(a.work)
os.makedirs(work, exist_ok=True)

with open(os.path.join(src, "src", "amd", "vulkan", "radv_physical_device.c"), encoding="utf-8") as f:
    device_source = f.read()
start = device_source.index("enum radv_heap {")
end = device_source.index("\nstatic void\nradv_get_binning_settings(", start)
body = device_source[start:end]
if a.baseline:
    body, n = re.subn(r"#ifdef _WIN32\n   if \(cached_index < 0[^\n]*\n[^\n]*\n#endif\n", "", body)
    if n != 1:
        sys.exit("baseline: the Windows rule was not found")
with open(os.path.join(src, "src", "amd", "vulkan", "radv_radeon_winsys.h"), encoding="utf-8") as f:
    winsys = f.read()
enums = "".join(re.search(r"enum %s \{.*?\n\};\n" % name, winsys, re.S).group(0)
                for name in ("radeon_bo_domain", "radeon_bo_flag"))

C = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define BITFIELD_BIT(b) (1u << (b))
static inline uint64_t align64(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
@ENUMS@
struct radv_physical_device {
   struct {
      bool has_dedicated_vram, all_vram_visible;
      uint32_t vram_size_kb, vram_vis_size_kb, gart_size_kb, gart_page_size;
   } info;
   struct {
      struct { bool enable_unified_heap_on_apu; } performance;
      struct { bool hide_rebar_on_dgpu; } debug;
      struct { int override_vram_size; } misc;
   } drirc;
   VkPhysicalDeviceMemoryProperties memory_properties;
   uint32_t heaps;
   enum radeon_bo_domain memory_domains[VK_MAX_MEMORY_TYPES];
   enum radeon_bo_flag memory_flags[VK_MAX_MEMORY_TYPES];
   uint32_t memory_types_32bit, memory_types_host_visible, memory_types_protected;
};
static bool radv_tmz_enabled(const struct radv_physical_device *pdev) { (void)pdev; return false; }
static bool radv_device_coherent_memory_enabled(const struct radv_physical_device *pdev) { (void)pdev; return false; }
#define _WIN32 1
@BODY@
/* Taken before the macros below rename the function for the non-Windows copy. */
static void (*const init_windows)(struct radv_physical_device *) = radv_physical_device_init_mem_types;
#undef _WIN32
#define radv_physical_device_init_mem_types radv_physical_device_init_mem_types_non_windows
#define radv_get_adjusted_vram_size radv_get_adjusted_vram_size_nw
#define radv_get_visible_vram_size radv_get_visible_vram_size_nw
#define radv_get_vram_size radv_get_vram_size_nw
#define radv_heap radv_heap_nw
#define RADV_HEAP_VRAM RADV_HEAP_VRAM_NW
#define RADV_HEAP_GTT RADV_HEAP_GTT_NW
#define RADV_HEAP_VRAM_VIS RADV_HEAP_VRAM_VIS_NW
#define RADV_HEAP_MAX RADV_HEAP_MAX_NW
@BODY@

static unsigned checks, failures;
static void check(bool ok, const char *what) { checks++; if (!ok) failures++; printf("%s %s\n", ok ? "PASS" : "FAIL", what); }

static void layout(struct radv_physical_device *p, bool apu, bool unified)
{
   *p = (struct radv_physical_device){0};
   p->info.has_dedicated_vram = !apu;
   p->info.gart_page_size = 4096;
   p->info.vram_size_kb = 8u << 20;      /* 8 GiB carve-out */
   p->info.vram_vis_size_kb = 8u << 20;
   p->info.gart_size_kb = 4u << 20;      /* 4 GiB GTT */
   p->drirc.misc.override_vram_size = -1;
   p->drirc.performance.enable_unified_heap_on_apu = unified;
}

static int cached_type(const struct radv_physical_device *p)
{
   for (unsigned i = 0; i < p->memory_properties.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags f = p->memory_properties.memoryTypes[i].propertyFlags;
      if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         return (int)i;
   }
   return -1;
}

static bool heaps_valid(const struct radv_physical_device *p)
{
   for (unsigned i = 0; i < p->memory_properties.memoryTypeCount; i++)
      if (p->memory_properties.memoryTypes[i].heapIndex >= p->memory_properties.memoryHeapCount)
         return false;
   return true;
}

static void dump(const char *name, const struct radv_physical_device *p)
{
   printf("%s: %u heaps, %u types:", name, p->memory_properties.memoryHeapCount, p->memory_properties.memoryTypeCount);
   for (unsigned i = 0; i < p->memory_properties.memoryTypeCount; i++)
      printf(" [%u flags 0x%x heap %u domain %d]", i, p->memory_properties.memoryTypes[i].propertyFlags,
             p->memory_properties.memoryTypes[i].heapIndex, p->memory_domains[i]);
   printf("\n");
}

int main(void)
{
   struct radv_physical_device p;
   int c;

   layout(&p, true, false);
   init_windows(&p);
   dump("split", &p);
   c = cached_type(&p);
   check(p.memory_properties.memoryHeapCount == 2, "split: two heaps");
   check(c >= 0 && p.memory_domains[c] == RADEON_DOMAIN_GTT && (p.memory_flags[c] & RADEON_FLAG_CPU_ACCESS) &&
            !(p.memory_properties.memoryHeaps[p.memory_properties.memoryTypes[c].heapIndex].flags &
              VK_MEMORY_HEAP_DEVICE_LOCAL_BIT),
         "split: a HOST_CACHED type, GTT with CPU access, in the GTT heap");
   check(heaps_valid(&p), "split: every type names an existing heap");
   const uint32_t split_types = p.memory_properties.memoryTypeCount;

   layout(&p, true, true);
   init_windows(&p);
   dump("unified", &p);
   c = cached_type(&p);
   check(p.memory_properties.memoryHeapCount == 1 &&
            p.memory_properties.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT &&
            p.memory_properties.memoryHeaps[0].size == (12ull << 30),
         "unified: one device-local heap of the carve-out plus GTT (12 GiB)");
   check(c >= 0 && p.memory_domains[c] == RADEON_DOMAIN_GTT && (p.memory_flags[c] & RADEON_FLAG_CPU_ACCESS) &&
            p.memory_properties.memoryTypes[c].heapIndex == 0,
         "unified: a HOST_CACHED type, GTT with CPU access, in the unified heap");
   check(c >= 0 && (p.memory_types_host_visible & BITFIELD_BIT(c)), "unified: the cached type is host-visible");
   check(heaps_valid(&p), "unified: every type names an existing heap");
   check(p.memory_properties.memoryTypeCount == split_types - 1,
         "unified: the split types less the GTT write-combined type");

   layout(&p, true, true);
   radv_physical_device_init_mem_types_non_windows(&p);
   check(cached_type(&p) < 0 && p.memory_properties.memoryHeapCount == 1,
         "unified, non-Windows build: unchanged (no HOST_CACHED type, as upstream)");

   layout(&p, false, false);
   p.info.vram_vis_size_kb = 256u << 10;
   init_windows(&p);
   check(cached_type(&p) >= 0 && heaps_valid(&p), "dedicated VRAM: HOST_CACHED in the GTT heap, as before");

   printf("radv_mem_types_test: %u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
'''
code = C.replace("@ENUMS@", enums).replace("@BODY@", body)
cfile = os.path.join(work, "radv_mem_types_test.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write(code)
exe = os.path.join(work, "radv_mem_types_test.exe")
cmd = [a.clang_cl, "/nologo", "/MT", "/W3", "/clang:-std=c11", "-Wno-unused-function",
       "-I" + os.path.join(src, "include"), cfile, "/Fo" + work + "\\", "/Fe" + exe]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stdout[-6000:], r.stderr[-3000:])
    sys.exit("build failed")
r = subprocess.run([exe], capture_output=True, text=True)
print(r.stdout.rstrip())
sys.exit(r.returncode)
