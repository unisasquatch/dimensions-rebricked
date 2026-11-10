#pragma once
#include <array>
#include <cstdint>

// GCN register state as set by SET_*_REG packets. Offsets are relative to each space's base:
// SH 0x2C00, context 0xA000, uconfig 0xC000 (dword register indices).
namespace gpu {

struct Registers {
    std::array<uint32_t, 0x400> sh{};
    std::array<uint32_t, 0x400> context{};
    std::array<uint32_t, 0x1000> uconfig{};
};

namespace reg {
// SH: per-stage program address (>> 8) and resources; user SGPR data follows.
constexpr uint32_t kSpiShaderPgmLoPs = 0x08, kSpiShaderPgmRsrc2Ps = 0x0B, kSpiShaderUserDataPs0 = 0x0C;
constexpr uint32_t kSpiShaderPgmLoVs = 0x48, kSpiShaderPgmRsrc2Vs = 0x4B, kSpiShaderUserDataVs0 = 0x4C;
constexpr uint32_t kComputePgmLo = 0x20C, kComputePgmRsrc2 = 0x213, kComputeUserData0 = 0x240;
// Context.
constexpr uint32_t kDbZInfo = 0x10, kDbZReadBase = 0x12, kDbDepthSize = 0x16;
constexpr uint32_t kCbColor0Base = 0x318, kCbColorStride = 0xF;  // BASE, PITCH, SLICE, VIEW, INFO, ATTRIB...
constexpr uint32_t kCbColorPitch = 1, kCbColorSlice = 2, kCbColorView = 3, kCbColorInfo = 4, kCbColorAttrib = 5;
constexpr uint32_t kCbTargetMask = 0x8E, kCbShaderMask = 0x8F;
constexpr uint32_t kPaScScreenScissorBr = 0x0D, kPaClVportXscale = 0x10F;
constexpr uint32_t kSpiPsInputEna = 0x1B3, kSpiShaderColFormat = 0x1C5;
constexpr uint32_t kCbBlend0Control = 0x1E0, kDbDepthControl = 0x200, kCbColorControl = 0x202;
constexpr uint32_t kPaSuScModeCntl = 0x205;
// Uconfig.
constexpr uint32_t kVgtPrimitiveType = 0x242;
}  // namespace reg

// Program address from a PGM_LO/PGM_HI pair (the address is stored shifted right by 8).
inline uint64_t ShaderAddress(uint32_t lo, uint32_t hi) { return (uint64_t(hi & 0xFF) << 40) | (uint64_t(lo) << 8); }
// Number of user SGPRs (RSRC2.USER_SGPR, bits 1-5).
inline uint32_t UserSgprCount(uint32_t rsrc2) { return (rsrc2 >> 1) & 0x1F; }

}  // namespace gpu
