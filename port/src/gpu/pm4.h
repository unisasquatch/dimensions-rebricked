#pragma once
#include <cstdint>

// AMD GCN (Sea Islands) PM4 command packets as used by the PS4's command processor.
namespace pm4 {

constexpr uint32_t Type(uint32_t header) { return header >> 30; }
constexpr uint32_t Opcode(uint32_t header) { return (header >> 8) & 0xFF; }
// Total packet length in dwords, header included (type 3).
constexpr uint32_t Length(uint32_t header) { return ((header >> 16) & 0x3FFF) + 2; }

constexpr uint32_t Type3(uint32_t opcode, uint32_t body_dwords, bool compute = false) {
    return 0xC0000000u | ((body_dwords - 1) << 16) | (opcode << 8) | (compute ? 2u : 0u);
}

enum Op : uint32_t {
    kNop = 0x10,
    kSetBase = 0x11,
    kClearState = 0x12,
    kIndexBufferSize = 0x13,
    kDispatchDirect = 0x15,
    kDispatchIndirect = 0x16,
    kIndexBase = 0x26,
    kDrawIndex2 = 0x27,
    kContextControl = 0x28,
    kIndexType = 0x2A,
    kDrawIndexAuto = 0x2D,
    kNumInstances = 0x2F,
    kIndirectBufferConst = 0x33,
    kDrawIndexOffset2 = 0x35,
    kWriteData = 0x37,
    kWaitRegMem = 0x3C,
    kIndirectBuffer = 0x3F,
    kEventWrite = 0x46,
    kEventWriteEop = 0x47,
    kEventWriteEos = 0x48,
    kReleaseMem = 0x49,
    kDmaData = 0x50,
    kAcquireMem = 0x58,
    kSetConfigReg = 0x68,
    kSetContextReg = 0x69,
    kSetShReg = 0x76,
    kSetUconfigReg = 0x79,
};

// Shader-stage SH register offsets (relative to 0x2C00).
constexpr uint32_t kShPgmLoPs = 0x08, kShPgmLoVs = 0x48, kShPgmLoGs = 0x88, kShPgmLoEs = 0xC8,
                   kShPgmLoHs = 0x108, kShPgmLoLs = 0x148;
constexpr uint32_t kComputeNumThreadX = 0x207, kComputePgmLo = 0x20C, kComputePgmRsrc1 = 0x212;

// Context register offsets (relative to 0xA000).
constexpr uint32_t kCbShaderMask = 0x8F, kSpiVsOutConfig = 0x1B1, kSpiPsInputEna = 0x1B3, kSpiPsInputAddr = 0x1B4,
                   kSpiPsInControl = 0x1B6, kSpiBarycCntl = 0x1B8, kSpiShaderPosFormat = 0x1C3,
                   kSpiShaderZFormat = 0x1C4, kSpiShaderColFormat = 0x1C5, kDbShaderControl = 0x203,
                   kPaClVsOutCntl = 0x207;

// Our own NOP payload tags, for things the port emits that hardware has no packet for.
constexpr uint32_t kTagEmbeddedVs = 0x564D4245;  // "EBMV"
constexpr uint32_t kTagEmbeddedPs = 0x504D4245;  // "EBMP"
constexpr uint32_t kTagInitState = 0x54494E49;   // "INIT"

}  // namespace pm4
