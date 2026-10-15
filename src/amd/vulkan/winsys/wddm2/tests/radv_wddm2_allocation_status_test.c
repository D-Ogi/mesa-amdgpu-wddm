/* SPDX-License-Identifier: MIT */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT
#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_cs.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/bc250_host_bootstrap.h"
#ifdef Status
#undef Status
#endif
#include <windows.h>
#include "d3dkmthk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vk_wddm2_dispatch_table *vk_wddm2_dispatch_table_get(void)
{
   abort();
}

void print_hex_data(FILE *fp, const void *data, uint32_t size)
{
   (void)fp; (void)data; (void)size;
   abort();
}

const struct vk_sync_type vk_wddm2_monitored_fence_type = {0};
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %u: %s\n", __LINE__, #c); } } while (0)

struct host {
   uint32_t fail_op;
   NTSTATUS status;
   bool cleanup_removed;
   unsigned calls, creates, destroys, waits, live, unknown;
};

static int32_t dispatch(void *userdata, uint32_t op, void *arg)
{
   struct host *h = userdata;
   h->calls++;
   if (op == BC250_HOST_CreateAllocation2)
      h->creates++;
   if (op == BC250_HOST_DestroyAllocation2)
      h->destroys++;
   if (op == BC250_HOST_WaitForSynchronizationObjectFromCpu)
      h->waits++;
   if (h->cleanup_removed && op == BC250_HOST_DestroyAllocation2)
      return STATUS_DEVICE_REMOVED;
   if (op == h->fail_op)
      return h->status;
   switch (op) {
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *a = arg;
      a->pAllocationInfo2[0].hAllocation = 0x100 + ++h->live;
      return STATUS_SUCCESS;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      m->VirtualAddress = 0x2000000;
      m->PagingFenceValue = 2;
      return STATUS_PENDING;
   }
   case BC250_HOST_MakeResident:
      ((D3DDDI_MAKERESIDENT *)arg)->PagingFenceValue = 3;
      return STATUS_PENDING;
   case BC250_HOST_WaitForSynchronizationObjectFromCpu:
   case BC250_HOST_Evict:
   case BC250_HOST_FreeGpuVirtualAddress:
      return STATUS_SUCCESS;
   case BC250_HOST_DestroyAllocation2:
      CHECK(h->live != 0);
      h->live--;
      return STATUS_SUCCESS;
   default:
      h->unknown++;
      return STATUS_NOT_IMPLEMENTED;
   }
}

static void init(struct radv_wddm2_winsys *ws, struct host *h)
{
   memset(ws, 0, sizeof(*ws));
   memset(h, 0, sizeof(*h));
   ws->bc250 = true;
   ws->host.size = sizeof(ws->host);
   ws->host.identity = h;
   ws->host.userdata = h;
   ws->host.dispatch = dispatch;
   ws->device_h = 0x20;
   ws->paging_queue_h = 0x30;
   ws->paging_fence_h = 0x34;
   ws->gpu_info.pte_fragment_size = 0x200000;
   ws->gpu_info.gart_page_size = 4096;
   radv_wddm2_bo_init_functions(ws);
}

static VkResult create(struct radv_wddm2_winsys *ws, struct radeon_winsys_bo **bo)
{
   return ws->base.buffer_create(&ws->base, 4096, 4096, RADEON_DOMAIN_GTT,
                                RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, bo);
}

int main(void)
{
   const uint32_t ops[] = {BC250_HOST_CreateAllocation2, BC250_HOST_MapGpuVirtualAddress,
                          BC250_HOST_MakeResident, BC250_HOST_WaitForSynchronizationObjectFromCpu};
   const NTSTATUS statuses[] = {STATUS_DEVICE_REMOVED, STATUS_NO_MEMORY, STATUS_UNSUCCESSFUL};
   for (unsigned i = 0; i < 4; i++) {
      for (unsigned j = 0; j < 3; j++) {
         struct radv_wddm2_winsys ws, other;
         struct host h, other_host;
         init(&ws, &h);
         init(&other, &other_host);
         h.fail_op = ops[i];
         h.status = statuses[j];
         struct radeon_winsys_bo *bo = (void *)(uintptr_t)1;
         const bool lost = j == 0;
         CHECK(create(&ws, &bo) == (lost ? VK_ERROR_UNKNOWN : VK_ERROR_OUT_OF_DEVICE_MEMORY));
         CHECK(!bo && !h.live && !h.unknown && h.creates == 1);
         CHECK(ws.base.device_is_lost(&ws.base) == lost);
         CHECK(!other.base.device_is_lost(&other.base));
         h.fail_op = 0;
         const unsigned mark = h.calls;
         CHECK(create(&ws, &bo) == (lost ? VK_ERROR_UNKNOWN : VK_SUCCESS));
         CHECK(lost ? !bo && h.calls == mark : bo != NULL);
         if (bo)
            ws.base.buffer_destroy(&ws.base, bo);
         CHECK(!h.live && !h.unknown);
         CHECK(ws.base.device_is_lost(&ws.base) == lost);
      }
   }
   struct radv_wddm2_winsys ws;
   struct host h;
   init(&ws, &h);
   h.fail_op = BC250_HOST_MapGpuVirtualAddress;
   h.status = STATUS_NO_MEMORY;
   h.cleanup_removed = true;
   struct radeon_winsys_bo *bo = NULL;
   CHECK(create(&ws, &bo) == VK_ERROR_UNKNOWN && !bo);
   CHECK(ws.base.device_is_lost(&ws.base) && h.live == 1 && h.destroys == 1 && !h.unknown);
   const unsigned mark = h.calls;
   CHECK(create(&ws, &bo) == VK_ERROR_UNKNOWN && !bo && h.calls == mark);
   printf("allocation_status checks=%u failures=%u\n", checks, failures);
   return failures ? 1 : 0;
}
