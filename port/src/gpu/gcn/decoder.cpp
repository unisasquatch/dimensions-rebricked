#include "gpu/gcn/decoder.h"

#include <cstdio>
#include <map>

namespace gcn {
namespace {

uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }

bool IsLiteral(uint16_t code) { return code == src::kLiteral; }

// Opcode names for the instructions this game uses (Sea Islands ISA); others print as op_0xNN.
const std::map<std::pair<Fmt, uint16_t>, const char*>& Names() {
    static const std::map<std::pair<Fmt, uint16_t>, const char*> names = {
        {{Fmt::SOP2, 0x00}, "s_add_u32"}, {{Fmt::SOP2, 0x01}, "s_sub_u32"}, {{Fmt::SOP2, 0x02}, "s_add_i32"},
        {{Fmt::SOP2, 0x03}, "s_sub_i32"}, {{Fmt::SOP2, 0x04}, "s_addc_u32"}, {{Fmt::SOP2, 0x06}, "s_min_i32"},
        {{Fmt::SOP2, 0x07}, "s_min_u32"}, {{Fmt::SOP2, 0x08}, "s_max_i32"}, {{Fmt::SOP2, 0x09}, "s_max_u32"},
        {{Fmt::SOP2, 0x0A}, "s_cselect_b32"}, {{Fmt::SOP2, 0x0B}, "s_cselect_b64"}, {{Fmt::SOP2, 0x0E}, "s_and_b32"},
        {{Fmt::SOP2, 0x0F}, "s_and_b64"}, {{Fmt::SOP2, 0x10}, "s_or_b32"}, {{Fmt::SOP2, 0x11}, "s_or_b64"},
        {{Fmt::SOP2, 0x12}, "s_xor_b32"}, {{Fmt::SOP2, 0x13}, "s_xor_b64"}, {{Fmt::SOP2, 0x14}, "s_andn2_b32"},
        {{Fmt::SOP2, 0x15}, "s_andn2_b64"}, {{Fmt::SOP2, 0x16}, "s_orn2_b32"}, {{Fmt::SOP2, 0x17}, "s_orn2_b64"},
        {{Fmt::SOP2, 0x18}, "s_nand_b32"}, {{Fmt::SOP2, 0x19}, "s_nand_b64"}, {{Fmt::SOP2, 0x1E}, "s_lshl_b32"},
        {{Fmt::SOP2, 0x20}, "s_lshr_b32"}, {{Fmt::SOP2, 0x22}, "s_ashr_i32"}, {{Fmt::SOP2, 0x24}, "s_bfm_b32"},
        {{Fmt::SOP2, 0x26}, "s_mul_i32"}, {{Fmt::SOP2, 0x27}, "s_bfe_u32"}, {{Fmt::SOP2, 0x28}, "s_bfe_i32"},
        {{Fmt::SOPK, 0x00}, "s_movk_i32"}, {{Fmt::SOPK, 0x0F}, "s_addk_i32"}, {{Fmt::SOPK, 0x10}, "s_mulk_i32"},
        {{Fmt::SOP1, 0x03}, "s_mov_b32"}, {{Fmt::SOP1, 0x04}, "s_mov_b64"}, {{Fmt::SOP1, 0x07}, "s_not_b32"},
        {{Fmt::SOP1, 0x0A}, "s_wqm_b64"}, {{Fmt::SOP1, 0x1F}, "s_getpc_b64"}, {{Fmt::SOP1, 0x20}, "s_setpc_b64"},
        {{Fmt::SOP1, 0x21}, "s_swappc_b64"}, {{Fmt::SOP1, 0x24}, "s_and_saveexec_b64"},
        {{Fmt::SOP1, 0x25}, "s_or_saveexec_b64"},
        {{Fmt::SOPC, 0x00}, "s_cmp_eq_i32"}, {{Fmt::SOPC, 0x01}, "s_cmp_lg_i32"}, {{Fmt::SOPC, 0x02}, "s_cmp_gt_i32"},
        {{Fmt::SOPC, 0x03}, "s_cmp_ge_i32"}, {{Fmt::SOPC, 0x04}, "s_cmp_lt_i32"}, {{Fmt::SOPC, 0x05}, "s_cmp_le_i32"},
        {{Fmt::SOPC, 0x06}, "s_cmp_eq_u32"}, {{Fmt::SOPC, 0x07}, "s_cmp_lg_u32"}, {{Fmt::SOPC, 0x08}, "s_cmp_gt_u32"},
        {{Fmt::SOPC, 0x09}, "s_cmp_ge_u32"}, {{Fmt::SOPC, 0x0A}, "s_cmp_lt_u32"}, {{Fmt::SOPC, 0x0B}, "s_cmp_le_u32"},
        {{Fmt::SOPP, 0x00}, "s_nop"}, {{Fmt::SOPP, 0x01}, "s_endpgm"}, {{Fmt::SOPP, 0x02}, "s_branch"},
        {{Fmt::SOPP, 0x04}, "s_cbranch_scc0"}, {{Fmt::SOPP, 0x05}, "s_cbranch_scc1"},
        {{Fmt::SOPP, 0x06}, "s_cbranch_vccz"}, {{Fmt::SOPP, 0x07}, "s_cbranch_vccnz"},
        {{Fmt::SOPP, 0x08}, "s_cbranch_execz"}, {{Fmt::SOPP, 0x09}, "s_cbranch_execnz"},
        {{Fmt::SOPP, 0x0C}, "s_waitcnt"},
        {{Fmt::SMRD, 0x00}, "s_load_dword"}, {{Fmt::SMRD, 0x01}, "s_load_dwordx2"}, {{Fmt::SMRD, 0x02}, "s_load_dwordx4"},
        {{Fmt::SMRD, 0x03}, "s_load_dwordx8"}, {{Fmt::SMRD, 0x04}, "s_load_dwordx16"},
        {{Fmt::SMRD, 0x08}, "s_buffer_load_dword"}, {{Fmt::SMRD, 0x09}, "s_buffer_load_dwordx2"},
        {{Fmt::SMRD, 0x0A}, "s_buffer_load_dwordx4"}, {{Fmt::SMRD, 0x0B}, "s_buffer_load_dwordx8"},
        {{Fmt::SMRD, 0x0C}, "s_buffer_load_dwordx16"},
        {{Fmt::VOP2, 0x00}, "v_cndmask_b32"}, {{Fmt::VOP2, 0x03}, "v_add_f32"}, {{Fmt::VOP2, 0x04}, "v_sub_f32"},
        {{Fmt::VOP2, 0x05}, "v_subrev_f32"}, {{Fmt::VOP2, 0x06}, "v_mac_legacy_f32"},
        {{Fmt::VOP2, 0x07}, "v_mul_legacy_f32"}, {{Fmt::VOP2, 0x08}, "v_mul_f32"}, {{Fmt::VOP2, 0x0B}, "v_mul_u32_u24"},
        {{Fmt::VOP2, 0x0D}, "v_min_legacy_f32"}, {{Fmt::VOP2, 0x0E}, "v_max_legacy_f32"}, {{Fmt::VOP2, 0x0F}, "v_min_f32"},
        {{Fmt::VOP2, 0x10}, "v_max_f32"}, {{Fmt::VOP2, 0x11}, "v_min_i32"}, {{Fmt::VOP2, 0x12}, "v_max_i32"},
        {{Fmt::VOP2, 0x13}, "v_min_u32"}, {{Fmt::VOP2, 0x14}, "v_max_u32"}, {{Fmt::VOP2, 0x15}, "v_lshr_b32"},
        {{Fmt::VOP2, 0x16}, "v_lshrrev_b32"}, {{Fmt::VOP2, 0x17}, "v_ashr_i32"}, {{Fmt::VOP2, 0x18}, "v_ashrrev_i32"},
        {{Fmt::VOP2, 0x19}, "v_lshl_b32"}, {{Fmt::VOP2, 0x1A}, "v_lshlrev_b32"}, {{Fmt::VOP2, 0x1B}, "v_and_b32"},
        {{Fmt::VOP2, 0x1C}, "v_or_b32"}, {{Fmt::VOP2, 0x1D}, "v_xor_b32"}, {{Fmt::VOP2, 0x1E}, "v_bfm_b32"},
        {{Fmt::VOP2, 0x1F}, "v_mac_f32"}, {{Fmt::VOP2, 0x20}, "v_madmk_f32"}, {{Fmt::VOP2, 0x21}, "v_madak_f32"},
        {{Fmt::VOP2, 0x25}, "v_add_i32"}, {{Fmt::VOP2, 0x26}, "v_sub_i32"}, {{Fmt::VOP2, 0x27}, "v_subrev_i32"},
        {{Fmt::VOP2, 0x2B}, "v_ldexp_f32"}, {{Fmt::VOP2, 0x2F}, "v_cvt_pkrtz_f16_f32"},
        {{Fmt::VOP1, 0x00}, "v_nop"}, {{Fmt::VOP1, 0x01}, "v_mov_b32"}, {{Fmt::VOP1, 0x05}, "v_cvt_f32_i32"},
        {{Fmt::VOP1, 0x06}, "v_cvt_f32_u32"}, {{Fmt::VOP1, 0x07}, "v_cvt_u32_f32"}, {{Fmt::VOP1, 0x08}, "v_cvt_i32_f32"},
        {{Fmt::VOP1, 0x0A}, "v_cvt_f16_f32"}, {{Fmt::VOP1, 0x0B}, "v_cvt_f32_f16"}, {{Fmt::VOP1, 0x11}, "v_cvt_f32_ubyte0"},
        {{Fmt::VOP1, 0x12}, "v_cvt_f32_ubyte1"}, {{Fmt::VOP1, 0x13}, "v_cvt_f32_ubyte2"},
        {{Fmt::VOP1, 0x14}, "v_cvt_f32_ubyte3"}, {{Fmt::VOP1, 0x20}, "v_fract_f32"}, {{Fmt::VOP1, 0x21}, "v_trunc_f32"},
        {{Fmt::VOP1, 0x22}, "v_ceil_f32"}, {{Fmt::VOP1, 0x23}, "v_rndne_f32"}, {{Fmt::VOP1, 0x24}, "v_floor_f32"},
        {{Fmt::VOP1, 0x25}, "v_exp_f32"}, {{Fmt::VOP1, 0x27}, "v_log_f32"}, {{Fmt::VOP1, 0x2A}, "v_rcp_f32"},
        {{Fmt::VOP1, 0x2C}, "v_rsq_clamp_f32"}, {{Fmt::VOP1, 0x2E}, "v_rsq_f32"}, {{Fmt::VOP1, 0x33}, "v_sqrt_f32"},
        {{Fmt::DS, 0x35}, "ds_swizzle_b32"}, {{Fmt::VOP2, 0x28}, "v_addc_u32"}, {{Fmt::VOP2, 0x29}, "v_subb_u32"},
        {{Fmt::VOP2, 0x01}, "v_readlane_b32"}, {{Fmt::VOP2, 0x02}, "v_writelane_b32"},
        {{Fmt::VOP1, 0x35}, "v_sin_f32"}, {{Fmt::VOP1, 0x36}, "v_cos_f32"}, {{Fmt::VOP1, 0x37}, "v_not_b32"},
        {{Fmt::VOPC, 0x00}, "v_cmp_f_f32"}, {{Fmt::VOPC, 0x01}, "v_cmp_lt_f32"}, {{Fmt::VOPC, 0x02}, "v_cmp_eq_f32"},
        {{Fmt::VOPC, 0x03}, "v_cmp_le_f32"}, {{Fmt::VOPC, 0x04}, "v_cmp_gt_f32"}, {{Fmt::VOPC, 0x05}, "v_cmp_lg_f32"},
        {{Fmt::VOPC, 0x06}, "v_cmp_ge_f32"}, {{Fmt::VOPC, 0x09}, "v_cmp_nge_f32"}, {{Fmt::VOPC, 0x0B}, "v_cmp_ngt_f32"},
        {{Fmt::VOPC, 0x0C}, "v_cmp_nle_f32"}, {{Fmt::VOPC, 0x0D}, "v_cmp_neq_f32"}, {{Fmt::VOPC, 0x0E}, "v_cmp_nlt_f32"},
        {{Fmt::VOPC, 0x81}, "v_cmp_lt_i32"}, {{Fmt::VOPC, 0x82}, "v_cmp_eq_i32"}, {{Fmt::VOPC, 0x83}, "v_cmp_le_i32"},
        {{Fmt::VOPC, 0x84}, "v_cmp_gt_i32"}, {{Fmt::VOPC, 0x85}, "v_cmp_ne_i32"}, {{Fmt::VOPC, 0x86}, "v_cmp_ge_i32"},
        {{Fmt::VOPC, 0xC1}, "v_cmp_lt_u32"}, {{Fmt::VOPC, 0xC2}, "v_cmp_eq_u32"}, {{Fmt::VOPC, 0xC3}, "v_cmp_le_u32"},
        {{Fmt::VOPC, 0xC4}, "v_cmp_gt_u32"}, {{Fmt::VOPC, 0xC5}, "v_cmp_ne_u32"}, {{Fmt::VOPC, 0xC6}, "v_cmp_ge_u32"},
        {{Fmt::VOP3, 0x140}, "v_mad_legacy_f32"}, {{Fmt::VOP3, 0x141}, "v_mad_f32"}, {{Fmt::VOP3, 0x143}, "v_mad_u32_u24"},
        {{Fmt::VOP3, 0x144}, "v_cubeid_f32"}, {{Fmt::VOP3, 0x145}, "v_cubesc_f32"}, {{Fmt::VOP3, 0x146}, "v_cubetc_f32"},
        {{Fmt::VOP3, 0x147}, "v_cubema_f32"}, {{Fmt::VOP3, 0x148}, "v_bfe_u32"}, {{Fmt::VOP3, 0x149}, "v_bfe_i32"},
        {{Fmt::VOP3, 0x14A}, "v_bfi_b32"}, {{Fmt::VOP3, 0x14B}, "v_fma_f32"}, {{Fmt::VOP3, 0x151}, "v_min3_f32"},
        {{Fmt::VOP3, 0x154}, "v_max3_f32"}, {{Fmt::VOP3, 0x157}, "v_med3_f32"},
        {{Fmt::VINTRP, 0}, "v_interp_p1_f32"}, {{Fmt::VINTRP, 1}, "v_interp_p2_f32"}, {{Fmt::VINTRP, 2}, "v_interp_mov_f32"},
        {{Fmt::MUBUF, 0}, "buffer_load_format_x"}, {{Fmt::MUBUF, 1}, "buffer_load_format_xy"},
        {{Fmt::MUBUF, 2}, "buffer_load_format_xyz"}, {{Fmt::MUBUF, 3}, "buffer_load_format_xyzw"},
        {{Fmt::MUBUF, 0x0C}, "buffer_load_dword"},
        {{Fmt::MIMG, 0x00}, "image_load"}, {{Fmt::MIMG, 0x01}, "image_load_mip"}, {{Fmt::MIMG, 0x0E}, "image_get_resinfo"},
        {{Fmt::MIMG, 0x20}, "image_sample"}, {{Fmt::MIMG, 0x22}, "image_sample_d"}, {{Fmt::MIMG, 0x24}, "image_sample_l"},
        {{Fmt::MIMG, 0x25}, "image_sample_b"}, {{Fmt::MIMG, 0x27}, "image_sample_lz"}, {{Fmt::MIMG, 0x28}, "image_sample_c"},
        {{Fmt::MIMG, 0x2F}, "image_sample_c_lz"}, {{Fmt::MIMG, 0x30}, "image_sample_o"},
        {{Fmt::MIMG, 0x37}, "image_sample_lz_o"}, {{Fmt::MIMG, 0x47}, "image_gather4_lz"},
        {{Fmt::MIMG, 0x57}, "image_gather4_lz_o"},
        {{Fmt::EXP, 0}, "exp"},
    };
    return names;
}

}  // namespace

const char* OpName(const Inst& inst) {
    Fmt fmt = inst.fmt;
    uint16_t op = inst.op;
    // VOP3 shares opcode spaces: 0x000-0x0FF compares, 0x100-0x13F VOP2, 0x180-0x1FF VOP1.
    if (fmt == Fmt::VOP3) {
        if (op < 0x100) fmt = Fmt::VOPC;
        else if (op < 0x140) { fmt = Fmt::VOP2; op -= 0x100; }
        else if (op >= 0x180) { fmt = Fmt::VOP1; op -= 0x180; }
    }
    auto it = Names().find({fmt, op});
    if (it != Names().end()) return it->second;
    static thread_local char buf[24];
    std::snprintf(buf, sizeof buf, "op_%#x", inst.op);
    return buf;
}

std::vector<Inst> Decode(const uint32_t* code, uint32_t max_dwords, bool stop_at_setpc) {
    std::vector<Inst> out;
    uint32_t pc = 0;
    while (pc < max_dwords) {
        const uint32_t w = code[pc];
        const uint32_t w1 = pc + 1 < max_dwords ? code[pc + 1] : 0;
        Inst in;
        in.pc = pc;
        bool literal_possible = false;

        if ((w & 0xFF800000) == 0xBE800000) {
            in.fmt = Fmt::SOP1;
            in.dst = Bits(w, 22, 16);
            in.op = Bits(w, 15, 8);
            in.src0 = Bits(w, 7, 0);
            literal_possible = IsLiteral(in.src0);
        } else if ((w & 0xFF800000) == 0xBF000000) {
            in.fmt = Fmt::SOPC;
            in.op = Bits(w, 22, 16);
            in.src1 = Bits(w, 15, 8);
            in.src0 = Bits(w, 7, 0);
            literal_possible = IsLiteral(in.src0) || IsLiteral(in.src1);
        } else if ((w & 0xFF800000) == 0xBF800000) {
            in.fmt = Fmt::SOPP;
            in.op = Bits(w, 22, 16);
            in.simm16 = int16_t(Bits(w, 15, 0));
        } else if ((w & 0xF0000000) == 0xB0000000) {
            in.fmt = Fmt::SOPK;
            in.op = Bits(w, 27, 23);
            in.dst = Bits(w, 22, 16);
            in.simm16 = int16_t(Bits(w, 15, 0));
        } else if ((w & 0xF8000000) == 0xC0000000) {
            in.fmt = Fmt::SMRD;
            in.op = Bits(w, 26, 22);
            in.dst = Bits(w, 21, 15);
            in.sbase = Bits(w, 14, 9) * 2;
            in.smrd_imm = Bits(w, 8, 8);
            in.smrd_offset = Bits(w, 7, 0);
            if (!in.smrd_imm && in.smrd_offset == src::kLiteral) {  // 32-bit literal offset (bytes)
                in.literal = w1;
                in.size = 2;
            }
        } else if ((w & 0xC0000000) == 0x80000000) {
            in.fmt = Fmt::SOP2;
            in.op = Bits(w, 29, 23);
            in.dst = Bits(w, 22, 16);
            in.src1 = Bits(w, 15, 8);
            in.src0 = Bits(w, 7, 0);
            literal_possible = IsLiteral(in.src0) || IsLiteral(in.src1);
        } else if ((w & 0xFC000000) == 0xC8000000) {
            in.fmt = Fmt::VINTRP;
            in.dst = Bits(w, 25, 18);
            in.op = Bits(w, 17, 16);
            in.attr = Bits(w, 15, 10);
            in.chan = Bits(w, 9, 8);
            in.src0 = src::kVgpr0 + Bits(w, 7, 0);
        } else if ((w & 0xFC000000) == 0xD0000000) {
            in.fmt = Fmt::VOP3;
            in.size = 2;
            in.op = Bits(w, 25, 17);
            in.dst = Bits(w, 7, 0);
            in.sdst = Bits(w, 14, 8);
            in.clamp = Bits(w, 11, 11);
            in.abs = Bits(w, 10, 8);
            in.neg = Bits(w1, 31, 29);
            in.omod = Bits(w1, 28, 27);
            in.src2 = Bits(w1, 26, 18);
            in.src1 = Bits(w1, 17, 9);
            in.src0 = Bits(w1, 8, 0);
        } else if ((w & 0xFC000000) == 0xD8000000 || (w & 0xFC000000) == 0xDC000000) {
            in.fmt = (w & 0xFC000000) == 0xD8000000 ? Fmt::DS : Fmt::FLAT;
            in.size = 2;
            in.op = Bits(w, 25, 18);
            if (in.fmt == Fmt::DS) {
                in.offset = Bits(w, 15, 0);  // offset1:offset0 (ds_swizzle: the pattern)
                in.vaddr = Bits(w1, 7, 0);
                in.vdata = Bits(w1, 15, 8);
                in.dst = Bits(w1, 31, 24);
            }
        } else if ((w & 0xFC000000) == 0xE0000000 || (w & 0xFC000000) == 0xE8000000) {
            const bool mtbuf = (w & 0xFC000000) == 0xE8000000;
            in.fmt = mtbuf ? Fmt::MTBUF : Fmt::MUBUF;
            in.size = 2;
            in.op = mtbuf ? Bits(w, 18, 16) : Bits(w, 24, 18);
            if (mtbuf) {
                in.nfmt = Bits(w, 25, 23);
                in.dfmt = Bits(w, 22, 19);
            }
            in.glc = Bits(w, 14, 14);
            in.idxen = Bits(w, 13, 13);
            in.offen = Bits(w, 12, 12);
            in.offset = Bits(w, 11, 0);
            in.soffset = Bits(w1, 31, 24);
            in.srsrc = Bits(w1, 20, 16) * 4;
            in.vdata = Bits(w1, 15, 8);
            in.vaddr = Bits(w1, 7, 0);
        } else if ((w & 0xFC000000) == 0xF0000000) {
            in.fmt = Fmt::MIMG;
            in.size = 2;
            in.op = Bits(w, 24, 18);
            in.da = Bits(w, 14, 14);
            in.glc = Bits(w, 13, 13);
            in.unorm = Bits(w, 12, 12);
            in.dmask = Bits(w, 11, 8);
            in.ssamp = Bits(w1, 25, 21) * 4;
            in.srsrc = Bits(w1, 20, 16) * 4;
            in.vdata = Bits(w1, 15, 8);
            in.vaddr = Bits(w1, 7, 0);
        } else if ((w & 0xFC000000) == 0xF8000000) {
            in.fmt = Fmt::EXP;
            in.size = 2;
            in.vm = Bits(w, 12, 12);
            in.done = Bits(w, 11, 11);
            in.compr = Bits(w, 10, 10);
            in.target = Bits(w, 9, 4);
            in.en = Bits(w, 3, 0);
            for (int i = 0; i < 4; ++i) in.vsrc[i] = Bits(w1, 8 * i + 7, 8 * i);
        } else if ((w & 0xFE000000) == 0x7C000000) {
            in.fmt = Fmt::VOPC;
            in.op = Bits(w, 24, 17);
            in.src1 = src::kVgpr0 + Bits(w, 16, 9);
            in.src0 = Bits(w, 8, 0);
            in.dst = src::kVccLo;
            literal_possible = IsLiteral(in.src0);
        } else if ((w & 0xFE000000) == 0x7E000000) {
            in.fmt = Fmt::VOP1;
            in.dst = Bits(w, 24, 17);
            in.op = Bits(w, 16, 9);
            in.src0 = Bits(w, 8, 0);
            literal_possible = IsLiteral(in.src0);
        } else if ((w & 0x80000000) == 0) {
            in.fmt = Fmt::VOP2;
            in.op = Bits(w, 30, 25);
            in.dst = Bits(w, 24, 17);
            in.src1 = src::kVgpr0 + Bits(w, 16, 9);
            // v_readlane_b32 / v_writelane_b32: the lane select is a scalar operand (SGPR, M0 or an
            // inline constant), not a VGPR.
            if (in.op == 0x01 || in.op == 0x02) in.src1 = Bits(w, 16, 9);
            in.src0 = Bits(w, 8, 0);
            literal_possible = IsLiteral(in.src0) || in.op == op::kVMadmkF32 || in.op == op::kVMadakF32;
        }

        if (literal_possible) {
            in.literal = w1;
            in.size = 2;
        }
        out.push_back(in);
        pc += in.size;
        if (in.fmt == Fmt::SOPP && in.op == op::kSEndpgm) break;
        if (stop_at_setpc && in.fmt == Fmt::SOP1 && in.op == op::kSSetpcB64) break;
    }
    return out;
}

namespace {

std::string Operand(uint16_t code, uint32_t literal) {
    char buf[32];
    if (code <= src::kSgprMax) std::snprintf(buf, sizeof buf, "s%u", code);
    else if (code >= src::kVgpr0) std::snprintf(buf, sizeof buf, "v%u", code - src::kVgpr0);
    else if (code == src::kVccLo) return "vcc_lo";
    else if (code == src::kVccHi) return "vcc_hi";
    else if (code == src::kM0) return "m0";
    else if (code == src::kExecLo) return "exec_lo";
    else if (code == src::kExecHi) return "exec_hi";
    else if (code >= src::kZero && code <= src::kIntPosLast) std::snprintf(buf, sizeof buf, "%d", code - src::kZero);
    else if (code >= src::kIntNegFirst && code <= src::kIntNegLast) std::snprintf(buf, sizeof buf, "%d", -int(code - 192));
    else if (code >= src::kFloatHalf && code <= src::kFloatNegFour) {
        static const char* f[] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"};
        return f[code - src::kFloatHalf];
    } else if (code == src::kLiteral) std::snprintf(buf, sizeof buf, "0x%x", literal);
    else if (code == src::kScc) return "scc";
    else std::snprintf(buf, sizeof buf, "src%u", code);
    return buf;
}

}  // namespace

std::string Disassemble(const Inst& in) {
    char buf[160];
    const std::string s0 = Operand(in.src0, in.literal), s1 = Operand(in.src1, in.literal),
                      s2 = Operand(in.src2, in.literal);
    switch (in.fmt) {
        case Fmt::SOP1: std::snprintf(buf, sizeof buf, "%s s%u, %s", OpName(in), in.dst, s0.c_str()); break;
        case Fmt::SOP2: std::snprintf(buf, sizeof buf, "%s s%u, %s, %s", OpName(in), in.dst, s0.c_str(), s1.c_str()); break;
        case Fmt::SOPC: std::snprintf(buf, sizeof buf, "%s %s, %s", OpName(in), s0.c_str(), s1.c_str()); break;
        case Fmt::SOPK: std::snprintf(buf, sizeof buf, "%s s%u, %d", OpName(in), in.dst, in.simm16); break;
        case Fmt::SOPP: std::snprintf(buf, sizeof buf, "%s %d", OpName(in), in.simm16); break;
        case Fmt::SMRD:
            std::snprintf(buf, sizeof buf, "%s s%u, s[%u:%u], %s%u", OpName(in), in.dst, in.sbase, in.sbase + 1,
                          in.smrd_imm ? "" : "s", in.smrd_offset);
            break;
        case Fmt::VOP1: std::snprintf(buf, sizeof buf, "%s v%u, %s", OpName(in), in.dst, s0.c_str()); break;
        case Fmt::VOP2: std::snprintf(buf, sizeof buf, "%s v%u, %s, %s", OpName(in), in.dst, s0.c_str(), s1.c_str()); break;
        case Fmt::VOPC: std::snprintf(buf, sizeof buf, "%s vcc, %s, %s", OpName(in), s0.c_str(), s1.c_str()); break;
        case Fmt::VOP3:
            std::snprintf(buf, sizeof buf, "%s %s%u, %s, %s, %s%s%s", OpName(in), in.op < 0x100 ? "s" : "v", in.dst,
                          s0.c_str(), s1.c_str(), s2.c_str(), in.abs ? " abs" : "", in.neg ? " neg" : "");
            break;
        case Fmt::VINTRP:
            std::snprintf(buf, sizeof buf, "%s v%u, %s, attr%u.%c", OpName(in), in.dst, s0.c_str(), in.attr, "xyzw"[in.chan]);
            break;
        case Fmt::MUBUF:
        case Fmt::MTBUF:
            std::snprintf(buf, sizeof buf, "%s v%u, v%u, s[%u:%u] offset:%u%s%s", OpName(in), in.vdata, in.vaddr,
                          in.srsrc, in.srsrc + 3, in.offset, in.idxen ? " idxen" : "", in.offen ? " offen" : "");
            break;
        case Fmt::MIMG:
            std::snprintf(buf, sizeof buf, "%s v%u, v%u, s[%u:%u], s[%u:%u] dmask:%x%s", OpName(in), in.vdata, in.vaddr,
                          in.srsrc, in.srsrc + 7, in.ssamp, in.ssamp + 3, in.dmask, in.da ? " da" : "");
            break;
        case Fmt::EXP:
            std::snprintf(buf, sizeof buf, "exp target:%u en:%x v%u v%u v%u v%u%s%s%s", in.target, in.en, in.vsrc[0],
                          in.vsrc[1], in.vsrc[2], in.vsrc[3], in.compr ? " compr" : "", in.done ? " done" : "",
                          in.vm ? " vm" : "");
            break;
        default: std::snprintf(buf, sizeof buf, "%s", OpName(in)); break;
    }
    return buf;
}

}  // namespace gcn
