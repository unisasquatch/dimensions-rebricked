#include "hle/clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace hle {
namespace {

using std::chrono::steady_clock;
constexpr uint64_t kFrameNs = 16'666'667;

const steady_clock::time_point g_start = steady_clock::now();
const uint64_t g_fast_until = [] {
    const char* e = std::getenv("LDPORT_FAST");
    return e ? std::strtoull(e, nullptr, 10) : 0ull;
}();
std::atomic<bool> g_fast{g_fast_until > 0};
std::atomic<uint64_t> g_virtual_ns{0};  // while fast: flips * frame
std::atomic<int64_t> g_offset_ns{0};    // after fast: real-time game clock = real + offset

// Default "vsync clock": game time moves in whole 1/60 s frames and only at flips, the way a
// PS4 locked to vsync sees it: at least one frame per flip, more when a flip comes late, so
// time keeps up with real time. A free-running host clock lets the game's fixed-step loop see
// an occasional frame with no update (and none of that update's draws: missing fades and
// effects, shown as single-frame flashes). LDPORT_REALTIME=1 restores the free-running clock;
// LDPORT_FRAMELOCK=1 makes it exactly one frame per flip (slow motion when rendering is slow).
const bool g_realtime = std::getenv("LDPORT_REALTIME") != nullptr;
const bool g_framelock = std::getenv("LDPORT_FRAMELOCK") != nullptr;
std::atomic<uint64_t> g_frames{0};          // vsync clock: game time in frames
std::atomic<uint64_t> g_last_flip_real{0};  // host time of the last flip
constexpr uint64_t kNoFlipCatchUpNs = 250'000'000;
// Uncapped (experiment: LDPORT_NOCAP=1 or /debug/set?nocap=1): no 60 Hz hold on flips, and the game clock
// follows real time (the vsync clock would add a frame per flip and fast-forward the game).
std::atomic<bool> g_nocap{std::getenv("LDPORT_NOCAP") != nullptr};

uint64_t RealNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - g_start).count());
}
// The real-time game clock (host time plus the fast-forward offset), in whole frames.
uint64_t RealFrames() {
    const int64_t t = int64_t(RealNs()) + g_offset_ns.load(std::memory_order_relaxed);
    return t <= 0 ? 0 : uint64_t(std::llround(double(t) / double(kFrameNs)));
}

}  // namespace

uint64_t GuestNowNs() {
    if (g_fast.load(std::memory_order_relaxed) || g_framelock) return g_virtual_ns.load(std::memory_order_relaxed);
    if (g_realtime || g_nocap.load(std::memory_order_relaxed)) return uint64_t(int64_t(RealNs()) + g_offset_ns.load(std::memory_order_relaxed));
    // No flips for a while (loading, a stalled renderer): follow real time so waits still end.
    if (RealNs() - g_last_flip_real.load(std::memory_order_relaxed) > kNoFlipCatchUpNs) {
        uint64_t f = g_frames.load(std::memory_order_relaxed);
        const uint64_t target = RealFrames();
        while (f < target && !g_frames.compare_exchange_weak(f, target)) {
        }
    }
    return g_frames.load(std::memory_order_relaxed) * kFrameNs;
}

int64_t GuestWallOffsetNs() { return int64_t(GuestNowNs()) - int64_t(RealNs()); }

bool FastForwarding() { return g_fast.load(std::memory_order_relaxed); }

// Holds flips to 60 Hz in real time. Presentation doesn't wait for vblank (mailbox), so this is what
// paces the game. A deadline that has fallen well behind (loading, a hitch) restarts from now rather
// than letting a burst of frames catch up.
// Sleeps about `ns` on a high-resolution waitable timer: a plain Sleep / sleep_for lasts until the next tick
// of the system timer (15.6 ms by default), which made flips land 20-32 ms apart.
static void PreciseSleep(uint64_t ns) {
#ifdef _WIN32
    static thread_local HANDLE timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(ns / 100);  // relative, in 100 ns units
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

// Frame pacing stats, logged every 600 flips: flip-to-flip intervals, and how long the game took between
// flips (its own frame, before pacing).
static void RecordPacing(uint64_t interval_ns, uint64_t work_ns) {
    static uint32_t buckets[6] = {};  // <17.5, <20, <25, <34, <50, >=50 ms
    static uint64_t worst_ns = 0, worst_work_ns = 0, sum_work_ns = 0;
    static uint32_t n = 0;
    static const double kEdges[5] = {17.5, 20, 25, 34, 50};
    const double ms = interval_ns / 1e6;
    int b = 0;
    while (b < 5 && ms >= kEdges[b]) ++b;
    ++buckets[b];
    worst_ns = std::max(worst_ns, interval_ns);
    worst_work_ns = std::max(worst_work_ns, work_ns);
    sum_work_ns += work_ns;
    static uint64_t sum_interval_ns = 0;
    sum_interval_ns += interval_ns;
    if (++n == 600) {
        std::fprintf(stderr,
                     "[pacing] 600 flips (%.0f fps): <17.5ms %u, 17.5-20 %u, 20-25 %u, 25-34 %u, 34-50 %u, >=50 %u; "
                     "worst %.1f ms; game work avg %.1f ms, worst %.1f ms\n",
                     600e9 / double(std::max<uint64_t>(sum_interval_ns, 1)), buckets[0], buckets[1], buckets[2],
                     buckets[3], buckets[4], buckets[5], worst_ns / 1e6, sum_work_ns / 600 / 1e6, worst_work_ns / 1e6);
        sum_interval_ns = 0;
        for (uint32_t& x : buckets) x = 0;
        worst_ns = worst_work_ns = sum_work_ns = 0;
        n = 0;
    }
}

void PaceFlip() {
    static uint64_t next = 0, last_done = 0;
#ifdef _WIN32
    static const bool period = timeBeginPeriod(1) == TIMERR_NOERROR;  // finer waits for every thread
    (void)period;
#endif
    uint64_t now = RealNs();
    const uint64_t work = last_done ? now - last_done : 0;  // the game's frame, since the last flip was released
    if (g_nocap.load(std::memory_order_relaxed)) next = now;
    if (next == 0 || now > next + 2 * kFrameNs) next = now;
    if (now + 1'000'000 < next) PreciseSleep(next - now - 1'000'000);  // most of the way, then spin
    now = RealNs();
    while (now < next) {
        std::this_thread::yield();
        now = RealNs();
    }
    if (last_done) RecordPacing(now - last_done, work);
    last_done = now;
    next += kFrameNs;
}

void SetUncapped(bool on) {
    if (on == g_nocap.load()) return;
    if (on) {
        g_offset_ns = int64_t(g_frames.load() * kFrameNs) - int64_t(RealNs());  // carry on from the vsync clock
    } else {
        g_frames = RealFrames();
        g_last_flip_real = RealNs();
    }
    g_nocap = on;
    std::fprintf(stderr, "[clock] uncapped %s\n", on ? "on" : "off");
}

void ClockOnFlip(uint64_t flips) {
    if (!g_fast.load(std::memory_order_relaxed)) {
        PaceFlip();
        if (g_framelock) {
            g_virtual_ns = flips * kFrameNs;
        } else if (!g_realtime && !g_nocap.load(std::memory_order_relaxed)) {
            // At least one frame per flip; catch up to real time when flips are late, but never run
            // more than a frame ahead of it.
            const uint64_t target = RealFrames();
            const uint64_t f = g_frames.load(std::memory_order_relaxed);
            g_frames = std::min(std::max(f + 1, target), target + 1);
            g_last_flip_real = RealNs();
        }
        return;
    }
    // Game time may run at most kMaxSpeed x real time, so work that really takes time (loading
    // threads, timed waits) still finishes at a sensible point in game time.
    constexpr uint64_t kMaxSpeed = 8;
    while (flips * kFrameNs > RealNs() * kMaxSpeed) std::this_thread::sleep_for(std::chrono::microseconds(500));
    g_virtual_ns = flips * kFrameNs;
    if (flips >= g_fast_until) {
        g_offset_ns = int64_t(flips * kFrameNs) - int64_t(RealNs());
        g_frames = flips;
        g_last_flip_real = RealNs();
        g_fast = false;
        std::fprintf(stderr, "[clock] fast-forward done at flip %llu (%.1f s of game time in %.1f s)\n",
                     (unsigned long long)flips, flips * kFrameNs / 1e9, RealNs() / 1e9);
    }
}

}  // namespace hle
