// SPDX-License-Identifier: GPL-2.0-or-later
// Histogram of the subgroup widths the driver compiled pipeline stages at (vk_wave_stats.h).
// No game, Vulkan device or window required.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_wave_stats.h"

using namespace Vulkan;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    WaveSizeHistogram histogram;
    assert(histogram.Summary() == "");

    // Add reports whether the (stages, width) pair is new: the caller prints on news.
    assert(histogram.Add("{ Fragment }", 64));
    assert(!histogram.Add("{ Fragment }", 64));
    assert(histogram.Add("{ Fragment }", 32));
    assert(histogram.Add("{ Compute }", 64));
    assert(histogram.Add("{ Vertex }", 64));
    assert(!histogram.Add("{ Vertex }", 64));

    // Sorted by stage then width, with the count of executables seen.
    assert(histogram.Summary() ==
           "{ Compute }@64 x1, { Fragment }@32 x1, { Fragment }@64 x2, { Vertex }@64 x2");

    // One pipeline per call; a report every 256 pipelines even without news.
    for (int i = 0; i < 255; ++i) {
        assert(!histogram.EndPipeline(false));
    }
    assert(histogram.EndPipeline(false));
    assert(histogram.Pipelines() == 256u);
    assert(histogram.EndPipeline(true));
    assert(histogram.Pipelines() == 257u);

    std::puts("wave-stats-test: ok");
    return 0;
}
