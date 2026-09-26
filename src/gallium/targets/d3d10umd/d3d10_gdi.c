/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


/* BC250 experimental native D3D -> Zink path. Not a system UMD yet.
 * A nonzero adapter LUID is mandatory to prevent accidental GPU selection.
 * The bounded test supplies it from DXGI enumeration of the BC250 adapter.
 */
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>
#include "util/u_dl.h"
#include "util/os_misc.h"
#include "util/bc250_host_bootstrap.h"
#include <stdlib.h>
#include <errno.h>
#include "util/u_debug.h"
#include "pipe/p_screen.h"
#include "target-helpers/inline_debug_helper.h"
#include "zink/zink_public.h"

extern struct pipe_screen *d3d10_create_screen(void);

struct pipe_screen *
d3d10_create_screen(void)
{
   const char *value = debug_get_option("BC250_D3D_ZINK_LUID", "");
   char *end;
   errno = 0;
   uint64_t luid = strtoull(value, &end, 16);
   if (errno || end == value || *end || !luid) {
      debug_printf("BC250 D3D Zink: explicit adapter LUID required\n");
      return NULL;
   }
   struct pipe_screen *screen = zink_win32_create_screen(luid);
   if (!screen)
      return NULL;
   debug_printf("BC250 D3D renderer: %s\n", screen->get_name(screen));
   return debug_screen_wrap(screen);
}

static bool
bc250_hosted_content(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical)
{
   PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(instance,"vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance,"vkGetDeviceProcAddr");
   PFN_vkGetPhysicalDeviceMemoryProperties props = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance,"vkGetPhysicalDeviceMemoryProperties");
   float priority=1.0f;
   VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
   VkDeviceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
   VkDevice device=VK_NULL_HANDLE;
   VkResult result=create(physical,&ci,NULL,&device);
   fprintf(stderr,"BC250 hosted device result=%d\n",result);
   if(result!=VK_SUCCESS) return false;
#define LOAD(name) PFN_vk##name name=(PFN_vk##name)gdpa(device,"vk" #name)
   LOAD(DestroyDevice); LOAD(GetDeviceQueue); LOAD(CreateBuffer); LOAD(DestroyBuffer);
   LOAD(GetBufferMemoryRequirements); LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindBufferMemory);
   LOAD(MapMemory); LOAD(UnmapMemory); LOAD(InvalidateMappedMemoryRanges);
   LOAD(CreateCommandPool); LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers);
   LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer); LOAD(CmdFillBuffer); LOAD(CmdPipelineBarrier);
   LOAD(CreateFence); LOAD(DestroyFence); LOAD(QueueSubmit); LOAD(WaitForFences);
#undef LOAD
   VkBuffer buffer=VK_NULL_HANDLE;
   VkDeviceMemory memory=VK_NULL_HANDLE;
   VkCommandPool pool=VK_NULL_HANDLE;
   VkFence fence=VK_NULL_HANDLE;
   bool ok=false;
   VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16384,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
#define CHECK(call) do { result=(call); if(result!=VK_SUCCESS) { fprintf(stderr,"BC250 hosted %s result=%d\n",#call,result); goto done; } } while(0)
   CHECK(CreateBuffer(device,&bi,NULL,&buffer));
   VkMemoryRequirements req;
   GetBufferMemoryRequirements(device,buffer,&req);
   VkPhysicalDeviceMemoryProperties mp;
   props(physical,&mp);
   uint32_t type=UINT32_MAX;
   for(uint32_t i=0;i<mp.memoryTypeCount;i++)
      if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {type=i;break;}
   if(type==UINT32_MAX) goto done;
   VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
   CHECK(AllocateMemory(device,&ai,NULL,&memory));
   CHECK(BindBufferMemory(device,buffer,memory,0));
   VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
   CHECK(CreateCommandPool(device,&pi,NULL,&pool));
   VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer command;
   CHECK(AllocateCommandBuffers(device,&ca,&command));
   VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(command,&begin));
   CmdFillBuffer(command,buffer,0,16384,0x39c57a16);
   VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
   CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
   CHECK(EndCommandBuffer(command));
   VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(device,&fi,NULL,&fence));
   VkQueue queue;
   GetDeviceQueue(device,0,0,&queue);
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
   CHECK(QueueSubmit(queue,1,&si,fence));
   CHECK(WaitForFences(device,1,&fence,VK_TRUE,10000000000ull));
   uint32_t *mapped=NULL;
   CHECK(MapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void **)&mapped));
   VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
   result=InvalidateMappedMemoryRanges(device,1,&range);
   unsigned mismatches=0;
   if(result==VK_SUCCESS) for(unsigned i=0;i<4096;i++) mismatches+=mapped[i]!=0x39c57a16;
   UnmapMemory(device,memory);
   ok=result==VK_SUCCESS && mismatches==0;
   fprintf(stderr,"BC250 hosted content words=4096 mismatches=%u result=%d pass=%d\n",mismatches,result,ok);
done:
   if(fence) DestroyFence(device,fence,NULL);
   if(pool) DestroyCommandPool(device,pool,NULL);
   if(buffer) DestroyBuffer(device,buffer,NULL);
   if(memory) FreeMemory(device,memory,NULL);
   DestroyDevice(device,NULL);
#undef CHECK
   return ok;
}

/* Enumeration-only hosted bootstrap. Keep its module until instance teardown. */
bool
d3d10_hosted_bootstrap(struct bc250_host *host)
{
   const char *luid = os_get_option("BC250_D3D_ZINK_LUID");
   if (!luid) return false;
   host->adapter_luid = strtoull(luid, NULL, 16);
   const char *path = os_get_option("BC250_HOSTED_ICD");
   if (!path) return false;
   struct util_dl_library *lib = util_dl_open(path);
   if (!lib) return false;
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)
      util_dl_get_proc_address(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { util_dl_close(lib); return false; }
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "BC250 hosted bootstrap", .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = host, .pApplicationInfo = &app };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create(&ci, NULL, &instance);
   fprintf(stderr, "BC250 hosted instance result=%d runtime=%p\n", result, host->identity);
   bool ok = false;
   if (result == VK_SUCCESS) {
      PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
      PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
      uint32_t count = 0;
      result = enumerate(instance, &count, NULL);
      fprintf(stderr, "BC250 hosted enumerate result=%d devices=%u\n", result, count);
      ok = result == VK_SUCCESS && count == 1;
      if (ok) {
         VkPhysicalDevice physical;
         count=1;
         result=enumerate(instance,&count,&physical);
         ok=result==VK_SUCCESS && bc250_hosted_content(gipa,instance,physical);
      }
      destroy(instance, NULL);
   }
   util_dl_close(lib);
   return ok;
}

struct pipe_screen *
d3d10_create_hosted_screen(struct bc250_host *host)
{
   const char *luid=os_get_option("BC250_D3D_ZINK_LUID");
   if (!luid) return NULL;
   host->adapter_luid=strtoull(luid,NULL,16);
   const char *path=os_get_option("BC250_HOSTED_ICD");
   if (!host->adapter_luid || !path || !*path) return NULL;
   return zink_win32_create_hosted_screen(host->adapter_luid,host);
}
