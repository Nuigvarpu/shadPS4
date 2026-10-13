// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

namespace {
/// Push constants and push descriptors identical to the ones already pushed with the same
/// pipeline layout in the current command buffer are skipped.
struct PushedState {
    VkCommandBuffer cmdbuf{};
    VkPipelineLayout layout{};
    u64 epoch{};
    bool has_constants{};
    Shader::PushData constants{};
    bool has_descriptors{};
    u64 descriptor_hash{};
};

thread_local std::array<PushedState, 2> pushed_state{};

u64 HashWrites(const std::vector<vk::WriteDescriptorSet>& writes) {
    u64 hash = writes.size();
    for (const auto& write : writes) {
        const std::array<u32, 4> header{write.dstBinding, write.dstArrayElement,
                                        write.descriptorCount,
                                        static_cast<u32>(write.descriptorType)};
        hash = XXH3_64bits_withSeed(header.data(), sizeof(header), hash);
        if (write.pBufferInfo) {
            hash = XXH3_64bits_withSeed(
                write.pBufferInfo, sizeof(vk::DescriptorBufferInfo) * write.descriptorCount, hash);
        }
        if (write.pImageInfo) {
            hash = XXH3_64bits_withSeed(
                write.pImageInfo, sizeof(vk::DescriptorImageInfo) * write.descriptorCount, hash);
        }
        if (write.pTexelBufferView) {
            hash = XXH3_64bits_withSeed(write.pTexelBufferView,
                                        sizeof(vk::BufferView) * write.descriptorCount, hash);
        }
    }
    return hash;
}

} // Anonymous namespace

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    // Identical pushes are skipped while nothing else touched the bind point: the same command
    // buffer, no foreign pipeline bound since (they bump the epoch) and the same layout.
    auto& pushed = pushed_state[IsCompute() ? 1 : 0];
    const VkCommandBuffer raw_cmdbuf = static_cast<VkCommandBuffer>(cmdbuf);
    const VkPipelineLayout raw_layout = static_cast<VkPipelineLayout>(*pipeline_layout);
    const u64 epoch = g_pipeline_bind_epoch.load(std::memory_order_relaxed);
    if (pushed.cmdbuf != raw_cmdbuf || pushed.epoch != epoch || pushed.layout != raw_layout) {
        pushed = PushedState{.cmdbuf = raw_cmdbuf, .layout = raw_layout, .epoch = epoch};
    }

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    if (!pushed.has_constants ||
        std::memcmp(&pushed.constants, &push_data, sizeof(push_data)) != 0) {
        cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);
        pushed.constants = push_data;
        pushed.has_constants = true;
    }

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        const u64 hash = HashWrites(set_writes);
        if (pushed.has_descriptors && pushed.descriptor_hash == hash) {
            return;
        }
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        pushed.has_descriptors = true;
        pushed.descriptor_hash = hash;
        return;
    }
    pushed.has_descriptors = false;

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
