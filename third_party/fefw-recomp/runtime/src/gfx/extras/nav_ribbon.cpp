// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/nav_ribbon.h"

#include <algorithm>
#include <cstring>

#include "nxrt/log.h"
#include "vk_gauges.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "navrib";

#include "extras/nav_ribbon_shaders.inc"

struct RibbonPush {
    float vp[16];
    float color[4];
    float anim[4];
    float depth_info[4];
};
static_assert(sizeof(RibbonPush) == 112);

VkShaderModule make_module(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_gauge_shader_modules().add(1);
    return m;
}

}  // namespace

bool NavRibbonPass::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed", what);
        shutdown();
        return false;
    };

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d, &sci, nullptr, &sampler_) != VK_SUCCESS) return fail("sampler");
    vk_gauge_samplers().add(1);

    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) return fail("set layout");
    vk_gauge_descriptor_set_layouts().add(1);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RibbonPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("pipeline layout");
    vk_gauge_pipeline_layouts().add(1);

    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetRing};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSetRing;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return fail("descriptor pool");
    vk_gauge_descriptor_pools().add(1);

    VkDescriptorSetLayout ls[kSetRing];
    for (auto& x : ls) x = set_layout_;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSetRing;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &ai, sets_) != VK_SUCCESS) return fail("descriptor sets");
    vk_gauge_descriptor_sets().add(kSetRing);

    vs_ = make_module(d, kNavRibbonVertSpv, sizeof kNavRibbonVertSpv);
    fs_ = make_module(d, kNavRibbonFragSpv, sizeof kNavRibbonFragSpv);
    if (!vs_ || !fs_) return fail("shader modules");

    // Per-slot upload vertex buffers (zero GPU stalls across 3 frames in flight).
    const VkDeviceSize vb_bytes = static_cast<VkDeviceSize>(kMaxVerts) * sizeof(gfx::Ribbon3DVertex);
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        if (dev.create_buffer(vb_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gfx::MemoryUse::Upload, &vbs_[i], "nav_ribbon_vb") != VK_SUCCESS) {
            return fail("vertex buffer");
        }
    }

    // 1x1 dummy texture so descriptor binding 0 is always valid even when depth is absent.
    gfx::ImageDesc id;
    id.format = VK_FORMAT_R8G8B8A8_UNORM;
    id.width = 1;
    id.height = 1;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.name = "nav_ribbon_dummy_depth";
    if (dev.create_image(id, &dummy_depth_) != VK_SUCCESS) return fail("dummy depth");
    dev.submit_once([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dummy_depth_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);

        VkClearColorValue cv = {{1.0f, 1.0f, 1.0f, 1.0f}};
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dummy_depth_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);

        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cb, &di);
    });

    return true;
}

void NavRibbonPass::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    for (auto& [f, p] : pipes_) {
        if (p) { vkDestroyPipeline(d, p, nullptr); vk_gauge_pipelines().sub(1); }
    }
    pipes_.clear();
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        if (vbs_[i]) dev_->destroy_buffer(vbs_[i]);
        vbs_[i] = gfx::Buffer{};
    }
    if (dummy_depth_) dev_->destroy_image(dummy_depth_);
    dummy_depth_ = gfx::Image{};
    if (layout_) { vkDestroyPipelineLayout(d, layout_, nullptr); vk_gauge_pipeline_layouts().sub(1); }
    if (pool_) { vkDestroyDescriptorPool(d, pool_, nullptr); vk_gauge_descriptor_pools().sub(1); }
    if (sets_[0]) vk_gauge_descriptor_sets().sub(kSetRing);
    if (set_layout_) { vkDestroyDescriptorSetLayout(d, set_layout_, nullptr); vk_gauge_descriptor_set_layouts().sub(1); }
    if (vs_) { vkDestroyShaderModule(d, vs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (fs_) { vkDestroyShaderModule(d, fs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (sampler_) { vkDestroySampler(d, sampler_, nullptr); vk_gauge_samplers().sub(1); }
    sampler_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    std::memset(sets_, 0, sizeof sets_);
    vs_ = fs_ = VK_NULL_HANDLE;
    dev_ = nullptr;
}

VkPipeline NavRibbonPass::pipeline_for(VkFormat fmt) {
    auto it = pipes_.find(fmt);
    if (it != pipes_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbind = {};
    vbind.binding = 0;
    vbind.stride = sizeof(gfx::Ribbon3DVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[3] = {};
    vattr[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gfx::Ribbon3DVertex, x)};
    vattr[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(gfx::Ribbon3DVertex, u)};
    vattr[2] = {2, 0, VK_FORMAT_R32_SFLOAT,       offsetof(gfx::Ribbon3DVertex, fade)};

    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo bl = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &ba;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &fmt;

    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &pri;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &bl;
    pci.pDynamicState = &dyn;
    pci.layout = layout_;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_->device(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
        NXLOG_E(kTag, "pipeline creation failed for format %d", static_cast<int>(fmt));
        return VK_NULL_HANDLE;
    }
    vk_gauge_pipelines().add(1);
    pipes_[fmt] = pipe;
    return pipe;
}

void NavRibbonPass::record(VkCommandBuffer cb, VkFormat color_format, VkExtent2D extent, uint32_t slot,
                           const gfx::Ribbon3DVertex* verts, uint32_t vert_count,
                           const gfx::Ribbon3DParams& params, const NavRibbonDepth& depth) {
    if (!dev_ || !verts || vert_count < 4 || extent.width == 0 || extent.height == 0) return;
    VkPipeline pipe = pipeline_for(color_format);
    if (!pipe) return;

    const uint32_t count = std::min(vert_count, kMaxVerts);
    gfx::Buffer& vb = vbs_[slot % kMaxSlots];
    if (!vb.mapped) return;
    std::memcpy(vb.mapped, verts, static_cast<size_t>(count) * sizeof(gfx::Ribbon3DVertex));
    dev_->flush_buffer(vb);

    const bool has_depth = (depth.view != VK_NULL_HANDLE && depth.layout != VK_IMAGE_LAYOUT_UNDEFINED);
    VkDescriptorSet ds = sets_[set_cursor_];
    set_cursor_ = (set_cursor_ + 1) % kSetRing;

    VkDescriptorImageInfo di = {};
    di.sampler = sampler_;
    di.imageView = has_depth ? depth.view : dummy_depth_.view;
    di.imageLayout = has_depth ? depth.layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);

    RibbonPush pc = {};
    std::memcpy(pc.vp, params.view_proj, sizeof pc.vp);
    std::memcpy(pc.color, params.color, sizeof pc.color);
    pc.anim[0] = params.time_sec;
    pc.anim[1] = params.reveal_dist;
    pc.anim[2] = params.occluded_alpha;
    pc.anim[3] = has_depth ? 1.0f : 0.0f;
    pc.depth_info[0] = 1.0f / static_cast<float>(extent.width);
    pc.depth_info[1] = 1.0f / static_cast<float>(extent.height);
    pc.depth_info[2] = depth.flip_y ? 1.0f : 0.0f;
    pc.depth_info[3] = static_cast<float>(depth.depth_mode);

    VkViewport vp = {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, extent};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &ds, 0, nullptr);
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb.buffer, &off);
    vkCmdDraw(cb, count, 1, 0, 0);
}

}  // namespace nxrt::extras
