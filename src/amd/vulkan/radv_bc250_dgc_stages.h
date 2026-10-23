/* SPDX-License-Identifier: MIT
 *
 * BD-121. What a BC-250 reports in
 * VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT::supportedIndirectCommandsShaderStages, and the
 * rule a D3D12 runtime on top of this driver applies to that number.
 *
 * The measured defect (DEFECTS.md BD-121, vi-r1 lab round 2026-10-11, mesh ICD 5A14D09C, arm B with
 * RADV_BC250_HYBRID_TASK=0): the mesh ICD exposes meshShader, its DGC stage mask held MESH but not
 * TASK, and vkd3d-proton (libs/vkd3d/device.c:2674-2688 @ 4e9a98e9) then wrote "Not all relevant
 * pipeline stages are supported by EXT_dgc. Skipping." and cleared deviceGeneratedCommands for the
 * WHOLE device. Every D3D12 command signature that needs the extension was refused afterwards,
 * including ones with no mesh in them at all: test_view_instancing_indirect_state failed to create a
 * CONSTANT+DRAW signature while the control ICD F43E4392, which does not expose meshShader, passed the
 * same group. The consumer's rule is all-or-nothing:
 *
 *     VkShaderStageFlags required_stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
 *     if (physical_device_info->mesh_shader_features.meshShader)
 *         required_stages |= VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT;
 *     if ((supported_stages & required_stages) != required_stages) ... skip
 *
 * so a mesh-capable device must report both mesh stages or lose device-generated commands entirely.
 *
 * What the mask means, and why reporting TASK is a true statement here. The mask is a filter on what a
 * client's layouts and executes may name, not a claim that a stage exists:
 * VUID-VkIndirectCommandsLayoutCreateInfoEXT-shaderStages-11091 requires
 * VkIndirectCommandsLayoutCreateInfoEXT::shaderStages to contain only stages from this mask, and
 * VUID-vkCmdExecuteGeneratedCommandsEXT-supportedIndirectCommandsShaderStages-11060/11061 require the
 * same of the bound stages (Vulkan 1.4.364, chapters/limits.adoc "the stages which can be used to
 * generate indirect commands"). Whether a task shader can exist at all is
 * VkPhysicalDeviceMeshShaderFeaturesEXT::taskShader, a separate feature. So:
 *
 *   - The TASK bit is needed even for a mesh draw with no amplification stage, because a D3D12
 *     runtime builds every DISPATCH_MESH command signature's layout with
 *     shaderStages = MESH | TASK | FRAGMENT (vkd3d-proton libs/vkd3d/command.c:27727-27731), so VU
 *     11091 would reject the layout without it.
 *   - While taskShader is false, no pipeline and no shader object can carry a task stage, so no layout
 *     and no execute can put a task stage behind the permission. The claim cannot be falsified.
 *   - While taskShader is true (the hybrid-task route), the BC-250 DGC route carries the task producer
 *     itself: bc250_dgc_shape() in radv_bc250_dgc.c sets s->task from the pipeline's task pipeline and
 *     sizes the capture for it. So a task stage behind the permission has a producer behind it, which
 *     is what this mask claims and all it claims.
 *
 * What this mask does NOT claim, so that the record is exact. The route's accepted STATE is narrower
 * than the direct DispatchMesh path's: bc250_dgc_state_valid() in radv_bc250_dgc.c refuses a mesh DGC
 * execute under a non-zero view mask, streamout, any VRS attachment or non-1x1 fragment size, active
 * pipeline-statistics or transform-feedback queries, a secondary or simultaneous-use command buffer, a
 * bound descriptor heap or dynamic offset, and with SQTT, utrace, the BC-250 timer, chaining or
 * split-batch prepare on - and the refusal is a hard vk_command_buffer_set_error(RADV_BC250_CMD_REFUSED)
 * at vkEndCommandBuffer, not a fallback to the generic path. That restriction set belongs to the BD-119
 * mesh DGC route, not to this mask. BD-121 makes it newly reachable in the hybrid-task-off arm, where a
 * Vulkan client can now create a mesh DGC layout that the extension used to deny it device-wide; a
 * D3D12 client cannot reach it there at all, because MeshShaderTier is 0 without taskShader.
 *
 * Both functions below are pure functions of their arguments and this header pulls in nothing from
 * Mesa, so the host gate scratch\bd121\gates\dgc-stages-test.c runs the shipped code instead of a
 * transcription of it.
 */
#ifndef RADV_BC250_DGC_STAGES_H
#define RADV_BC250_DGC_STAGES_H

#include <stdbool.h>
#include <stdio.h>

#include "vulkan/vulkan_core.h"

/* The two stages a BC-250 mesh DGC layout may name. They are reported together or not at all: a mesh
 * draw token needs MESH (VUID-VkIndirectCommandsLayoutCreateInfoEXT-pTokens-11106) and a D3D12
 * consumer's mesh layout also names TASK, so one bit without the other is of no use to anyone. */
#define RADV_BC250_DGC_MESH_STAGES (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT)

/* The task-and-mesh part of supportedIndirectCommandsShaderStages.
 *
 * native_mesh   this is a BC-250 whose native mesh route is on (radv_physical_device::bc250_native_mesh)
 * expose_dgc    radv_bc250_dgc can execute a mesh draw token on it (::bc250_expose_dgc, which is off
 *               with RADV_BC250_EXPOSE_DGC=0 and with the experimental native-task compile)
 * upstream      what RADV computes for any device: MESH when radv_mesh_enabled(), TASK when
 *               radv_taskmesh_enabled()
 *
 * Any device that is not on the BC-250 mesh route keeps the upstream value untouched: this rule is
 * about one board's emulated mesh path and may not reach a part with hardware task rings.
 *
 * On the BC-250 mesh route the value does not depend on bc250_hybrid_task. Before BD-121 the TASK bit
 * was added only with hybrid task on, which is why the lab's arm B (hybrid task off, mesh exposed) lost
 * device-generated commands. Whether an application task shader may be compiled is taskShader's
 * business, not this mask's. */
static inline VkShaderStageFlags
radv_bc250_dgc_taskmesh_stages(bool native_mesh, bool expose_dgc, VkShaderStageFlags upstream)
{
   if (!native_mesh)
      return upstream;
   return expose_dgc ? RADV_BC250_DGC_MESH_STAGES : 0;
}

/* The rule a D3D12 runtime applies to the mask before it enables VK_EXT_device_generated_commands for
 * the device, quoted from vkd3d-proton libs/vkd3d/device.c:2674-2688 @ 4e9a98e9. It is here so that the
 * driver's own self-check line can state the verdict the consumer will reach, and so that a host gate
 * can assert the ICD's numbers against the consumer's rule rather than against a restatement of the
 * fix. A false verdict means no D3D12 ExecuteIndirect at all on this device, mesh or not. */
static inline bool
radv_bc250_dgc_consumer_keeps_dgc(VkShaderStageFlags supported, bool mesh_shader_feature)
{
   VkShaderStageFlags required = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

   if (mesh_shader_feature)
      required |= RADV_BC250_DGC_MESH_STAGES;

   return (supported & required) == required;
}

/* The stage mask written out in names, for the device-creation self-check line. It lives here, next to
 * the rule, so the host gate asserts the exact text a lab log will carry. A bit this table does not
 * know is printed as a hexadecimal remainder rather than dropped: a stage added to Vulkan later must
 * not be able to hide inside the line. An empty mask reads "none", never as an empty field. */
static inline void
radv_bc250_dgc_stage_names(VkShaderStageFlags stages, char *out, size_t size)
{
   static const struct {
      VkShaderStageFlags bit;
      const char *name;
   } table[] = {
      {VK_SHADER_STAGE_VERTEX_BIT, "VERTEX"},
      {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "TESS_CTRL"},
      {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "TESS_EVAL"},
      {VK_SHADER_STAGE_GEOMETRY_BIT, "GEOMETRY"},
      {VK_SHADER_STAGE_FRAGMENT_BIT, "FRAGMENT"},
      {VK_SHADER_STAGE_COMPUTE_BIT, "COMPUTE"},
      {VK_SHADER_STAGE_TASK_BIT_EXT, "TASK"},
      {VK_SHADER_STAGE_MESH_BIT_EXT, "MESH"},
      {VK_SHADER_STAGE_RAYGEN_BIT_KHR, "RAYGEN"},
      {VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "ANY_HIT"},
      {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "CLOSEST_HIT"},
      {VK_SHADER_STAGE_MISS_BIT_KHR, "MISS"},
      {VK_SHADER_STAGE_INTERSECTION_BIT_KHR, "INTERSECTION"},
      {VK_SHADER_STAGE_CALLABLE_BIT_KHR, "CALLABLE"},
   };
   size_t used = 0;
   VkShaderStageFlags left = stages;

   if (!size)
      return;
   out[0] = '\0';
   for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
      if (!(stages & table[i].bit))
         continue;
      left &= ~table[i].bit;
      const int wrote = snprintf(out + used, size - used, "%s%s", used ? "|" : "", table[i].name);
      if (wrote < 0 || (size_t)wrote >= size - used)
         return;
      used += (size_t)wrote;
   }
   if (left)
      snprintf(out + used, size - used, "%s0x%08x", used ? "|" : "", left);
   else if (!used)
      snprintf(out, size, "none");
}

#endif /* RADV_BC250_DGC_STAGES_H */
