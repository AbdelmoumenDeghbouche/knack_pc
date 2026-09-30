// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
inline Size ForPreset(int preset) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<Size, 5> sizes{{{1920,1080}, {1280,720}, {1130,636},
                                       {960,540}, {640,360}}};
    return sizes[std::clamp(preset, 0, 4)];
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
