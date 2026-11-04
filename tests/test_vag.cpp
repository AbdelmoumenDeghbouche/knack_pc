// HE-VAG decoding (src/ngs2/Vag.h): several channels take turns frame by frame, each with its own history.
#include <cassert>
#include <cstdio>
#include "ngs2/Vag.h"

int main() {
    // A loud frame (predictor 1, shift 0) and a silent one (flag 7).
    std::uint8_t loud[16] = {0x10, 0x00}, quiet[16] = {0x00, 0x07};
    for (int i = 2; i < 16; ++i) loud[i] = 0x71;
    std::uint8_t stereo[64];
    for (int f = 0; f < 2; ++f) {  // channel 0 loud, channel 1 silent, two frames each
        std::copy(loud, loud + 16, stereo + f * 32);
        std::copy(quiet, quiet + 16, stereo + f * 32 + 16);
    }
    std::uint8_t mono_data[32];
    std::copy(loud, loud + 16, mono_data);
    std::copy(loud, loud + 16, mono_data + 16);
    const auto mono = DecodeVag(mono_data, 32);
    const auto both = DecodeVag(stereo, 64, 2);
    assert(mono.size() == 56 && both.size() == 112);
    for (int i = 0; i < 56; ++i) {
        assert(both[i * 2] == mono[i]);  // channel 0 decodes as if alone (history carried across its frames)
        assert(both[i * 2 + 1] == 0);    // channel 1 stays silent
    }
    assert(mono[30] != 0);
    std::puts("PASS: vag");
}
