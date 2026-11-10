#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Decoder for AMD GCN "Sea Islands" (gfx7) shader code, the PS4 GPU's instruction set.
namespace gcn {

enum class Fmt : uint8_t { SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP2, VOP1, VOPC, VOP3, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP, FLAT, Unknown };

// 9-bit source operand codes.
namespace src {
constexpr uint16_t kSgprMax = 103, kVccLo = 106, kVccHi = 107, kM0 = 124, kExecLo = 126, kExecHi = 127;
constexpr uint16_t kZero = 128, kIntPosLast = 192, kIntNegFirst = 193, kIntNegLast = 208;
constexpr uint16_t kFloatHalf = 240, kFloatNegHalf = 241, kFloatOne = 242, kFloatNegOne = 243, kFloatTwo = 244,
                   kFloatNegTwo = 245, kFloatFour = 246, kFloatNegFour = 247;
constexpr uint16_t kVccz = 251, kExecz = 252, kScc = 253, kLiteral = 255, kVgpr0 = 256;
}  // namespace src

struct Inst {
    Fmt fmt = Fmt::Unknown;
    uint16_t op = 0;
    uint32_t pc = 0;    // dword offset of the instruction
    uint32_t size = 1;  // dwords, literal included
    uint32_t literal = 0;

    // Common operand slots (9-bit source codes for sources; destinations as noted per format).
    uint16_t dst = 0;   // SGPR (scalar formats, VOPC/VOP3 compare) or VGPR index (vector formats)
    uint16_t sdst = 0;  // VOP3b carry-out SGPR
    uint16_t src0 = 0, src1 = 0, src2 = 0;
    int32_t simm16 = 0;  // SOPP/SOPK immediate (sign-extended)

    // VOP3 modifiers.
    uint8_t abs = 0, neg = 0, omod = 0;
    bool clamp = false;

    // SMRD.
    uint16_t sbase = 0;  // first SGPR of the base pointer pair / buffer descriptor
    bool smrd_imm = false;
    uint32_t smrd_offset = 0;  // dwords if imm, else SGPR index

    // MUBUF/MTBUF/MIMG.
    uint16_t vaddr = 0, vdata = 0, srsrc = 0, ssamp = 0, soffset = 0;
    uint32_t offset = 0;
    bool idxen = false, offen = false, glc = false, unorm = false, da = false;
    uint8_t dmask = 0, dfmt = 0, nfmt = 0;

    // VINTRP.
    uint8_t attr = 0, chan = 0;

    // EXP.
    uint8_t target = 0, en = 0;
    bool compr = false, done = false, vm = false;
    uint16_t vsrc[4] = {};
};

// Decodes until s_endpgm (or `max_dwords`); fetch shaders end at s_setpc_b64 instead.
std::vector<Inst> Decode(const uint32_t* code, uint32_t max_dwords, bool stop_at_setpc = false);

// Human-readable form, for logs and the capture tool.
std::string Disassemble(const Inst& inst);
const char* OpName(const Inst& inst);

// Opcode values used by the translator.
namespace op {
// SOPP
constexpr uint16_t kSNop = 0x00, kSEndpgm = 0x01, kSBranch = 0x02, kSCbranchScc0 = 0x04, kSCbranchScc1 = 0x05,
                   kSCbranchVccz = 0x06, kSCbranchVccnz = 0x07, kSCbranchExecz = 0x08, kSCbranchExecnz = 0x09,
                   kSWaitcnt = 0x0C;
// SOP1
constexpr uint16_t kSMovB32 = 0x03, kSMovB64 = 0x04, kSNotB32 = 0x07, kSWqmB64 = 0x0A, kSGetpcB64 = 0x1F,
                   kSSetpcB64 = 0x20, kSSwappcB64 = 0x21, kSAndSaveexecB64 = 0x24, kSOrSaveexecB64 = 0x25;
// VOP2 with an inline 32-bit constant.
constexpr uint16_t kVMadmkF32 = 0x20, kVMadakF32 = 0x21;
}  // namespace op

}  // namespace gcn
