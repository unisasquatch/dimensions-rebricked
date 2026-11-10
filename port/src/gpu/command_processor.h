#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "gpu/registers.h"

namespace gpu {

// Walks PM4 command buffers in submission order. Stage 1 performs the memory side effects the
// game synchronises on (labels, EOP/EOS writes, CP DMA) and counts draws; it renders nothing.
class CommandProcessor {
public:
    void Execute(const uint32_t* dwords, size_t count, bool compute);

    struct Stats {
        uint64_t packets = 0, draws = 0, dispatches = 0, labels = 0, eop_interrupts = 0, unknown = 0;
    };
    const Stats& stats() const { return stats_; }
    const Registers& registers() const { return regs_; }
    // Called once per flip, before the next frame's submissions.
    void EndFrame();

private:
    void SetRegisters(uint32_t opcode, const uint32_t* body, uint32_t count);
    void Draw(bool indexed, uint32_t count, uint64_t index_address);

    void WriteLabel(uint64_t addr, uint32_t data_sel, uint32_t lo, uint32_t hi);

    Stats stats_;
    Registers regs_;
    uint64_t frame_ = 0;
    uint32_t frame_draw_ = 0;
    // LDPORT_SYNC_LABELS=1: end-of-pipe labels wait for the Vulkan work before the game sees them.
    bool sync_labels_ = std::getenv("LDPORT_SYNC_LABELS") != nullptr;
    uint32_t index_type_ = 0, instances_ = 1;
    uint64_t index_base_ = 0;
};

CommandProcessor& Gpu();

// Debug remote: draws [from, to) of every frame (numbered as in a capture) are not rendered.
extern std::atomic<uint32_t> g_skip_from, g_skip_to;
void SetDebugDrawSkip(uint32_t from, uint32_t to);

// Gnm event id for graphics end-of-pipe interrupts.
constexpr uint64_t kEopEventId = 0x40;

}  // namespace gpu
