#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hle {

// PS4 kernel event queues (FreeBSD kqueue style). Video flips and GPU end-of-pipe
// interrupts are delivered through these.
constexpr int16_t kFilterVideoOut = -13;
constexpr int16_t kFilterGraphicsCore = -14;

struct KernelEvent {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    void* udata;
};
static_assert(sizeof(KernelEvent) == 32);

class EventQueue {
public:
    void Add(uint64_t ident, int16_t filter, void* udata);
    void Remove(uint64_t ident, int16_t filter);
    // Marks matching registered events as triggered and wakes waiters.
    void Trigger(uint64_t ident, int16_t filter, int64_t data);
    // Returns how many events were copied out (0 on timeout). timeout_us < 0 waits forever.
    int Wait(KernelEvent* out, int max, int64_t timeout_us);

private:
    struct Entry { KernelEvent event; bool triggered; };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
};

// Every queue the game created, so producers (VideoOut, Gnm) can trigger by filter.
void TriggerAll(uint64_t ident, int16_t filter, int64_t data);

}  // namespace hle
