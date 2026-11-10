// libSceAudioOut: PCM output ports on SDL audio streams. sceAudioOutOutput blocks the way the
// PS4 does, so the game's mixer thread is paced by playback.
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "hle/clock.h"
#include "hle/hle.h"

namespace {

constexpr int kAudioOutErrorInvalidPort = int(0x80260004);
constexpr int kAudioOutErrorInvalidFormat = int(0x8026000A);

struct Port {
    SDL_AudioStream* stream = nullptr;
    uint32_t frames = 0;
    int bytes_per_frame = 0;
    uint32_t freq = 48000;
    uint64_t due_ns = 0;  // fast-forward pacing: game time the next buffer is due
};
std::mutex g_mutex;
std::vector<Port> g_ports;  // handle = index + 1

// How much audio to keep queued ahead of the device. Two 256-frame buffers (~11 ms) ran dry whenever the
// mixer thread was a few ms late (Windows timer sleeps alone overshoot by 1-15 ms), and each gap is a click;
// steady gaps sound like static. ~50 ms is still below what reads as lag. LDPORT_AUDIO_MS overrides.
uint32_t QueueTargetMs() {
    static const uint32_t ms = [] {
        const char* e = std::getenv("LDPORT_AUDIO_MS");
        const int v = e ? std::atoi(e) : 0;
        return uint32_t(v >= 10 && v <= 500 ? v : 50);
    }();
    return ms;
}
std::atomic<uint32_t> g_underruns{0}, g_outputs{0};

SYSV int Init() { return 0; }

SYSV int Open(int /*user*/, int /*type*/, int /*index*/, uint32_t frames, uint32_t freq, uint32_t format) {
    // Formats 0-2: s16 mono/stereo/8ch; 3-5: float mono/stereo/8ch; 6/7: "standard" 8ch s16/float.
    static constexpr int kChannels[] = {1, 2, 8, 1, 2, 8, 8, 8};
    const uint32_t f = format & 0xFF;
    std::fprintf(stderr, "[audio] open: %u frames, %u Hz, format %#x\n", frames, freq, format);
    if (f >= 8) return kAudioOutErrorInvalidFormat;
    const bool is_float = f >= 3 && f != 6;
    const SDL_AudioSpec spec{is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, kChannels[f], int(freq)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        std::fprintf(stderr, "[audio] open failed: %s\n", SDL_GetError());
        return kAudioOutErrorInvalidPort;
    }
    SDL_ResumeAudioStreamDevice(stream);
    std::lock_guard lock(g_mutex);
    g_ports.push_back({stream, frames, kChannels[f] * (is_float ? 4 : 2), freq, 0});
    return int(g_ports.size());
}

SYSV int Output(int handle, const void* data) {
    Port port;
    {
        std::lock_guard lock(g_mutex);
        if (handle < 1 || handle > int(g_ports.size())) return kAudioOutErrorInvalidPort;
        port = g_ports[handle - 1];
    }
    const int bytes = int(port.frames) * port.bytes_per_frame;
    if (hle::FastForwarding()) {
        // Pace by the game clock (which steps per frame while fast-forwarding), not by playback,
        // so audio time and game time stay in step. Nothing is played.
        uint64_t due;
        {
            std::lock_guard lock(g_mutex);
            Port& p = g_ports[handle - 1];
            if (p.due_ns == 0) p.due_ns = hle::GuestNowNs();
            p.due_ns += uint64_t(port.frames) * 1000000000ull / (port.freq ? port.freq : 48000);
            due = p.due_ns;
        }
        while (hle::FastForwarding() && hle::GuestNowNs() < due) std::this_thread::yield();
        return 0;
    }
    const int target = int(uint64_t(port.freq ? port.freq : 48000) * QueueTargetMs() / 1000) * port.bytes_per_frame;
    // An empty queue here (after the first buffers) means the device already played silence: a click.
    const uint32_t outputs = g_outputs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (outputs > 64 && SDL_GetAudioStreamQueued(port.stream) == 0) g_underruns.fetch_add(1, std::memory_order_relaxed);
    if (outputs % 9375 == 0) {  // ~every 50 s of 256-frame buffers
        const uint32_t u = g_underruns.exchange(0);
        std::fprintf(stderr, "[audio] %u underruns in the last %u buffers (queue %u ms)\n", u, 9375u, QueueTargetMs());
    }
    if (data) SDL_PutAudioStreamData(port.stream, data, bytes);
    // LDPORT_AUDIO_DUMP=<file>: the raw output (this port's format, as opened) appended to a file, to look for clicks.
    static FILE* dump = [] { const char* p = std::getenv("LDPORT_AUDIO_DUMP"); return p ? std::fopen(p, "wb") : nullptr; }();
    if (dump && data) std::fwrite(data, 1, size_t(bytes), dump);
    // Block like the PS4 does once the queue is full; the sleep is short so the queue never runs low.
    while (SDL_GetAudioStreamQueued(port.stream) > target) SDL_DelayPrecise(500000);
    return 0;
}

}  // namespace

HLE_EXPORT("libSceAudioOut", "sceAudioOutInit", reinterpret_cast<const void*>(&Init));
HLE_EXPORT("libSceAudioOut", "sceAudioOutOpen", reinterpret_cast<const void*>(&Open));
HLE_EXPORT("libSceAudioOut", "sceAudioOutOutput", reinterpret_cast<const void*>(&Output));
