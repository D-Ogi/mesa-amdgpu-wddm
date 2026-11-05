/* SPDX-License-Identifier: MIT
 *
 * BD-309: the instance-level part of bc250GetMemoryAllocation (radv_bc250_memory.h), as shipped in
 * radv_instance.c. The query is reached as the D3D11 shell reaches it: vk_icdGetInstanceProcAddr with a
 * NULL instance. A device and a memory object are made by hand (zeroed structs with their object
 * types), and the winsys hook buffer_get_kmt_allocation is a fake that records its calls.
 *
 * Checked:
 *   - the name routes to the query through both GetInstanceProcAddr entries with a NULL instance;
 *   - a NULL argument, a revision or a size other than revision 1 and sizeof(struct
 *     bc250_memory_allocation): VK_ERROR_INITIALIZATION_FAILED, no winsys call, *out not written;
 *   - memory with no BO, an import (each import handle type the runtime uses), host-pointer memory and
 *     a winsys without the hook: VK_ERROR_FEATURE_NOT_PRESENT before any winsys call, the outputs
 *     cleared;
 *   - a BO the winsys refuses (a sparse reservation, an opened or a destroyed BO: the winsys test
 *     radv_wddm2_bo_lifetime_test kmt_allocation checks those states): VK_ERROR_FEATURE_NOT_PRESENT,
 *     the outputs cleared;
 *   - an accepted BO: VK_SUCCESS with the hook's allocation, BC250_MEMORY_ALLOCATION_CPU_MAPPED exactly
 *     while the hook reports a CPU map, and the BO's size; the hook gets the device's winsys and the
 *     memory's BO.
 *
 * Built and run by tools/build/radv-memory-query-test.py of the driver repository: radv_instance.c is
 * compiled from this tree with the build's own flags and linked with /FORCE:UNRESOLVED, because the
 * rest of the instance code needs the whole driver and none of it runs here.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radv_bc250_memory.h"
#include "radv_device.h"
#include "radv_device_memory.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL radv_GetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

static int checks, failures;

static void
check(bool ok, const char *what)
{
   checks++;
   if (!ok) {
      failures++;
      printf("FAIL %s\n", what);
   } else {
      printf("PASS %s\n", what);
   }
}

static struct {
   unsigned calls;
   struct radeon_winsys *ws;
   struct radeon_winsys_bo *bo;
   bool answer, mapped;
   uint32_t allocation;
} hook;

static bool
fake_get_kmt_allocation(struct radeon_winsys *ws, struct radeon_winsys_bo *bo, uint32_t *allocation, bool *cpu_mapped)
{
   hook.calls++;
   hook.ws = ws;
   hook.bo = bo;
   /* The winsys contract: zero outputs on a refusal. */
   *allocation = hook.answer ? hook.allocation : 0;
   *cpu_mapped = hook.answer && hook.mapped;
   return hook.answer;
}

static const struct bc250_memory_allocation sentinel = {
   BC250_MEMORY_ALLOCATION_REVISION, sizeof(struct bc250_memory_allocation), 0xa110ca7e, 0xf1a65, 0xb17e5b17e5ull};

static bool
same(const struct bc250_memory_allocation *a, const struct bc250_memory_allocation *b)
{
   return !memcmp(a, b, sizeof(*a));
}

static bool
cleared(const struct bc250_memory_allocation *o)
{
   return o->revision == BC250_MEMORY_ALLOCATION_REVISION && o->size == sizeof(*o) && !o->allocation && !o->flags &&
          !o->bytes;
}

int
main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   PFN_bc250_get_memory_allocation query =
      (PFN_bc250_get_memory_allocation)radv_GetInstanceProcAddr(VK_NULL_HANDLE, BC250_MEMORY_ALLOCATION_QUERY_NAME);
   check(query != NULL, "the name answers the query with a NULL instance");
   check((PFN_vkVoidFunction)query ==
            vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, BC250_MEMORY_ALLOCATION_QUERY_NAME),
         "vk_icdGetInstanceProcAddr, the shell's entry, gives the same function");
   check(strcmp(BC250_MEMORY_ALLOCATION_QUERY_NAME, "bc250GetMemoryAllocation") == 0 &&
            BC250_MEMORY_ALLOCATION_REVISION == 1u && BC250_MEMORY_ALLOCATION_CPU_MAPPED == 0x1u &&
            sizeof(struct bc250_memory_allocation) == 24,
         "the contract of revision 1: name, revision, flag, size 24");
   if (!query) {
      printf("%d checks, %d failures\n", checks, failures);
      return 1;
   }

   static struct radeon_winsys ws;
   ws.buffer_get_kmt_allocation = fake_get_kmt_allocation;
   struct radv_device *device = calloc(1, sizeof(*device));
   struct radv_device_memory *memory = calloc(1, sizeof(*memory));
   if (!device || !memory)
      return 2;
   device->vk.base.type = VK_OBJECT_TYPE_DEVICE;
   device->ws = &ws;
   memory->vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY;
   static struct radeon_winsys_bo bo;
   bo.size = 3 * 65536;
   memory->bo = &bo;
   const VkDevice dev = (VkDevice)device;
   const VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)memory;
   hook.answer = true;
   hook.allocation = 0x40000123;

   /* Arguments: refused before anything is read or written. */
   struct bc250_memory_allocation out = sentinel;
   hook.calls = 0;
   check(query(VK_NULL_HANDLE, mem, &out) == VK_ERROR_INITIALIZATION_FAILED && same(&out, &sentinel),
         "a NULL device: VK_ERROR_INITIALIZATION_FAILED, out not written");
   check(query(dev, VK_NULL_HANDLE, &out) == VK_ERROR_INITIALIZATION_FAILED && same(&out, &sentinel),
         "a NULL memory: VK_ERROR_INITIALIZATION_FAILED, out not written");
   check(query(dev, mem, NULL) == VK_ERROR_INITIALIZATION_FAILED, "a NULL out: VK_ERROR_INITIALIZATION_FAILED");
   const uint32_t revisions[] = {0, 2, 0xffffffffu};
   for (unsigned i = 0; i < sizeof(revisions) / sizeof(revisions[0]); i++) {
      struct bc250_memory_allocation bad = sentinel;
      bad.revision = revisions[i];
      struct bc250_memory_allocation want = bad;
      char what[128];
      snprintf(what, sizeof(what), "revision %u: VK_ERROR_INITIALIZATION_FAILED, out not written", revisions[i]);
      check(query(dev, mem, &bad) == VK_ERROR_INITIALIZATION_FAILED && same(&bad, &want), what);
   }
   const uint32_t sizes[] = {0, 16, sizeof(struct bc250_memory_allocation) - 1, sizeof(struct bc250_memory_allocation) + 8};
   for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
      struct bc250_memory_allocation bad = sentinel;
      bad.size = sizes[i];
      struct bc250_memory_allocation want = bad;
      char what[128];
      snprintf(what, sizeof(what), "size %u: VK_ERROR_INITIALIZATION_FAILED, out not written", sizes[i]);
      check(query(dev, mem, &bad) == VK_ERROR_INITIALIZATION_FAILED && same(&bad, &want), what);
   }
   check(hook.calls == 0, "no argument refusal asked the winsys");

   /* Memory that is not an allocation of this ICD: refused at the instance level, the winsys not asked. */
   memory->bo = NULL;
   out = sentinel;
   check(query(dev, mem, &out) == VK_ERROR_FEATURE_NOT_PRESENT && cleared(&out) && !hook.calls,
         "memory with no BO: VK_ERROR_FEATURE_NOT_PRESENT, outputs cleared, winsys not asked");
   memory->bo = &bo;
   const VkExternalMemoryHandleTypeFlagBits imports[] = {
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
   for (unsigned i = 0; i < sizeof(imports) / sizeof(imports[0]); i++) {
      memory->vk.import_handle_type = imports[i];
      out = sentinel;
      char what[160];
      snprintf(what, sizeof(what),
               "an import (handle type 0x%x): VK_ERROR_FEATURE_NOT_PRESENT, outputs cleared, winsys not asked",
               (unsigned)imports[i]);
      check(query(dev, mem, &out) == VK_ERROR_FEATURE_NOT_PRESENT && cleared(&out) && !hook.calls, what);
   }
   memory->vk.import_handle_type = 0;
   static char host_memory[64];
   memory->vk.host_ptr = host_memory;
   out = sentinel;
   check(query(dev, mem, &out) == VK_ERROR_FEATURE_NOT_PRESENT && cleared(&out) && !hook.calls,
         "host-pointer memory: VK_ERROR_FEATURE_NOT_PRESENT, outputs cleared, winsys not asked");
   memory->vk.host_ptr = NULL;
   ws.buffer_get_kmt_allocation = NULL;
   out = sentinel;
   check(query(dev, mem, &out) == VK_ERROR_FEATURE_NOT_PRESENT && cleared(&out),
         "a winsys without the hook: VK_ERROR_FEATURE_NOT_PRESENT, outputs cleared");
   ws.buffer_get_kmt_allocation = fake_get_kmt_allocation;

   /* A BO the winsys refuses: sparse reservation, opened or destroyed BO. */
   hook.answer = false;
   out = sentinel;
   check(query(dev, mem, &out) == VK_ERROR_FEATURE_NOT_PRESENT && cleared(&out) && hook.calls == 1 &&
            hook.ws == &ws && hook.bo == &bo,
         "a BO the winsys refuses: VK_ERROR_FEATURE_NOT_PRESENT, outputs cleared, the winsys asked once for that BO");

   /* Accepted. */
   hook.answer = true;
   hook.calls = 0;
   hook.bo = NULL;
   hook.ws = NULL;
   for (int mapped = 0; mapped < 2; mapped++) {
      hook.mapped = mapped;
      out = sentinel;
      const VkResult result = query(dev, mem, &out);
      char what[192];
      snprintf(what, sizeof(what),
               "an accepted BO, %s: VK_SUCCESS allocation 0x%x flags 0x%x bytes %llu (want 0x%x, 0x%x, %llu)",
               mapped ? "mapped" : "not mapped", out.allocation, out.flags, (unsigned long long)out.bytes,
               hook.allocation, mapped ? BC250_MEMORY_ALLOCATION_CPU_MAPPED : 0u, (unsigned long long)bo.size);
      check(result == VK_SUCCESS && out.allocation == hook.allocation &&
               out.flags == (mapped ? BC250_MEMORY_ALLOCATION_CPU_MAPPED : 0u) && out.bytes == bo.size &&
               out.revision == BC250_MEMORY_ALLOCATION_REVISION && out.size == sizeof(out),
            what);
   }
   check(hook.calls == 2 && hook.ws == &ws && hook.bo == &bo, "each accepted query asked the device's winsys for the memory's BO");

   free(memory);
   free(device);
   printf("%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
