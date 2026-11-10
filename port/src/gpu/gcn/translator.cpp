#include "gpu/gcn/translator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gcn {
namespace {

constexpr uint32_t kUserDataDwords = 16;
constexpr uint32_t kBufferSiteDwords = 8;

uint32_t SmrdDwords(uint16_t op) {
    switch (op & 7) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 4;
        case 3: return 8;
        default: return 16;
    }
}
bool IsBufferSmrd(uint16_t op) { return op >= 8; }

// Normalised view of a vector ALU instruction: VOP3 opcodes mapped back to their VOP2/VOP1/VOPC space.
struct Valu {
    Fmt space;
    uint16_t op;
};
Valu NormaliseValu(const Inst& in) {
    if (in.fmt != Fmt::VOP3) return {in.fmt, in.op};
    if (in.op < 0x100) return {Fmt::VOPC, in.op};
    if (in.op < 0x140) return {Fmt::VOP2, uint16_t(in.op - 0x100)};
    if (in.op >= 0x180) return {Fmt::VOP1, uint16_t(in.op - 0x180)};
    return {Fmt::VOP3, in.op};
}

// MIMG opcode properties.
struct ImageOp {
    bool sample = false, gather = false, load = false, resinfo = false;
    bool offset = false, bias = false, compare = false, lod = false, lz = false, grad = false;
};
ImageOp DescribeImage(uint16_t op) {
    ImageOp o;
    if (op == 0x00 || op == 0x01) { o.load = true; o.lod = op == 0x01; return o; }
    if (op == 0x0E) { o.resinfo = true; return o; }
    if (op >= 0x40) { o.gather = true; op -= 0x20; }  // gather4 variants mirror the sample table at +0x20
    o.sample = !o.gather;
    const uint16_t v = op - 0x20;  // 0..0x1F
    o.offset = v >= 0x10;
    const uint16_t base = v & 0xF;
    o.compare = base >= 0x08;
    switch (base & 7) {
        case 0: break;                   // plain
        case 1: break;                   // _cl (clamp ignored)
        case 2: case 3: o.grad = true; break;
        case 4: o.lod = true; break;
        case 5: case 6: o.bias = true; break;
        case 7: o.lz = true; break;
    }
    return o;
}

}  // namespace

// --- building ---

namespace {
// Lane transfers between SGPRs and VGPR lanes (compilers spill SGPRs this way): 1 v_readlane_b32,
// 2 v_writelane_b32, 3 v_readfirstlane_b32, else 0.
int LaneOp(const Inst& in) {
    if ((in.fmt == Fmt::VOP2 && in.op == 0x01) || (in.fmt == Fmt::VOP3 && in.op == 0x101)) return 1;
    if ((in.fmt == Fmt::VOP2 && in.op == 0x02) || (in.fmt == Fmt::VOP3 && in.op == 0x102)) return 2;
    if ((in.fmt == Fmt::VOP1 && in.op == 0x02) || (in.fmt == Fmt::VOP3 && in.op == 0x182)) return 3;
    return 0;
}
}  // namespace

ShaderProgram Build(Stage stage, const uint32_t* code, const uint32_t* fetch, uint32_t user_sgprs) {
    ShaderProgram p;
    p.stage = stage;
    (void)user_sgprs;
    // content_hash: FNV-1a over the shader's (and its fetch shader's) machine code, the same in every run.
    uint64_t hash = 0xcbf29ce484222325ull ^ uint64_t(stage);
    auto mix = [&](const uint32_t* words, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            hash ^= words[i];
            hash *= 0x100000001b3ull;
        }
    };
    uint32_t code_end = 0;
    for (const Inst& in : Decode(code, 0x10000)) {
        code_end = (std::max)(code_end, in.pc + in.size);
        if (in.fmt == Fmt::SOP1 && in.op == op::kSSwappcB64 && fetch) {
            // Inline the fetch subroutine (minus its s_setpc_b64 return).
            p.has_fetch = true;
            uint32_t fetch_end = 0;
            for (Inst f : Decode(fetch, 0x4000, true)) {
                fetch_end = (std::max)(fetch_end, f.pc + f.size);
                if (f.fmt == Fmt::SOP1 && f.op == op::kSSetpcB64) continue;
                f.pc |= 0x80000000u;  // keeps branch targets in the main shader unambiguous
                p.code.push_back(f);
            }
            mix(fetch, fetch_end);
            continue;
        }
        p.code.push_back(in);
    }
    mix(code, code_end);
    p.content_hash = hash;

    uint32_t offset = kUserDataDwords;
    for (uint32_t i = 0; i < p.code.size(); ++i) {
        const Inst& in = p.code[i];
        if (in.fmt == Fmt::SMRD) {
            p.sites.push_back({Site::Smrd, i, offset, SmrdDwords(in.op)});
            offset += SmrdDwords(in.op);
        } else if (in.fmt == Fmt::MUBUF || in.fmt == Fmt::MTBUF) {
            p.sites.push_back({Site::Buffer, i, offset, kBufferSiteDwords});
            offset += kBufferSiteDwords;
        } else if (in.fmt == Fmt::MIMG) {
            Site s{Site::Image, i, 0, 0, p.image_sites++};
            s.depth_compare = DescribeImage(in.op).compare;
            p.sites.push_back(s);
        } else if (in.fmt == Fmt::EXP) {
            if (stage == Stage::Vertex && in.target >= 32) p.outputs_mask |= 1u << (in.target - 32);
            if (stage == Stage::Pixel && in.target < 8) p.outputs_mask |= 1u << in.target;
            if (stage == Stage::Pixel && in.target == 8) p.writes_depth = true;
        } else if (in.fmt == Fmt::VINTRP) {
            p.inputs_mask |= 1u << in.attr;
        }
    }
    p.data_dwords = offset;
    // The per-draw pre-pass only needs scalar ALU ops and the sites themselves, and nothing after
    // the last site; precomputing them saves walking the whole shader on every draw.
    const uint32_t last = p.sites.empty() ? 0 : p.sites.back().inst_index + 1;
    for (uint32_t i = 0, s = 0; i < last; ++i) {
        const Fmt f = p.code[i].fmt;
        const bool scalar = f == Fmt::SOP1 || f == Fmt::SOP2 || f == Fmt::SOPK || f == Fmt::SOPC || LaneOp(p.code[i]);
        const bool site = s < p.sites.size() && p.sites[s].inst_index == i;
        if (site) ++s;
        if (scalar || site) p.resolve_steps.push_back(i);
    }
    return p;
}

// --- CPU scalar pre-pass ---

namespace {

struct ScalarState {
    // 128 SGPR slots plus room for a 16-dword load whose destination is near the top (dst <= 127),
    // so a wide load never runs past the array.
    uint32_t s[128 + 16] = {};
    uint32_t scc = 0;
    // VGPR lanes written by v_writelane_b32 (SGPR spills), keyed by vgpr << 6 | lane.
    struct Lane { uint32_t key, value; };
    Lane lanes[64];
    uint32_t lane_count = 0;
    void WriteLane(uint32_t key, uint32_t value) {
        for (uint32_t i = 0; i < lane_count; ++i)
            if (lanes[i].key == key) { lanes[i].value = value; return; }
        if (lane_count < 64) lanes[lane_count++] = {key, value};
    }
    uint32_t ReadLane(uint32_t key) const {
        for (uint32_t i = 0; i < lane_count; ++i)
            if (lanes[i].key == key) return lanes[i].value;
        return 0;
    }
};

uint32_t ReadScalar(const ScalarState& st, uint16_t code, uint32_t literal) {
    if (code <= src::kSgprMax || (code >= src::kVccLo && code <= src::kExecHi)) return st.s[code];
    if (code >= src::kZero && code <= src::kIntPosLast) return code - src::kZero;
    if (code >= src::kIntNegFirst && code <= src::kIntNegLast) return uint32_t(-int32_t(code - 192));
    if (code >= src::kFloatHalf && code <= src::kFloatNegFour) {
        static const float f[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        uint32_t bits;
        std::memcpy(&bits, &f[code - src::kFloatHalf], 4);
        return bits;
    }
    if (code == src::kLiteral) return literal;
    if (code == src::kScc) return st.scc;
    return 0;
}
uint64_t ReadScalar64(const ScalarState& st, uint16_t code, uint32_t literal) {
    const uint32_t lo = ReadScalar(st, code, literal);
    uint32_t hi = 0;
    if (code <= src::kSgprMax || (code >= src::kVccLo && code <= src::kExecHi)) hi = st.s[code + 1];
    else if (code >= src::kIntNegFirst && code <= src::kIntNegLast) hi = 0xFFFFFFFF;
    return (uint64_t(hi) << 32) | lo;
}
void Write64(ScalarState& st, uint16_t dst, uint64_t v) {
    st.s[dst] = uint32_t(v);
    st.s[dst + 1] = uint32_t(v >> 32);
}

void ExecScalar(ScalarState& st, const Inst& in) {
    const uint32_t a = ReadScalar(st, in.src0, in.literal), b = ReadScalar(st, in.src1, in.literal);
    const uint64_t a64 = ReadScalar64(st, in.src0, in.literal), b64 = ReadScalar64(st, in.src1, in.literal);
    uint32_t& scc = st.scc;
    if (in.fmt == Fmt::SOP2) {
        uint32_t r = 0;
        switch (in.op) {
            case 0x00: { const uint64_t t = uint64_t(a) + b; r = uint32_t(t); scc = t >> 32; break; }
            case 0x01: r = a - b; scc = b > a; break;
            case 0x02: r = a + b; break;
            case 0x03: r = a - b; break;
            case 0x04: { const uint64_t t = uint64_t(a) + b + scc; r = uint32_t(t); scc = t >> 32; break; }
            case 0x06: r = int32_t(a) < int32_t(b) ? a : b; scc = int32_t(a) < int32_t(b); break;
            case 0x07: r = a < b ? a : b; scc = a < b; break;
            case 0x08: r = int32_t(a) > int32_t(b) ? a : b; scc = int32_t(a) > int32_t(b); break;
            case 0x09: r = a > b ? a : b; scc = a > b; break;
            case 0x0A: r = scc ? a : b; break;
            case 0x0B: Write64(st, in.dst, scc ? a64 : b64); return;
            case 0x0E: r = a & b; scc = r != 0; break;
            case 0x10: r = a | b; scc = r != 0; break;
            case 0x12: r = a ^ b; scc = r != 0; break;
            case 0x14: r = a & ~b; scc = r != 0; break;
            case 0x16: r = a | ~b; scc = r != 0; break;
            case 0x0F: case 0x11: case 0x13: case 0x15: case 0x17: case 0x19: case 0x1B: case 0x1D: {
                uint64_t v = 0;
                switch (in.op) {
                    case 0x0F: v = a64 & b64; break;
                    case 0x11: v = a64 | b64; break;
                    case 0x13: v = a64 ^ b64; break;
                    case 0x15: v = a64 & ~b64; break;
                    case 0x17: v = a64 | ~b64; break;
                    case 0x19: v = ~(a64 & b64); break;
                    case 0x1B: v = ~(a64 | b64); break;
                    case 0x1D: v = ~(a64 ^ b64); break;
                }
                scc = v != 0;
                Write64(st, in.dst, v);
                return;
            }
            case 0x1E: r = a << (b & 31); scc = r != 0; break;
            case 0x20: r = a >> (b & 31); scc = r != 0; break;
            case 0x22: r = uint32_t(int32_t(a) >> (b & 31)); scc = r != 0; break;
            case 0x24: r = ((1u << (a & 31)) - 1) << (b & 31); break;
            case 0x26: r = uint32_t(int32_t(a) * int32_t(b)); break;
            case 0x27: case 0x28: {
                const uint32_t off = b & 31, width = (b >> 16) & 0x7F;
                if (width == 0) { r = 0; }
                else if (in.op == 0x27) { r = (a >> off) & (width >= 32 ? ~0u : ((1u << width) - 1)); }
                else { r = uint32_t((int32_t(a << (32 - off - width))) >> (32 - width)); }
                scc = r != 0;
                break;
            }
            default: return;
        }
        st.s[in.dst] = r;
    } else if (in.fmt == Fmt::SOP1) {
        switch (in.op) {
            case op::kSMovB32: st.s[in.dst] = a; break;
            case op::kSMovB64: Write64(st, in.dst, a64); break;
            case op::kSNotB32: st.s[in.dst] = ~a; scc = st.s[in.dst] != 0; break;
            case op::kSWqmB64: Write64(st, in.dst, a64); break;
            case op::kSAndSaveexecB64:
            case op::kSOrSaveexecB64: Write64(st, in.dst, (uint64_t(st.s[src::kExecHi]) << 32) | st.s[src::kExecLo]); break;
            default: break;
        }
    } else if (in.fmt == Fmt::SOPK) {
        switch (in.op) {
            case 0x00: st.s[in.dst] = uint32_t(in.simm16); break;
            case 0x0F: st.s[in.dst] += uint32_t(in.simm16); break;
            case 0x10: st.s[in.dst] = uint32_t(int32_t(st.s[in.dst]) * in.simm16); break;
            default: break;
        }
    } else if (in.fmt == Fmt::SOPC) {
        switch (in.op) {
            case 0x00: case 0x06: scc = a == b; break;
            case 0x01: case 0x07: scc = a != b; break;
            case 0x02: scc = int32_t(a) > int32_t(b); break;
            case 0x03: scc = int32_t(a) >= int32_t(b); break;
            case 0x04: scc = int32_t(a) < int32_t(b); break;
            case 0x05: scc = int32_t(a) <= int32_t(b); break;
            case 0x08: scc = a > b; break;
            case 0x09: scc = a >= b; break;
            case 0x0A: scc = a < b; break;
            case 0x0B: scc = a <= b; break;
            default: break;
        }
    }
}

// Reads guest memory for a scalar load; returns false (and zeros) if the address is bad.
bool SafeRead(uint64_t address, uint32_t* out, uint32_t dwords) {
    if (address < 0x10000) {
        std::memset(out, 0, dwords * 4);
        return false;
    }
#ifdef _WIN32
    // Check the pages are readable before copying (a fault here is not reliably recoverable once
    // the crash handler is involved). The last readable region is cached per thread, so this
    // rarely costs a VirtualQuery.
    struct Region { uint64_t begin, end; };
    thread_local Region cache[16] = {};
    thread_local uint32_t next_slot = 0;
    const uint64_t end = address + uint64_t(dwords) * 4;
    bool cached = false;
    for (const Region& r : cache)
        if (address >= r.begin && end <= r.end) { cached = true; break; }
    if (!cached) {
        uint64_t& ok_begin = cache[next_slot].begin;
        uint64_t& ok_end = cache[next_slot].end;
        next_slot = (next_slot + 1) % 16;
        ok_begin = ok_end = 0;
        uint64_t at = address;
        uint64_t region_begin = 0;
        while (at < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect) {
                std::memset(out, 0, dwords * 4);
                return false;
            }
            if (!region_begin) region_begin = reinterpret_cast<uint64_t>(mbi.BaseAddress);
            at = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        ok_begin = region_begin;
        ok_end = at;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), dwords * 4);
    return true;
#else
    if (!host::SafeCopy(out, reinterpret_cast<const void*>(address), dwords * 4)) {
        std::memset(out, 0, dwords * 4);
        return false;
    }
    return true;
#endif
}

uint64_t VsharpBase(const uint32_t* v) { return (uint64_t(v[1] & 0xFFFF) << 32) | v[0]; }

}  // namespace

void ResolveSites(const ShaderProgram& program, const uint32_t* user_data, uint32_t* data,
                  std::vector<ResolvedImage>& images, uint64_t (*to_device)(uint64_t guest, uint64_t bytes)) {
    ScalarState st;
    std::memcpy(st.s, user_data, kUserDataDwords * 4);
    std::memcpy(data, user_data, kUserDataDwords * 4);
    st.s[src::kExecLo] = 1;
    images.assign(program.image_sites, ResolvedImage{});

    size_t next_site = 0;
    for (const uint32_t i : program.resolve_steps) {
        const Inst& in = program.code[i];
        // Scalar control flow is approximated by walking every instruction in order: branches in
        // these shaders skip work, they do not change which descriptors are loaded.
        switch (in.fmt) {
            case Fmt::SOP1: case Fmt::SOP2: case Fmt::SOPK: case Fmt::SOPC:
                ExecScalar(st, in);
                break;
            default:
                if (const int lane_op = LaneOp(in)) {
                    // Only spills are followed: a VGPR lane holds what an SGPR wrote into it.
                    const uint32_t lane = lane_op == 3 ? 0 : ReadScalar(st, in.src1, in.literal) & 63;
                    if (lane_op == 2) {
                        st.WriteLane((uint32_t(in.dst) << 6) | lane, ReadScalar(st, in.src0, in.literal));
                    } else if (in.src0 >= src::kVgpr0 && in.dst <= src::kSgprMax) {
                        st.s[in.dst] = st.ReadLane((uint32_t(in.src0 - src::kVgpr0) << 6) | lane);
                    }
                }
                break;
        }
        if (next_site >= program.sites.size() || program.sites[next_site].inst_index != i) continue;
        const Site& site = program.sites[next_site++];
        if (site.kind == Site::Smrd) {
            uint64_t address;
            if (IsBufferSmrd(in.op)) address = VsharpBase(&st.s[in.sbase]);
            else address = (uint64_t(st.s[in.sbase + 1] & 0xFFFF) << 32) | st.s[in.sbase];
            if (in.smrd_imm) address += uint64_t(in.smrd_offset) * 4;
            else if (in.smrd_offset == src::kLiteral) address += in.literal;
            else address += st.s[in.smrd_offset];
            // `data` is write-combined GPU memory: never read it back (that is very slow). Load into
            // the scalar state first, then copy out.
            SafeRead(address, &st.s[in.dst], site.dwords);
            std::memcpy(data + site.data_offset, &st.s[in.dst], site.dwords * 4);
        } else if (site.kind == Site::Buffer) {
            const uint32_t* v = &st.s[in.srsrc];
            uint32_t* out = data + site.data_offset;
            // Byte range the shader can address (records x stride, plus the scalar offset), so the
            // renderer may snapshot small read-only buffers. 0 = may be written: keep it live.
            const uint32_t stride = (v[1] >> 16) & 0x3FFF;
            const uint32_t soff = st.s[in.soffset <= src::kSgprMax ? in.soffset : 0] * (in.soffset <= src::kSgprMax);
            const bool store = in.fmt == Fmt::MTBUF ? in.op >= 4 : (in.op >= 4 && in.op <= 7) || in.op >= 0x18;
            const uint64_t bytes = store ? 0 : (stride ? uint64_t(stride) * v[2] : uint64_t(v[2])) + soff;
            const uint64_t device = to_device(VsharpBase(v), bytes);
            out[0] = uint32_t(device);
            out[1] = uint32_t(device >> 32);
            out[2] = (v[1] >> 16) & 0x3FFF;   // stride
            out[3] = v[2];                     // num_records
            out[4] = in.fmt == Fmt::MTBUF ? in.dfmt : (v[3] >> 15) & 0xF;
            out[5] = in.fmt == Fmt::MTBUF ? in.nfmt : (v[3] >> 12) & 0x7;
            out[6] = v[3] & 0xFFF;             // dst_sel x/y/z/w
            out[7] = st.s[in.soffset <= src::kSgprMax ? in.soffset : 0] * (in.soffset <= src::kSgprMax);
        } else {
            ResolvedImage& img = images[site.binding];
            std::memcpy(img.tsharp, &st.s[in.srsrc], 32);
            std::memcpy(img.ssharp, &st.s[in.ssamp], 16);
        }
    }
}

// --- GLSL generation ---

namespace {

class Glsl {
public:
    Glsl(const ShaderProgram& p, const ShaderKey& k) : p_(p), k_(k) {}
    std::string Generate();

private:
    std::string S(uint16_t code, uint32_t literal);   // 32-bit source as uint expression
    std::string Shi(uint16_t code);                   // high half of a 64-bit source
    std::string Sgpr(uint16_t code);                  // scalar destination name
    std::string Vgpr(uint32_t index) { vgprs_.insert(index); return "v" + std::to_string(index); }
    std::string Fsrc(const Inst& in, int n);          // float source with VOP3 abs/neg
    void Line(const std::string& s) { body_ += "    " + s + "\n"; }
    void WriteV(const Inst& in, uint32_t vdst, const std::string& uint_expr);
    void WriteVf(const Inst& in, uint32_t vdst, const std::string& float_expr);
    void Scalar(const Inst& in);
    void Vector(const Inst& in, uint32_t index);
    void Smrd(const Inst& in, const Site& site);
    void BufferLoad(const Inst& in, const Site& site);
    void Image(const Inst& in, const Site& site);
    void Export(const Inst& in);
    void Ds(const Inst& in);
    bool uses_subgroup_ = false;  // ds_swizzle: GL_KHR_shader_subgroup quad / shuffle
    std::string Branch(const Inst& in, bool& conditional);

    const ShaderProgram& p_;
    const ShaderKey& k_;
    std::string body_;
    std::set<uint32_t> sgprs_, vgprs_;
    std::set<std::string> lanes_;  // VGPR lanes used for SGPR spills
    bool guard_exec_ = false;
    std::map<uint32_t, size_t> site_by_inst_;
};

std::string ScaleLiteral(float scale) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", double(scale));
    std::string out = buf;
    if (out.find('.') == std::string::npos && out.find('e') == std::string::npos) out += ".0";
    return out;
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08xu", v);
    return buf;
}

std::string Glsl::S(uint16_t code, uint32_t literal) {
    if (code <= src::kSgprMax) { sgprs_.insert(code); return "s" + std::to_string(code); }
    if (code >= src::kVgpr0) return Vgpr(code - src::kVgpr0);
    switch (code) {
        case src::kVccLo: return "vcc_lo";
        case src::kVccHi: return "vcc_hi";
        case src::kM0: return "m0";
        case src::kExecLo: return "exec_lo";
        case src::kExecHi: return "exec_hi";
        case src::kScc: return "scc";
        case src::kVccz: return "((vcc_lo & 1u) == 0u ? 1u : 0u)";
        case src::kExecz: return "(exec_lo == 0u ? 1u : 0u)";
        case src::kLiteral: return Hex(literal);
    }
    if (code >= src::kZero && code <= src::kIntPosLast) return std::to_string(code - src::kZero) + "u";
    if (code >= src::kIntNegFirst && code <= src::kIntNegLast) return "uint(-" + std::to_string(code - 192) + ")";
    if (code >= src::kFloatHalf && code <= src::kFloatNegFour) {
        static const uint32_t f[] = {0x3F000000, 0xBF000000, 0x3F800000, 0xBF800000,
                                     0x40000000, 0xC0000000, 0x40800000, 0xC0800000};
        return Hex(f[code - src::kFloatHalf]);
    }
    return "0u";
}

std::string Glsl::Shi(uint16_t code) {
    if (code <= src::kSgprMax) { sgprs_.insert(code + 1); return "s" + std::to_string(code + 1); }
    switch (code) {
        case src::kVccLo: return "vcc_hi";
        case src::kExecLo: return "exec_hi";
    }
    if (code >= src::kIntNegFirst && code <= src::kIntNegLast) return "0xffffffffu";
    return "0u";
}

std::string Glsl::Sgpr(uint16_t code) {
    if (code <= src::kSgprMax) { sgprs_.insert(code); return "s" + std::to_string(code); }
    switch (code) {
        case src::kVccLo: return "vcc_lo";
        case src::kVccHi: return "vcc_hi";
        case src::kM0: return "m0";
        case src::kExecLo: return "exec_lo";
        case src::kExecHi: return "exec_hi";
    }
    return "s_null";
}

std::string Glsl::Fsrc(const Inst& in, int n) {
    const uint16_t code = n == 0 ? in.src0 : n == 1 ? in.src1 : in.src2;
    std::string e = "F(" + S(code, in.literal) + ")";
    if (in.fmt == Fmt::VOP3) {
        if (in.abs & (1 << n)) e = "abs(" + e + ")";
        if (in.neg & (1 << n)) e = "(-" + e + ")";
    }
    return e;
}

void Glsl::WriteV(const Inst& in, uint32_t vdst, const std::string& expr) {
    (void)in;
    const std::string d = Vgpr(vdst);
    Line(guard_exec_ ? "if (exec_lo != 0u) " + d + " = " + expr + ";" : d + " = " + expr + ";");
}

// LDS ops: only ds_swizzle_b32 (lanes exchanging values - pixel shaders use it to read the neighbouring
// pixels of their 2x2 quad, e.g. for a hand-made mip level). Done with subgroup operations: Vulkan
// fragment quads match GCN's (lane & 3: top-left, top-right, bottom-left, bottom-right).
void Glsl::Ds(const Inst& in) {
    if (in.op != 0x35) {
        Line("// unhandled " + Disassemble(in));
        return;
    }
    uses_subgroup_ = true;
    const std::string t = "sw" + std::to_string(in.pc);
    const std::string src = Vgpr(in.vaddr);
    const uint32_t off = in.offset;
    // Which lane of the quad each lane reads, when the pattern stays inside the quad.
    int sel[4] = {-1, -1, -1, -1};
    if (off & 0x8000) {  // quad-permute mode: 2 bits per lane
        for (int i = 0; i < 4; ++i) sel[i] = (off >> (2 * i)) & 3;
    } else {  // bit mode: lane = ((lane & and) | or) ^ xor, within 32 lanes
        const uint32_t and_mask = off & 31, or_mask = (off >> 5) & 31, xor_mask = (off >> 10) & 31;
        if ((and_mask & 0x1C) == 0x1C && !(or_mask & 0x1C) && !(xor_mask & 0x1C))
            for (int i = 0; i < 4; ++i) sel[i] = int(((uint32_t(i) & and_mask) | or_mask) ^ xor_mask) & 3;
    }
    // Computed outside the exec guard: every lane of the quad must take part.
    if (sel[0] >= 0) {
        Line("uint " + t + "_q = gl_SubgroupInvocationID & 3u;");
        for (int i = 0; i < 4; ++i)
            Line("uint " + t + "_" + std::to_string(i) + " = subgroupQuadBroadcast(" + src + ", " + std::to_string(i) + "u);");
        auto pick = [&](int lane) { return t + "_" + std::to_string(sel[lane]); };
        WriteV(in, in.dst, "(" + t + "_q == 0u ? " + pick(0) + " : " + t + "_q == 1u ? " + pick(1) + " : " + t +
                               "_q == 2u ? " + pick(2) + " : " + pick(3) + ")");
    } else {
        const uint32_t and_mask = off & 31, or_mask = (off >> 5) & 31, xor_mask = (off >> 10) & 31;
        Line("uint " + t + "_l = gl_SubgroupInvocationID;");
        Line("uint " + t + "_v = subgroupShuffle(" + src + ", (" + t + "_l & ~31u) | (((" + t + "_l & " +
             std::to_string(and_mask) + "u) | " + std::to_string(or_mask) + "u) ^ " + std::to_string(xor_mask) + "u));");
        WriteV(in, in.dst, t + "_v");
    }
}

void Glsl::WriteVf(const Inst& in, uint32_t vdst, const std::string& f) {
    std::string e = f;
    if (in.fmt == Fmt::VOP3) {
        static const char* omod[] = {"", " * 2.0", " * 4.0", " * 0.5"};
        if (in.omod) e = "(" + e + ")" + omod[in.omod];
        if (in.clamp) e = "gsat(" + e + ")";  // GCN clamp: NaN -> 0
    }
    WriteV(in, vdst, "U(" + e + ")");
}

void Glsl::Scalar(const Inst& in) {
    const std::string a = S(in.src0, in.literal), b = S(in.src1, in.literal);
    const std::string d = Sgpr(in.dst);
    auto pair64 = [&](const std::string& lo, const std::string& hi) {
        Line(d + " = " + lo + ";");
        Line(Sgpr(in.dst + 1) + " = " + hi + ";");
    };
    if (in.fmt == Fmt::SOP2) {
        const std::string ahi = Shi(in.src0), bhi = Shi(in.src1);
        switch (in.op) {
            case 0x00: Line("scc = (uint(" + a + ") > ~uint(" + b + ")) ? 1u : 0u; " + d + " = " + a + " + " + b + ";"); break;
            case 0x01: Line("scc = (" + b + " > " + a + ") ? 1u : 0u; " + d + " = " + a + " - " + b + ";"); break;
            case 0x02: Line(d + " = " + a + " + " + b + ";"); break;
            case 0x03: Line(d + " = " + a + " - " + b + ";"); break;
            case 0x04: Line(d + " = " + a + " + " + b + " + scc;"); break;
            case 0x06: Line(d + " = uint(min(int(" + a + "), int(" + b + ")));"); break;
            case 0x07: Line(d + " = min(" + a + ", " + b + ");"); break;
            case 0x08: Line(d + " = uint(max(int(" + a + "), int(" + b + ")));"); break;
            case 0x09: Line(d + " = max(" + a + ", " + b + ");"); break;
            case 0x0A: Line(d + " = scc != 0u ? " + a + " : " + b + ";"); break;
            case 0x0B: pair64("scc != 0u ? " + a + " : " + b, "scc != 0u ? " + ahi + " : " + bhi); break;
            case 0x0E: Line(d + " = " + a + " & " + b + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x10: Line(d + " = " + a + " | " + b + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x12: Line(d + " = " + a + " ^ " + b + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x14: Line(d + " = " + a + " & ~" + b + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x16: Line(d + " = " + a + " | ~" + b + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x0F: case 0x11: case 0x13: case 0x15: case 0x17: case 0x19: case 0x1B: case 0x1D: {
                static const std::map<uint16_t, std::pair<const char*, const char*>> ops = {
                    {0x0F, {"(", ") & ("}}, {0x11, {"(", ") | ("}}, {0x13, {"(", ") ^ ("}},
                    {0x15, {"(", ") & ~("}}, {0x17, {"(", ") | ~("}}, {0x19, {"~((", ") & ("}},
                    {0x1B, {"~((", ") | ("}}, {0x1D, {"~((", ") ^ ("}}};
                const auto& [pre, mid] = ops.at(in.op);
                const std::string close = (in.op >= 0x19) ? "))" : ")";
                Line("{ uint lo_ = " + std::string(pre) + a + mid + b + close + "; uint hi_ = " + pre + ahi + mid + bhi +
                     close + "; " + d + " = lo_; " + Sgpr(in.dst + 1) + " = hi_; scc = (lo_ | hi_) != 0u ? 1u : 0u; }");
                break;
            }
            case 0x1E: Line(d + " = " + a + " << (" + b + " & 31u); scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x20: Line(d + " = " + a + " >> (" + b + " & 31u); scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x22: Line(d + " = uint(int(" + a + ") >> int(" + b + " & 31u)); scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x24: Line(d + " = ((1u << (" + a + " & 31u)) - 1u) << (" + b + " & 31u);"); break;
            case 0x26: Line(d + " = uint(int(" + a + ") * int(" + b + "));"); break;
            case 0x27: Line(d + " = bfe_u(" + a + ", " + b + "); scc = " + d + " != 0u ? 1u : 0u;"); break;
            case 0x28: Line(d + " = bfe_i(" + a + ", " + b + "); scc = " + d + " != 0u ? 1u : 0u;"); break;
            default: Line("// unhandled " + Disassemble(in));
        }
    } else if (in.fmt == Fmt::SOP1) {
        switch (in.op) {
            case op::kSMovB32: Line(d + " = " + a + ";"); break;
            case op::kSMovB64: pair64(a, Shi(in.src0)); break;
            case op::kSNotB32: Line(d + " = ~" + a + "; scc = " + d + " != 0u ? 1u : 0u;"); break;
            case op::kSWqmB64: pair64("(" + a + " != 0u ? 1u : 0u)", "0u"); break;  // one lane: any active -> active
            case op::kSAndSaveexecB64:
                Line("{ uint lo_ = " + a + "; " + d + " = exec_lo; " + Sgpr(in.dst + 1) + " = exec_hi; exec_lo = exec_lo & lo_; exec_hi = 0u; scc = exec_lo != 0u ? 1u : 0u; }");
                break;
            case op::kSOrSaveexecB64:
                Line("{ uint lo_ = " + a + "; " + d + " = exec_lo; " + Sgpr(in.dst + 1) + " = exec_hi; exec_lo = exec_lo | lo_; exec_hi = 0u; scc = exec_lo != 0u ? 1u : 0u; }");
                break;
            case op::kSGetpcB64: case op::kSSetpcB64: case op::kSSwappcB64: break;
            default: Line("// unhandled " + Disassemble(in));
        }
    } else if (in.fmt == Fmt::SOPK) {
        switch (in.op) {
            case 0x00: Line(d + " = " + Hex(uint32_t(in.simm16)) + ";"); break;
            case 0x0F: Line(d + " += " + Hex(uint32_t(in.simm16)) + ";"); break;
            case 0x10: Line(d + " = uint(int(" + d + ") * " + std::to_string(in.simm16) + ");"); break;
            default: Line("// unhandled " + Disassemble(in));
        }
    } else if (in.fmt == Fmt::SOPC) {
        static const char* cmp[] = {"==", "!=", ">", ">=", "<", "<="};
        if (in.op <= 0x05) Line("scc = int(" + a + ") " + cmp[in.op] + " int(" + b + ") ? 1u : 0u;");
        else if (in.op <= 0x0B) Line("scc = " + a + " " + cmp[in.op - 6] + " " + b + " ? 1u : 0u;");
        else Line("// unhandled " + Disassemble(in));
    }
}

void Glsl::Vector(const Inst& in, uint32_t) {
    const Valu v = NormaliseValu(in);
    const uint32_t d = in.dst;
    auto f = [&](int n) { return Fsrc(in, n); };
    auto u = [&](int n) { return S(n == 0 ? in.src0 : n == 1 ? in.src1 : in.src2, in.literal); };
    const std::string K = Hex(in.literal);

    if (v.space == Fmt::VOP2 && (v.op == 0x01 || v.op == 0x02)) {
        // v_readlane_b32 / v_writelane_b32 with a constant lane: SGPR spills to VGPR lanes, kept as
        // one variable per lane (the shader only reads back what it wrote).
        const bool constant_lane = in.src1 >= src::kZero && in.src1 <= src::kIntPosLast;
        const uint32_t vgpr = v.op == 0x01 ? uint32_t(in.src0 - src::kVgpr0) : d;
        const std::string name = "lane_v" + std::to_string(vgpr) + "_" +
                                 (constant_lane ? std::to_string(in.src1 - src::kZero) : std::string("dyn"));
        lanes_.insert(name);
        if (v.op == 0x02) Line(name + " = " + u(0) + ";");
        else Line(Sgpr(d) + " = " + name + ";");
        return;
    }
    if (v.space == Fmt::VOP2) {
        switch (v.op) {
            case 0x00: {  // v_cndmask_b32; in VOP3 the abs / neg modifiers apply to the sign bit
                const std::string mask = in.fmt == Fmt::VOP3 ? S(in.src2, 0) : std::string("vcc_lo");
                // Experiment: LDPORT_NO_CNDMASK_MODS=1 ignores the modifiers, =2 only on src1, =3 only on src0.
                static const int no_mods = std::getenv("LDPORT_NO_CNDMASK_MODS") ? std::atoi(std::getenv("LDPORT_NO_CNDMASK_MODS")) : 0;
                auto m = [&](int n) {
                    std::string e = u(n);
                    if (no_mods == 1 || (no_mods == 2 && n == 1) || (no_mods == 3 && n == 0)) return e;
                    if (in.fmt == Fmt::VOP3 && (in.abs & (1 << n))) e = "(" + e + " & 0x7fffffffu)";
                    if (in.fmt == Fmt::VOP3 && (in.neg & (1 << n))) e = "(" + e + " ^ 0x80000000u)";
                    return e;
                };
                WriteV(in, d, "((" + mask + " & 1u) != 0u) ? " + m(1) + " : " + m(0));
                return;
            }
            case 0x03: WriteVf(in, d, f(0) + " + " + f(1)); return;
            case 0x04: WriteVf(in, d, f(0) + " - " + f(1)); return;
            case 0x05: WriteVf(in, d, f(1) + " - " + f(0)); return;
            case 0x06: WriteVf(in, d, "mul_legacy(" + f(0) + ", " + f(1) + ") + F(" + Vgpr(d) + ")"); return;
            case 0x07: WriteVf(in, d, "mul_legacy(" + f(0) + ", " + f(1) + ")"); return;
            case 0x08: WriteVf(in, d, f(0) + " * " + f(1)); return;
            case 0x0B: WriteV(in, d, "(" + u(0) + " & 0xffffffu) * (" + u(1) + " & 0xffffffu)"); return;
            case 0x0D: case 0x0F: WriteVf(in, d, "gmin(" + f(0) + ", " + f(1) + ")"); return;
            case 0x0E: case 0x10: WriteVf(in, d, "gmax(" + f(0) + ", " + f(1) + ")"); return;
            case 0x11: WriteV(in, d, "uint(min(int(" + u(0) + "), int(" + u(1) + ")))"); return;
            case 0x12: WriteV(in, d, "uint(max(int(" + u(0) + "), int(" + u(1) + ")))"); return;
            case 0x13: WriteV(in, d, "min(" + u(0) + ", " + u(1) + ")"); return;
            case 0x14: WriteV(in, d, "max(" + u(0) + ", " + u(1) + ")"); return;
            case 0x15: WriteV(in, d, u(0) + " >> (" + u(1) + " & 31u)"); return;
            case 0x16: WriteV(in, d, u(1) + " >> (" + u(0) + " & 31u)"); return;
            case 0x17: WriteV(in, d, "uint(int(" + u(0) + ") >> int(" + u(1) + " & 31u))"); return;
            case 0x18: WriteV(in, d, "uint(int(" + u(1) + ") >> int(" + u(0) + " & 31u))"); return;
            case 0x19: WriteV(in, d, u(0) + " << (" + u(1) + " & 31u)"); return;
            case 0x1A: WriteV(in, d, u(1) + " << (" + u(0) + " & 31u)"); return;
            case 0x1B: WriteV(in, d, u(0) + " & " + u(1)); return;
            case 0x1C: WriteV(in, d, u(0) + " | " + u(1)); return;
            case 0x1D: WriteV(in, d, u(0) + " ^ " + u(1)); return;
            case 0x1E: WriteV(in, d, "((1u << (" + u(0) + " & 31u)) - 1u) << (" + u(1) + " & 31u)"); return;
            case 0x1F: WriteVf(in, d, f(0) + " * " + f(1) + " + F(" + Vgpr(d) + ")"); return;
            case 0x20: WriteVf(in, d, f(0) + " * F(" + K + ") + " + f(1)); return;
            case 0x21: WriteVf(in, d, f(0) + " * " + f(1) + " + F(" + K + ")"); return;
            // Integer add/subtract with carry out (VOP3b: to sdst; VOP2: to VCC) and, for addc/subb, carry in
            // (VOP3b: src2; VOP2: VCC). One lane is modelled: bit 0 of the mask.
            case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: {
                const std::string t = "ca" + std::to_string(in.pc);
                const bool sub = v.op == 0x26 || v.op == 0x27 || v.op == 0x29 || v.op == 0x2A;
                const bool rev = v.op == 0x27 || v.op == 0x2A;
                const bool with_in = v.op >= 0x28;
                const std::string cin = !with_in ? std::string("0u")
                                                 : "(" + (in.fmt == Fmt::VOP3 ? S(in.src2, 0) : std::string("vcc_lo")) + " & 1u)";
                const std::string cout = in.fmt == Fmt::VOP3 ? Sgpr(in.sdst) : std::string("vcc_lo");
                Line("uint " + t + "_a = " + (rev ? u(1) : u(0)) + ", " + t + "_b = " + (rev ? u(0) : u(1)) + ", " + t +
                     "_c = " + cin + ";");
                if (!sub) {
                    Line("uint " + t + "_r = " + t + "_a + " + t + "_b + " + t + "_c;");
                    Line(cout + " = (" + t + "_r < " + t + "_a || (" + t + "_c != 0u && " + t + "_r == " + t +
                         "_a)) ? 1u : 0u;");
                } else {
                    Line("uint " + t + "_r = " + t + "_a - " + t + "_b - " + t + "_c;");
                    Line(cout + " = (uint64_t(" + t + "_b) + uint64_t(" + t + "_c) > uint64_t(" + t + "_a)) ? 1u : 0u;");
                }
                WriteV(in, d, t + "_r");
                return;
            }
            case 0x2B: WriteVf(in, d, "ldexp(" + f(0) + ", int(" + u(1) + "))"); return;
            case 0x2F: WriteV(in, d, "pkrtz(" + f(0) + ", " + f(1) + ")"); return;  // v_cvt_pkrtz_f16_f32
        }
    } else if (v.space == Fmt::VOP1) {
        switch (v.op) {
            case 0x00: return;
            case 0x01: WriteV(in, d, u(0)); return;
            // v_readfirstlane_b32: scalar dst. Each invocation keeps its own scalar copy here, and the
            // value is uniform in practice, so the lane's own value stands in for lane 0's.
            case 0x02: Line(Sgpr(d) + " = " + u(0) + ";"); return;
            case 0x05: WriteVf(in, d, "float(int(" + u(0) + "))"); return;
            case 0x06: WriteVf(in, d, "float(" + u(0) + ")"); return;
            case 0x07: WriteV(in, d, "uint(max(" + f(0) + ", 0.0))"); return;
            case 0x08: WriteV(in, d, "uint(int(" + f(0) + "))"); return;
            case 0x0A: WriteV(in, d, "packHalf2x16(vec2(" + f(0) + ", 0.0)) & 0xffffu"); return;
            case 0x0B: WriteVf(in, d, "unpackHalf2x16(" + u(0) + ").x"); return;
            case 0x11: case 0x12: case 0x13: case 0x14:
                WriteVf(in, d, "float((" + u(0) + " >> " + std::to_string((v.op - 0x11) * 8) + "u) & 0xffu)"); return;
            case 0x20: WriteVf(in, d, "fract(" + f(0) + ")"); return;
            case 0x21: WriteVf(in, d, "trunc(" + f(0) + ")"); return;
            case 0x22: WriteVf(in, d, "ceil(" + f(0) + ")"); return;
            case 0x23: WriteVf(in, d, "roundEven(" + f(0) + ")"); return;
            case 0x24: WriteVf(in, d, "floor(" + f(0) + ")"); return;
            case 0x25: WriteVf(in, d, "exp2(" + f(0) + ")"); return;
            case 0x27: WriteVf(in, d, "log2(" + f(0) + ")"); return;
            case 0x2A: WriteVf(in, d, "1.0 / " + f(0)); return;
            case 0x2C: WriteVf(in, d, "clamp(inversesqrt(" + f(0) + "), -3.402823466e38, 3.402823466e38)"); return;
            case 0x2E: WriteVf(in, d, "inversesqrt(" + f(0) + ")"); return;
            case 0x33: WriteVf(in, d, "sqrt(" + f(0) + ")"); return;
            case 0x35: WriteVf(in, d, "sin(" + f(0) + " * 6.283185307)"); return;
            case 0x36: WriteVf(in, d, "cos(" + f(0) + " * 6.283185307)"); return;
            case 0x37: WriteV(in, d, "~" + u(0)); return;
        }
    } else if (v.space == Fmt::VOPC) {
        const uint16_t op = v.op & 0x7F, kind = v.op >> 7;  // kind 0: f32, 1: i32 (0x80), u32 at 0xC0
        const bool write_exec = (op & 0x10) != 0;          // v_cmpx_*
        const uint16_t c = op & 0x0F;
        std::string cond;
        if (v.op < 0x80) {
            static const char* fc[] = {"false", "<", "==", "<=", ">", "!=", ">=", "", "", "<", "!=", "<=", ">", "!=", ">=", "true"};
            const std::string a = f(0), b = f(1);
            switch (c) {
                case 0x00: cond = "false"; break;
                case 0x07: cond = "!isnan(" + a + ") && !isnan(" + b + ")"; break;
                case 0x08: cond = "isnan(" + a + ") || isnan(" + b + ")"; break;
                case 0x09: cond = "!(" + a + " >= " + b + ")"; break;  // nge
                case 0x0A: cond = "!(" + a + " != " + b + ")"; break;  // nlg
                case 0x0B: cond = "!(" + a + " > " + b + ")"; break;   // ngt
                case 0x0C: cond = "!(" + a + " <= " + b + ")"; break;  // nle
                case 0x0D: cond = "!(" + a + " == " + b + ")"; break;  // neq
                case 0x0E: cond = "!(" + a + " < " + b + ")"; break;   // nlt
                case 0x0F: cond = "true"; break;
                default: cond = a + " " + fc[c] + " " + b;
            }
        } else {
            static const char* ic[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
            const bool is_signed = (v.op & 0xC0) == 0x80;
            const std::string a = is_signed ? "int(" + u(0) + ")" : u(0), b = is_signed ? "int(" + u(1) + ")" : u(1);
            const uint16_t ci = c & 7;
            cond = ci == 0 ? "false" : ci == 7 ? "true" : a + " " + ic[ci] + " " + b;
            (void)kind;
        }
        const std::string dst = in.fmt == Fmt::VOP3 ? Sgpr(in.dst) : std::string("vcc_lo");
        const std::string dst_hi = in.fmt == Fmt::VOP3 ? Sgpr(in.dst + 1) : std::string("vcc_hi");
        Line(dst + " = (exec_lo != 0u && (" + cond + ")) ? 1u : 0u; " + dst_hi + " = 0u;");
        if (write_exec) Line("exec_lo = " + dst + "; exec_hi = 0u;");
        return;
    } else {  // VOP3-only
        switch (v.op) {
            case 0x140: WriteVf(in, d, "mul_legacy(" + f(0) + ", " + f(1) + ") + " + f(2)); return;
            case 0x141: WriteVf(in, d, f(0) + " * " + f(1) + " + " + f(2)); return;
            case 0x14B: WriteVf(in, d, "fma(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            case 0x143: WriteV(in, d, "(" + u(0) + " & 0xffffffu) * (" + u(1) + " & 0xffffffu) + " + u(2)); return;
            case 0x144: WriteVf(in, d, "cube_id(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            case 0x145: WriteVf(in, d, "cube_sc(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            case 0x146: WriteVf(in, d, "cube_tc(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            case 0x147: WriteVf(in, d, "cube_ma(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            case 0x148: WriteV(in, d, "bitfieldExtract(" + u(0) + ", int(" + u(1) + " & 31u), int(min(" + u(2) + " & 31u, 32u - (" + u(1) + " & 31u))))"); return;
            case 0x149: WriteV(in, d, "uint(bitfieldExtract(int(" + u(0) + "), int(" + u(1) + " & 31u), int(min(" + u(2) + " & 31u, 32u - (" + u(1) + " & 31u)))))"); return;
            case 0x14A: WriteV(in, d, "(" + u(0) + " & " + u(1) + ") | (~" + u(0) + " & " + u(2) + ")"); return;
            case 0x151: WriteVf(in, d, "gmin(gmin(" + f(0) + ", " + f(1) + "), " + f(2) + ")"); return;
            case 0x154: WriteVf(in, d, "gmax(gmax(" + f(0) + ", " + f(1) + "), " + f(2) + ")"); return;
            case 0x157: WriteVf(in, d, "med3(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); return;
            // v_mad_i32_i24: sign-extended 24-bit multiply-add.
            case 0x142:
                WriteV(in, d, "uint(bitfieldExtract(int(" + u(0) + "), 0, 24) * bitfieldExtract(int(" + u(1) +
                                  "), 0, 24) + int(" + u(2) + "))");
                return;
            // v_alignbit_b32 / v_alignbyte_b32: ({s0,s1} >> shift)[31:0].
            case 0x14E:
                WriteV(in, d, "uint((((uint64_t(" + u(0) + ") << 32) | uint64_t(" + u(1) + ")) >> (" + u(2) +
                                  " & 31u)) & 0xffffffffUL)");
                return;
            case 0x14F:
                WriteV(in, d, "uint((((uint64_t(" + u(0) + ") << 32) | uint64_t(" + u(1) + ")) >> ((" + u(2) +
                                  " & 3u) * 8u)) & 0xffffffffUL)");
                return;
            case 0x152: WriteV(in, d, "uint(min(min(int(" + u(0) + "), int(" + u(1) + ")), int(" + u(2) + ")))"); return;
            case 0x153: WriteV(in, d, "min(min(" + u(0) + ", " + u(1) + "), " + u(2) + ")"); return;
            case 0x155: WriteV(in, d, "uint(max(max(int(" + u(0) + "), int(" + u(1) + ")), int(" + u(2) + ")))"); return;
            case 0x156: WriteV(in, d, "max(max(" + u(0) + ", " + u(1) + "), " + u(2) + ")"); return;
            case 0x158:
                WriteV(in, d, "uint(max(min(int(" + u(0) + "), int(" + u(1) + ")), min(max(int(" + u(0) + "), int(" + u(1) +
                                  ")), int(" + u(2) + "))))");
                return;
            case 0x159:
                WriteV(in, d, "max(min(" + u(0) + ", " + u(1) + "), min(max(" + u(0) + ", " + u(1) + "), " + u(2) + "))");
                return;
            case 0x15D:  // v_sad_u32
                WriteV(in, d, "(max(" + u(0) + ", " + u(1) + ") - min(" + u(0) + ", " + u(1) + ")) + " + u(2));
                return;
            // 32-bit integer multiplies.
            case 0x169: WriteV(in, d, u(0) + " * " + u(1)); return;  // v_mul_lo_u32
            case 0x16B: WriteV(in, d, "uint(int(" + u(0) + ") * int(" + u(1) + "))"); return;  // v_mul_lo_i32
            case 0x16A: {  // v_mul_hi_u32
                const std::string t = "mh" + std::to_string(in.pc);
                Line("uint " + t + "_hi, " + t + "_lo; umulExtended(" + u(0) + ", " + u(1) + ", " + t + "_hi, " + t + "_lo);");
                WriteV(in, d, t + "_hi");
                return;
            }
            case 0x16C: {  // v_mul_hi_i32
                const std::string t = "mh" + std::to_string(in.pc);
                Line("int " + t + "_hi, " + t + "_lo; imulExtended(int(" + u(0) + "), int(" + u(1) + "), " + t + "_hi, " + t +
                     "_lo);");
                WriteV(in, d, "uint(" + t + "_hi)");
                return;
            }
        }
    }
    Line("// unhandled " + Disassemble(in));
}

void Glsl::Smrd(const Inst& in, const Site& site) {
    for (uint32_t i = 0; i < site.dwords; ++i)
        Line(Sgpr(in.dst + i) + " = D(" + std::to_string(site.data_offset + i) + "u);");
}

void Glsl::BufferLoad(const Inst& in, const Site& site) {
    // Index from vaddr when idxen; byte offset from the following VGPR when offen too.
    uint32_t va = in.vaddr;
    const std::string index = in.idxen ? Vgpr(va++) : "0u";
    const std::string voffset = in.offen ? Vgpr(va) : "0u";
    const uint32_t comps = in.fmt == Fmt::MTBUF ? (in.op & 3) + 1 : (in.op <= 3 ? in.op + 1 : 1);
    const std::string call = "buffer_load(" + std::to_string(site.data_offset) + "u, " + index + ", " + voffset +
                             " + " + std::to_string(in.offset) + "u)";
    Line("{ uvec4 r_ = " + call + ";");
    for (uint32_t c = 0; c < comps; ++c) WriteV(in, in.vdata + c, std::string("r_.") + "xyzw"[c]);
    Line("}");
}

void Glsl::Image(const Inst& in, const Site& site) {
    const ImageOp o = DescribeImage(in.op);
    const ImageType type = site.binding < k_.image_types.size() ? k_.image_types[site.binding] : ImageType::Tex2D;
    const std::string tex = "tex" + std::to_string(site.binding);
    uint32_t a = in.vaddr;
    auto vf = [&](uint32_t r) { return "F(" + Vgpr(r) + ")"; };

    // A scaled render target: report the game's size, and fetch the matching texel at full resolution
    // (the pixel's own sub-position inside the scaled block when fetching from a pixel shader).
    const bool scaled = k_.scale != 1.0f && site.binding < 32 && ((k_.scaled_images >> site.binding) & 1);
    char sbuf[32];
    std::snprintf(sbuf, sizeof sbuf, "%.9g", double(k_.scale));
    std::string S = sbuf;
    if (S.find('.') == std::string::npos && S.find('e') == std::string::npos) S += ".0";
    // The game's size of a scaled target (rounded like the target itself).
    auto game_size = [&](const std::string& sz) { return "ivec2(round(vec2(" + sz + ") / " + S + "))"; };
    if (o.resinfo) {
        const std::string sz = "textureSize(" + tex + ", int(" + Vgpr(a) + ")).xy";
        Line("{ ivec2 sz_ = " + (scaled ? game_size(sz) : sz) + "; uvec4 r_ = uvec4(uint(sz_.x), uint(sz_.y), 1u, uint(textureQueryLevels(" + tex + ")));");
    } else if (o.load) {
        std::string at = "ivec2(int(" + Vgpr(a) + "), int(" + Vgpr(a + 1) + "))";
        // A game texel covers S x S target pixels: a pixel shader takes the one at its own position inside
        // its game pixel (so a full-screen pass reading its own pixel gets exactly it); others the centre.
        if (scaled)
            at = "ivec2(floor((vec2(" + at + ") + " +
                 (k_.stage == Stage::Pixel ? "fract(gl_FragCoord.xy / " + S + ")" : std::string("vec2(0.5)")) + ") * " + S + "))";
        Line("{ uvec4 r_ = floatBitsToUint(texelFetch(" + tex + ", " + at + ", " +
             (o.lod ? "int(" + Vgpr(a + 2) + ")" : std::string("0")) + "));");
    } else {
        std::string offset, bias, ref;
        if (o.offset) offset = Vgpr(a++);
        if (o.bias) bias = vf(a++);
        if (o.compare) ref = vf(a++);
        std::string coord;
        switch (type) {
            case ImageType::Tex1D: coord = vf(a); a += 1; break;
            case ImageType::Tex3D: case ImageType::Tex2DArray:
                coord = "vec3(" + vf(a) + ", " + vf(a + 1) + ", " + vf(a + 2) + ")"; a += 3; break;
            case ImageType::Cube: coord = "cube_dir(" + vf(a) + ", " + vf(a + 1) + ", " + vf(a + 2) + ")"; a += 3; break;
            default: coord = "vec2(" + vf(a) + ", " + vf(a + 1) + ")"; a += 2; break;
        }
        if (!offset.empty() && (type == ImageType::Tex2D || type == ImageType::Tex2DArray))
            coord = "apply_offset(" + coord + ", " + offset + ", " +
                    (scaled ? game_size("textureSize(" + tex + ", 0).xy") : "textureSize(" + tex + ", 0).xy") + ")";
        const bool vertex = k_.stage == Stage::Vertex;
        std::string lod = o.lz ? "0.0" : o.lod ? vf(a) : "";
        if (vertex && lod.empty()) lod = "0.0";  // no implicit derivatives outside pixel shaders
        std::string call;
        if (o.gather) {
            int comp = 0;
            while (comp < 4 && !(in.dmask & (1 << comp))) ++comp;
            call = o.compare ? "textureGather(" + tex + ", " + coord + ", " + ref + ")"
                             : "textureGather(" + tex + ", " + coord + ", " + std::to_string(comp & 3) + ")";
        } else if (o.compare) {
            const std::string c3 = type == ImageType::Tex2D ? "vec3(" + coord + ", " + ref + ")" : "vec4(" + coord + ", " + ref + ")";
            call = "vec4(" + (lod.empty() ? "texture(" + tex + ", " + c3 + ")" : "textureLod(" + tex + ", " + c3 + ", 0.0)") + ")";
        } else if (!lod.empty()) {
            call = "textureLod(" + tex + ", " + coord + ", " + lod + ")";
        } else if (!bias.empty()) {
            call = "texture(" + tex + ", " + coord + ", " + bias + ")";
        } else {
            call = "texture(" + tex + ", " + coord + ")";
        }
        Line("{ uvec4 r_ = floatBitsToUint(" + call + ");");
    }
    // dmask selects which components are written, packed into consecutive registers (gather: all four).
    uint32_t r = in.vdata;
    for (int c = 0; c < 4; ++c) {
        if (!o.gather && !(in.dmask & (1 << c))) continue;
        WriteV(in, r++, std::string("r_.") + "xyzw"[c]);
    }
    Line("}");
}

void Glsl::Export(const Inst& in) {
    std::string comps[4];
    if (in.compr) {
        const std::string lo = "unpackHalf2x16(" + Vgpr(in.vsrc[0]) + ")", hi = "unpackHalf2x16(" + Vgpr(in.vsrc[1]) + ")";
        comps[0] = lo + ".x"; comps[1] = lo + ".y"; comps[2] = hi + ".x"; comps[3] = hi + ".y";
    } else {
        for (int c = 0; c < 4; ++c) comps[c] = (in.en & (1 << c)) ? "F(" + Vgpr(in.vsrc[c]) + ")" : (c == 3 ? "1.0" : "0.0");
    }
    const std::string v4 = "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")";
    if (p_.stage == Stage::Vertex) {
        if (in.target == 12) Line("gl_Position = " + v4 + ";");
        else if (in.target >= 32) Line("out_param" + std::to_string(in.target - 32) + " = " + v4 + ";");
    } else {
        if (in.target < 8) {
            const uint8_t* m = k_.mrt_map[in.target];
            std::string swizzled = "vec4(" + comps[m[0]] + ", " + comps[m[1]] + ", " + comps[m[2]] + ", " + comps[m[3]] + ")";
            // LDPORT_DEBUG_PS=<shader address>:<vgpr>,<vgpr>,<vgpr>: that pixel shader outputs those
            // registers (as floats) instead of its colour, to read intermediate values from a dump.
            if ((k_.fp16_targets >> in.target) & 1) swizzled = "sat16(" + swizzled + ")";
            if (const std::string dbg_spec = DebugPsSpec(); !dbg_spec.empty()) {
                const char* dbg = dbg_spec.c_str();
                char* e = nullptr;
                const uint64_t a = std::strtoull(dbg, &e, 16);
                if (a == p_.address && e && *e == '@') {  // <address>@<byte pc>:...: values right after that instruction
                    swizzled = "vec4(F(dbg0_), F(dbg1_), F(dbg2_), 1.0)";
                } else if (a == p_.address && e && *e == ':') {
                    uint32_t r[3] = {0, 0, 0};
                    std::sscanf(e + 1, "%u,%u,%u", &r[0], &r[1], &r[2]);
                    swizzled = "vec4(F(" + Vgpr(r[0]) + "), F(" + Vgpr(r[1]) + "), F(" + Vgpr(r[2]) + "), 1.0)";
                }
            }
            Line("if (exec_lo != 0u) out_color" + std::to_string(in.target) + " = " + swizzled + ";");
        }
        else if (in.target == 8) Line("gl_FragDepth = " + comps[0] + ";");
        if (in.done && guard_exec_) Line("if (exec_lo == 0u) discard;");
    }
}

std::string Glsl::Branch(const Inst& in, bool& conditional) {
    conditional = true;
    switch (in.op) {
        case op::kSBranch: conditional = false; return "true";
        case op::kSCbranchScc0: return "scc == 0u";
        case op::kSCbranchScc1: return "scc != 0u";
        case op::kSCbranchVccz: return "(vcc_lo & 1u) == 0u";
        case op::kSCbranchVccnz: return "(vcc_lo & 1u) != 0u";
        case op::kSCbranchExecz: return "exec_lo == 0u";
        case op::kSCbranchExecnz: return "exec_lo != 0u";
    }
    conditional = false;
    return "";
}

constexpr const char* kPrelude = R"(#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Dwords { uint d[]; };
layout(push_constant, scalar) uniform Push { Dwords vs_data; Dwords ps_data; } pc;
#define F(x) uintBitsToFloat(x)
#define U(x) floatBitsToUint(x)
float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
// GCN float min/max with IEEE mode off (pixel/vertex shader default): a NaN operand yields the
// other operand, so max(0, NaN) == 0. Shaders rely on this to scrub NaNs; GLSL min/max do not.
float gmin(float a, float b) { return isnan(a) ? b : isnan(b) ? a : min(a, b); }
float gmax(float a, float b) { return isnan(a) ? b : isnan(b) ? a : max(a, b); }
// v_cvt_pkrtz_f16_f32 rounds toward zero, so finite values too large for a half saturate to
// +-65504 instead of becoming Inf (packHalf2x16 rounds to nearest: overflow -> Inf, which later
// passes turn into NaN - bright white-outs and lasers came out magenta/green).
float rtz16(float x) { return isnan(x) || isinf(x) ? x : clamp(x, -65504.0, 65504.0); }
uint pkrtz(float a, float b) { return packHalf2x16(vec2(rtz16(a), rtz16(b))); }
vec4 sat16(vec4 v) { return vec4(rtz16(v.x), rtz16(v.y), rtz16(v.z), rtz16(v.w)); }
float gsat(float x) { return isnan(x) ? 0.0 : clamp(x, 0.0, 1.0); }
float med3(float a, float b, float c) { return gmax(gmin(a, b), gmin(gmax(a, b), c)); }
uint bfe_u(uint v, uint f) { uint o = f & 31u, w = (f >> 16) & 127u; return w == 0u ? 0u : bitfieldExtract(v, int(o), int(min(w, 32u - o))); }
uint bfe_i(uint v, uint f) { uint o = f & 31u, w = (f >> 16) & 127u; return w == 0u ? 0u : uint(bitfieldExtract(int(v), int(o), int(min(w, 32u - o)))); }
float cube_id(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? 5.0 : 4.0;
    if (abs(y) >= abs(x)) return y < 0.0 ? 3.0 : 2.0;
    return x < 0.0 ? 1.0 : 0.0;
}
float cube_sc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? -x : x;
    if (abs(y) >= abs(x)) return x;
    return x < 0.0 ? z : -z;
}
float cube_tc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return -y;
    if (abs(y) >= abs(x)) return y < 0.0 ? -z : z;
    return -y;
}
float cube_ma(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return 2.0 * z;
    if (abs(y) >= abs(x)) return 2.0 * y;
    return 2.0 * x;
}
// GCN cube coordinates (s, t in [1, 2] after the usual sc/|ma| + 1.5, face id) back to a direction.
vec3 cube_dir(float s, float t, float face) {
    float sc = (s - 1.5) * 2.0, tc = (t - 1.5) * 2.0;
    int f = int(face + 0.5);
    if (f == 0) return vec3(1.0, -tc, -sc);
    if (f == 1) return vec3(-1.0, -tc, sc);
    if (f == 2) return vec3(sc, 1.0, tc);
    if (f == 3) return vec3(sc, -1.0, -tc);
    if (f == 4) return vec3(sc, -tc, 1.0);
    return vec3(-sc, -tc, -1.0);
}
vec2 apply_offset(vec2 uv, uint off, ivec2 size) {
    ivec2 o = ivec2(bitfieldExtract(int(off), 0, 6), bitfieldExtract(int(off), 8, 6));
    return uv + vec2(o) / vec2(size);
}
vec3 apply_offset(vec3 uv, uint off, ivec2 size) { return vec3(apply_offset(uv.xy, off, size), uv.z); }
)";

constexpr const char* kBufferLoad = R"(
uint load_u32(uint64_t a) {
    uint64_t al = a & ~uint64_t(3);
    uint sh = uint(a & uint64_t(3)) * 8u;
    Dwords p = Dwords(al);
    return sh == 0u ? p.d[0] : (p.d[0] >> sh) | (p.d[1] << (32u - sh));
}
float unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float snorm(uint v, uint bits) { int s = bitfieldExtract(int(v), 0, int(bits)); return max(float(s) / float((1 << (bits - 1u)) - 1), -1.0); }
float uf11(uint v) { return unpackHalf2x16(((v >> 6u) << 10u) | ((v & 0x3fu) << 4u)).x; }
float uf10(uint v) { return unpackHalf2x16(((v >> 5u) << 10u) | ((v & 0x1fu) << 5u)).x; }
uint conv(uint raw, uint bits, uint nfmt) {
    switch (nfmt) {
        case 0u: return U(unorm(raw, bits));
        case 1u: return U(snorm(raw, bits));
        case 2u: return U(float(raw));
        case 3u: return U(float(bitfieldExtract(int(raw), 0, int(bits))));
        case 4u: return raw;
        case 5u: return uint(bitfieldExtract(int(raw), 0, int(bits)));
        default: return bits == 16u ? U(unpackHalf2x16(raw).x) : raw;  // float
    }
}
uint sel(uvec4 v, uint s) {
    switch (s) { case 0u: return 0u; case 1u: return U(1.0); case 4u: return v.x; case 5u: return v.y; case 6u: return v.z; case 7u: return v.w; }
    return 0u;
}
uvec4 buffer_load(uint site, uint index, uint offset) {
    uint64_t base = packUint2x32(uvec2(D(site), D(site + 1u)));
    uint stride = D(site + 2u), records = D(site + 3u), dfmt = D(site + 4u), nfmt = D(site + 5u), dst = D(site + 6u);
    // Out of range reads return 0, as on the hardware: by index for structured buffers, by byte offset
    // for raw ones (stride 0, records = bytes). Device addresses have no bounds of their own.
    if (base == uint64_t(0) || (stride != 0u && index >= records) ||
        (stride == 0u && offset + D(site + 7u) + 4u > records)) return uvec4(0u, 0u, 0u, U(1.0));
    uint64_t a = base + uint64_t(index) * uint64_t(stride) + uint64_t(offset + D(site + 7u));
    uvec4 r = uvec4(0u, 0u, 0u, nfmt == 4u || nfmt == 5u ? 1u : U(1.0));
    uint w = load_u32(a);
    switch (dfmt) {
        case 1u: r.x = conv(w & 0xffu, 8u, nfmt); break;
        case 2u: r.x = conv(w & 0xffffu, 16u, nfmt); break;
        case 3u: r.x = conv(w & 0xffu, 8u, nfmt); r.y = conv((w >> 8u) & 0xffu, 8u, nfmt); break;
        case 4u: r.x = conv(w, 32u, nfmt); break;
        case 5u: r.x = conv(w & 0xffffu, 16u, nfmt); r.y = conv(w >> 16u, 16u, nfmt); break;
        case 6u: r.x = U(uf10(w & 0x3ffu)); r.y = U(uf11((w >> 10u) & 0x7ffu)); r.z = U(uf11(w >> 21u)); break;
        case 7u: r.x = U(uf11(w & 0x7ffu)); r.y = U(uf11((w >> 11u) & 0x7ffu)); r.z = U(uf10(w >> 22u)); break;
        case 8u: r.x = conv(w & 0x3ffu, 10u, nfmt); r.y = conv((w >> 10u) & 0x3ffu, 10u, nfmt); r.z = conv((w >> 20u) & 0x3ffu, 10u, nfmt); r.w = conv(w >> 30u, 2u, nfmt); break;
        case 9u: r.x = conv(w & 0x3u, 2u, nfmt); r.y = conv((w >> 2u) & 0x3ffu, 10u, nfmt); r.z = conv((w >> 12u) & 0x3ffu, 10u, nfmt); r.w = conv(w >> 22u, 10u, nfmt); break;
        case 10u: r.x = conv(w & 0xffu, 8u, nfmt); r.y = conv((w >> 8u) & 0xffu, 8u, nfmt); r.z = conv((w >> 16u) & 0xffu, 8u, nfmt); r.w = conv(w >> 24u, 8u, nfmt); break;
        case 11u: r.x = conv(w, 32u, nfmt); r.y = conv(load_u32(a + 4u), 32u, nfmt); break;
        case 12u: { uint w2 = load_u32(a + 4u); r.x = conv(w & 0xffffu, 16u, nfmt); r.y = conv(w >> 16u, 16u, nfmt); r.z = conv(w2 & 0xffffu, 16u, nfmt); r.w = conv(w2 >> 16u, 16u, nfmt); break; }
        case 13u: r.x = conv(w, 32u, nfmt); r.y = conv(load_u32(a + 4u), 32u, nfmt); r.z = conv(load_u32(a + 8u), 32u, nfmt); break;
        case 14u: r.x = conv(w, 32u, nfmt); r.y = conv(load_u32(a + 4u), 32u, nfmt); r.z = conv(load_u32(a + 8u), 32u, nfmt); r.w = conv(load_u32(a + 12u), 32u, nfmt); break;
    }
    return uvec4(sel(r, dst & 7u), sel(r, (dst >> 3u) & 7u), sel(r, (dst >> 6u) & 7u), sel(r, (dst >> 9u) & 7u));
}
)";

std::string Glsl::Generate() {
    // Exec only needs per-instruction guards if something other than the whole-quad prologue writes it.
    bool has_branch = false;
    for (const Inst& in : p_.code) {
        if (in.fmt == Fmt::SOPP && in.op >= op::kSBranch && in.op <= op::kSCbranchExecnz) has_branch = true;
        const bool scalar_dst = in.fmt == Fmt::SOP1 || in.fmt == Fmt::SOP2 || in.fmt == Fmt::SOPK;
        if (scalar_dst && in.dst == src::kExecLo && !(in.fmt == Fmt::SOP1 && in.op == op::kSWqmB64)) guard_exec_ = true;
        if (in.fmt == Fmt::SOP1 && (in.op == op::kSAndSaveexecB64 || in.op == op::kSOrSaveexecB64)) guard_exec_ = true;
        const Valu v = NormaliseValu(in);
        if (v.space == Fmt::VOPC && (v.op & 0x10)) guard_exec_ = true;
    }
    for (size_t s = 0; s < p_.sites.size(); ++s) site_by_inst_[p_.sites[s].inst_index] = s;

    // Branchy shaders become a dispatch loop over basic blocks, labelled by instruction index.
    std::map<uint32_t, uint32_t> index_of_pc;
    for (uint32_t i = 0; i < p_.code.size(); ++i)
        if (!(p_.code[i].pc & 0x80000000u)) index_of_pc[p_.code[i].pc] = i;
    std::set<uint32_t> block_starts = {0};
    auto target_of = [&](const Inst& in) -> int {
        auto it = index_of_pc.find(uint32_t(int32_t(in.pc) + 1 + in.simm16));
        return it == index_of_pc.end() ? -1 : int(it->second);
    };
    if (has_branch) {
        for (uint32_t i = 0; i < p_.code.size(); ++i) {
            const Inst& in = p_.code[i];
            if (in.fmt != Fmt::SOPP || in.op < op::kSBranch || in.op > op::kSCbranchExecnz) continue;
            if (target_of(in) >= 0) block_starts.insert(uint32_t(target_of(in)));
            if (i + 1 < p_.code.size()) block_starts.insert(i + 1);
        }
    }

    if (has_branch) body_ += "    int blk_ = 0;\n    while (blk_ >= 0) {\n    switch (blk_) {\n    case 0:\n";
    for (uint32_t i = 0; i < p_.code.size(); ++i) {
        const Inst& in = p_.code[i];
        if (has_branch && i > 0 && block_starts.count(i))
            body_ += "        blk_ = " + std::to_string(i) + "; break;\n    case " + std::to_string(i) + ":\n";
        auto site = site_by_inst_.find(i);
        switch (in.fmt) {
            case Fmt::SOP1: case Fmt::SOP2: case Fmt::SOPK: case Fmt::SOPC: Scalar(in); break;
            case Fmt::SOPP: {
                if (in.op == op::kSEndpgm) {
                    if (has_branch) body_ += "        blk_ = -1; break;\n";
                    break;
                }
                bool conditional = false;
                const std::string cond = Branch(in, conditional);
                if (cond.empty()) break;  // s_nop, s_waitcnt, ...
                const int target = target_of(in);
                const int next = i + 1 < p_.code.size() ? int(i + 1) : -1;
                body_ += "        blk_ = (" + cond + ") ? " + std::to_string(target) + " : " + std::to_string(next) +
                         "; break;\n";
                break;
            }
            case Fmt::SMRD: Smrd(in, p_.sites[site->second]); break;
            case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOPC: case Fmt::VOP3: Vector(in, i); break;
            case Fmt::VINTRP:
                if (in.op == 1 || in.op == 2)
                    WriteV(in, in.dst, "U(in_attr" + std::to_string(in.attr) + "." + "xyzw"[in.chan] + ")");
                break;
            case Fmt::MUBUF: case Fmt::MTBUF: BufferLoad(in, p_.sites[site->second]); break;
            case Fmt::MIMG: Image(in, p_.sites[site->second]); break;
            case Fmt::EXP: Export(in); break;
            case Fmt::DS: Ds(in); break;
            default: Line("// unhandled " + Disassemble(in));
        }
        // Debug snapshot (<address>@<byte pc>:<vgpr>,<vgpr>,<vgpr>): copy the registers after this instruction.
        if (const std::string dbg = DebugPsSpec(); !dbg.empty()) {
            char* e = nullptr;
            const uint64_t a = std::strtoull(dbg.c_str(), &e, 16);
            if (a == p_.address && e && *e == '@') {
                const uint32_t at = uint32_t(std::strtoul(e + 1, &e, 16));
                uint32_t r[3] = {0, 0, 0};
                if (e && *e == ':') std::sscanf(e + 1, "%u,%u,%u", &r[0], &r[1], &r[2]);
                if (in.pc * 4 == at)
                    Line("dbg0_ = " + Vgpr(r[0]) + "; dbg1_ = " + Vgpr(r[1]) + "; dbg2_ = " + Vgpr(r[2]) + ";");
            }
        }
    }
    if (has_branch) body_ += "        blk_ = -1; break;\n    default: blk_ = -1; break;\n    }\n    }\n";

    // Assemble: prelude, interface, register declarations, main.
    std::string out = kPrelude;
    if (uses_subgroup_)
        out.insert(out.find('\n') + 1, "#extension GL_KHR_shader_subgroup_quad : require\n"
                                       "#extension GL_KHR_shader_subgroup_shuffle : require\n");
    out += p_.stage == Stage::Vertex ? "uint D(uint i) { return pc.vs_data.d[i]; }\n"
                                     : "uint D(uint i) { return pc.ps_data.d[i]; }\n";
    out += kBufferLoad;
    for (const Site& s : p_.sites) {
        if (s.kind != Site::Image) continue;
        const ImageType t = s.binding < k_.image_types.size() ? k_.image_types[s.binding] : ImageType::Tex2D;
        const char* sampler = "sampler2D";
        switch (t) {
            case ImageType::Tex1D: sampler = s.depth_compare ? "sampler1DShadow" : "sampler1D"; break;
            case ImageType::Tex3D: sampler = "sampler3D"; break;
            case ImageType::Cube: sampler = s.depth_compare ? "samplerCubeShadow" : "samplerCube"; break;
            case ImageType::Tex1DArray: sampler = "sampler1DArray"; break;
            case ImageType::Tex2DArray: sampler = s.depth_compare ? "sampler2DArrayShadow" : "sampler2DArray"; break;
            default: sampler = s.depth_compare ? "sampler2DShadow" : "sampler2D"; break;
        }
        const uint32_t binding = s.binding + (p_.stage == Stage::Vertex ? 0 : kPixelBindingBase);
        out += "layout(set = 0, binding = " + std::to_string(binding) + ") uniform " + sampler +
               " tex" + std::to_string(s.binding) + ";\n";
    }
    uint32_t declared_locations = 0;
    for (uint32_t n = 0; n < 32; ++n) {
        if (p_.stage == Stage::Vertex && (p_.outputs_mask & (1u << n)))
            out += "layout(location = " + std::to_string(n) + ") out vec4 out_param" + std::to_string(n) + ";\n";
        if (p_.stage == Stage::Pixel && (p_.inputs_mask & (1u << n))) {
            if (!k_.ps_input_map) {
                out += "layout(location = " + std::to_string(n) + ") in vec4 in_attr" + std::to_string(n) + ";\n";
            } else {
                const uint32_t cntl = k_.ps_input_cntl[n];
                if (cntl & 0x20) {  // no vertex output: a constant
                    static const char* kDefault[4] = {"vec4(0.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(1.0, 1.0, 1.0, 0.0)",
                                                      "vec4(1.0)"};
                    out += "#define in_attr" + std::to_string(n) + " " + kDefault[(cntl >> 8) & 3] + "\n";
                } else {
                    const uint32_t loc = cntl & 0x1F;
                    const std::string name = "vin" + std::to_string(loc);
                    if (!(declared_locations & (1u << loc))) {
                        declared_locations |= 1u << loc;
                        out += "layout(location = " + std::to_string(loc) + ") " + ((cntl & 0x400) ? "flat " : "") +
                               "in vec4 " + name + ";\n";
                    }
                    out += "#define in_attr" + std::to_string(n) + " " + name + "\n";
                }
            }
        }
        if (p_.stage == Stage::Pixel && n < 8 && (p_.outputs_mask & (1u << n)))
            out += "layout(location = " + std::to_string(n) + ") out vec4 out_color" + std::to_string(n) + ";\n";
    }

    out += "void main() {\n    uint scc = 0u, m0 = 0u, s_null = 0u, vcc_lo = 0u, vcc_hi = 0u, exec_lo = 1u, exec_hi = 0u;\n";
    for (uint32_t s : sgprs_) {
        out += "    uint s" + std::to_string(s) + " = ";
        out += s < k_.user_sgprs && s < kUserDataDwords ? "D(" + std::to_string(s) + "u);\n" : "0u;\n";
    }
    for (uint32_t v : vgprs_) out += "    uint v" + std::to_string(v) + " = 0u;\n";
    for (const std::string& l : lanes_) out += "    uint " + l + " = 0u;\n";
    out += "    uint dbg0_ = 0u, dbg1_ = 0u, dbg2_ = 0u;\n";
    if (p_.stage == Stage::Vertex) {
        if (vgprs_.count(0)) out += "    v0 = uint(gl_VertexIndex);\n";
        if (vgprs_.count(3)) out += "    v3 = uint(gl_InstanceIndex);\n";
    } else {
        // Input VGPRs follow SPI_PS_INPUT_ENA: barycentrics first (replaced by real interpolation),
        // then fragment position components.
        static const uint32_t kSizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t reg = 0;
        for (uint32_t bit = 0; bit < 16; ++bit) {
            if (!(k_.ps_input_ena & (1u << bit))) continue;
            if (bit >= 8 && bit <= 11 && vgprs_.count(reg))
                out += "    v" + std::to_string(reg) + " = U(gl_FragCoord." + "xyzw"[bit - 8] +
                       (k_.scale != 1.0f && bit <= 9 ? " / " + ScaleLiteral(k_.scale) : std::string()) + ");\n";
            if (bit == 12 && vgprs_.count(reg)) out += "    v" + std::to_string(reg) + " = gl_FrontFacing ? 1u : 0u;\n";
            reg += kSizes[bit];
        }
    }
    out += body_;
    out += "}\n";
    return out;
}

}  // namespace

namespace {
std::mutex g_debug_ps_mutex;
std::string g_debug_ps = std::getenv("LDPORT_DEBUG_PS") ? std::getenv("LDPORT_DEBUG_PS") : "";
std::atomic<bool> g_debug_changed{false};
}  // namespace
std::string DebugPsSpec() {
    std::lock_guard lock(g_debug_ps_mutex);
    return g_debug_ps;
}
void SetDebugPsSpec(const std::string& spec) {
    {
        std::lock_guard lock(g_debug_ps_mutex);
        g_debug_ps = spec;
    }
    g_debug_changed = true;
}
bool TakeShaderDebugChanged() { return g_debug_changed.exchange(false); }

std::string GenerateGlsl(const ShaderProgram& program, const ShaderKey& key) {
    return Glsl(program, key).Generate();
}

}  // namespace gcn
