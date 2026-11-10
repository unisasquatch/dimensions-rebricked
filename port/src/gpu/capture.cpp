#include "gpu/capture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace gpu {
namespace {

constexpr uint32_t kSEndpgm = 0xBF810000;  // s_endpgm
constexpr uint32_t kMaxShaderDwords = 0x10000;

FILE* Out(void* f) { return static_cast<FILE*>(f); }

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

Capture& Capture::Get() {
    static Capture c;
    return c;
}

Capture::Capture() {
    if (const char* v = std::getenv("LDPORT_CAPTURE")) target_frame_ = std::atoll(v);
    rolling_ = std::getenv("LDPORT_FLAT_TRAP") != nullptr;
}

void Capture::SaveRolling(const char* name) {
    if (!rolling_ || !active_) return;
    std::fflush(Out(file_));
    std::error_code ec;
    std::filesystem::copy_file("run/capture/rolling.txt", std::string("run/capture/") + name,
                               std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::copy_file("run/capture/rolling_prev.txt", std::string("run/capture/prev_") + name,
                               std::filesystem::copy_options::overwrite_existing, ec);
    save_next_ = name;
    std::fprintf(stderr, "[capture] saved the current frame as run/capture/%s (%u draws so far)\n", name, draws_);
}

void Capture::Note(const char* line) {
    if (active_ && file_) std::fprintf(Out(file_), "%s\n", line);
}

void Capture::OnFrameStart(uint64_t frame) {
    if (rolling_) {
        if (file_) std::fclose(Out(file_));
        std::filesystem::create_directories("run/capture/shaders");
        std::error_code ec;  // keep the previous frame too, so a trap can compare
        std::filesystem::rename("run/capture/rolling.txt", "run/capture/rolling_prev.txt", ec);
        if (!save_next_.empty()) {  // the frame that just ended follows a trapped one
            std::filesystem::copy_file("run/capture/rolling_prev.txt", "run/capture/next_" + save_next_,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            save_next_.clear();
        }
        file_ = std::fopen("run/capture/rolling.txt", "w");
        active_ = file_ != nullptr;
        if (active_) std::fprintf(Out(file_), "# frame %llu\n", static_cast<unsigned long long>(frame));
        draws_ = 0;
        return;
    }
    if (active_) {
        std::fprintf(Out(file_), "# end of frame: %u draws\n", draws_);
        std::fclose(Out(file_));
        std::fprintf(stderr, "[capture] wrote frame %lld (%u draws)\n", static_cast<long long>(target_frame_), draws_);
        active_ = false;
        target_frame_ = -1;
    }
    if (target_frame_ == -2) target_frame_ = int64_t(frame);
    if (target_frame_ < 0 || int64_t(frame) != target_frame_) return;
    std::filesystem::create_directories("run/capture/shaders");
    file_ = std::fopen(("run/capture/frame_" + std::to_string(frame) + ".txt").c_str(), "w");
    active_ = file_ != nullptr;
    draws_ = 0;
}

void Capture::DumpShader(uint64_t address, const char* stage) {
    static std::set<uint64_t> dumped;
    if (!address || !dumped.insert(address).second) return;
    const auto* code = reinterpret_cast<const uint32_t*>(address);
    uint32_t n = 0;
    while (n < kMaxShaderDwords && code[n] != kSEndpgm) ++n;
    if (n == kMaxShaderDwords) return;
    const std::string path = "run/capture/shaders/" + std::string(stage) + "_" + Hex(address) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(code, 4, n + 1, f);
        std::fclose(f);
    }
}

void Capture::OnDraw(const Registers& r, const DrawInfo& d) {
    using namespace reg;
    const auto& sh = r.sh;
    const auto& cx = r.context;
    const uint64_t vs = ShaderAddress(sh[kSpiShaderPgmLoVs], sh[kSpiShaderPgmLoVs + 1]);
    const uint64_t ps = ShaderAddress(sh[kSpiShaderPgmLoPs], sh[kSpiShaderPgmLoPs + 1]);
    DumpShader(vs, "vs");
    DumpShader(ps, "ps");
    // Gnm vertex shaders call a fetch subroutine whose address is in user SGPRs 0-1.
    if (UserSgprCount(sh[kSpiShaderPgmRsrc2Vs]) >= 2)
        DumpShader(uint64_t(sh[kSpiShaderUserDataVs0]) | (uint64_t(sh[kSpiShaderUserDataVs0 + 1]) << 32), "fs");

    FILE* f = Out(file_);
    std::fprintf(f, "draw %u: prim %u %s count %u inst %u idx_type %u idx_addr %s\n", draws_++,
                 r.uconfig[kVgtPrimitiveType], d.indexed ? "indexed" : "auto", d.index_count, d.instances,
                 d.index_type, Hex(d.index_address).c_str());
    std::fprintf(f, "  vs %s ps %s col_format %08x ps_input %08x\n", Hex(vs).c_str(), Hex(ps).c_str(),
                 cx[kSpiShaderColFormat], cx[kSpiPsInputEna]);
    std::fprintf(f, "  vs_user");
    for (uint32_t i = 0; i < UserSgprCount(sh[kSpiShaderPgmRsrc2Vs]); ++i)
        std::fprintf(f, " %08x", sh[kSpiShaderUserDataVs0 + i]);
    std::fprintf(f, "\n  ps_user");
    for (uint32_t i = 0; i < UserSgprCount(sh[kSpiShaderPgmRsrc2Ps]); ++i)
        std::fprintf(f, " %08x", sh[kSpiShaderUserDataPs0 + i]);
    std::fprintf(f, "\n");
    for (uint32_t rt = 0; rt < 8; ++rt) {
        const uint32_t b = kCbColor0Base + rt * kCbColorStride;
        if (!cx[b] || !((cx[kCbTargetMask] >> (rt * 4)) & 0xF)) continue;
        std::fprintf(f, "  cb%u base %s pitch %08x slice %08x info %08x attrib %08x\n", rt,
                     Hex(uint64_t(cx[b]) << 8).c_str(), cx[b + kCbColorPitch], cx[b + kCbColorSlice],
                     cx[b + kCbColorInfo], cx[b + kCbColorAttrib]);
    }
    if (cx[kDbZInfo] & 3)
        std::fprintf(f, "  depth base %s z_info %08x size %08x depth_control %08x\n",
                     Hex(uint64_t(cx[kDbZReadBase]) << 8).c_str(), cx[kDbZInfo], cx[kDbDepthSize],
                     cx[kDbDepthControl]);
    if ((cx[kDbZInfo] & 3) && (cx[kDbDepthControl] & 1))  // stencil: DB_STENCIL_CONTROL, DB_STENCILREFMASK(_BF), clear
        std::fprintf(f, "  stencil control %08x refmask %08x refmask_bf %08x info %08x clear %08x\n",
                     cx[0x10B], cx[0x10C], cx[0x10D], cx[0x011], cx[0x00A]);
    std::fprintf(f, "  blend0 %08x color_control %08x su_sc_mode %08x\n", cx[kCbBlend0Control],
                 cx[kCbColorControl], cx[kPaSuScModeCntl]);
}

void Capture::OnDispatch(const Registers& r, uint32_t x, uint32_t y, uint32_t z) {
    using namespace reg;
    const uint64_t cs = ShaderAddress(r.sh[kComputePgmLo], r.sh[kComputePgmLo + 1]);
    DumpShader(cs, "cs");
    FILE* f = Out(file_);
    std::fprintf(f, "dispatch: cs %s groups %u %u %u\n  cs_user", Hex(cs).c_str(), x, y, z);
    for (uint32_t i = 0; i < UserSgprCount(r.sh[kComputePgmRsrc2]); ++i)
        std::fprintf(f, " %08x", r.sh[kComputeUserData0 + i]);
    std::fprintf(f, "\n");
}

}  // namespace gpu
