// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/host_shaders/depth_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_resample_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include <cstdio>

namespace Vulkan {
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r,
                           VideoCore::TextureCache& t)
    : SceneTargets(i, s, r, [&t](VideoCore::ImageId id, u64 uid) {
        return t.TryGetImage(id, uid);
    }) {}
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r, Lookup get)
    : instance{i}, scheduler{s}, runtime{r}, lookup{std::move(get)} {
    runtime.scene_targets = this;
    CreateResampleResources();
}
SceneTargets::~SceneTargets() {
    scheduler.Finish();
    runtime.scene_targets = nullptr;
}
vk::FormatFeatureFlags SceneTargets::Features(vk::Format format) const {
    // Called for every attachment of every draw: the driver query was ~5% of the GPU thread.
    // Core formats index a flat table; extension formats (large enum values) use the map.
    const auto index = u32(format);
    if (index < format_table.size() && format_known[index]) return format_table[index];
    if (index >= format_table.size()) {
        if (const auto it = format_features.find(format); it != format_features.end()) {
            return it->second;
        }
    }
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (index < format_table.size()) {
        format_table[index] = features;
        format_known[index] = true;
    } else {
        format_features.emplace(format, features);
    }
    return features;
}
bool SceneTargets::Blittable(vk::Format format) const {
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
    return (features & required) == required;
}
bool SceneTargets::ShaderResampled(const VideoCore::Image& image) const {
    const auto format = image.backing->image.image_ci.format;
    if (!image.info.props.is_depth || Blittable(format)) return false;
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eDepthStencilAttachment;
    return (features & required) == required &&
           (!(image.aspect_mask & vk::ImageAspectFlagBits::eStencil) ||
            instance.IsShaderStencilExportSupported());
}
bool SceneTargets::Eligible(const VideoCore::Image& image) const {
    const auto& i = image.info;
    if (i.size.width != 1920 || i.size.height != 1080 || i.size.depth != 1 ||
        i.resources.levels != 1 || i.resources.layers != 1 || i.num_samples != 1 ||
        i.props.is_block || !image.backing || image.backing->num_samples != 1) {
        return false;
    }
    return Blittable(image.backing->image.image_ci.format) || ShaderResampled(image);
}
bool SceneTargets::SetSize(SceneResolution::Size next) {
    if (next == size) return false;
    ResolveAll();
    scheduler.Finish();
    entries.clear(); // no command buffer can still reference these images/views
    recent = {};
    size = next;
    std::printf("Scene resolution: raster %ux%u, post/UI 1920x1080 (live)\n",
                size.width, size.height);
    return true;
}
void SceneTargets::ResolveAll() {
    for (auto& [uid, entry] : entries) {
        if (entry->state.dirty) {
            if (auto* image = lookup(entry->source, uid)) Copy(*entry, *image, true);
        }
    }
}
void SceneTargets::NativeAccess(VideoCore::Image& image, vk::AccessFlags2 access) {
    if (copying) return;
    const auto it = entries.find(image.image_uid);
    if (it == entries.end()) return;
    auto& entry = *it->second;
    if (entry.state.dirty) Copy(entry, image, true);
    constexpr auto writes = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
        vk::AccessFlagBits2::eMemoryWrite;
    if (access & writes) entry.state.NativeWrite();
}
void SceneTargets::Transition(Entry& e, vk::ImageAspectFlags aspect, vk::ImageLayout layout,
                               vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = stages, .dstAccessMask = access,
        .oldLayout = e.layout, .newLayout = layout,
        .image = e.image, .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    scheduler.Record([barrier](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    e.layout = layout;
}
void SceneTargets::Copy(Entry& e, VideoCore::Image& original, bool to_native) {
    copying = true;
    scheduler.EndRendering();
    const vk::Image src = to_native ? vk::Image(e.image) : original.GetImage();
    const vk::Image dst = to_native ? original.GetImage() : vk::Image(e.image);
    if (ShaderResampled(original)) {
        constexpr auto read_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        constexpr auto write_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        constexpr auto read_stage = vk::PipelineStageFlagBits2::eFragmentShader;
        constexpr auto write_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                     vk::PipelineStageFlagBits2::eLateFragmentTests;
        constexpr auto read_access = vk::AccessFlagBits2::eShaderSampledRead;
        constexpr auto write_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        runtime.Transit(&original, to_native ? write_layout : read_layout,
                        to_native ? write_stage : read_stage,
                        to_native ? write_access : read_access);
        runtime.FlushBarriers();
        Transition(e, original.aspect_mask, to_native ? read_layout : write_layout,
                   to_native ? read_stage : write_stage, to_native ? read_access : write_access);
        Resample(src, dst, original,
                 to_native ? vk::Extent2D{1920, 1080} : vk::Extent2D{size.width, size.height});
        if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
        copying = false;
        return;
    }
    runtime.Transit(&original, to_native ? vk::ImageLayout::eTransferDstOptimal
                                       : vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer,
                    to_native ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    Transition(e, original.aspect_mask,
               to_native ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eTransferDstOptimal,
               vk::PipelineStageFlagBits2::eTransfer,
               to_native ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eTransferWrite);
    const vk::Offset3D native{1920,1080,1}, reduced{s32(size.width),s32(size.height),1};
    for (auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                        vk::ImageAspectFlagBits::eStencil}) {
        if (!(original.aspect_mask & aspect)) continue;
        const vk::ImageBlit region{
            .srcSubresource = {aspect,0,0,1},
            .srcOffsets = std::array{vk::Offset3D{}, to_native ? reduced : native},
            .dstSubresource = {aspect,0,0,1},
            .dstOffsets = std::array{vk::Offset3D{}, to_native ? native : reduced},
        };
        // Nearest preserves depth/stencil and avoids assuming linear blit support for
        // integer G-buffer formats. FSR reconstructs color at the HDR scene boundary.
        scheduler.Record([src,dst,region](vk::CommandBuffer cmd) {
            cmd.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        });
    }
    if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
    copying = false;
}
void SceneTargets::Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                            vk::Extent2D dst_size) {
    const auto device = instance.GetDevice();
    const auto format = original.backing->image.image_ci.format;
    const bool stencil = bool(original.aspect_mask & vk::ImageAspectFlagBits::eStencil);
    const auto make_view = [&](vk::Image image, vk::ImageAspectFlags aspect,
                               vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        return Check(device.createImageView({
            .pNext = &usage_ci, .image = image, .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    const auto depth_view = make_view(src, vk::ImageAspectFlagBits::eDepth,
                                      vk::ImageUsageFlagBits::eSampled);
    const auto stencil_view = stencil ? make_view(src, vk::ImageAspectFlagBits::eStencil,
                                                  vk::ImageUsageFlagBits::eSampled)
                                      : vk::ImageView{};
    const auto target_view = make_view(dst, original.aspect_mask,
                                       vk::ImageUsageFlagBits::eDepthStencilAttachment);
    scheduler.DeferOperation([device, depth_view, stencil_view, target_view] {
        device.destroyImageView(depth_view);
        if (stencil_view) device.destroyImageView(stencil_view);
        device.destroyImageView(target_view);
    });
    const auto pipeline = ResamplePipeline(format, stencil);
    const auto layout = *resample_layout;
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target_view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmd.beginRendering({
            .renderArea = {{0, 0}, dst_size}, .layerCount = 1,
            .pDepthAttachment = &attachment,
            .pStencilAttachment = stencil ? &attachment : nullptr,
        });
        const std::array infos{
            vk::DescriptorImageInfo{{}, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal},
            vk::DescriptorImageInfo{{}, stencil_view, vk::ImageLayout::eShaderReadOnlyOptimal},
        };
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[1]},
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
                                 vk::ArrayProxy<const vk::WriteDescriptorSet>(
                                     stencil ? 2u : 1u, writes.data()));
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmd.setViewportWithCount(vk::Viewport{0.f, 0.f, float(dst_size.width),
                                              float(dst_size.height), 0.f, 1.f});
        cmd.setScissorWithCount(vk::Rect2D{{0, 0}, dst_size});
        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
    });
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    scheduler.GetDynamicState().Invalidate();
}
void SceneTargets::CreateResampleResources() {
    const auto device = instance.GetDevice();
    fs_tri_vert = vk::UniqueShaderModule(CompileSPV(FS_TRI_VERT, device), device);
    depth_frag = vk::UniqueShaderModule(CompileSPV(DEPTH_RESAMPLE_FRAG, device), device);
    if (instance.IsShaderStencilExportSupported()) {
        depth_stencil_frag =
            vk::UniqueShaderModule(CompileSPV(DEPTH_STENCIL_RESAMPLE_FRAG, device), device);
    }
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
    };
    resample_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data(),
    }));
    const auto set_layout = *resample_set_layout;
    resample_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    }));
}
vk::Pipeline SceneTargets::ResamplePipeline(vk::Format format, bool stencil) {
    const std::pair key{format, stencil};
    for (const auto& [k, pipeline] : resample_pipelines) if (k == key) return *pipeline;
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    // Reference, compare and write masks are static; the exported value replaces the reference.
    const vk::StencilOpState stencil_op{
        .failOp = vk::StencilOp::eReplace, .passOp = vk::StencilOp::eReplace,
        .depthFailOp = vk::StencilOp::eReplace, .compareOp = vk::CompareOp::eAlways,
        .compareMask = 0xff, .writeMask = 0xff, .reference = 0,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = stencil, .front = stencil_op, .back = stencil_op,
    };
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = u32(dynamic_states.size()), .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *fs_tri_vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = stencil ? *depth_stencil_frag : *depth_frag,
                                          .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .depthAttachmentFormat = format,
        .stencilAttachmentFormat = stencil ? format : vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.f};
    auto pipeline = Check(instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, {
        .pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending, .pDynamicState = &dynamic_info,
        .layout = *resample_layout,
    }));
    const auto result = *pipeline;
    resample_pipelines.emplace_back(key, std::move(pipeline));
    return result;
}
SceneTargets::Entry& SceneTargets::Get(VideoCore::ImageId id) {
    auto& original = *lookup(id, 0);
    // A draw has up to six targets, mostly the same as the previous draw's.
    for (const auto& [uid, recent_entry] : recent) {
        if (recent_entry && uid == original.image_uid) {
            if (!recent_entry->state.valid) Copy(*recent_entry, original, false);
            return *recent_entry;
        }
    }
    auto& entry = entries[original.image_uid];
    if (!entry) {
        entry = std::make_unique<Entry>();
        entry->source = id;
        entry->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        auto ci = original.backing->image.image_ci;
        ci.pNext = nullptr;
        ci.extent = vk::Extent3D{size.width,size.height,1};
        entry->image.Create(ci);
    }
    recent[recent_next++ % recent.size()] = {original.image_uid, entry.get()};
    if (!entry->state.valid) Copy(*entry, original, false);
    return *entry;
}
vk::ImageView SceneTargets::View(Entry& e, const VideoCore::Image& original,
                                 const VideoCore::ImageViewInfo& info) {
    for (const auto& [key,view] : e.views) if (key == info) return *view;
    const auto aspect = original.info.props.is_depth
        ? (info.is_storage ? original.aspect_mask : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth))
        : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
    const auto format = original.info.props.is_depth ? original.backing->image.image_ci.format : info.format;
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .image = e.image, .viewType = vk::ImageViewType::e2D, .format = format,
        .components = info.mapping, .subresourceRange = {aspect,0,1,0,1},
    }));
    const auto result = *view;
    e.views.emplace_back(info, std::move(view));
    return result;
}
SceneTargets::Target SceneTargets::Attachment(VideoCore::ImageId id,
                                               const VideoCore::ImageViewInfo& info) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id);
    const bool depth = original.info.props.is_depth;
    const auto layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                              : vk::ImageLayout::eColorAttachmentOptimal;
    // Consecutive draws can stay in the same render pass.
    if (e.layout != layout) {
        Transition(e, original.aspect_mask, layout,
            depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
                  : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eColorAttachmentOutput),
            depth ? vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                  : vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    }
    e.state.ProxyWrite();
    auto attachment_info = info;
    attachment_info.is_storage = true; // include stencil in attachment views
    return {e.image, View(e, original, attachment_info), e.layout, e.image.image_ci.usage};
}
SceneTargets::Target SceneTargets::Read(VideoCore::ImageId id,
                                       const VideoCore::ImageViewInfo& info,
                                       vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id);
    Transition(e, original.aspect_mask, vk::ImageLayout::eGeneral, stages, access);
    return {e.image, View(e, original, info), e.layout, e.image.image_ci.usage};
}
} // namespace Vulkan
