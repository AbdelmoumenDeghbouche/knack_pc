// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: histogram of the subgroup widths the driver compiled pipeline stages at, from
// VK_KHR_pipeline_executable_properties (GOW3_WAVE_STATS=1; Pipeline::RecordWaveSizes in
// vk_pipeline_common.cpp). The PS4 shaders assume 64 lanes; a fragment stage compiled at 32
// covers 8x4 pixels per wave.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace Vulkan {

class WaveSizeHistogram {
public:
    /// Counts one executable of `stages` (as vk::to_string prints the flags) compiled at
    /// `subgroup_size` lanes. Returns true the first time this pair is seen.
    bool Add(std::string stages, std::uint32_t subgroup_size) {
        auto& count = counts[{std::move(stages), subgroup_size}];
        ++count;
        return count == 1;
    }

    /// Closes one pipeline. Returns true when a report is due: on news, and every 256
    /// pipelines regardless.
    bool EndPipeline(bool news) {
        ++pipelines;
        return news || pipelines % 256 == 0;
    }

    std::uint32_t Pipelines() const {
        return pipelines;
    }

    /// "{ Compute }@64 x1, { Fragment }@32 x3", sorted by stages then width.
    std::string Summary() const {
        std::string out;
        for (const auto& [key, count] : counts) {
            if (!out.empty()) {
                out += ", ";
            }
            out += key.first + "@" + std::to_string(key.second) + " x" + std::to_string(count);
        }
        return out;
    }

private:
    std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> counts;
    std::uint32_t pipelines = 0;
};

} // namespace Vulkan
