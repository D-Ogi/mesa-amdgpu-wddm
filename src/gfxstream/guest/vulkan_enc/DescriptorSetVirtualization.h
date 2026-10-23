/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <unordered_set>
#include <variant>
#include <vector>

namespace gfxstream {
namespace vk {

enum DescriptorWriteType {
    Empty = 0,
    ImageInfo = 1,
    BufferInfo = 2,
    BufferView = 3,
    AccelerationStructure = 4,
};

struct DescriptorBindingElementWrite {
    DescriptorWriteType type = DescriptorWriteType::Empty;
    uint32_t dstArrayElement = 0;  // Only used for accelerationStructure.

    union {
        VkDescriptorImageInfo imageInfo;
        VkDescriptorBufferInfo bufferInfo;
        VkBufferView bufferView;
        VkWriteDescriptorSetAccelerationStructureKHR accelerationStructure;
    };

    std::weak_ptr<bool> alive;
    std::weak_ptr<bool> samplerAlive;
};

struct DescriptorInlineUniformBlockWrite {
    bool pendingWrite = false;
    std::vector<uint8_t> bytes;
    VkWriteDescriptorSetInlineUniformBlock writeInfo;
};

struct DescriptorBindingWrite {
    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    std::variant<
        // Unused / unconfigured binding.
        std::monostate,
        // The writes of non-inline uniform descriptors.
        std::vector<DescriptorBindingElementWrite>,
        // The writes of inline uniform descriptors.
        DescriptorInlineUniformBlockWrite
    > elements;
};

// All of the writes for a given descriptor.
struct DescriptorWriteTable {
    std::vector<DescriptorBindingWrite> bindings;
};

struct DescriptorWriteArrayRange {
    uint32_t begin;
    uint32_t count;
};

using DescriptorWriteDstArrayRangeTable = std::vector<std::vector<DescriptorWriteArrayRange>>;

struct ReifiedDescriptorSet {
    VkDescriptorPool pool;
    VkDescriptorSetLayout setLayout;
    uint64_t poolId;
    bool allocationPending;

    // Indexed first by binding number
    DescriptorWriteTable allWrites;

    // Indexed first by binding number
    DescriptorWriteDstArrayRangeTable pendingWriteArrayRanges;

    // Indexed by binding number
    std::vector<bool> bindingIsImmutableSampler;

    // Copied from the descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct DescriptorPoolAllocationInfo {
    VkDevice device;
    VkDescriptorPoolCreateFlags createFlags;

    // TODO: This should be in a single fancy data structure of some kind.
    std::vector<uint64_t> freePoolIds;
    std::unordered_set<uint32_t> allocedPoolIds;
    std::unordered_set<VkDescriptorSet> allocedSets;
    uint32_t maxSets;
    uint32_t usedSets;

    // Fine-grained tracking of descriptor counts in individual pools
    struct DescriptorCountInfo {
        VkDescriptorType type;
        uint32_t descriptorCount;
        uint32_t used;
    };
    std::vector<DescriptorCountInfo> descriptorCountInfo;
};

struct DescriptorSetLayoutInfo {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    uint32_t refcount;
};

void clearReifiedDescriptorSet(ReifiedDescriptorSet* set);

void initDescriptorWriteTable(const std::vector<VkDescriptorSetLayoutBinding>& layoutBindings,
                              DescriptorWriteTable& table);

bool isDescriptorTypeImageInfo(VkDescriptorType descType);
bool isDescriptorTypeBufferInfo(VkDescriptorType descType);
bool isDescriptorTypeBufferView(VkDescriptorType descType);
bool isDescriptorTypeInlineUniformBlock(VkDescriptorType descType);
bool isDescriptorTypeAccelerationStructure(VkDescriptorType descType);

void doEmulatedDescriptorWrite(const VkWriteDescriptorSet* write, ReifiedDescriptorSet* toWrite);
void doEmulatedDescriptorCopy(const VkCopyDescriptorSet* copy, const ReifiedDescriptorSet* src,
                              ReifiedDescriptorSet* dst);

void doEmulatedDescriptorImageInfoWriteFromTemplate(VkDescriptorType descType, uint32_t binding,
                                                    uint32_t dstArrayElement, uint32_t count,
                                                    const VkDescriptorImageInfo* imageInfos,
                                                    ReifiedDescriptorSet* set);

void doEmulatedDescriptorBufferInfoWriteFromTemplate(VkDescriptorType descType, uint32_t binding,
                                                     uint32_t dstArrayElement, uint32_t count,
                                                     const VkDescriptorBufferInfo* bufferInfos,
                                                     ReifiedDescriptorSet* set);

void doEmulatedDescriptorBufferViewWriteFromTemplate(VkDescriptorType descType, uint32_t binding,
                                                     uint32_t dstArrayElement, uint32_t count,
                                                     const VkBufferView* bufferViews,
                                                     ReifiedDescriptorSet* set);

void doEmulatedDescriptorInlineUniformBlockFromTemplate(VkDescriptorType descType, uint32_t binding,
                                                        uint32_t dstArrayElement, uint32_t count,
                                                        const void* pData,
                                                        ReifiedDescriptorSet* set);

void applyDescriptorSetAllocation(VkDescriptorPool pool, VkDescriptorSetLayout setLayout);
void fillDescriptorSetInfoForPool(VkDescriptorPool pool, VkDescriptorSetLayout setLayout,
                                  VkDescriptorSet set);
VkResult validateAndApplyVirtualDescriptorSetAllocation(
    const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pSets);

// Returns false if set wasn't found in its pool.
bool removeDescriptorSetFromPool(VkDescriptorSet set, bool usePoolIds);

std::vector<VkDescriptorSet> clearDescriptorPool(VkDescriptorPool pool, bool usePoolIds);

}  // namespace vk
}  // namespace gfxstream
