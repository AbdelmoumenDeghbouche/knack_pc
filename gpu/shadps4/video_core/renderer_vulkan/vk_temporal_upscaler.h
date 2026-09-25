// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: temporal upscaling of Bloodborne's HDR scene color (docs/upscaler.md). BB_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged. Render and output size
// are equal for now (anti-aliasing mode).

#pragma once

#include <chrono>
#include <memory>

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

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    void OnFrameStart();

private:
    void Run();
    bool EnsureResources(u32 width, u32 height);

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
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    u32 width = 0, height = 0;
    FfxVkPortableUpscaleContext* context = nullptr;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
};

} // namespace Vulkan
