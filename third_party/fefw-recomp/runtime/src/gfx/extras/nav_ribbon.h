// SPDX-License-Identifier: GPL-3.0-or-later
// Native Vulkan 3D translucent ground navigation ribbon pass (samples game G-buffer depth for soft occlusion).
#pragma once

#include <cstdint>
#include <unordered_map>

#include "nxrt/gfx.h"
#include "nxrt/overlay.h"

namespace nxrt::extras {

struct NavRibbonDepth {
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool flip_y = false;
    int depth_mode = 0;  // 0=fwd [0,1], 1=rev [0,1], 2=fwd [-1,1], 3=rev [-1,1]
};

class NavRibbonPass {
public:
    bool init(gfx::Device& dev);
    void shutdown();

    // Must be called inside an active dynamic rendering pass (vkCmdBeginRendering) targeting `color_format`.
    void record(VkCommandBuffer cb, VkFormat color_format, VkExtent2D extent, uint32_t slot,
                const gfx::Ribbon3DVertex* verts, uint32_t vert_count,
                const gfx::Ribbon3DParams& params, const NavRibbonDepth& depth);

private:
    static constexpr uint32_t kMaxSlots = 3;
    static constexpr uint32_t kMaxVerts = 4096;
    static constexpr uint32_t kSetRing = 16;

    VkPipeline pipeline_for(VkFormat fmt);

    gfx::Device* dev_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSetRing] = {};
    uint32_t set_cursor_ = 0;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    std::unordered_map<VkFormat, VkPipeline> pipes_;
    gfx::Buffer vbs_[kMaxSlots] = {};
    gfx::Image dummy_depth_;
};

}  // namespace nxrt::extras
