// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: temporal upscaling of Bloodborne's HDR scene color (docs/upscaler.md). BB_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged (Native AA preset).
//
// Scaled presets: the game renders at a lower resolution (resolution patch from patches.py) and
// FSR upscales its finished, tonemapped frame right before the UI. The UI passes and the pass
// copying the frame to the display buffer are redirected to output-size images, and the
// presenter shows those instead of the guest display buffer: the UI stays sharp.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;

class TemporalUpscaler {
public:
    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    void OnFrameStart();

    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return done_this_frame ? std::array<float, 2>{} : jitter;
    }

    // Scaled presets.

    /// Before every draw: the first UI pass's vertex shader starts the upscale.
    void OnDraw(u64 vs_hash);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    struct Target {
        vk::ImageView view;
        vk::ImageLayout layout;
        u32 width, height;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected views mirror its format (sRGB) and,
    /// for sampling, its channel swizzle.
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    /// A sampled image replaced by the upscaled frame (the display pass).
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        vk::Image image;
        vk::Format format;
        u32 width, height;
    };
    bool DisplayOverride(VAddr address, Display& display);

private:
    void Run();
    void RunScaled();
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    void CreatePipelines();
    /// Records the reactive mask pass; false when there is no snapshot this frame.
    bool RecordReactive(vk::CommandBuffer cmdbuf, vk::ImageView color_view);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;

    bool enabled = false;
    bool failed = false;
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    bool done_this_frame = false;
    bool snapshot_taken = false;
    bool opaque_valid = false;
    bool mask_ready = false;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    u32 width = 0, height = 0;             ///< render size
    u32 out_width = 0, out_height = 0;     ///< output size of the context
    bool context_hdr = true;
    u32 target_width = 1920, target_height = 1080; ///< output size of scaled presets
    u64 ui_trigger_vs = 0x34e8a281;
    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};
    bool ui_phase = false;         ///< from the upscale to the next display pass
    bool display_redirect = false; ///< the display pass of an upscaled frame
    bool ui_read_barrier = false;
    VideoCore::UniqueImage ui_image;
    VideoCore::UniqueImage ui_depth_image;
    vk::UniqueImageView ui_view;
    vk::UniqueImageView ui_depth_view;
    bool depth_blit = false;
    bool scaled_session = false;
    /// Views of the port's images in the formats/swizzles the game's views use.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        vk::UniqueImageView view;
    };
    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);
    std::vector<MirrorView> ui_views;
    struct DisplayImage {
        VideoCore::UniqueImage image;
        std::vector<MirrorView> views;
        vk::Format format{};
        bool valid = false;
    };
    std::mutex display_mutex;
    std::unordered_map<VAddr, DisplayImage> displays;
    FfxVkPortableUpscaleContext* context = nullptr;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
    VideoCore::UniqueImage opaque_image;   ///< scene color before the blended draws
    VideoCore::UniqueImage reactive_image; ///< R8 reactive mask
    vk::UniqueImageView opaque_view;
    vk::UniqueImageView reactive_view;
    vk::UniqueDescriptorSetLayout reactive_desc_layout;
    vk::UniquePipelineLayout reactive_pipeline_layout;
    vk::UniquePipeline reactive_pipeline;
    vk::UniqueDescriptorSetLayout merge_desc_layout;
    vk::UniquePipelineLayout merge_pipeline_layout;
    vk::UniquePipeline merge_pipeline;
};

} // namespace Vulkan
