#pragma once
#include <cstddef>
#include <cstdint>

namespace host {

// Host allocations that back guest memory the GPU may read (direct-memory mappings, the game
// image). The renderer imports these into Vulkan so shaders can read guest memory in place.
struct GuestRange {
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t id = 0;  // stable index, for caches keyed by range
};

void RegisterGuestRange(const void* base, size_t size);
void UnregisterGuestRange(const void* base);
// The registered range containing [address, address + 1), if any.
bool FindGuestRange(uint64_t address, GuestRange& out);
// Bumped whenever a range goes away: caches of ranges must be dropped when it changes.
uint64_t GuestRangeGeneration();
// Called by Unmap before the memory is freed (the renderer: finish GPU work that may read it and
// drop its Vulkan import). Set once at startup.
void SetGuestRangeReleaseHook(void (*hook)(uint64_t base));
void ReleaseGuestRange(const void* base);

}  // namespace host
