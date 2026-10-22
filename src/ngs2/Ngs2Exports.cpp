// libSceNgs2 for the runtime: the NGS2 implementation from AnyPS5 (this folder, PS5 structures),
// bound by name (runtime_lookup). PS4 games pass older structures to some calls (the layouts are in
// shadPS4's ngs2 headers); those calls are translated here. Calls whose PS4 structures differ and
// are not translated are not exported, so a game that imports them stops at the import check.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>

#include "Ngs2Internal.hpp"
#include "ngs2_compat.hpp"

extern "C" {
int APS5_VABI sceNgs2SystemCreate(const Ngs2SystemOption*, const Ngs2ContextBufferInfo*, Ngs2Handle*);
int APS5_VABI sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption*, const Ngs2BufferAllocator*, Ngs2Handle*);
int APS5_VABI sceNgs2SystemQueryBufferSize(const Ngs2SystemOption*, Ngs2ContextBufferInfo*);
int APS5_VABI sceNgs2RackCreate(Ngs2Handle, std::uint32_t, const Ngs2RackOption*, const Ngs2ContextBufferInfo*, Ngs2Handle*);
int APS5_VABI sceNgs2RackCreateWithAllocator(Ngs2Handle, std::uint32_t, const Ngs2RackOption*, const Ngs2BufferAllocator*, Ngs2Handle*);
int APS5_VABI sceNgs2RackQueryBufferSize(std::uint32_t, const Ngs2RackOption*, Ngs2ContextBufferInfo*);
int APS5_VABI sceNgs2VoiceControl(Ngs2Handle, const Ngs2VoiceParamHeader*);
int APS5_VABI sceNgs2VoiceGetState(Ngs2Handle, Ngs2VoiceState*, std::size_t);
int APS5_VABI sceNgs2VoiceGetStateFlags(Ngs2Handle, std::uint32_t*);
// Same structures on PS4 and PS5: exported as they are (only the address is taken).
void sceNgs2SystemDestroy(); void sceNgs2SystemRender(); void sceNgs2SystemLock(); void sceNgs2SystemUnlock();
void sceNgs2SystemSetUserData(); void sceNgs2SystemGetUserData(); void sceNgs2SystemSetGrainSamples();
void sceNgs2SystemSetSampleRate(); void sceNgs2RackDestroy(); void sceNgs2RackGetVoiceHandle(); void sceNgs2RackLock();
void sceNgs2RackUnlock(); void sceNgs2RackSetUserData(); void sceNgs2RackGetUserData(); void sceNgs2VoiceGetOwner();
}

namespace {
// PS4 SDK layouts (name[16], no voice/output channel limits).
struct Ps4SystemOption {
    std::size_t size;
    char name[16];
    std::uint32_t flags, max_grain_samples, num_grain_samples, sample_rate, reserved[6];
};
static_assert(sizeof(Ps4SystemOption) == 64);
struct Ps4RackOption {
    std::size_t size;
    char name[16];
    std::uint32_t flags, max_grain_samples, max_voices, max_input_delay_blocks, max_matrices, max_ports, reserved[20];
};
static_assert(sizeof(Ps4RackOption) == 128);
struct Ps4WaveformBlock {
    std::uint32_t data_offset, data_size, num_repeats, num_skip_samples, num_samples, reserved;
    std::uintptr_t user_data;
};
static_assert(sizeof(Ps4WaveformBlock) == 32);
struct Ps4SamplerVoiceState {
    std::uint32_t state_flags;
    float envelope_height, peak_height;
    std::uint32_t reserved;
    std::uint64_t num_decoded_samples, decoded_data_size;
    std::uintptr_t user_data;
    const void* waveform_data;
};
static_assert(sizeof(Ps4SamplerVoiceState) == 48);
struct Ps4SubmixerVoiceState {
    std::uint32_t state_flags;
    float envelope_height, peak_height, compressor_height;
};

const Ngs2SystemOption* SystemOption(const void* option, Ngs2SystemOption& out) {
    if (!option) return nullptr;
    const auto& in = *static_cast<const Ps4SystemOption*>(option);
    if (in.size != sizeof(Ps4SystemOption)) ngs2_stop("NGS2: PS4 system option size " + std::to_string(in.size));
    out = {};
    out.size = sizeof(out);
    std::memcpy(out.name, in.name, sizeof(in.name));
    out.flags = in.flags;
    out.max_grain_samples = in.max_grain_samples;
    out.num_grain_samples = in.num_grain_samples;
    out.sample_rate = in.sample_rate;
    return &out;
}

// The rack-specific fields that follow the common part (u32 each), by rack id.
std::size_t RackTailWords(std::uint32_t rackId) {
    switch (rackId) {
        case SCE_NGS2_RACK_ID_SAMPLER: return 9;
        case SCE_NGS2_RACK_ID_SUBMIXER: return 5;
        case SCE_NGS2_RACK_ID_MASTERING:
        case SCE_NGS2_RACK_ID_REVERB: return 2;
        default: ngs2_stop("NGS2: PS4 rack id " + Ngs2Hex(rackId) + " is not translated");
    }
}

struct RackOptionBuffer { alignas(8) unsigned char bytes[2048]; };

const Ngs2RackOption* RackOption(std::uint32_t rackId, const void* option, RackOptionBuffer& out) {
    if (!option) return nullptr;
    const auto& in = *static_cast<const Ps4RackOption*>(option);
    const std::size_t tail = RackTailWords(rackId) * 4;
    if (in.size != ((sizeof(Ps4RackOption) + tail + 7) & ~std::size_t(7))) {
        ngs2_stop("NGS2: PS4 rack option size " + std::to_string(in.size) + " for rack " + Ngs2Hex(rackId));
    }
    std::memset(out.bytes, 0, sizeof(out.bytes));
    auto& common = *reinterpret_cast<Ngs2RackOption*>(out.bytes);
    common.size = (sizeof(Ngs2RackOption) + tail + 7) & ~std::size_t(7);
    std::memcpy(common.name, in.name, sizeof(in.name));
    common.flags = in.flags;
    common.max_grain_samples = in.max_grain_samples;
    common.max_voices = in.max_voices;
    common.max_input_delay_blocks = in.max_input_delay_blocks;
    common.max_matrices = in.max_matrices;
    common.max_ports = in.max_ports;
    std::memcpy(out.bytes + sizeof(Ngs2RackOption), reinterpret_cast<const unsigned char*>(option) + sizeof(Ps4RackOption), tail);
    return &common;
}

int APS5_VABI SystemCreate(const void* option, const Ngs2ContextBufferInfo* buffer, Ngs2Handle* handle) {
    Ngs2SystemOption translated;
    return sceNgs2SystemCreate(SystemOption(option, translated), buffer, handle);
}
int APS5_VABI SystemCreateWithAllocator(const void* option, const Ngs2BufferAllocator* allocator, Ngs2Handle* handle) {
    Ngs2SystemOption translated;
    return sceNgs2SystemCreateWithAllocator(SystemOption(option, translated), allocator, handle);
}
int APS5_VABI SystemQueryBufferSize(const void* option, Ngs2ContextBufferInfo* buffer) {
    Ngs2SystemOption translated;
    return sceNgs2SystemQueryBufferSize(SystemOption(option, translated), buffer);
}
int APS5_VABI RackCreate(Ngs2Handle system, std::uint32_t rackId, const void* option, const Ngs2ContextBufferInfo* buffer,
                         Ngs2Handle* handle) {
    RackOptionBuffer translated;
    return sceNgs2RackCreate(system, rackId, RackOption(rackId, option, translated), buffer, handle);
}
int APS5_VABI RackCreateWithAllocator(Ngs2Handle system, std::uint32_t rackId, const void* option,
                                      const Ngs2BufferAllocator* allocator, Ngs2Handle* handle) {
    RackOptionBuffer translated;
    return sceNgs2RackCreateWithAllocator(system, rackId, RackOption(rackId, option, translated), allocator, handle);
}
int APS5_VABI RackQueryBufferSize(std::uint32_t rackId, const void* option, Ngs2ContextBufferInfo* buffer) {
    RackOptionBuffer translated;
    return sceNgs2RackQueryBufferSize(rackId, RackOption(rackId, option, translated), buffer);
}

bool Implemented(std::uint32_t id) {
    switch (id) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        case SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP: case SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS:
        case SCE_NGS2_SAMPLER_VOICE_PARAM_EXIT_LOOP: case SCE_NGS2_SAMPLER_VOICE_PARAM_PITCH:
        case SCE_NGS2_SAMPLER_VOICE_PARAM_FILTER: case SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP:
        case SCE_NGS2_SUBMIXER_VOICE_PARAM_USER_FX: case SCE_NGS2_REVERB_VOICE_PARAM_SETUP:
        case SCE_NGS2_REVERB_VOICE_PARAM_I3DL2: case SCE_NGS2_MASTERING_VOICE_PARAM_SETUP:
        case SCE_NGS2_MASTERING_VOICE_PARAM_GAIN: case SCE_NGS2_MASTERING_VOICE_PARAM_OUTPUT: return true;
        default: return false;
    }
}

// GOW3_NGS2_SKIP_UNKNOWN=1 (diagnostics): parameters NGS2 does not implement are left out, each id
// logged once, instead of stopping; the voice then sounds as if they were not set.
bool SkipUnknown(std::uint32_t id) {
    static const bool skip = [] { const char* v = std::getenv("GOW3_NGS2_SKIP_UNKNOWN"); return v && *v == '1'; }();
    if (!skip || Implemented(id)) return false;
    static std::vector<std::uint32_t> logged;
    static std::mutex lock;
    const std::lock_guard guard(lock);
    if (std::find(logged.begin(), logged.end(), id) == logged.end()) {
        logged.push_back(id);
        std::printf("NGS2: voice param %s skipped (not implemented)\n", Ngs2Hex(id).c_str());
    }
    return true;
}

// The parameter list is copied; waveform block lists are widened to the PS5 block (64-bit offset
// and size). The voice copies the blocks it keeps, so the copies only live for the call.
int APS5_VABI VoiceControl(Ngs2Handle voice, const Ngs2VoiceParamHeader* params) {
    if (!params) return sceNgs2VoiceControl(voice, params);
    std::vector<std::uint64_t> list; // 8-byte aligned parameters
    std::vector<std::vector<Ngs2WaveformBlock>> blocks;
    std::vector<std::size_t> starts;
    for (const auto* param = params;; param = reinterpret_cast<const Ngs2VoiceParamHeader*>(
                                         reinterpret_cast<const std::uint8_t*>(param) + param->next)) {
        if (SkipUnknown(param->id)) {
            if (param->next == 0) break;
            continue;
        }
        const std::size_t at = list.size() * 8;
        starts.push_back(at);
        list.resize(list.size() + (param->size + 7) / 8);
        std::memcpy(reinterpret_cast<std::uint8_t*>(list.data()) + at, param, param->size);
        if (param->id == SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS) {
            const auto& in = *reinterpret_cast<const Ngs2SamplerVoiceWaveformBlocksParam*>(param);
            const auto* ps4 = reinterpret_cast<const Ps4WaveformBlock*>(in.blocks);
            auto& wide = blocks.emplace_back(in.num_blocks);
            for (std::uint32_t i = 0; ps4 && i < in.num_blocks; ++i)
                wide[i] = {ps4[i].data_offset, ps4[i].data_size, ps4[i].num_repeats, ps4[i].num_skip_samples,
                           ps4[i].num_samples, ps4[i].reserved, ps4[i].user_data};
        }
        if (param->next == 0) break;
    }
    if (starts.empty()) return SCE_NGS2_OK;
    auto* base = reinterpret_cast<std::uint8_t*>(list.data());
    for (std::size_t i = 0, wide = 0; i < starts.size(); ++i) {
        auto* param = reinterpret_cast<Ngs2VoiceParamHeader*>(base + starts[i]);
        param->next = static_cast<std::int16_t>(i + 1 < starts.size() ? starts[i + 1] - starts[i] : 0);
        if (param->id == SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS) {
            auto& blocksParam = *reinterpret_cast<Ngs2SamplerVoiceWaveformBlocksParam*>(param);
            if (blocksParam.blocks) blocksParam.blocks = blocks[wide].data();
            ++wide;
        }
        // PS4 events are numbers (play 0, stop, stop now, kill, pause, resume 5); PS5 events are bits.
        if (param->id == SCE_NGS2_VOICE_PARAM_EVENT) {
            auto& event = reinterpret_cast<Ngs2VoiceEventParam*>(param)->event_id;
            if (event > 5) ngs2_stop("NGS2: PS4 voice event " + std::to_string(event));
            event = 1u << event;
        }
    }
    return sceNgs2VoiceControl(voice, reinterpret_cast<const Ngs2VoiceParamHeader*>(base));
}

// PS4 states: 4 bytes (flags only), 16 (submixer), 48 (sampler); no error code field.
int APS5_VABI VoiceGetState(Ngs2Handle voice, void* state, std::size_t size) {
    if (size == sizeof(std::uint32_t)) return sceNgs2VoiceGetStateFlags(voice, static_cast<std::uint32_t*>(state));
    if (size == sizeof(Ps4SamplerVoiceState)) {
        Ngs2SamplerVoiceState wide{};
        const int result = sceNgs2VoiceGetState(voice, &wide.voice_state, sizeof(wide));
        if (result == SCE_NGS2_OK && state)
            *static_cast<Ps4SamplerVoiceState*>(state) = {wide.voice_state.state_flags, wide.envelope_height, wide.peak_height,
                                                          0, wide.num_decoded_samples, wide.decoded_data_size,
                                                          wide.user_data, wide.waveform_data};
        return result;
    }
    if (size == sizeof(Ps4SubmixerVoiceState)) {
        Ngs2SubmixerVoiceState wide{};
        const int result = sceNgs2VoiceGetState(voice, &wide.voice_state, sizeof(wide));
        if (result == SCE_NGS2_OK && state)
            *static_cast<Ps4SubmixerVoiceState*>(state) = {wide.voice_state.state_flags, wide.envelope_height,
                                                           wide.peak_height, wide.compressor_height};
        return result;
    }
    ngs2_stop("NGS2: PS4 voice state size " + std::to_string(size) + " is not translated");
}

struct Export { const char* name; void* function; };
#define SAME(name) {#name, reinterpret_cast<void*>(&name)},
#define PS4(name, function) {#name, reinterpret_cast<void*>(&function)},
const Export exports[] = {
    PS4(sceNgs2SystemCreate, SystemCreate) PS4(sceNgs2SystemCreateWithAllocator, SystemCreateWithAllocator)
    PS4(sceNgs2SystemQueryBufferSize, SystemQueryBufferSize) PS4(sceNgs2RackCreate, RackCreate)
    PS4(sceNgs2RackCreateWithAllocator, RackCreateWithAllocator) PS4(sceNgs2RackQueryBufferSize, RackQueryBufferSize)
    PS4(sceNgs2VoiceControl, VoiceControl) PS4(sceNgs2VoiceGetState, VoiceGetState)
    SAME(sceNgs2VoiceGetStateFlags) SAME(sceNgs2SystemDestroy) SAME(sceNgs2SystemRender) SAME(sceNgs2SystemLock)
    SAME(sceNgs2SystemUnlock) SAME(sceNgs2SystemSetUserData) SAME(sceNgs2SystemGetUserData)
    SAME(sceNgs2SystemSetGrainSamples) SAME(sceNgs2SystemSetSampleRate) SAME(sceNgs2RackDestroy)
    SAME(sceNgs2RackGetVoiceHandle) SAME(sceNgs2RackLock) SAME(sceNgs2RackUnlock) SAME(sceNgs2RackSetUserData)
    SAME(sceNgs2RackGetUserData) SAME(sceNgs2VoiceGetOwner)
};
} // namespace

extern "C" std::uintptr_t runtime_lookup(const void* table, std::size_t count, const char* nid);
extern "C" std::uintptr_t runtime_ngs2_resolve(const char* nid) {
    return runtime_lookup(exports, sizeof(exports) / sizeof(*exports), nid);
}
