// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_toggles.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_} {
    const char* env = std::getenv("BB_UPSCALER");
    enabled = env && std::strcmp(env, "fsr3") == 0;
    if (const char* hash = std::getenv("BB_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (enabled && !instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        enabled = false;
    }
    if (enabled) {
        std::printf("Upscaler: FSR 3.1 on scene color before compute shader %016llx\n",
                    static_cast<unsigned long long>(trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    if (context) {
        instance.GetDevice().waitIdle();
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    scene_color = color;
}

namespace {
float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

void TemporalUpscaler::OnFrameStart() {
    done_this_frame = false;
    // Halton(2, 3) over 8 phases (FSR's count for a 1:1 ratio). BB_JITTER=0 or toggle 1 << 25
    // disables it.
    static const bool jitter_enabled = [] {
        const char* env = std::getenv("BB_JITTER");
        return !env || env[0] != '0';
    }();
    if (!enabled || failed || !jitter_enabled || BbToggle::Disabled(1u << 24) ||
        BbToggle::Disabled(1u << 25)) {
        jitter = {};
        return;
    }
    jitter_index = jitter_index % 8 + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed) {
        return;
    }
    done_this_frame = true;
    // Toggle 1 << 24 switches the upscaler off at run time (A/B); history restarts after.
    if (!scene_color || !camera_motion.Ready() || BbToggle::Disabled(1u << 24)) {
        reset = true;
        return;
    }
    Run();
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h) {
    if (context && w == width && h == height) {
        return true;
    }
    const auto device = instance.GetDevice();
    if (context) {
        device.waitIdle();
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {w, h};
    if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
        PrintIssues("create info", issues);
        return false;
    }
    if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 context creation failed\n");
        context = nullptr;
        return false;
    }

    const auto allocator = instance.GetAllocator();
    motion_image = VideoCore::UniqueImage(device, allocator);
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_view = Check(device.createImageViewUnique({
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    if (!merge_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        merge_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        merge_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*merge_desc_layout,
        }));
        const auto module = CompileSPV(UPSCALE_MERGE_COMP, device);
        merge_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *merge_pipeline_layout,
                }));
        device.destroyShaderModule(module);
    }
    motion_view = Check(device.createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    reset = true;
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage);
    std::printf("Upscaler: FSR 3 context %ux%u, %.1f MB\n", w, h, usage.totalUsageInBytes / 1e6);
    return true;
}

void TemporalUpscaler::Run() {
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 w = color.info.size.width, h = color.info.size.height;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != w || depth.info.size.height != h ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    if (!EnsureResources(w, h)) {
        failed = true;
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));

    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Previous contents are not needed: the layouts start from undefined every frame.
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h);
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmdbuf;
    info.color = Describe(vk::Image(color.backing->image), color.info.pixel_format, w, h,
                          color.usage_flags, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(vk::Image(depth.backing->image), depth_format, w, h, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat, w, h,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferSrc,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    // Optional inputs, absent: a described but null image.
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
    const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {w, h};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = camera_motion.Near();
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = 0.2f;
    info.enableSharpening = VK_TRUE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {w, h};
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
        static bool printed = false;
        if (!printed) {
            printed = true;
            PrintIssues("dispatch", issues);
        }
        failed = true;
    } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) !=
               FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 dispatch failed\n");
        failed = true;
    } else {
        reset = false;
        // The result replaces the scene color's RGB (its alpha carries data for the post).
        own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        const vk::DescriptorImageInfo out_info{.imageView = *output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 2> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
