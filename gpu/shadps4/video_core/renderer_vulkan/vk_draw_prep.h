// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: speculative draw preparation on worker threads (docs/parallel_gpu.md, step 1).
//
// Every submitted graphics command buffer is copied and handed to the workers. Each worker
// replays the register writes of the whole stream (ApplyGraphicsRegisterPacket, the same code
// the GPU thread runs) and, for the buffers assigned to it, selects the graphics pipeline of
// each direct draw ahead of the GPU thread. The GPU thread uses a prepared draw only when the
// running register checksums match and the flattened user data it computes itself equals the
// worker's; otherwise it takes the regular path. Workers never create or compile anything.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

struct PreparedStage {
    const Program* program;
    u64 hash;
    Shader::HwStage hw_stage;
    VAddr pgm_base;
    const u32* flat; ///< flattened user data the worker computed, owned by the submission
    u32 flat_size;
};

struct PreparedDraw {
    enum : u32 { Pending = 0, Ready = 1, Unavailable = 2 };
    std::atomic<u32> state{Pending};
    u64 reg_checksum{};
    GraphicsPipelineKey key{};
    u32 num_stages{};
    std::array<PreparedStage, MaxShaderStages> stages{};
};

class DrawPreparation {
public:
    explicit DrawPreparation(PipelineCache& pipeline_cache);
    ~DrawPreparation();

    DrawPreparation(const DrawPreparation&) = delete;
    DrawPreparation& operator=(const DrawPreparation&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return worker_count != 0;
    }

    struct Submission;
    /// Game submit thread, outside the queue lock: copies and scans a top-level graphics
    /// command buffer. Null when disabled.
    std::shared_ptr<Submission> Build(std::span<const u32> commands);
    /// Game submit thread, under the queue lock: hands it to the workers in submission order.
    void Enqueue(u64 seq, std::shared_ptr<Submission> submission);

    /// GPU thread: brackets the processing of submission `seq`. The first call hands the
    /// workers the exact register state and checksum they start replaying from.
    void BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum);
    void EndSubmission();

    /// GPU thread: the prepared state for the next direct draw of the current submission.
    const PreparedDraw* NextDraw();

    /// GPU thread: counts whether a prepared draw was used; prints every 5 s (BB_FRAME_STATS).
    void Count(bool used);

    struct Submission {
        u64 seq{};
        std::vector<u32> commands;
        std::unique_ptr<PreparedDraw[]> draws;
        u32 num_draws{};
        std::vector<std::unique_ptr<u32[]>> flat_chunks; ///< from the assigned worker
        std::atomic<u32> workers_done{0};
        std::atomic<bool> gpu_done{false};
    };

private:

    void WorkerLoop(std::stop_token stop, u32 index);
    void Collect();

    PipelineCache& pipeline_cache;
    std::unique_ptr<AmdGpu::Regs> initial_regs; ///< state at the start of `baseline_seq`
    u64 initial_checksum{};
    u64 baseline_seq{};
    bool baseline_ready{}; ///< guarded by `mutex`
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<Submission>> submissions; ///< ordered by seq
    std::atomic<u64> gpu_seq{0};
    std::shared_ptr<Submission> current;
    u32 current_draw = 0;
    u64 used = 0, unused = 0;
    u32 worker_count = 0; ///< fixed before the worker threads start
    std::vector<std::jthread> workers;
};

} // namespace Vulkan
