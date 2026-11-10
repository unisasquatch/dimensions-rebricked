// libSceNgs2: Sony's voice mixer. The game decodes and mixes its own sound (music, effects,
// dialogue) and streams the result as 16-bit PCM through a couple of sampler voices; this mixes
// those voices into the system's 48 kHz float output (resampling from the voice rate), and
// reports each voice's position and consumed bytes, which the game uses both to refill its
// stream buffers and to time cutscenes.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hle/hle.h"
#include "hle/port_settings.h"

namespace {

struct ContextBufferInfo {
    void* host_buffer;
    uint64_t host_buffer_size;
    uint64_t reserved[5];
    uint64_t user_data;
};
struct RenderBufferInfo {
    void* buffer;
    uint64_t buffer_size;
    uint32_t waveform_type;
    uint32_t num_channels;
};
struct ParamHeader { uint16_t size; int16_t next; uint32_t id; };

// SceNgs2WaveformBlock. For the game's streams num_samples is the whole stream's length (a stop
// point), not this block's: a block lasts data_size / frame size.
struct WaveformBlock {
    uint32_t data_offset, data_size, num_repeats, num_skip_samples, num_samples, reserved;
    uint64_t user_data;
};
static_assert(sizeof(WaveformBlock) == 32);
struct WaveformBlocksParam {
    ParamHeader header;
    const void* data;
    uint32_t flags;
    uint32_t num_blocks;
    const WaveformBlock* blocks;
};
struct EventParam { ParamHeader header; uint32_t event; };
struct PortVolumeParam { ParamHeader header; uint32_t port; float level; };
struct SamplerVoiceState {
    uint32_t state_flags;
    float envelope_height, peak_height;
    uint32_t reserved;
    uint64_t num_decoded_samples, decoded_data_size, user_data;
    const void* waveform_data;
};
static_assert(sizeof(SamplerVoiceState) == 48);

constexpr uint32_t kParamMatrixLevels = 1, kParamSamplerPitch = 0x10000005, kParamPortVolume = 2, kParamEvent = 6, kParamSamplerSetup = 0x10000000,
                   kParamSamplerAddBlocks = 0x10000001, kParamSamplerReplaceAddress = 0x10000002;
constexpr uint32_t kEventPlay = 0, kEventStop = 1, kEventStopImmediate = 2, kEventKill = 3, kEventPause = 4,
                   kEventResume = 5;
constexpr uint32_t kStateInUse = 0x1, kStatePlaying = 0x2, kStatePaused = 0x4;
constexpr uint32_t kWaveformPcmI16L = 0x12, kWaveformPcmF32L = 0x18, kWaveformVag = 0x1c;
constexpr uint32_t kOutputRate = 48000;

struct Object { uint32_t kind, id; };
constexpr uint32_t kSystem = 1, kRack = 2, kVoice = 3;
constexpr uint64_t kContextSize = 0x1000;

struct Block {
    const uint8_t* data = nullptr;  // first frame
    uint32_t frames = 0;            // frames in this block
    uint32_t bytes = 0;
    uint32_t repeats_left = 0;
    std::shared_ptr<std::vector<int16_t>> pcm;  // VAG blocks: the decoded samples `data` points into
};
struct Voice : Object {
    uint32_t flags = 0;
    uint32_t waveform = kWaveformPcmI16L, channels = 2, sample_rate = 48000;
    float volume = 1.0f;
    uint32_t rack = 0;           // its rack id (0x1000 sampler, 0x2001 submixer, 0x3000 mastering, ...)
    float pitch = 1.0f;          // sampler pitch ratio (param 0x10000005)
    bool has_matrix = false;     // matrix levels (param 1): gain from each input channel to each output channel
    float matrix[8][8] = {};
    uint64_t decoded_samples = 0, decoded_bytes = 0;
    uint64_t stream_limit = 0;  // stop after this many samples (0 = none)
    const void* data = nullptr;
    std::deque<Block> blocks;
    uint32_t frame = 0;  // integer frame within the front block
    double frac = 0;     // fractional position between frame and frame + 1
    int32_t vag_h1[8] = {}, vag_h2[8] = {};  // VAG decoder history per channel (carries across blocks)
};
struct Rack : Object { std::map<uint32_t, Voice*> voices; };

std::mutex g_mutex;
std::map<Voice*, bool> g_voices;  // every live voice, for rendering
std::atomic<uint64_t> g_renders{0};
const bool g_log = std::getenv("LDPORT_NGS2_LOG") != nullptr;
const bool g_mute = std::getenv("LDPORT_MUTE") != nullptr;

// Bytes per frame of the samples played (VAG is decoded to 16-bit PCM when its blocks are added).
uint32_t FrameBytes(const Voice& v) {
    return v.channels * (v.waveform == kWaveformPcmF32L ? 4u : 2u);
}

// Sony's VAG ADPCM (the game's sound effects): 16-byte frames of 28 4-bit samples, byte 0 = predictor << 4 |
// shift, byte 1 = flags. Multichannel data interleaves one frame per channel. Before this was decoded, VAG data
// was played as 16-bit PCM: noise, cut off at 8/28 of the sound's length.
std::shared_ptr<std::vector<int16_t>> DecodeVag(Voice& v, const uint8_t* src, uint32_t bytes, uint32_t& frames) {
    static constexpr int kCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    const uint32_t ch = v.channels;
    const uint32_t groups = bytes / (16 * ch);
    frames = groups * 28;
    auto pcm = std::make_shared<std::vector<int16_t>>(size_t(frames) * ch);
    for (uint32_t g = 0; g < groups; ++g)
        for (uint32_t c = 0; c < ch; ++c) {
            const uint8_t* f = src + (size_t(g) * ch + c) * 16;
            int shift = f[0] & 0xF;
            if (shift > 12) shift = 9;
            const int pred = std::min(f[0] >> 4, 4);
            int32_t h1 = v.vag_h1[c], h2 = v.vag_h2[c];
            for (int i = 0; i < 28; ++i) {
                const uint8_t byte = f[2 + i / 2];
                const int nibble = (i & 1) ? (byte >> 4) : (byte & 0xF);
                const int32_t raw = int32_t(int16_t(uint16_t(nibble << 12))) >> shift;
                int32_t s = raw + ((h1 * kCoef[pred][0] + h2 * kCoef[pred][1] + 32) >> 6);
                s = std::clamp(s, -32768, 32767);
                h2 = h1;
                h1 = s;
                (*pcm)[(size_t(g) * 28 + i) * ch + c] = int16_t(s);
            }
            v.vag_h1[c] = h1;
            v.vag_h2[c] = h2;
        }
    return pcm;
}

float SampleAt(const Voice& v, const uint8_t* frame, uint32_t c) {
    if (v.waveform == kWaveformPcmF32L) return reinterpret_cast<const float*>(frame)[c];
    return float(reinterpret_cast<const int16_t*>(frame)[c]) * (1.0f / 32768.0f);
}

// The frame `ahead` frames after the current one, crossing into later blocks; null past the end.
const uint8_t* FrameAhead(const Voice& v, uint32_t ahead) {
    uint64_t index = uint64_t(v.frame) + ahead;
    for (const Block& b : v.blocks) {
        if (index < b.frames) return b.data + index * FrameBytes(v);
        index -= b.frames;
    }
    return nullptr;
}

// Consumes `n` frames from the front of the queue, updating the reported position.
void Consume(Voice& v, uint32_t n) {
    while (n && !v.blocks.empty()) {
        Block& b = v.blocks.front();
        const uint32_t take = std::min(n, b.frames - v.frame);
        v.frame += take;
        n -= take;
        v.decoded_samples += take;
        // The game tracks its data by source bytes: for VAG, 16 per 28 samples per channel.
        v.decoded_bytes = v.waveform == kWaveformVag ? v.decoded_samples / 28 * 16 * v.channels
                                                     : v.decoded_bytes + uint64_t(take) * FrameBytes(v);
        if (v.frame >= b.frames) {
            v.frame = 0;
            if (b.repeats_left) {
                --b.repeats_left;
            } else {
                v.blocks.pop_front();
            }
        }
    }
    const bool limit = v.stream_limit && v.decoded_samples >= v.stream_limit;
    if (limit) v.blocks.clear();
    if (v.blocks.empty()) {
        // Ran off the end of its data: the voice stops. Before its stream limit, the game didn't refill in time
        // (a gap in the sound).
        static std::atomic<int> logged{0};
        if (!limit && v.stream_limit && (v.flags & kStatePlaying) && logged.fetch_add(1) < 50)
            std::fprintf(stderr, "[ngs2] voice #%u ran dry at sample %llu of %llu\n", v.id,
                         (unsigned long long)v.decoded_samples, (unsigned long long)v.stream_limit);
        v.flags = 0;
    }
}

// LDPORT_NGS2_METER=1: per output channel, the energy of the streams (waveform 0x12: music, dialogue) and of the
// VAG voices (effects) after their matrices, logged every ~4 s, to compare the mix with what the game asks for.
const bool g_meter = std::getenv("LDPORT_NGS2_METER") != nullptr;
double g_energy[2][8] = {}, g_peak_class[2] = {};
std::vector<float> g_scratch;

// Mixes `frames` output frames of a voice into `out` (interleaved float, out_channels wide),
// resampling with linear interpolation, and consumes what it used.
void MixVoiceInto(Voice& v, float* out, uint32_t frames, uint32_t out_channels);
void MixVoice(Voice& v, float* out, uint32_t frames, uint32_t out_channels) {
    if (!g_meter || !out) return MixVoiceInto(v, out, frames, out_channels);
    g_scratch.assign(size_t(frames) * out_channels, 0.0f);
    MixVoiceInto(v, g_scratch.data(), frames, out_channels);
    const int cls = v.waveform == kWaveformVag ? 1 : 0;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t o = 0; o < out_channels && o < 8; ++o) {
            const float x = g_scratch[i * out_channels + o];
            out[i * out_channels + o] += x;
            g_energy[cls][o] += double(x) * x;
            g_peak_class[cls] = std::max(g_peak_class[cls], double(std::fabs(x)));
        }
}
void MixVoiceInto(Voice& v, float* out, uint32_t frames, uint32_t out_channels) {
    // The player's balance (settings.ini streams_volume / effects_volume): streams are the music and dialogue
    // (16-bit PCM), effects the VAG sounds.
    const uint32_t pct = v.waveform == kWaveformVag ? port::Get().effects_volume.load(std::memory_order_relaxed)
                                                    : port::Get().streams_volume.load(std::memory_order_relaxed);
    const float vol = v.volume * float(std::min(pct, 400u)) * 0.01f;
    const double step = double(v.sample_rate) / kOutputRate * v.pitch;
    const uint32_t channels = std::min(v.channels, out_channels);
    const uint32_t outs = std::min(out_channels, 8u);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* a = FrameAhead(v, 0);
        if (!a) break;
        const uint8_t* b = FrameAhead(v, 1);
        const float t = float(v.frac);
        if (out && !g_mute && v.has_matrix) {  // each input channel spread over the outputs by the voice's matrix
            for (uint32_t c = 0; c < v.channels && c < 8; ++c) {
                const float s0 = SampleAt(v, a, c), s1 = b ? SampleAt(v, b, c) : s0;
                const float x = (s0 + (s1 - s0) * t) * vol;
                for (uint32_t o = 0; o < outs; ++o) out[i * out_channels + o] += x * v.matrix[c][o];
            }
        } else if (out && !g_mute) {
            for (uint32_t c = 0; c < channels; ++c) {
                const float s0 = SampleAt(v, a, c), s1 = b ? SampleAt(v, b, c) : s0;
                out[i * out_channels + c] += (s0 + (s1 - s0) * t) * vol;
            }
        }
        v.frac += step;
        const uint32_t whole = uint32_t(v.frac);
        v.frac -= whole;
        if (whole) Consume(v, whole);
        if (!(v.flags & kStatePlaying)) break;
    }
}

SYSV int SystemQueryBufferSize(const void*, ContextBufferInfo* info) {
    info->host_buffer_size = kContextSize;
    return 0;
}
SYSV int SystemCreate(const void*, const ContextBufferInfo*, Object** out) {
    *out = new Object{kSystem, 0};
    return 0;
}

SYSV int SystemRender(Object*, const RenderBufferInfo* buffers, uint32_t count) {
    // The game renders one float buffer per call (8 channels, 256 frames at 48 kHz).
    float* out = nullptr;
    uint32_t out_channels = 2, frames = 256;
    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i].buffer) continue;
        std::memset(buffers[i].buffer, 0, buffers[i].buffer_size);
        if (!out && buffers[i].waveform_type == kWaveformPcmF32L && buffers[i].num_channels) {
            out = static_cast<float*>(buffers[i].buffer);
            out_channels = buffers[i].num_channels;
            frames = uint32_t(buffers[i].buffer_size / (out_channels * 4ull));
        }
    }
    std::lock_guard lock(g_mutex);
    for (auto& [v, _] : g_voices)
        if ((v->flags & kStatePlaying) && !(v->flags & kStatePaused)) MixVoice(*v, out, frames, out_channels);
    static float peak = 0;
    // The voices' matrices keep the mix well below full scale (music at 0.15, effects below 0.1); the
    // PS4's output stage isn't modelled, so a fixed makeup gain brings it back to a normal level, and a soft
    // limiter keeps peaks under full scale instead of clipping them (clipped peaks crackle). LDPORT_AUDIO_GAIN.
    static const float gain = [] {
        const char* e = std::getenv("LDPORT_AUDIO_GAIN");
        const float g = e ? float(std::atof(e)) : 0.0f;
        return g > 0.0f && g <= 32.0f ? g : 5.0f;
    }();
    if (out)
        for (uint32_t i = 0; i < frames * out_channels; ++i) {
            float x = out[i] * gain;
            const float ax = std::fabs(x);
            if (ax > 0.8f) x = std::copysign(0.8f + 0.2f * std::tanh((ax - 0.8f) / 0.2f), x);
            out[i] = x;
        }
    if (out)  // keep the sum in range
        for (uint32_t i = 0; i < frames * out_channels; ++i) {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
            peak = std::max(peak, std::fabs(out[i]));
        }
    if (g_meter && g_renders % 750 == 749) {  // RMS per channel over the last ~4 s (750 x 256 frames)
        const double n = 750.0 * frames;
        for (int cls = 0; cls < 2; ++cls) {
            std::fprintf(stderr, "[ngs2meter] %s peak %.3f rms:", cls ? "vag   " : "stream", g_peak_class[cls]);
            for (int o = 0; o < 8; ++o) std::fprintf(stderr, " %.4f", std::sqrt(g_energy[cls][o] / n));
            std::fprintf(stderr, "\n");
            std::fill(std::begin(g_energy[cls]), std::end(g_energy[cls]), 0.0);
            g_peak_class[cls] = 0;
        }
    }
    if (++g_renders % 750 == 0 && g_log) {  // ~every 4 s: what each live voice is doing
        std::fprintf(stderr, "[ngs2] peak %.3f, voices:", peak);
        peak = 0;
        for (auto& [v, _] : g_voices)
            if (v->flags || !v->blocks.empty())
                std::fprintf(stderr, " #%u flags %x pos %llu blocks %zu;", v->id, v->flags,
                             (unsigned long long)v->decoded_samples, v->blocks.size());
        std::fprintf(stderr, "\n");
    }
    return 0;
}
SYSV int RackQueryBufferSize(uint32_t, const void*, ContextBufferInfo* info) {
    info->host_buffer_size = kContextSize;
    return 0;
}
SYSV int RackCreate(Object*, uint32_t rack_id, const void*, const void*, Object** out) {
    auto* r = new Rack;
    r->kind = kRack;
    r->id = rack_id;
    *out = r;
    return 0;
}
SYSV int RackDestroy(Object* rack, ContextBufferInfo*) {
    std::lock_guard lock(g_mutex);
    auto* r = static_cast<Rack*>(rack);
    for (auto& [i, v] : r->voices) {
        g_voices.erase(v);
        delete v;
    }
    delete r;
    return 0;
}
SYSV int RackGetVoiceHandle(Object* rack, uint32_t index, Object** out) {
    std::lock_guard lock(g_mutex);
    auto* r = static_cast<Rack*>(rack);
    Voice*& v = r->voices[index];
    if (!v) {
        v = new Voice;
        v->kind = kVoice;
        v->id = index;
        v->rack = r->id;
        g_voices[v] = true;
    }
    *out = v;
    return 0;
}

void LogParam(const Voice* v, const ParamHeader* h) {
    {  // LDPORT_NGS2_PARAMS=1: each (rack, param id, waveform) triple's first few, with bytes; patches and every
       // non-sampler rack's params more often
        static const bool all = std::getenv("LDPORT_NGS2_PARAMS") != nullptr;
        static std::map<uint64_t, int> seen;
        const int limit = (h->id == 5 || v->rack != 0x1000) ? 40 : 3;
        if (all && seen[(uint64_t(v->rack) << 48) ^ (uint64_t(h->id) << 16) ^ v->waveform]++ < limit) {
            if (h->id == 5) {  // patch: {header, port, dest port?, dest voice handle}
                const auto* q = reinterpret_cast<const uint32_t*>(h);
                Voice* dest = *reinterpret_cast<Voice* const*>(reinterpret_cast<const uint8_t*>(h) + 16);
                const bool known = dest && g_voices.count(dest);
                std::fprintf(stderr, "[ngs2patch] rack %#x voice %u (wf %#x, %p) port %u/%u -> %s rack %#x voice %u\n",
                             v->rack, v->id, v->waveform, static_cast<const void*>(v), q[2], q[3],
                             known ? "" : "UNKNOWN", known ? dest->rack : 0u, known ? dest->id : 0u);
            }
            std::fprintf(stderr, "[ngs2p] rack %#x ", v->rack);
            const auto* p = reinterpret_cast<const uint8_t*>(h);
            std::fprintf(stderr, "voice %u (wf %#x) param %#x size %u:", v->id, v->waveform, h->id, h->size);
            for (int i = 0; i < h->size && i < 96; ++i) std::fprintf(stderr, " %02x", p[i]);
            std::fprintf(stderr, "\n");
        }
    }
    static int logged = 0;
    if (!g_log || (h->id != kParamEvent && h->id != kParamSamplerSetup) || logged++ > 60) return;
    const auto* p = reinterpret_cast<const uint8_t*>(h);
    std::fprintf(stderr, "[ngs2] voice %u param %#x size %u:", v->id, h->id, h->size);
    for (int i = 0; i < h->size && i < 48; ++i) std::fprintf(stderr, " %02x", p[i]);
    std::fprintf(stderr, "\n");
}

SYSV int VoiceControl(Object* obj, const void* params) {
    if (!obj || obj->kind != kVoice) return 0;
    auto* v = static_cast<Voice*>(obj);
    std::lock_guard lock(g_mutex);
    for (auto p = static_cast<const uint8_t*>(params); p;) {
        const auto* h = reinterpret_cast<const ParamHeader*>(p);
        LogParam(v, h);
        switch (h->id) {
            case kParamSamplerSetup: {
                // SceNgs2SamplerVoiceSetupParam: waveform format {type, channels, rate, ...} at +8.
                const auto* f = reinterpret_cast<const uint32_t*>(p + 8);
                if (f[0]) v->waveform = f[0];
                {
                    static std::set<uint32_t> seen;  // each waveform type once
                    if (seen.insert(v->waveform).second)
                        std::fprintf(stderr, "[ngs2] waveform type %#x (%u ch, %u Hz)%s\n", v->waveform, f[1], f[2],
                                     v->waveform == kWaveformPcmI16L || v->waveform == kWaveformPcmF32L ||
                                             v->waveform == kWaveformVag
                                         ? ""
                                         : "  <-- not supported: played as 16-bit PCM");
                }
                std::fill(std::begin(v->vag_h1), std::end(v->vag_h1), 0);
                std::fill(std::begin(v->vag_h2), std::end(v->vag_h2), 0);
                if (f[1] >= 1 && f[1] <= 8) v->channels = f[1];
                if (f[2] >= 8000 && f[2] <= 192000) v->sample_rate = f[2];
                v->blocks.clear();
                v->flags = 0;
                v->frame = 0;
                v->frac = 0;
                v->decoded_samples = v->decoded_bytes = 0;
                v->stream_limit = 0;
                v->pitch = 1.0f;
                break;
            }
            case kParamSamplerAddBlocks: {
                const auto* w = reinterpret_cast<const WaveformBlocksParam*>(h);
                if (w->data) v->data = w->data;
                const uint32_t frame_bytes = FrameBytes(*v);
                for (uint32_t i = 0; i < w->num_blocks && w->blocks && w->data; ++i) {
                    const WaveformBlock& wb = w->blocks[i];
                    Block b;
                    b.data = static_cast<const uint8_t*>(w->data) + wb.data_offset;
                    b.bytes = wb.data_size;
                    b.frames = frame_bytes ? wb.data_size / frame_bytes : 0;
                    if (v->waveform == kWaveformVag) {
                        b.pcm = DecodeVag(*v, b.data, wb.data_size, b.frames);
                        b.data = reinterpret_cast<const uint8_t*>(b.pcm->data());
                    }
                    b.repeats_left = wb.num_repeats;
                    if (wb.num_samples > b.frames) v->stream_limit = wb.num_samples;  // whole-stream length
                    else if (wb.num_samples) b.frames = wb.num_samples;
                    if (b.frames) v->blocks.push_back(b);
                }
                break;
            }
            case kParamSamplerReplaceAddress: break;
            case kParamMatrixLevels: {
                // SceNgs2VoiceMatrixLevelsParam {header, matrix id, number of levels, levels}: levels are
                // output-major (outputs x channels: a stereo voice is FL{L,R} FR{L,R} ...). Matrix 0 is the one port 0
                // uses (param 3 selects it). These carry the game's volumes: music fades, 3D panning of effects.
                struct M { ParamHeader header; uint32_t id, count; const float* levels; };
                const auto* m = reinterpret_cast<const M*>(h);
                const uint32_t in = std::max(v->channels, 1u);
                if (m->id == 0 && m->levels && m->count >= in && m->count % in == 0 && m->count / in <= 8) {
                    const uint32_t outs = m->count / in;
                    bool ok = true;
                    for (uint32_t k = 0; k < m->count; ++k) ok = ok && std::isfinite(m->levels[k]);
                    if (std::getenv("LDPORT_NGS2_PARAMS")) {  // each distinct matrix once
                        static std::set<std::string> seen;
                        char line[2048];
                        int n = std::snprintf(line, sizeof line, "[ngs2m] rack %#x voice %u wf %#x %u ch, %u levels:",
                                              v->rack, v->id, v->waveform, v->channels, m->count);
                        for (uint32_t k = 0; k < m->count && n < 1900; ++k)
                            n += std::snprintf(line + n, sizeof line - n, " %.3f", m->levels[k]);
                        if (seen.size() < 80 && seen.insert(line).second) std::fprintf(stderr, "%s\n", line);
                    }
                    if (ok) {
                        for (auto& row : v->matrix) std::fill(std::begin(row), std::end(row), 0.0f);
                        for (uint32_t c = 0; c < in && c < 8; ++c)
                            for (uint32_t o = 0; o < outs; ++o) v->matrix[c][o] = std::clamp(m->levels[o * in + c], 0.0f, 4.0f);
                        v->has_matrix = true;
                    }
                }
                break;
            }
            case kParamSamplerPitch: {
                const float r = *reinterpret_cast<const float*>(p + 8);
                if (std::isfinite(r) && r > 0.01f && r < 16.0f) v->pitch = r;
                break;
            }
            case kParamPortVolume: {
                const auto* pv = reinterpret_cast<const PortVolumeParam*>(h);
                if (pv->port == 0 && std::isfinite(pv->level)) v->volume = std::clamp(pv->level, 0.0f, 4.0f);
                break;
            }
            case kParamEvent: {
                const uint32_t e = reinterpret_cast<const EventParam*>(h)->event;
                if (e == kEventPlay) {
                    v->flags = kStateInUse | kStatePlaying;
                    v->decoded_samples = v->decoded_bytes = 0;
                    v->frac = 0;
                } else if (e == kEventPause) {
                    v->flags |= kStatePaused;
                } else if (e == kEventResume) {
                    v->flags &= ~kStatePaused;
                } else if (e == kEventStop || e == kEventStopImmediate || e == kEventKill) {
                    v->flags = 0;
                    v->blocks.clear();
                    v->frame = 0;
                }
                break;
            }
            default: break;
        }
        if (h->next <= 0) break;
        p += h->next;
    }
    return 0;
}

SYSV int VoiceGetState(Object* obj, void* state, uint64_t size) {
    std::memset(state, 0, size);
    if (!obj || obj->kind != kVoice) return 0;
    auto* v = static_cast<Voice*>(obj);
    std::lock_guard lock(g_mutex);
    SamplerVoiceState s{};
    s.state_flags = v->flags;
    s.num_decoded_samples = v->decoded_samples;
    s.decoded_data_size = v->decoded_bytes;
    s.waveform_data = v->data;
    std::memcpy(state, &s, std::min<uint64_t>(size, sizeof s));
    return 0;
}

}  // namespace

#define NGS2(name, fn) HLE_EXPORT("libSceNgs2", name, reinterpret_cast<const void*>(&fn))
NGS2("sceNgs2SystemQueryBufferSize", SystemQueryBufferSize);
NGS2("sceNgs2SystemCreate", SystemCreate);
NGS2("sceNgs2SystemRender", SystemRender);
NGS2("sceNgs2RackQueryBufferSize", RackQueryBufferSize);
NGS2("sceNgs2RackCreate", RackCreate);
NGS2("sceNgs2RackCreateWithAllocator", RackCreate);
NGS2("sceNgs2RackDestroy", RackDestroy);
NGS2("sceNgs2RackGetVoiceHandle", RackGetVoiceHandle);
NGS2("sceNgs2VoiceControl", VoiceControl);
NGS2("sceNgs2VoiceGetState", VoiceGetState);
