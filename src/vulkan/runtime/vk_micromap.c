/* SPDX-License-Identifier: MIT
 * VK_EXT_opacity_micromap on top of VK_KHR_opacity_micromap.
 *
 * A VkMicromapEXT is a KHR micromap: an acceleration structure of type
 * VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR. The commands below
 * translate the EXT micromap commands to the device's VK_KHR_acceleration_structure
 * commands. The micromap data, the special indices and the pipeline, instance,
 * build and ray flags have the same values in both extensions. Host commands are
 * not supported (micromapHostCommands is VK_FALSE), as for acceleration
 * structures.
 */

#include "vk_acceleration_structure.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_log.h"

#include "util/macros.h"
#include "util/stack_array.h"

/* The EXT and KHR usage structures are passed through unchanged. */
static_assert(sizeof(VkMicromapUsageEXT) == sizeof(VkMicromapUsageKHR), "");
static_assert(offsetof(VkMicromapUsageEXT, count) == offsetof(VkMicromapUsageKHR, count), "");
static_assert(offsetof(VkMicromapUsageEXT, subdivisionLevel) == offsetof(VkMicromapUsageKHR, subdivisionLevel),
              "");
static_assert(offsetof(VkMicromapUsageEXT, format) == offsetof(VkMicromapUsageKHR, format), "");

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateMicromapEXT(VkDevice _device, const VkMicromapCreateInfoEXT *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator, VkMicromapEXT *pMicromap)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   assert(pCreateInfo->type == VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT);

   struct vk_micromap *micromap =
      vk_object_zalloc(device, pAllocator, sizeof(struct vk_micromap), VK_OBJECT_TYPE_MICROMAP_EXT);
   if (!micromap)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* micromapCaptureReplay is not supported: createFlags and deviceAddress are 0. */
   const VkAccelerationStructureCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .buffer = pCreateInfo->buffer,
      .offset = pCreateInfo->offset,
      .size = pCreateInfo->size,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR,
   };
   VkResult result =
      device->dispatch_table.CreateAccelerationStructureKHR(_device, &info, pAllocator, &micromap->accel_struct);
   if (result != VK_SUCCESS) {
      vk_object_free(device, pAllocator, micromap);
      return result;
   }

   *pMicromap = vk_micromap_to_handle(micromap);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_DestroyMicromapEXT(VkDevice _device, VkMicromapEXT _micromap, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_micromap, micromap, _micromap);

   if (!micromap)
      return;

   device->dispatch_table.DestroyAccelerationStructureKHR(_device, micromap->accel_struct, pAllocator);
   vk_object_free(device, pAllocator, micromap);
}

static VkAccelerationStructureKHR
vk_micromap_accel_struct(VkMicromapEXT _micromap)
{
   VK_FROM_HANDLE(vk_micromap, micromap, _micromap);
   return micromap ? micromap->accel_struct : VK_NULL_HANDLE;
}

static VkBuildAccelerationStructureFlagsKHR
vk_micromap_build_flags(VkBuildMicromapFlagsEXT flags)
{
   VkBuildAccelerationStructureFlagsKHR result = 0;
   if (flags & VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT)
      result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
   if (flags & VK_BUILD_MICROMAP_PREFER_FAST_BUILD_BIT_EXT)
      result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
   if (flags & VK_BUILD_MICROMAP_ALLOW_COMPACTION_BIT_EXT)
      result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
   return result;
}

static VkCopyAccelerationStructureModeKHR
vk_micromap_copy_mode(VkCopyMicromapModeEXT mode)
{
   switch (mode) {
   case VK_COPY_MICROMAP_MODE_CLONE_EXT:
      return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
   case VK_COPY_MICROMAP_MODE_SERIALIZE_EXT:
      return VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
   case VK_COPY_MICROMAP_MODE_DESERIALIZE_EXT:
      return VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
   case VK_COPY_MICROMAP_MODE_COMPACT_EXT:
      return VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
   default:
      UNREACHABLE("Invalid micromap copy mode");
   }
}

/* The KHR build input of one EXT micromap build. info points at data and geometry, so the structure must stay
 * where it is while info is in use.
 */
struct vk_micromap_build {
   VkAccelerationStructureGeometryMicromapDataKHR data;
   VkAccelerationStructureGeometryKHR geometry;
   VkAccelerationStructureBuildGeometryInfoKHR info;
};

/* dst is the KHR micromap of dstMicromap. A size query ignores dstMicromap and gives VK_NULL_HANDLE. */
static void
vk_micromap_build_init(struct vk_micromap_build *build, const VkMicromapBuildInfoEXT *info,
                       VkAccelerationStructureKHR dst)
{
   assert(info->type == VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT);
   assert(info->mode == VK_BUILD_MICROMAP_MODE_BUILD_EXT);

   build->data = (VkAccelerationStructureGeometryMicromapDataKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MICROMAP_DATA_KHR,
      .usageCountsCount = info->usageCountsCount,
      .pUsageCounts = (const VkMicromapUsageKHR *)info->pUsageCounts,
      .ppUsageCounts = (const VkMicromapUsageKHR *const *)info->ppUsageCounts,
      .data = info->data.deviceAddress,
      .triangleArray = info->triangleArray.deviceAddress,
      .triangleArrayStride = info->triangleArrayStride,
   };
   build->geometry = (VkAccelerationStructureGeometryKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .pNext = &build->data,
      .geometryType = VK_GEOMETRY_TYPE_MICROMAP_KHR,
   };
   build->info = (VkAccelerationStructureBuildGeometryInfoKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR,
      .flags = vk_micromap_build_flags(info->flags),
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .dstAccelerationStructure = dst,
      .geometryCount = 1,
      .pGeometries = &build->geometry,
      .scratchData.deviceAddress = info->scratchData.deviceAddress,
   };
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetMicromapBuildSizesEXT(VkDevice _device, VkAccelerationStructureBuildTypeKHR buildType,
                                   const VkMicromapBuildInfoEXT *pBuildInfo, VkMicromapBuildSizesInfoEXT *pSizeInfo)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   struct vk_micromap_build build;
   vk_micromap_build_init(&build, pBuildInfo, VK_NULL_HANDLE);

   /* A micromap geometry has no primitive count. */
   const uint32_t max_primitive_count = 0;
   VkAccelerationStructureBuildSizesInfoKHR sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
   };
   device->dispatch_table.GetAccelerationStructureBuildSizesKHR(_device, buildType, &build.info,
                                                                &max_primitive_count, &sizes);

   pSizeInfo->micromapSize = sizes.accelerationStructureSize;
   pSizeInfo->buildScratchSize = sizes.buildScratchSize;
   /* Traversal reads the micromap: an acceleration structure keeps a reference to it, not a copy. */
   pSizeInfo->discardable = VK_FALSE;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdBuildMicromapsEXT(VkCommandBuffer commandBuffer, uint32_t infoCount, const VkMicromapBuildInfoEXT *pInfos)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   struct vk_device *device = cmd_buffer->base.device;

   if (infoCount == 0)
      return;

   STACK_ARRAY(struct vk_micromap_build, builds, infoCount);
   STACK_ARRAY(VkAccelerationStructureBuildGeometryInfoKHR, infos, infoCount);
   /* A micromap build has no build range infos. */
   STACK_ARRAY(const VkAccelerationStructureBuildRangeInfoKHR *, ranges, infoCount);
   if (!builds || !infos || !ranges) {
      vk_command_buffer_set_error(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }

   for (uint32_t i = 0; i < infoCount; i++) {
      vk_micromap_build_init(&builds[i], &pInfos[i], vk_micromap_accel_struct(pInfos[i].dstMicromap));
      infos[i] = builds[i].info;
      ranges[i] = NULL;
   }

   device->dispatch_table.CmdBuildAccelerationStructuresKHR(commandBuffer, infoCount, infos, ranges);

out:
   STACK_ARRAY_FINISH(ranges);
   STACK_ARRAY_FINISH(infos);
   STACK_ARRAY_FINISH(builds);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdCopyMicromapEXT(VkCommandBuffer commandBuffer, const VkCopyMicromapInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);

   const VkCopyAccelerationStructureInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
      .src = vk_micromap_accel_struct(pInfo->src),
      .dst = vk_micromap_accel_struct(pInfo->dst),
      .mode = vk_micromap_copy_mode(pInfo->mode),
   };
   cmd_buffer->base.device->dispatch_table.CmdCopyAccelerationStructureKHR(commandBuffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdCopyMicromapToMemoryEXT(VkCommandBuffer commandBuffer, const VkCopyMicromapToMemoryInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);

   const VkCopyAccelerationStructureToMemoryInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,
      .src = vk_micromap_accel_struct(pInfo->src),
      .dst = pInfo->dst,
      .mode = vk_micromap_copy_mode(pInfo->mode),
   };
   cmd_buffer->base.device->dispatch_table.CmdCopyAccelerationStructureToMemoryKHR(commandBuffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdCopyMemoryToMicromapEXT(VkCommandBuffer commandBuffer, const VkCopyMemoryToMicromapInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);

   const VkCopyMemoryToAccelerationStructureInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,
      .src = pInfo->src,
      .dst = vk_micromap_accel_struct(pInfo->dst),
      .mode = vk_micromap_copy_mode(pInfo->mode),
   };
   cmd_buffer->base.device->dispatch_table.CmdCopyMemoryToAccelerationStructureKHR(commandBuffer, &info);
}

/* queryType is VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT or VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT. The
 * driver's CmdWriteAccelerationStructuresPropertiesKHR and query pools accept both.
 */
VKAPI_ATTR void VKAPI_CALL
vk_common_CmdWriteMicromapsPropertiesEXT(VkCommandBuffer commandBuffer, uint32_t micromapCount,
                                         const VkMicromapEXT *pMicromaps, VkQueryType queryType,
                                         VkQueryPool queryPool, uint32_t firstQuery)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);

   if (micromapCount == 0)
      return;

   STACK_ARRAY(VkAccelerationStructureKHR, accel_structs, micromapCount);
   if (!accel_structs) {
      vk_command_buffer_set_error(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   for (uint32_t i = 0; i < micromapCount; i++)
      accel_structs[i] = vk_micromap_accel_struct(pMicromaps[i]);

   cmd_buffer->base.device->dispatch_table.CmdWriteAccelerationStructuresPropertiesKHR(
      commandBuffer, micromapCount, accel_structs, queryType, queryPool, firstQuery);

   STACK_ARRAY_FINISH(accel_structs);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetDeviceMicromapCompatibilityEXT(VkDevice _device, const VkMicromapVersionInfoEXT *pVersionInfo,
                                            VkAccelerationStructureCompatibilityKHR *pCompatibility)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   /* A serialized micromap starts with the same version data as a serialized acceleration structure. */
   const VkAccelerationStructureVersionInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR,
      .pVersionData = pVersionInfo->pVersionData,
   };
   device->dispatch_table.GetDeviceAccelerationStructureCompatibilityKHR(_device, &info, pCompatibility);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_BuildMicromapsEXT(VkDevice _device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
                            const VkMicromapBuildInfoEXT *pInfos)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CopyMicromapEXT(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                          const VkCopyMicromapInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CopyMicromapToMemoryEXT(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  const VkCopyMicromapToMemoryInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CopyMemoryToMicromapEXT(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  const VkCopyMemoryToMicromapInfoEXT *pInfo)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_WriteMicromapsPropertiesEXT(VkDevice _device, uint32_t micromapCount, const VkMicromapEXT *pMicromaps,
                                      VkQueryType queryType, size_t dataSize, void *pData, size_t stride)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}
