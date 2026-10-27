/* Private, version-negotiated hosted dynamic view-instancing contract. */
#ifndef BC250_PGP_VIEW_CONTRACT_H
#define BC250_PGP_VIEW_CONTRACT_H
#define BC250_PGP_VIEW_QUERY_NAME "bc250GetDynamicViewInstancingContract"
#define BC250_PGP_VIEW_REVISION 1u
#define BC250_PGP_VIEW_DYNAMIC_SPEC_ID 0xfffffffeu
#define BC250_PGP_VIEW_DESCRIPTOR_SET 31u
#define BC250_PGP_VIEW_DESCRIPTOR_BINDING 0x42563235u
struct bc250_pgp_view_data {
    uint32_t view_id;
    uint32_t layer_offset;
    uint32_t viewport_offset;
    uint32_t layer_count;
    uint32_t viewport_count;
};
typedef void (VKAPI_PTR *PFN_bc250_cmd_set_pgp_view)(VkCommandBuffer command_buffer,
        const struct bc250_pgp_view_data *data);
typedef VkResult (VKAPI_PTR *PFN_bc250_pgp_view_query)(VkPhysicalDevice physical_device,
        uint32_t revision, uint32_t data_size, PFN_bc250_cmd_set_pgp_view *command);
#endif

VKAPI_ATTR void VKAPI_CALL radv_bc250_cmd_set_pgp_view(VkCommandBuffer command_buffer, const struct bc250_pgp_view_data *data);
