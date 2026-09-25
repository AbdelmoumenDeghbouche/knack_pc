// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <cstdint>

extern "C" std::uint32_t runtime_disabled_optimizations;

namespace BbToggle {
enum : std::uint32_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
};
inline bool Disabled(std::uint32_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
} // namespace BbStats
