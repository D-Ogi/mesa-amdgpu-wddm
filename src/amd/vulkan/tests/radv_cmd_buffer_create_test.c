/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of the error path of radv_create_cmd_buffer (BD-149): vkAllocateCommandBuffers when the
 * winsys cannot create the command stream. On a removed device (a PnP stop or a TDR) pfnAllocateCb
 * returns D3DDDIERR_DEVICEREMOVED, the IB allocation fails and cs_create returns NULL. The command
 * buffer that was half built must then be destroyed without a fault, every allocation must be given
 * back, and the call must return an error that vkAllocateCommandBuffers may return
 * (VK_ERROR_OUT_OF_DEVICE_MEMORY) with every output handle set to VK_NULL_HANDLE.
 *
 * The device is a zeroed radv_device with only the members that this path reads; its winsys is a
 * fake whose cs_create fails from a given call on. No GPU and no Windows driver are needed. The test
 * links against the compiled objects of vulkan_radeon.dll, so it checks the code that ships.
 *
 * A fault inside the call is caught (structured exception handling) and reported as a failed check
 * with its exception code and address, so the unfixed code shows its access violation at address 8.
 *
 * Usage: radv_cmd_buffer_create_test <case>; each case runs in its own process.
 */
#include "../radv_cmd_buffer.h"
#include "../radv_device.h"
#include "../radv_physical_device.h"
#include "../radv_radeon_winsys.h"
#include "vk_command_pool.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_instance.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static unsigned checks, failures;

static void
check(bool ok, const char *what, ...)
{
   char text[512];
   va_list va;
   va_start(va, what);
   vsnprintf(text, sizeof(text), what, va);
   va_end(va);
   checks++;
   if (!ok)
      failures++;
   printf("%s %s\n", ok ? "PASS" : "FAIL", text);
   fflush(stdout);
}

/* The pool's allocator counts what is still allocated. */
static int live_allocs;

static void *VKAPI_PTR
t_alloc(void *user, size_t size, size_t align, VkSystemAllocationScope scope)
{
   void *p = _aligned_malloc(size, align);
   if (p)
      live_allocs++;
   return p;
}

static void *VKAPI_PTR
t_realloc(void *user, void *old, size_t size, size_t align, VkSystemAllocationScope scope)
{
   if (!old)
      return t_alloc(user, size, align, scope);
   return _aligned_realloc(old, size, align);
}

static void VKAPI_PTR
t_free(void *user, void *p)
{
   if (!p)
      return;
   live_allocs--;
   _aligned_free(p);
}

static const VkAllocationCallbacks counting_alloc = {
   .pfnAllocation = t_alloc,
   .pfnReallocation = t_realloc,
   .pfnFree = t_free,
};

/* The fake winsys: cs_create fails from call number fail_from on (0: never). */
static struct {
   unsigned calls, fail_from, created, destroyed, failed;
} cs;

static struct ac_cmdbuf *
fake_cs_create(struct radeon_winsys *ws, enum amd_ip_type ip, bool is_secondary)
{
   cs.calls++;
   if (cs.fail_from && cs.calls >= cs.fail_from) {
      cs.failed++;
      return NULL; /* what radv_wddm2_cs_create returns when its IB allocation fails */
   }
   cs.created++;
   return calloc(1, sizeof(struct ac_cmdbuf));
}

static void
fake_cs_destroy(struct ac_cmdbuf *b)
{
   cs.destroyed++;
   free(b);
}

static struct radeon_winsys fake_ws;
static struct vk_instance instance;
static struct radv_physical_device *pdev;
static struct radv_device *device;

/* Queue family 0 is the general queue, 1 the compute queue. */
static void
make_device(void)
{
   memset(&cs, 0, sizeof(cs));
   live_allocs = 0;

   fake_ws.cs_create = fake_cs_create;
   fake_ws.cs_destroy = fake_cs_destroy;

   memset(&instance, 0, sizeof(instance));
   vk_object_base_instance_init(&instance, &instance.base, VK_OBJECT_TYPE_INSTANCE);
   list_inithead(&instance.debug_utils.instance_callbacks);
   list_inithead(&instance.debug_utils.callbacks);
   list_inithead(&instance.debug_report.callbacks);

   pdev = calloc(1, sizeof(*pdev));
   pdev->vk.instance = &instance;
   pdev->ws = &fake_ws;
   pdev->info.gfx_level = GFX10_3;
   pdev->vk_queue_to_radv[0] = RADV_QUEUE_GENERAL;
   pdev->vk_queue_to_radv[1] = RADV_QUEUE_COMPUTE;

   device = calloc(1, sizeof(*device));
   vk_object_base_init(&device->vk, &device->vk.base, VK_OBJECT_TYPE_DEVICE);
   device->vk.physical = &pdev->vk;
   device->vk.alloc = counting_alloc;
   device->vk.command_buffer_ops = &radv_cmd_buffer_ops;
   device->ws = &fake_ws;
}

static void
free_device(void)
{
   free(device);
   free(pdev);
}

/* The exception record is read in the filter: the handler runs after the unwind, when it is gone. */
static DWORD fault_code;
static ULONG_PTR fault_address, fault_rva;

static int
fault_filter(EXCEPTION_POINTERS *ep)
{
   fault_code = ep->ExceptionRecord->ExceptionCode;
   fault_rva = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress - (ULONG_PTR)GetModuleHandleA(NULL);
   if (fault_code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
      fault_address = ep->ExceptionRecord->ExceptionInformation[1];
   return EXCEPTION_EXECUTE_HANDLER;
}

/* Calls vkAllocateCommandBuffers through the common pool code, as DXVK does, and catches a fault. */
static VkResult
allocate(struct vk_command_pool *pool, VkCommandBufferLevel level, uint32_t count, VkCommandBuffer *out,
         DWORD *code, ULONG_PTR *address)
{
   const VkCommandBufferAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = vk_command_pool_to_handle(pool),
      .level = level,
      .commandBufferCount = count,
   };
   VkResult result = VK_RESULT_MAX_ENUM;
   fault_code = 0;
   fault_address = 0;
   fault_rva = 0;
   __try {
      result = vk_common_AllocateCommandBuffers(vk_device_to_handle(&device->vk), &info, out);
   } __except (fault_filter(GetExceptionInformation())) {
   }
   *code = fault_code;
   *address = fault_address;
   return result;
}

/* One allocation call of count buffers whose stream number fail_from (counted from 1) fails. */
static void
stream_failure(uint32_t family, VkCommandBufferLevel level, uint32_t count, unsigned fail_from)
{
   make_device();
   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = family,
   };
   struct vk_command_pool pool;
   vk_command_pool_init(&device->vk, &pool, &pool_info, &counting_alloc);
   cs.fail_from = fail_from;

   VkCommandBuffer out[4];
   for (unsigned i = 0; i < ARRAY_SIZE(out); i++)
      out[i] = (VkCommandBuffer)(uintptr_t)0x5a5a5a5a;
   DWORD code;
   ULONG_PTR address;
   const VkResult result = allocate(&pool, level, count, out, &code, &address);

   check(code == 0,
         "family %u level %u: no fault in vkAllocateCommandBuffers (exception 0x%08lX reading address 0x%llX, "
         "instruction at RVA 0x%llX)",
         family, level, code, (unsigned long long)address, (unsigned long long)fault_rva);
   if (code)
      return; /* the pool and its half-built buffer are in an unknown state */
   check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "the call returns VK_ERROR_OUT_OF_DEVICE_MEMORY (%d)", result);
   unsigned set = 0;
   for (unsigned i = 0; i < count; i++)
      set += out[i] != VK_NULL_HANDLE;
   check(set == 0, "every output handle is VK_NULL_HANDLE (%u of %u set)", set, count);
   check(cs.failed == 1 && cs.created == fail_from - 1,
         "the winsys refused stream %u (%u made, %u refused)", fail_from, cs.created, cs.failed);
   check(cs.destroyed == cs.created, "every stream made is destroyed (%u made, %u destroyed)", cs.created,
         cs.destroyed);
   check(list_is_empty(&pool.command_buffers), "the pool holds no command buffer");
   check(live_allocs == 0, "every pool allocation is freed (%d left)", live_allocs);

   vk_command_pool_finish(&pool);
   free_device();
}

static void
test_primary_gfx(void)
{
   stream_failure(0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, 1);
}

static void
test_secondary_gfx(void)
{
   stream_failure(0, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1, 1);
}

static void
test_primary_compute(void)
{
   stream_failure(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, 1);
}

/* The first buffer of the call is complete; the call destroys it again when the second one fails. */
static void
test_second_of_two(void)
{
   stream_failure(0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2, 2);
}

/* Control: when the winsys answers, the buffers are made and their destroy releases everything. */
static void
test_control(void)
{
   make_device();
   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   struct vk_command_pool pool;
   vk_command_pool_init(&device->vk, &pool, &pool_info, &counting_alloc);

   VkCommandBuffer out[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   DWORD code;
   ULONG_PTR address;
   const VkResult result = allocate(&pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2, out, &code, &address);
   check(code == 0 && result == VK_SUCCESS, "without a failure the call succeeds (%d, exception 0x%08lX)", result,
         code);
   if (code)
      return;
   check(out[0] && out[1] && cs.created == 2 && live_allocs == 2,
         "two buffers, two streams, two pool allocations (%u streams, %d allocations)", cs.created, live_allocs);

   vk_common_FreeCommandBuffers(vk_device_to_handle(&device->vk), vk_command_pool_to_handle(&pool), 2, out);
   check(cs.destroyed == 2 && live_allocs == 0 && list_is_empty(&pool.command_buffers),
         "vkFreeCommandBuffers releases them (%u streams destroyed, %d allocations left)", cs.destroyed, live_allocs);

   vk_command_pool_finish(&pool);
   free_device();
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"primary_gfx", test_primary_gfx},
   {"secondary_gfx", test_secondary_gfx},
   {"primary_compute", test_primary_compute},
   {"second_of_two", test_second_of_two},
   {"control", test_control},
};

int
main(int argc, char **argv)
{
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (argc > 1 && strcmp(argv[1], tests[i].name))
         continue;
      tests[i].fn();
   }
   printf("tests=1 checks=%u failures=%u\n", checks, failures);
   return checks && !failures ? 0 : 1;
}
