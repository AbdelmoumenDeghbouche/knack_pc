// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
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
};
inline bool Disabled(std::uint32_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle
