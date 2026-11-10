#include "hle/equeue.h"

#include <chrono>
#include <cstdio>

#include "hle/hle.h"
#include "hle/sce_error.h"

namespace hle {
namespace {

std::mutex g_queues_mutex;
std::vector<EventQueue*> g_queues;

}  // namespace

void EventQueue::Add(uint64_t ident, int16_t filter, void* udata) {
    std::lock_guard lock(mutex_);
    entries_.push_back({{ident, filter, 0, 0, 0, udata}, false});
}

void EventQueue::Remove(uint64_t ident, int16_t filter) {
    std::lock_guard lock(mutex_);
    std::erase_if(entries_, [&](const Entry& e) { return e.event.ident == ident && e.event.filter == filter; });
}

void EventQueue::Trigger(uint64_t ident, int16_t filter, int64_t data) {
    bool any = false;
    {
        std::lock_guard lock(mutex_);
        for (auto& e : entries_) {
            if (e.event.filter != filter || e.event.ident != ident) continue;
            e.event.data = data;
            e.triggered = true;
            any = true;
        }
    }
    if (any) cv_.notify_all();
}

int EventQueue::Wait(KernelEvent* out, int max, int64_t timeout_us) {
    std::unique_lock lock(mutex_);
    auto ready = [&] {
        for (const auto& e : entries_) if (e.triggered) return true;
        return false;
    };
    if (timeout_us < 0) {
        if (!cv_.wait_for(lock, std::chrono::seconds(3), ready)) {
            std::fprintf(stderr, "[wait] blocked >3s on event queue %p\n", static_cast<void*>(this));
            cv_.wait(lock, ready);
        }
    }
    else if (!cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready)) return 0;

    int n = 0;
    for (auto& e : entries_) {
        if (!e.triggered || n >= max) continue;
        out[n++] = e.event;
        e.triggered = false;  // events are edge-triggered (EV_CLEAR)
    }
    return n;
}

void TriggerAll(uint64_t ident, int16_t filter, int64_t data) {
    std::lock_guard lock(g_queues_mutex);
    for (EventQueue* q : g_queues) q->Trigger(ident, filter, data);
}

namespace {

SYSV int CreateEqueue(EventQueue** out, const char*) {
    *out = new EventQueue;
    std::lock_guard lock(g_queues_mutex);
    g_queues.push_back(*out);
    return 0;
}
SYSV int WaitEqueue(EventQueue* q, KernelEvent* events, int max, int* out, const uint32_t* timeout_us) {
    const int n = q->Wait(events, max, timeout_us ? int64_t(*timeout_us) : -1);
    if (out) *out = n;
    return n ? 0 : SceError(ETIMEDOUT_BSD);
}

}  // namespace

HLE_EXPORT("libkernel", "sceKernelCreateEqueue", reinterpret_cast<const void*>(&CreateEqueue));
HLE_EXPORT("libkernel", "sceKernelWaitEqueue", reinterpret_cast<const void*>(&WaitEqueue));

}  // namespace hle
