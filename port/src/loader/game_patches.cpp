// Small fixed patches to the game's code, applied after loading (before it runs). Each checks the
// original bytes first, so a different build of the executable is left alone. Offsets are image
// offsets (Ghidra address - 0x1000000). LDPORT_NO_PATCHES=1 skips them all.
#include "loader/game_patches.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hle/hle.h"
#include "hle/options_menu.h"
#include "hle/port_settings.h"
#include "host/memory.h"

namespace loader {
namespace {

struct Patch {
    const char* what;
    uint64_t offset;
    std::vector<uint8_t> original, replacement;
};

const std::vector<Patch>& Patches() {
    static const std::vector<Patch> patches = {};
    return patches;
}

// Skip any cutscene. The cutscene's "can skip" virtual (FUN_0273fd80) first rules out cutscenes flagged as
// never skippable (flags at +0x390 on it and its parents), then ends with
//     mov al, 1; cmp byte [global + 0x1f8], 0; jne done; mov al, [rbx + 0xe8]; and al, 1; done: ret
// i.e. "yes" when the global is set (everything unlocked), else the cutscene's own bit 0 at +0xe8 (watched).
// The whole function (159 bytes, the global's RIP offset left out of the match) must occur exactly once, or
// nothing is touched. Its entry is hooked (trampoline over the first 12 position-independent bytes); with the
// option on, the hook sets the watched bit for the length of the call, so only the final test changes and the
// never-skippable checks stay. Only the cutscene update's skip handling (FUN_02743500: "if it can be skipped,
// read the skip button") gets that answer; any other caller gets the game's own. That call site is found by
// its own pattern (kSkipCaller, exactly once) too; without it nothing is hooked. LDPORT_LOG_SKIPCHECK=1 logs
// each new caller. Idea from vascon-es/lego-dimensions-pc's cutscene_patch.h; the patch itself is different.
// The function and the call site are recognised by an FNV-1a hash of their bytes (the global's disp32 zeroed),
// filtered by their first bytes; the game's code itself isn't kept here.
constexpr size_t kSkipFnSize = 159;
constexpr uint64_t kSkipFnHash = 0x68932d80c5974f3dull;
constexpr size_t kSkipFnRip = 0x7e;     // the global's disp32 (lea rax, [rip + x]): not compared
constexpr size_t kSkipFnPrologue = 12;  // push rbp; mov rbp, rsp; push rbx; push rax; mov rbx, rdi; mov rax, [rbx]
constexpr uint8_t kSkipFnStart[kSkipFnPrologue] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50,
                                                   0x48, 0x89, 0xfb, 0x48, 0x8b, 0x03};
// FUN_02743500 at +0x21: cmp qword [rax+0xd0], 0; jne out; mov rax, [rbx]; mov rdi, rbx; call [rax+0x18];
// test al, al; je out (31 bytes). The return address is the TEST.
constexpr size_t kSkipCallerSize = 31;
constexpr uint64_t kSkipCallerHash = 0x1e0e23cadb780b31ull;
constexpr uint8_t kSkipCallerStart[4] = {0x48, 0x83, 0xb8, 0xd0};  // cmp qword [rax + 0xd0], ...
constexpr size_t kSkipCallerReturn = 0x17;

using SkipFn = SYSV uint64_t (*)(uint64_t cutscene);
uint8_t* g_base_for_log = nullptr;
uint64_t g_skip_caller = 0;  // the skip handling's return address
uint8_t* g_skip_fn = nullptr;
SkipFn g_skip_original = nullptr;
std::atomic<bool> g_skip_on{false};
bool g_skip_log = false;

// FNV-1a over n bytes, with the 4 bytes at `skip` (if any) taken as zero.
uint64_t Fnv(const uint8_t* p, size_t n, size_t skip = ~size_t(0)) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ (i >= skip && i - skip < 4 ? 0 : p[i])) * 0x100000001b3ull;
    return h;
}

bool MatchSkipFn(const uint8_t* p) {
    return std::memcmp(p, kSkipFnStart, kSkipFnPrologue) == 0 && Fnv(p, kSkipFnSize, kSkipFnRip) == kSkipFnHash;
}
bool MatchSkipCaller(const uint8_t* p) {
    return std::memcmp(p, kSkipCallerStart, sizeof kSkipCallerStart) == 0 && Fnv(p, kSkipCallerSize) == kSkipCallerHash;
}

SYSV uint64_t SkipHook(uint64_t cutscene) {
    const uint64_t caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    uint64_t r;
    if (caller == g_skip_caller && g_skip_on.load(std::memory_order_relaxed)) {
        auto* watched = reinterpret_cast<volatile uint8_t*>(cutscene + 0xe8);
        const uint8_t saved = *watched;
        *watched = saved | 1;
        r = g_skip_original(cutscene);
        *watched = saved;
    } else {
        r = g_skip_original(cutscene);
    }
    if (g_skip_log) {  // each new (caller, cutscene, answer) once
        static std::atomic<uint64_t> seen[64];
        const uint64_t key = (caller - reinterpret_cast<uint64_t>(g_base_for_log)) ^ (cutscene << 24) ^ (r & 1) << 63;
        bool known = false;
        for (auto& s : seen) {
            uint64_t v = s.load();
            if (v == key) { known = true; break; }
            if (v == 0 && s.compare_exchange_strong(v, key)) break;
        }
        if (!known)
            std::fprintf(stderr, "[skipcheck] caller %llx cutscene %llx -> %d (watched bit %d)%s\n",
                         (unsigned long long)(caller - reinterpret_cast<uint64_t>(g_base_for_log)),
                         (unsigned long long)cutscene, int(r & 0xff), *reinterpret_cast<uint8_t*>(cutscene + 0xe8) & 1,
                         caller == g_skip_caller && g_skip_on.load() ? " [skip any cutscene on]" : "");
    }
    return r;
}

void InstallSkipHook(uint8_t* base, size_t image_size) {
    g_base_for_log = base;
    g_skip_log = std::getenv("LDPORT_LOG_SKIPCHECK") != nullptr;
    uint8_t* found = nullptr;
    uint8_t* caller = nullptr;
    int count = 0, callers = 0;
    for (size_t off = 0; off + kSkipFnSize <= image_size; ++off) {
        if (base[off] == kSkipFnStart[0] && MatchSkipFn(base + off) && ++count == 1) found = base + off;
        if (base[off] == kSkipCallerStart[0] && MatchSkipCaller(base + off) && ++callers == 1) caller = base + off;
    }
    if (count != 1 || callers != 1) {
        std::fprintf(stderr, "[patch] skip any cutscene: check found %d times, skip handling %d times (want 1 "
                     "each), not hooked\n", count, callers);
        return;
    }
    g_skip_caller = reinterpret_cast<uint64_t>(caller + kSkipCallerReturn);
    uint8_t* tramp = host::AllocateImage(0x1000);
    host::SetProtection(tramp, 0x1000, host::kRead | host::kWrite | host::kExec);
    std::memcpy(tramp, found, kSkipFnPrologue);
    uint8_t* t = tramp + kSkipFnPrologue;
    const uint64_t back = reinterpret_cast<uint64_t>(found + kSkipFnPrologue);
    *t++ = 0x49; *t++ = 0xbb;  // movabs r11, back; jmp r11 (r11 is scratch in the SysV ABI)
    std::memcpy(t, &back, 8); t += 8;
    *t++ = 0x41; *t++ = 0xff; *t++ = 0xe3;
    g_skip_original = reinterpret_cast<SkipFn>(tramp);
    uint8_t h[kSkipFnPrologue] = {0x48, 0xb8};  // movabs rax, SkipHook; jmp rax
    const uint64_t hook = reinterpret_cast<uint64_t>(&SkipHook);
    std::memcpy(h + 2, &hook, 8);
    h[10] = 0xff; h[11] = 0xe0;
    std::memcpy(found, h, sizeof h);
    g_skip_fn = found;
    std::fprintf(stderr, "[patch] skip any cutscene: check at %llx hooked\n", (unsigned long long)(found - base));
}

}  // namespace

bool SetSkipAnyCutscene(bool on) {
    if (!g_skip_fn) return false;
    g_skip_on = on;
    std::fprintf(stderr, "[patch] skip any cutscene: %s\n", on ? "on" : "off");
    return true;
}

size_t ApplyGamePatches(uint8_t* base, size_t image_size) {
    if (std::getenv("LDPORT_NO_PATCHES")) return 0;
    size_t applied = 0;
    for (const Patch& p : Patches()) {
        if (p.offset + p.original.size() > image_size) continue;
        uint8_t* at = base + p.offset;
        if (std::memcmp(at, p.original.data(), p.original.size()) != 0) {
            std::fprintf(stderr, "[patch] %s: bytes differ at %llx, not applied\n", p.what, (unsigned long long)p.offset);
            continue;
        }
        std::memcpy(at, p.replacement.data(), p.replacement.size());
        std::fprintf(stderr, "[patch] %s\n", p.what);
        ++applied;
    }
    // Ultrawide (Display Settings, after a restart): the game's main buffers are made 1920 x 1080 with the
    // width as an immediate (FUN_01714870 / FUN_01714dd0 and the calls that create them). A wider width makes
    // the engine build and register wider display buffers and set wider viewports; its gameplay cameras follow
    // the viewport (cutscene cameras keep 16:9 and are widened by the renderer, see TrackCamera).
    port::Load();
    if (const uint32_t w = port::UltrawideWidth(port::BootUltrawide()); w != 1920) {
        int n = 0;
        for (uint64_t off : {0x7148f8ull, 0x714e04ull, 0x714e4bull, 0x714edcull, 0x714f23ull}) {
            uint32_t v;
            std::memcpy(&v, base + off, 4);
            if (v == 1920) {
                std::memcpy(base + off, &w, 4);
                ++n;
            }
        }
        std::fprintf(stderr, "[patch] ultrawide: screen %ux1080 (%d of 5 sites)\n", w, n);
    }
    // Local players (Display Settings: Local Players, after a restart; LDPORT_MAX_PLAYERS overrides). Drop-in
    // (MechDropInOut, FUN_021b9f50) lets a pad join while the active players are fewer than a global limit
    // (FUN_024b5a80 reads it; 2 in the image). Its setter (FUN_024b5f90: lea rax,[limit]; mov [rax],edi; ret) is
    // called with 4 by some modes and 2 by others; with 3 or 4 here the limit starts at N and the setter's store
    // is skipped. Each area also has a "max_players" (2-4, read in FUN_02a29d50; default 2 via
    // mov byte [r14+0x298], 2), raised to N as well. The engine's own 4-player split screen and HUD do the rest.
    // With 2 nothing is touched.
    if (const int n = int(port::BootLocalPlayers()); n > 2) {
        static const uint8_t kAreaDefault[] = {0x41, 0xc6, 0x86, 0x98, 0x02, 0x00, 0x00, 0x02};
        static const uint8_t kSetterStore[] = {0x89, 0x38, 0xc3};
        constexpr uint64_t kAreaAt = 0x1a2b347, kSetterAt = 0x14b5f97, kLimitAt = 0x2e76b30;
        int32_t limit = 0;
        if (kLimitAt + 4 <= image_size) std::memcpy(&limit, base + kLimitAt, 4);
        if (kAreaAt + sizeof kAreaDefault <= image_size &&
            std::memcmp(base + kAreaAt, kAreaDefault, sizeof kAreaDefault) == 0 &&
            std::memcmp(base + kSetterAt, kSetterStore, sizeof kSetterStore) == 0 && limit == 2) {
            base[kAreaAt + 7] = uint8_t(n);
            base[kSetterAt] = base[kSetterAt + 1] = 0x90;
            limit = n;
            std::memcpy(base + kLimitAt, &limit, 4);
            std::fprintf(stderr, "[patch] local players: up to %d\n", n);
        } else {
            std::fprintf(stderr, "[patch] local players %d: game code differs, left at 2\n", n);
        }
    }
    InstallSkipHook(base, image_size);
    if (port::Get().skip_cutscenes.load()) SetSkipAnyCutscene(true);
    port::InstallOptionsMenu(base);  // the port's settings rows in the game's Options page
    return applied;
}

}  // namespace loader
