// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_toggles.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"

namespace Vulkan {

namespace {

bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    using AmdGpu::PM4ItOpcode;
    return opcode == PM4ItOpcode::DrawIndex2 || opcode == PM4ItOpcode::DrawIndexOffset2 ||
           opcode == PM4ItOpcode::DrawIndexAuto;
}

/// Walks the type-3 packets of a command buffer (type-2 padding skipped).
template <typename Func>
void ForEachPacket(std::span<const u32> commands, Func&& func) {
    for (size_t at = 0; at < commands.size();) {
        const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t words = header->type3.NumWords() + 1;
        if (at + words > commands.size()) {
            return;
        }
        func(header);
        at += words;
    }
}

u32 DefaultWorkerCount() {
    if (const char* env = std::getenv("BB_PREP_WORKERS")) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 8));
    }
    // The game keeps ~9 threads busy (main, 5 render workers, physics, audio) next to the GPU
    // and recording threads: prepare only where there are cores to spare.
    const u32 threads = std::thread::hardware_concurrency();
    return threads >= 12 ? std::min<u32>(4, threads / 4) : 0;
}

/// Flattened user data of one submission: chunks never move, so the GPU thread can read a
/// prepared draw's data while the worker keeps appending.
struct FlatArena {
    static constexpr size_t ChunkWords = 16 * 1024;
    std::vector<std::unique_ptr<u32[]>> chunks;
    size_t used = ChunkWords;

    const u32* Append(const std::vector<u32>& data) {
        if (chunks.empty() || data.size() > ChunkWords - used) {
            chunks.push_back(std::make_unique<u32[]>(std::max(ChunkWords, data.size())));
            used = 0;
        }
        u32* dst = chunks.back().get() + used;
        std::memcpy(dst, data.data(), data.size() * sizeof(u32));
        used += data.size();
        return dst;
    }
};

} // namespace

DrawPreparation::DrawPreparation(PipelineCache& pipeline_cache_)
    : pipeline_cache{pipeline_cache_} {
    worker_count = DefaultWorkerCount();
    for (u32 i = 0; i < worker_count; ++i) {
        workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    }
    std::printf("GPU: draw preparation workers: %u\n", worker_count);
}

DrawPreparation::~DrawPreparation() {
    for (auto& worker : workers) {
        worker.request_stop();
    }
    cv.notify_all();
    workers.clear();
}

void DrawPreparation::Enqueue(u64 seq, std::span<const u32> commands) {
    if (!Enabled()) {
        return;
    }
    auto submission = std::make_shared<Submission>();
    submission->seq = seq;
    submission->commands.assign(commands.begin(), commands.end());
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
        submission->num_draws += IsDirectDraw(header->type3.opcode);
    });
    submission->draws = std::make_unique<PreparedDraw[]>(submission->num_draws);
    {
        std::scoped_lock lk{mutex};
        submissions.push_back(std::move(submission));
    }
    cv.notify_all();
}

void DrawPreparation::BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum) {
    current_draw = 0;
    current.reset();
    if (!Enabled()) {
        return;
    }
    gpu_seq.store(seq, std::memory_order_release);
    std::scoped_lock lk{mutex};
    if (!baseline_ready) {
        initial_regs = std::make_unique<AmdGpu::Regs>(regs);
        initial_checksum = reg_checksum;
        baseline_seq = seq;
        baseline_ready = true;
        cv.notify_all();
    }
    if (!submissions.empty() && seq >= submissions.front()->seq &&
        seq - submissions.front()->seq < submissions.size()) {
        current = submissions[seq - submissions.front()->seq];
    }
}

void DrawPreparation::EndSubmission() {
    if (current) {
        current->gpu_done.store(true, std::memory_order_release);
        current.reset();
    }
    if (Enabled()) {
        Collect();
    }
}

const PreparedDraw* DrawPreparation::NextDraw() {
    if (!current || current_draw >= current->num_draws) {
        return nullptr;
    }
    const PreparedDraw* draw = &current->draws[current_draw++];
    if (BbToggle::Disabled(BbToggle::DrawPreparation)) {
        return nullptr;
    }
    return draw;
}

void DrawPreparation::Collect() {
    std::scoped_lock lk{mutex};
    while (!submissions.empty()) {
        const auto& front = submissions.front();
        // Buffers before the baseline are never replayed by the workers.
        if (!baseline_ready || !front->gpu_done.load(std::memory_order_acquire) ||
            (front->seq >= baseline_seq &&
             front->workers_done.load(std::memory_order_acquire) < worker_count)) {
            break;
        }
        submissions.pop_front();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 index) {
    Common::SetCurrentThreadName(("bb:DrawPrep" + std::to_string(index)).c_str());
    std::unique_ptr<AmdGpu::Regs> regs;
    u64 checksum = 0;
    u64 next_seq = 0;
    {
        std::unique_lock lk{mutex};
        cv.wait(lk, stop, [&] { return baseline_ready; });
        if (stop.stop_requested()) {
            return;
        }
        regs = std::make_unique<AmdGpu::Regs>(*initial_regs);
        checksum = initial_checksum;
        next_seq = baseline_seq;
    }
    PipelineSelection sel{};
    sel.regs = regs.get();
    PrepWorker prep_worker{};
    sel.worker = &prep_worker;
    FlatArena arena;

    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, stop, [&] {
                return !submissions.empty() && submissions.back()->seq >= next_seq;
            });
            if (stop.stop_requested()) {
                return;
            }
            // The buffer cannot be collected before this worker has replayed it.
            submission = submissions[next_seq - submissions.front()->seq];
        }
        // Stale buffers (the GPU thread is already there) only update the register state.
        const bool assigned = submission->seq % worker_count == index;
        u32 ordinal = 0;
        arena = FlatArena{};
        ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, header, checksum);
            if (!IsDirectDraw(header->type3.opcode)) {
                return;
            }
            if (assigned && ordinal < submission->num_draws &&
                submission->seq >= gpu_seq.load(std::memory_order_acquire)) {
                auto& draw = submission->draws[ordinal];
                prep_worker.stages.clear();
                prep_worker.failed = false;
                sel.draw_indirect_params = {};
                if (pipeline_cache.PrepareGraphicsPipeline(sel)) {
                    draw.reg_checksum = checksum;
                    draw.key = sel.graphics_key;
                    draw.num_stages = static_cast<u32>(prep_worker.stages.size());
                    for (u32 i = 0; i < draw.num_stages; ++i) {
                        const auto& stage = prep_worker.stages[i];
                        draw.stages[i] = {
                            .program = stage.program,
                            .hash = stage.hash,
                            .hw_stage = stage.hw_stage,
                            .pgm_base = stage.pgm_base,
                            .flat = arena.Append(*stage.flat),
                            .flat_size = static_cast<u32>(stage.flat->size()),
                        };
                    }
                    draw.state.store(PreparedDraw::Ready, std::memory_order_release);
                } else {
                    draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
                }
            }
            ++ordinal;
        });
        if (assigned) {
            // The arena's chunks live as long as the submission.
            submission->flat_chunks = std::move(arena.chunks);
        }
        submission->workers_done.fetch_add(1, std::memory_order_acq_rel);
        ++next_seq;
        cv.notify_all();
    }
}

void DrawPreparation::Count(bool was_used) {
    ++(was_used ? used : unused);
    static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (!stats || now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    std::printf("Draw preparation: %llu prepared pipelines used, %llu not (%u workers)\n",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(unused),
                worker_count);
    used = unused = 0;
}

} // namespace Vulkan
