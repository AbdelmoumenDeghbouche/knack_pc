// What the NGS2 sources from AnyPS5 (core/libs/prx/libSceNgs2.native, GPL-2.0) take from AnyPS5's
// libc headers, for this runtime.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "libatrac9.h"

#define APS5_VABI __attribute__((sysv_abi))
// AnyPS5 throws on invalid or unimplemented input; an exception cannot unwind through the game's
// frames here, so the message is printed and the process stops, as the runtime's STOP contracts do.
[[noreturn]] static inline void ngs2_stop(const std::string& message) {
    std::fprintf(stderr, "STOP: %s\n", message.c_str());
    std::fflush(nullptr);
    std::_Exit(21); // exit() can hang: game threads still hold runtime locks
}
// Invalid arguments end the process with the function's name.
#define APS5_INVALID_ARG_EX ngs2_stop(std::string(__func__) + ": invalid argument")

extern "C" int runtime_file_translate(const char* guest, char* out, std::size_t size);
// sceNgs2ParseWaveformFile's guest path on the host (runtime_file.c mounts); unmounted paths stay
// as given and fail to open.
static inline std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    char host[1024];
    return runtime_file_translate(path, host, sizeof(host)) == 0 ? std::filesystem::path(host) : std::filesystem::path(path);
}

// shadPS4's LibAtrac9 fork decodes to float; upstream LibAtrac9 (third_party) decodes to 16-bit.
// ponytail: 16-bit precision, decode to float directly if a game's music needs it.
static inline int Atrac9DecodeF32(void* handle, const unsigned char* in, int size, float* out, int* used, int) {
    (void)size;
    Atrac9CodecInfo info;
    if (Atrac9GetCodecInfo(handle, &info) != 0) return -1;
    std::vector<short> pcm(static_cast<std::size_t>(info.frameSamples) * info.channels);
    const int status = Atrac9Decode(handle, in, pcm.data(), used);
    for (std::size_t i = 0; status == 0 && i < pcm.size(); ++i) out[i] = pcm[i] / 32768.0f;
    return status;
}
