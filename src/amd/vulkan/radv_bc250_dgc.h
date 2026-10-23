/* SPDX-License-Identifier: MIT */
#ifndef RADV_BC250_DGC_H
#define RADV_BC250_DGC_H
#include "radv_dgc.h"
#include "radv_bc250_dgc_stages.h"

/* BD-121. The vk_indirect_command_layout::dgc_info bits a BC-250 mesh DGC layout may carry: a mesh
 * draw token (VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT and its COUNT and NV forms all set
 * MESA_VK_DGC_DRAW_MESH, src/vulkan/runtime/vk_device_generated_commands.c:139-145), push constants
 * and the sequence index. A mesh layout carrying anything else is refused at creation
 * (radv_CreateIndirectCommandsLayoutEXT). radv_dgc.c's refusal, bc250_dgc_shape() and the
 * device-creation self-check all read this one definition, so what the driver prints and what it
 * accepts cannot drift apart. */
#define RADV_BC250_DGC_LAYOUT_INFO                                                                                     \
   (BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH) | BITFIELD_BIT(MESA_VK_DGC_PC) | BITFIELD_BIT(MESA_VK_DGC_SI))

/* The device-creation self-check: one line naming the DGC stage mask this ICD reports, the token types
 * the mesh route accepts, why, and the verdict a D3D12 runtime reaches on that mask, so a lab log says
 * whether ExecuteIndirect will work before any test runs. Silent on a device that is not on the BC-250
 * native mesh route. */
void radv_bc250_dgc_report(const struct radv_device *device);

bool radv_bc250_dgc_before(struct radv_cmd_buffer *cmd);
bool radv_bc250_dgc_layout(const struct radv_device *device,
                           const struct radv_indirect_command_layout *layout);
struct nir_shader *radv_bc250_dgc_shader(struct radv_device *device,
                                        const struct radv_indirect_command_layout *layout);
void radv_bc250_dgc_requirements(struct radv_device *device,
                                 const VkGeneratedCommandsMemoryRequirementsInfoEXT *info,
                                 VkMemoryRequirements2 *requirements);
void radv_bc250_dgc_prepare(struct radv_cmd_buffer *cmd,
                            const VkGeneratedCommandsInfoEXT *info, struct radv_cmd_buffer *state);
void radv_bc250_dgc_execute(struct radv_cmd_buffer *cmd, VkBool32 preprocessed,
                            const VkGeneratedCommandsInfoEXT *info);
#endif
