// bbport: parallel copies of guest memory. Streaming a new area uploads 50-400 MB of textures
// and buffers per frame; copied on one thread that is tens of milliseconds (stutter).
#pragma once
#include <cstddef>
#include <functional>

namespace BbCopy {

/// Runs `task(i)` for every i in [0, count) on the copy threads and the caller; returns when
/// all are done. Runs inline when count is 1, the pool is disabled, or when called from a copy
/// thread (no nesting).
void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task);

/// True when ParallelFor would split work (more than one thread available).
bool Enabled();

} // namespace BbCopy
