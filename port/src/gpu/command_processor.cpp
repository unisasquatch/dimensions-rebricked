#include "gpu/command_processor.h"

#include "hle/clock.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "gpu/capture.h"
#include "gpu/pm4.h"
#include "gpu/vk/renderer.h"
#include "hle/equeue.h"

namespace gpu {
namespace {

uint64_t Addr(uint32_t lo, uint32_t hi) { return (uint64_t(hi & 0xFFFF) << 32) | lo; }

uint64_t GpuClock() {
    // The PS4 GPU timestamp counter runs at 100 MHz.
    return hle::GuestNowNs() / 10;
}

}  // namespace

void CommandProcessor::WriteLabel(uint64_t addr, uint32_t data_sel, uint32_t lo, uint32_t hi) {
    if (!addr) return;
    switch (data_sel) {
        case 1: *reinterpret_cast<uint32_t*>(addr) = lo; break;
        case 2: *reinterpret_cast<uint64_t*>(addr) = (uint64_t(hi) << 32) | lo; break;
        case 3: case 4: *reinterpret_cast<uint64_t*>(addr) = GpuClock(); break;
        default: return;
    }
    ++stats_.labels;
}

void CommandProcessor::Execute(const uint32_t* dw, size_t count, bool compute) {
    size_t i = 0;
    while (i < count) {
        const uint32_t header = dw[i];
        switch (pm4::Type(header)) {
            case 2: ++i; continue;  // filler
            case 0: i += ((header >> 16) & 0x3FFF) + 2; continue;
            case 3: break;
            default: ++stats_.unknown; ++i; continue;
        }
        const uint32_t len = pm4::Length(header);
        const uint32_t* b = dw + i + 1;  // packet body
        ++stats_.packets;

        switch (pm4::Opcode(header)) {
            case pm4::kIndirectBuffer:
                Execute(reinterpret_cast<const uint32_t*>(Addr(b[0], b[1])), b[2] & 0xFFFFF, compute);
                break;
            case pm4::kEventWriteEop: {
                const uint32_t int_sel = (b[2] >> 24) & 3, data_sel = b[2] >> 29;
                if (sync_labels_) vkr::GetRenderer().Sync();
                WriteLabel(Addr(b[1], b[2]), data_sel, b[3], b[4]);
                if (int_sel) {
                    ++stats_.eop_interrupts;
                    hle::TriggerAll(kEopEventId, hle::kFilterGraphicsCore, int64_t(GpuClock()));
                }
                break;
            }
            case pm4::kReleaseMem: {
                const uint32_t int_sel = (b[1] >> 24) & 7, data_sel = b[1] >> 29;
                if (sync_labels_) vkr::GetRenderer().Sync();
                WriteLabel(Addr(b[2], b[3]), data_sel, b[4], b[5]);
                if (int_sel) {
                    ++stats_.eop_interrupts;
                    hle::TriggerAll(kEopEventId, hle::kFilterGraphicsCore, int64_t(GpuClock()));
                }
                break;
            }
            case pm4::kEventWrite: {
                const uint32_t type = b[0] & 0x3F;
                static uint32_t seen[64];
                if (seen[type]++ == 0 || (type == 0x15 && seen[type] % 20000 == 0))
                    std::fprintf(stderr, "[gnm] EVENT_WRITE type %#x index %u len %u (x%u)%s\n", type, (b[0] >> 8) & 0xF, len,
                                 seen[type], len > 2 ? " with address" : "");
                break;
            }
            case pm4::kEventWriteEos:
                if ((b[2] >> 30) == 2) WriteLabel(Addr(b[1], b[2]), 1, b[3], 0);  // store 32-bit data
                break;
            case pm4::kWriteData: {
                const uint32_t dst_sel = (b[0] >> 8) & 0xF;
                if (dst_sel == 2 || dst_sel == 5) {  // memory (via L2 or synchronously)
                    auto* dst = reinterpret_cast<uint32_t*>(Addr(b[1], b[2]));
                    std::memcpy(dst, b + 3, (len - 4) * 4);
                    ++stats_.labels;
                }
                break;
            }
            case pm4::kDmaData: {
                const uint32_t src_sel = (b[0] >> 29) & 3, dst_sel = (b[0] >> 20) & 3;
                const uint32_t bytes = b[5] & 0x1FFFFF;
                if (dst_sel != 0) break;  // only memory destinations matter here
                auto* dst = reinterpret_cast<uint8_t*>(Addr(b[3], b[4]));
                if (src_sel == 2) {  // fill with a dword
                    for (uint32_t o = 0; o + 4 <= bytes; o += 4) std::memcpy(dst + o, &b[1], 4);
                } else if (src_sel == 0) {
                    std::memmove(dst, reinterpret_cast<const uint8_t*>(Addr(b[1], b[2])), bytes);
                }
                break;
            }
            case pm4::kSetContextReg:
            case pm4::kSetShReg:
            case pm4::kSetUconfigReg:
                SetRegisters(pm4::Opcode(header), b, len - 2);
                break;
            case pm4::kIndexType: index_type_ = b[0] & 3; break;
            case pm4::kNumInstances: instances_ = b[0] ? b[0] : 1; break;
            case pm4::kIndexBase: index_base_ = Addr(b[0], b[1]); break;
            case pm4::kDrawIndex2: Draw(true, b[3], Addr(b[1], b[2])); break;
            case pm4::kDrawIndexOffset2: Draw(true, b[2], index_base_ + uint64_t(b[1]) * (index_type_ ? 4 : 2)); break;
            case pm4::kDrawIndexAuto: Draw(false, b[0], 0); break;
            case pm4::kDispatchDirect:
                ++stats_.dispatches;
                if (Capture::Get().active()) Capture::Get().OnDispatch(regs_, b[0], b[1], b[2]);
                break;
            case pm4::kDispatchIndirect:
                ++stats_.dispatches;
                break;
            default:
                break;  // state packets: recorded once the renderer exists
        }
        i += len;
    }
}

void CommandProcessor::SetRegisters(uint32_t opcode, const uint32_t* body, uint32_t count) {
    const uint32_t first = body[0] & 0xFFFF;
    uint32_t* space = regs_.context.data();
    size_t size = regs_.context.size();
    if (opcode == pm4::kSetShReg) { space = regs_.sh.data(); size = regs_.sh.size(); }
    if (opcode == pm4::kSetUconfigReg) { space = regs_.uconfig.data(); size = regs_.uconfig.size(); }
    for (uint32_t i = 0; i < count && first + i < size; ++i) space[first + i] = body[1 + i];
}

void CommandProcessor::Draw(bool indexed, uint32_t count, uint64_t index_address) {
    ++stats_.draws;
    if (hle::FastForwarding()) return;
    if (Capture::Get().active())
        Capture::Get().OnDraw(regs_, {indexed, count, index_address, index_type_, instances_});
    if (hle::FastForwarding()) return;
    const uint32_t index = frame_draw_++;  // debug remote: /debug/skip drops a range of this frame's draws
    if (index >= g_skip_from.load(std::memory_order_relaxed) && index < g_skip_to.load(std::memory_order_relaxed)) return;
    vkr::GetRenderer().Draw(regs_, {indexed, count, index_address, index_type_, instances_});
}

void CommandProcessor::EndFrame() {
    frame_draw_ = 0;
    Capture::Get().OnFrameStart(++frame_);
}

std::atomic<uint32_t> g_skip_from{~0u}, g_skip_to{0};

void SetDebugDrawSkip(uint32_t from, uint32_t to) {
    g_skip_to = 0;
    g_skip_from = from;
    g_skip_to = to;
}

CommandProcessor& Gpu() {
    static CommandProcessor cp;
    return cp;
}

}  // namespace gpu
