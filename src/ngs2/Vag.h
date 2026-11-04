// knack_pc: HE-VAG decoding for NGS2 voices (Voice.cpp) and tests/test_vag.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "hevag_coefs.h"

// knack_pc: VAG waveforms (type 0x1c) are HE-VAG (High Efficiency VAG, mono): 16-byte frames of 28
// samples. Byte 0 holds the low predictor bits (high nibble) and the shift (low nibble), byte 1 the high
// predictor bits (high nibble) and the flags (low nibble; 7 = silent frame); 4-bit samples follow, low
// nibble first. 128 four-tap predictors (hevag_coefs.h); the first five are classic VAG's filters.
// As vgmstream's decode_hevag (src/coding/psv_decoder.c); history is kept unclamped.

// Several channels (#15: the last chapter's giant relic scene): their 16-byte frames take turns
// (channel 0's frame, channel 1's, ...), as vgmstream reads HE-VAG with interleave 0x10; each channel
// keeps its own history and the samples come out interleaved, as PCM.
inline std::vector<std::int16_t> DecodeVag(const std::uint8_t* data, std::uint64_t bytes, std::uint32_t channels = 1) {
    std::vector<std::int16_t> out;
    const std::uint64_t frames = bytes / 16 / channels;
    out.resize(frames * 28 * channels);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
    std::int32_t h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const std::uint64_t at = (frame * channels + ch) * 16;
        std::uint32_t index = (data[at + 1] & 0xf0) | (data[at] >> 4);
        if (index > 127) index = 0;
        const int shift = data[at] & 15, flag = data[at + 1] & 15;
        const float* c = HEVAG_COEFS[index];
        for (int i = 0; i < 28; ++i) {
            std::int32_t sample = 0;
            if (flag < 7) {
                const std::int32_t code = ((static_cast<std::int32_t>(data[at + 2 + i / 2] >> (i % 2 * 4)) & 15) ^ 8) - 8;
                sample = (code * (1 << 12)) >> shift;
                sample += static_cast<std::int32_t>(h1 * c[0] + h2 * c[1] + h3 * c[2] + h4 * c[3]);
            }
            out[(frame * 28 + i) * channels + ch] = static_cast<std::int16_t>(std::clamp(sample, -32768, 32767));
            h4 = h3; h3 = h2; h2 = h1; h1 = sample;
        }
    }
    }
    return out;
}

