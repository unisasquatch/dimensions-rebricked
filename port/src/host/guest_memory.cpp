#include "host/guest_memory.h"

#include <atomic>
#include <iterator>
#include <map>
#include <mutex>

namespace host {
namespace {

std::mutex g_mutex;
std::map<uint64_t, GuestRange> g_ranges;  // base -> range
uint32_t g_next_id = 1;
std::atomic<uint64_t> g_generation{0};
void (*g_release_hook)(uint64_t) = nullptr;

}  // namespace

void RegisterGuestRange(const void* base, size_t size) {
    std::lock_guard lock(g_mutex);
    const uint64_t b = reinterpret_cast<uint64_t>(base);
    g_ranges[b] = {b, size, g_next_id++};
}

void UnregisterGuestRange(const void* base) {
    std::lock_guard lock(g_mutex);
    g_ranges.erase(reinterpret_cast<uint64_t>(base));
    ++g_generation;
}

uint64_t GuestRangeGeneration() { return g_generation.load(std::memory_order_acquire); }
void SetGuestRangeReleaseHook(void (*hook)(uint64_t)) { g_release_hook = hook; }
void ReleaseGuestRange(const void* base) {
    if (g_release_hook) g_release_hook(reinterpret_cast<uint64_t>(base));
}

bool FindGuestRange(uint64_t address, GuestRange& out) {
    std::lock_guard lock(g_mutex);
    auto it = g_ranges.upper_bound(address);
    if (it == g_ranges.begin()) return false;
    --it;
    if (address >= it->second.base + it->second.size) return false;
    out = it->second;
    return true;
}

}  // namespace host
