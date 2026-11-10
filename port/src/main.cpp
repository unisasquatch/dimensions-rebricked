#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "host/memory.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

#include "gpu/command_processor.h"
#include "hle/keyboard_mouse.h"
#include "hle/toypad.h"
#include "hle/videoout.h"

#include "hle/hle.h"
#include "host/guest_memory.h"
#include "host/thread_tls.h"
#include "loader/game_patches.h"
#include "loader/nid.h"
#include "loader/orbis_elf.h"
#include "hle/toypad_source.h"
#include "hle/toypad_web.h"
#include "host/save_transfer.h"
#include "host/shadps4_import.h"

namespace hle {
std::vector<std::pair<std::string, ThreadHandle>> GuestThreads();
}

namespace {

loader::OrbisElf g_elf;
std::vector<loader::Import> g_stub_imports;     // index = stub number
std::map<std::string, std::string> g_names;     // nid -> symbol, from docs/imports.tsv

std::string Describe(const loader::Import& imp) {
    auto it = g_names.find(imp.nid);
    return imp.library + "::" + (it != g_names.end() ? it->second : imp.nid);
}

// Every import without a host implementation lands here (stage 2: report and stop).
extern "C" SYSV void UnresolvedImport(uint32_t index) {
    std::fprintf(stderr, "[stub] game called unimplemented %s\n", Describe(g_stub_imports[index]).c_str());
    std::exit(2);
}

// Emits `mov edi, index; mov rax, UnresolvedImport; jmp rax` for one import.
uint64_t MakeTrapStub(uint8_t*& cursor, uint32_t index) {
    uint8_t* p = cursor;
    p[0] = 0xBF;
    std::memcpy(p + 1, &index, 4);
    p[5] = 0x48; p[6] = 0xB8;
    const uint64_t handler = reinterpret_cast<uint64_t>(&UnresolvedImport);
    std::memcpy(p + 7, &handler, 8);
    p[15] = 0xFF; p[16] = 0xE0;
    cursor += 32;
    return reinterpret_cast<uint64_t>(p);
}

void LoadNames(const char* path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string lib, sym, nid;
        if (std::getline(row, lib, '\t') && std::getline(row, sym, '\t') && std::getline(row, nid, '\t'))
            g_names[nid] = sym;
    }
}

#ifdef _WIN32
std::string WhereIs(CONTEXT ctx);

// --- LDPORT_TRACE=<offset>[,<offset>...]: log every call to those game functions ---
// Each entry gets an int3; the handler logs the call, steps over the original byte with the trap
// flag, then re-arms. Calls racing on other threads during that single step are missed.
struct TracePoint { uint64_t address; uint8_t original; };
TracePoint g_trace_points[16];
int g_trace_count = 0;
thread_local uint64_t t_rearm = 0;
uint64_t g_wf_func = 0, g_wf_field = 0, g_wf_address = 0;
bool g_wf_armed = false;
int g_wf_reg = 0;      // 0 rdi, 1 rsi, 2 rdx, 3 rcx
int g_wf_nth = 1;      // arm on this call
int g_wf_calls = 0;

void PatchCodeByte(uint64_t address, uint8_t value) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile uint8_t*>(address) = value;
    VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);
}

// Copies a printable C string at `p` into out (empty if p is not one).
void TryString(uint64_t p, char* out, size_t size) {
    out[0] = 0;
    if (p < 0x10000) return;
    // ReadProcessMemory fails cleanly on unreadable memory; faulting inside the vectored handler does not.
    char s[128];
    SIZE_T got = 0;
    const size_t want = std::min(size, sizeof s);
    for (size_t chunk = want; chunk >= 8 && !got; chunk /= 2)
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), s, chunk, &got)) got = 0;
    size_t i = 0;
    for (; i + 1 < size && i < got && s[i]; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7e) { out[0] = 0; return; }
        out[i] = s[i];
    }
    out[i] = 0;
    if (i < 3) out[0] = 0;
}

uint64_t TryRead64(uint64_t p) {
    uint64_t v = 0;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), &v, 8, &got) ? v : 0;
}

void SetupTracePoints() {
    if (const char* wf = std::getenv("LDPORT_WATCH_FIELD")) {
        char* colon = nullptr;
        g_wf_func = std::strtoull(wf, &colon, 16);
        // <func>:<field>[:<reg 0-3 = rdi,rsi,rdx,rcx>[:<nth call>]]
        if (colon && *colon == ':') {
            char* rest = nullptr;
            g_wf_field = std::strtoull(colon + 1, &rest, 16);
            if (rest && *rest == ':') {
                g_wf_reg = int(std::strtol(rest + 1, &rest, 10)) & 3;
                if (rest && *rest == ':') g_wf_nth = int(std::strtol(rest + 1, nullptr, 10));
            }
        }
        // The watch is armed from that function's trace point.
        static std::string combined;
        const char* existing = std::getenv("LDPORT_TRACE");
        char buf[32];
        std::snprintf(buf, sizeof buf, "%llx", (unsigned long long)g_wf_func);
        combined = existing ? std::string(existing) + "," + buf : buf;
        _putenv_s("LDPORT_TRACE", combined.c_str());
    }
    const char* list = std::getenv("LDPORT_TRACE");
    if (!list) return;
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    for (const char* p = list; *p && g_trace_count < 16;) {
        char* end = nullptr;
        const uint64_t offset = std::strtoull(p, &end, 16);
        if (end == p) break;
        TracePoint& tp = g_trace_points[g_trace_count++];
        tp.address = base + offset;
        tp.original = *reinterpret_cast<const uint8_t*>(tp.address);
        PatchCodeByte(tp.address, 0xCC);
        std::fprintf(stderr, "[trace] armed image+%#llx\n", (unsigned long long)offset);
        p = *end ? end + 1 : end;
    }
}

// --- LDPORT_WATCH_FIELD=<func offset>:<field offset>: hardware write watchpoint ---
// On the first call to the traced function, watches the 4 aligned bytes at (rdi + field) on every
// guest thread and logs each write with the writer's location and a few return addresses.

void ArmWatchpoint(CONTEXT& self, uint64_t address) {
    const uint64_t dr7 = 1ull | (1ull << 16) | (3ull << 18);  // L0, break on write, 4 bytes
    self.Dr0 = address;
    self.Dr7 = dr7;
    int armed = 1;
    for (const auto& [name, handle] : hle::GuestThreads()) {
        if (GetThreadId(handle) == GetCurrentThreadId()) continue;
        SuspendThread(handle);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(handle, &c)) {
            c.Dr0 = address;
            c.Dr7 = dr7;
            if (SetThreadContext(handle, &c)) ++armed;
        }
        ResumeThread(handle);
    }
    std::fprintf(stderr, "[watchpoint] %llx armed on %d threads\n", (unsigned long long)address, armed);
}

bool HandleWatchpoint(CONTEXT& c) {
    if (!(c.Dr6 & 1)) return false;
    c.Dr6 = 0;
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    uint32_t value = uint32_t(TryRead64(g_wf_address));
    char line[512];
    int n = std::snprintf(line, sizeof line, "[watchpoint] write -> %08x at +%#llx; stack:", value,
                          (unsigned long long)(c.Rip - base));
    int shown = 0;
    for (int i = 0; i < 64 && shown < 6 && n < 450; ++i) {
        const uint64_t v = TryRead64(c.Rsp + i * 8);
        if (v > base && v < base + g_elf.image_size()) {
            n += std::snprintf(line + n, sizeof line - n, " +%llx", (unsigned long long)(v - base));
            ++shown;
        }
    }
    std::fprintf(stderr, "%s\n", line);
    return true;
}

bool HandleTracePoint(EXCEPTION_POINTERS* info) {
    CONTEXT& c = *info->ContextRecord;
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && g_wf_armed && HandleWatchpoint(c) && !t_rearm) return true;
    if (code == EXCEPTION_SINGLE_STEP && t_rearm) {
        PatchCodeByte(t_rearm, 0xCC);
        t_rearm = 0;
        c.EFlags &= ~0x100u;
        return true;
    }
    if (code != EXCEPTION_BREAKPOINT) return false;
    const uint64_t at = reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < g_trace_count; ++i) {
        const TracePoint& tp = g_trace_points[i];
        if (tp.address != at) continue;
        const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
        if (!g_wf_armed && g_wf_func && at == base + g_wf_func && ++g_wf_calls == g_wf_nth) {
            g_wf_armed = true;
            const uint64_t regs[4] = {c.Rdi, c.Rsi, c.Rdx, c.Rcx};
            g_wf_address = (regs[g_wf_reg] + g_wf_field) & ~3ull;
            ArmWatchpoint(c, g_wf_address);
        }
        const uint64_t args[4] = {c.Rdi, c.Rsi, c.Rdx, c.Rcx};
        char line[1024];
        int n = std::snprintf(line, sizeof line, "[trace] %.2fs +%#llx from +%#llx:", GetTickCount64() / 1000.0, (unsigned long long)(at - base),
                              (unsigned long long)(TryRead64(c.Rsp) - base));
        // LDPORT_TRACE_FIELD=<hex offset>: also show the dword at that offset in the first argument.
        static const char* field_env = std::getenv("LDPORT_TRACE_FIELD");
        if (field_env) {
            const uint64_t off = std::strtoull(field_env, nullptr, 16);
            n += std::snprintf(line + n, sizeof line - n, " {+%llx=%08x}", (unsigned long long)off,
                               uint32_t(TryRead64(c.Rdi + off)));
        }
        for (int a = 0; a < 4 && n < 900; ++a) {
            char s[96];
            TryString(args[a], s, sizeof s);
            if (s[0]) {
                n += std::snprintf(line + n, sizeof line - n, " %llx \"%s\"", (unsigned long long)args[a], s);
            } else if (args[a] >= 0x10000) {  // pointer: show the dword it points at
                uint64_t v = 0;
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(args[a]), &v, 8, &got) &&
                    v >= base && v < base + g_elf.image_size())  // an object: show its vtable's image offset
                {
                    // Game objects that carry a name pointer at +8 (events, for example): show it.
                    char name[96];
                    TryString(TryRead64(args[a] + 8), name, sizeof name);
                    n += std::snprintf(line + n, sizeof line - n, name[0] ? " %llx[vt +%llx \"%s\"]" : " %llx[vt +%llx]",
                                       (unsigned long long)args[a], (unsigned long long)(v - base), name);
                } else if (got)
                    n += std::snprintf(line + n, sizeof line - n, " %llx[%x]", (unsigned long long)args[a], uint32_t(v));
                else
                    n += std::snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)args[a]);
            } else {
                n += std::snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)args[a]);
            }
        }
        std::fprintf(stderr, "%s\n", line);
        PatchCodeByte(at, tp.original);
        c.Rip = at;
        c.EFlags |= 0x100u;  // single-step the original instruction, then re-arm
        t_rearm = at;
        return true;
    }
    return false;
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C) return EXCEPTION_CONTINUE_SEARCH;
    if (g_trace_count && HandleTracePoint(info)) return EXCEPTION_CONTINUE_EXECUTION;
    const uint64_t rip = info->ContextRecord->Rip;
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    if (rip >= base && rip < base + g_elf.image_size())
        std::fprintf(stderr, "[crash] exception %#lx in game code at image+%#llx\n", code,
                     static_cast<unsigned long long>(rip - base));
    else
        std::fprintf(stderr, "[crash] exception %#lx at %s\n", code, WhereIs(*info->ContextRecord).c_str());
    PWSTR thread_name = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &thread_name)) && thread_name) {
        std::fprintf(stderr, "[crash] on thread '%ls'\n", thread_name);
        LocalFree(thread_name);
    }
    // The game's out-of-memory handler (image+0x4a8410) leaves its report in the memory manager
    // (R15) at +0x65b0, then traps with ud2.
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && rip - base == 0x4a8797)
        std::fprintf(stderr, "[crash] game report:\n%s\n",
                     reinterpret_cast<const char*>(info->ContextRecord->R15 + 0x65b0));
    std::fflush(stdout);  // the game's own last log lines
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#ifdef _WIN32
// Diagnostics: where a thread is. Host frames are unwound with Windows unwind data until the
// instruction pointer reaches game code; the result is the exact game call site (image offset;
// Ghidra shows it at 0x1000000 + offset) plus the host frames above it.
constexpr int kMaxFrames = 48;

// Collects instruction pointers by unwinding host frames until one lands in game code.
// Allocates nothing and takes no locks, so it is safe while the target thread is suspended;
// faults from reading a changing stack are caught. Returns the frame count, or -1.
int WalkFrames(CONTEXT ctx, uint64_t* rips, uint64_t* game_rsp = nullptr) {
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    int n = 0;
    __try {
        while (n < kMaxFrames && ctx.Rip) {
            rips[n++] = ctx.Rip;
            if (ctx.Rip >= base && ctx.Rip < base + g_elf.image_size()) {
                if (game_rsp) *game_rsp = ctx.Rsp;
                break;
            }
            DWORD64 image_base = 0;
            RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (!fn) {  // leaf function: return address is on top of the stack
                ctx.Rip = *reinterpret_cast<uint64_t*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

std::string FormatFrames(const uint64_t* rips, int n) {
    if (n < 0) return "(stack walk failed)";
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    std::string out;
    for (int i = 0; i < n; ++i) {
        char buf[96];
        if (rips[i] >= base && rips[i] < base + g_elf.image_size()) {
            std::snprintf(buf, sizeof buf, "game image+%#llx", (unsigned long long)(rips[i] - base));
            return out + buf;
        }
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rips[i]), &module);
        char name[MAX_PATH] = "?";
        if (module) GetModuleFileNameA(module, name, MAX_PATH);
        const char* leaf = std::strrchr(name, '\\');
        std::snprintf(buf, sizeof buf, "%s+%#llx <- ", leaf ? leaf + 1 : name,
                      (unsigned long long)(rips[i] - reinterpret_cast<uint64_t>(module)));
        out += buf;
    }
    return out + "(no game frame found)";
}

std::string WhereIs(CONTEXT ctx) {
    uint64_t rips[kMaxFrames];
    return FormatFrames(rips, WalkFrames(ctx, rips));
}

// True if the bytes before `ret` in the game image end with a call instruction.
bool FollowsCall(uint64_t ret) {
    const auto* p = reinterpret_cast<const uint8_t*>(ret);
    if (p[-5] == 0xE8) return true;                                    // call rel32
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;         // call r/m (register)
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;         // call [r+disp8]
    if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;         // call [r+disp32] / [rip+disp32]
    if (p[-4] == 0xFF && ((p[-3] >> 3) & 7) == 2) return true;         // call [r+sib+disp8]
    if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;         // call [r+sib+disp32]
    return false;
}

bool SafeFollowsCall(uint64_t ret) {
    __try { return FollowsCall(ret); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
size_t CopyStack(uint64_t rsp, uint64_t* out, size_t max) {
    size_t i = 0;
    __try {
        for (; i < max; ++i) out[i] = reinterpret_cast<const uint64_t*>(rsp)[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return i;
}

std::string WhereIs(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    uint64_t rips[kMaxFrames];
    uint64_t game_rsp = 0;
    static uint64_t stack_copy[2048];  // 16 KB of the guest stack, copied while suspended
    static const bool deep = std::getenv("LDPORT_DEEP_STACKS") != nullptr;
    size_t copied = 0;
    SuspendThread(thread);  // suspended only for the walk, which takes no locks
    GetThreadContext(thread, &ctx);
    const int n = WalkFrames(ctx, rips, &game_rsp);
    if (deep && game_rsp) {  // stay inside the stack's committed region
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(game_rsp), &mbi, sizeof mbi)) {
            const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            copied = CopyStack(game_rsp, stack_copy, std::min<size_t>(2048, (end - game_rsp) / 8));
        }
    }
    ResumeThread(thread);
    std::string out = FormatFrames(rips, n);
    if (copied) {  // heuristic guest backtrace: stack slots holding return addresses into the game
        const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
        int shown = 0;
        for (size_t i = 0; i < copied && shown < 24; ++i) {
            const uint64_t v = stack_copy[i];
            if (v < base + 16 || v >= base + g_elf.image_size()) continue;
            if (!SafeFollowsCall(v)) continue;
            char buf[32];
            std::snprintf(buf, sizeof buf, " <= +%#llx", (unsigned long long)(v - base));
            out += buf;
            ++shown;
        }
    }
    return out;
}

#endif  // _WIN32

// What the PS4 kernel passes to a process entry point in rdi.
struct EntryParams {
    int32_t argc;
    uint32_t pad;
    const char* argv[3];
};

// Console control for the virtual Toy Pad:
//   place <zone>:<figure id>   zone 1 = center, 2 = left, 3 = right
//   remove <slot>              slot as shown by `list`
//   list
void RunToyPadCommand(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    auto& pad = toypad::Instance();
    if (cmd == "place") {
        int zone = 0;
        char colon = 0;
        uint32_t id = 0;
        if (!(in >> zone >> colon >> id) || colon != ':' || zone < 1 || zone > 3) {
            std::fprintf(stderr, "[toypad] usage: place <zone 1-3>:<figure id>\n");
            return;
        }
        const int slot = pad.Place(static_cast<toypad::Zone>(zone), id);
        std::fprintf(stderr, slot < 0 ? "[toypad] pad is full\n" : "[toypad] placed figure %u in slot %d\n", id, slot);
    } else if (cmd == "remove") {
        int slot = -1;
        in >> slot;
        std::fprintf(stderr, pad.Remove(slot) ? "[toypad] removed slot %d\n" : "[toypad] slot %d is empty\n", slot);
    } else if (cmd == "list") {
        std::fprintf(stderr, "%s", pad.Describe().c_str());
    } else if (!cmd.empty()) {
        std::fprintf(stderr, "[toypad] commands: place <zone>:<id>, remove <slot>, list\n");
    }
}

constexpr size_t kGuestMainStack = 0x1000000;  // 16 MB

#ifdef _WIN32
DWORD WINAPI GuestMain(void*) {
    const auto& tls = g_elf.tls();
    host::CreateGuestThreadTls(g_elf.base() + tls.vaddr, tls.filesz, tls.memsz, tls.align);
    static EntryParams params{1, 0, {"eboot.bin", nullptr, nullptr}};
    using Entry = SYSV void (*)(EntryParams*, void (*)());
    reinterpret_cast<Entry>(g_elf.base() + g_elf.entry())(&params, nullptr);
    return 0;
}
#else
void* GuestMain(void*) {
    const auto& tls = g_elf.tls();
    host::CreateGuestThreadTls(g_elf.base() + tls.vaddr, tls.filesz, tls.memsz, tls.align);
    static EntryParams params{1, 0, {"eboot.bin", nullptr, nullptr}};
    using Entry = SYSV void (*)(EntryParams*, void (*)());
    reinterpret_cast<Entry>(g_elf.base() + g_elf.entry())(&params, nullptr);
    return nullptr;
}

// Fatal signals: where it happened (game image offset or host address), then the default action.
void CrashSignal(int sig, siginfo_t* info, void* uc_void) {
    const auto* uc = static_cast<const ucontext_t*>(uc_void);
    const uint64_t rip = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
    const uint64_t base = reinterpret_cast<uint64_t>(g_elf.base());
    char name[32] = "?";
    pthread_getname_np(pthread_self(), name, sizeof name);
    char line[256];
    int n;
    if (rip >= base && rip < base + g_elf.image_size())
        n = std::snprintf(line, sizeof line, "[crash] signal %d (address %p) in game code at image+%#llx on thread '%s'\n",
                          sig, info->si_addr, (unsigned long long)(rip - base), name);
    else
        n = std::snprintf(line, sizeof line, "[crash] signal %d (address %p) at host %#llx on thread '%s'\n", sig,
                          info->si_addr, (unsigned long long)rip, name);
    if (n > 0) (void)!write(2, line, size_t(n));
    if (sig == SIGILL && rip - base == 0x4a8797) {  // the game's out-of-memory report (see the Windows handler)
        const char* report = reinterpret_cast<const char*>(uc->uc_mcontext.gregs[REG_R15] + 0x65b0);
        (void)!write(2, report, std::strlen(report));
        (void)!write(2, "\n", 1);
    }
    std::fflush(stdout);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

}  // namespace

namespace hle {
[[noreturn]] void GuestExit(int status);
std::vector<std::pair<std::string, ThreadHandle>> GuestThreads();
extern std::atomic<bool> g_thread_dump_requested;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the game's prints in order with ours
    if (argc >= 2 && std::strcmp(argv[1], "--import-shadps4") == 0)  // ldport --import-shadps4 <path> --data <dir>
        return host::ImportShadps4(std::vector<std::string>(argv + 2, argv + argc));
    if (argc >= 2 && std::strcmp(argv[1], "--save-transfer") == 0)  // shadPS4 / Xenia / Cemu / RPCS3 saves, both ways
        return host::SaveTransfer(std::vector<std::string>(argv + 2, argv + argc));
    if (argc < 2) {
        std::fprintf(stderr, "usage: ldport <eboot.elf> [--imports | --run] [--names imports.tsv]\n");
        return 1;
    }
    const std::string mode = argc > 2 ? argv[2] : "";
    std::vector<std::string> app0_layers;  // --app0 may repeat: patch first, then base game
    std::string data = "./savedata";
    std::vector<std::string> figures;  // "zone:id", placed on the virtual Toy Pad at start
    for (int i = 2; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--names") LoadNames(argv[i + 1]);
        if (arg == "--app0") app0_layers.push_back(argv[i + 1]);
        if (arg == "--data") data = argv[i + 1];
        if (arg == "--figure") figures.push_back(argv[i + 1]);
        if (arg == "--addcont") hle::SetAddcontRoot(argv[i + 1]);
    }
    if (app0_layers.empty()) app0_layers.push_back(".");
    hle::SetPathRoots(app0_layers, data);

    // Self-check against a NID GhidraOrbis resolved for this game.
    if (loader::ComputeNid("ferror") != "AHxyhN96dy4") {
        std::fprintf(stderr, "NID self-test failed: %s\n", loader::ComputeNid("ferror").c_str());
        return 1;
    }

    std::string error;
    if (!g_elf.Load(argv[1], error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 1;
    }

    // Functions get a trap stub each; data imports get a zeroed block until implemented.
    uint8_t* stubs = host::AllocateImage(0x100000);
    host::SetProtection(stubs, 0x100000, host::kRead | host::kWrite | host::kExec);
    uint8_t* cursor = stubs;
    std::map<std::string, uint64_t> bound;  // nid -> address
    size_t native_count = 0;
    auto resolve = [&](const loader::Import& imp) -> uint64_t {
        auto [it, inserted] = bound.try_emplace(imp.nid, 0);
        if (inserted) {
            if (const void* native = hle::Lookup(imp.library, imp.nid)) {
                it->second = reinterpret_cast<uint64_t>(native);
                ++native_count;
            } else if (imp.sym_type == 1) {  // STT_OBJECT
                it->second = reinterpret_cast<uint64_t>(std::calloc(1, 0x1000));
            } else {
                g_stub_imports.push_back(imp);
                it->second = MakeTrapStub(cursor, uint32_t(g_stub_imports.size() - 1));
            }
        }
        return it->second;
    };
    if (!g_elf.Relocate(resolve, error)) {
        std::fprintf(stderr, "relocation failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("image %p size %#zx entry %#llx\n", static_cast<void*>(g_elf.base()), g_elf.image_size(),
                static_cast<unsigned long long>(g_elf.entry()));
    std::printf("%zu needed modules, %zu import libraries, %zu imports, %zu relative relocations\n",
                g_elf.modules().size(), g_elf.libraries().size(), g_elf.imports().size(), g_elf.relative_relocs());
    std::printf("%zu of %zu imported symbols have native implementations\n", native_count, bound.size());

    if (mode == "--imports") {
        std::map<std::string, std::map<std::string, int>> by_library;  // library -> nid -> uses
        for (const auto& imp : g_elf.imports()) ++by_library[imp.library][imp.nid];
        for (const auto& [lib, nids] : by_library)
            for (const auto& [nid, uses] : nids) std::printf("%s\t%s\t%d\n", lib.c_str(), nid.c_str(), uses);
        return 0;
    }
    if (mode != "--run") return 0;

    if (!host::InitGuestTls()) {
        std::fprintf(stderr, "no TLS slot for the guest TCB\n");
        return 1;
    }
    const size_t patched = g_elf.PatchTcbReads(host::GuestTcbDisplacement());
    loader::ApplyGamePatches(g_elf.base(), g_elf.image_size());
    g_elf.MakeExecutable();
    host::RegisterGuestRange(g_elf.base(), g_elf.image_size());  // static vertex data can live in the image
#ifdef _WIN32
    SetupTracePoints();
#endif
    std::printf("patched %zu TCB reads; jumping to entry\n", patched);
    std::fflush(stdout);

    // The main thread owns SDL (window and input events); the game runs on its own thread.
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    for (const auto& f : figures) RunToyPadCommand("place " + f);
    toypad::StartSource();  // this PC (web page with LDPORT_TOYPAD_WEB), the physical pad, or LegoToypad
    std::thread([] {
        for (std::string line; std::getline(std::cin, line);) RunToyPadCommand(line);
    }).detach();

#ifdef _WIN32
    AddVectoredExceptionHandler(1, CrashHandler);
    HANDLE game = CreateThread(nullptr, kGuestMainStack, GuestMain, nullptr, 0, nullptr);  // committed, see Create()
    SetThreadDescription(game, L"GameMain");
    uint64_t next_status = SDL_GetTicks() + 5000;
    while (WaitForSingleObject(game, 0) == WAIT_TIMEOUT) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 10)) {
            if (event.type == SDL_EVENT_QUIT) hle::GuestExit(0);
            input::OnSdlEvent(event);
        }
        hle::VideoOutMainThreadTick();
        // LDPORT_PROFILE=1: sample the render thread's instruction pointer each loop pass and print the
        // hottest host/game locations every 10 s (host offsets go through llvm-symbolizer).
        static const std::string profile_thread = std::getenv("LDPORT_PROFILE") ? std::getenv("LDPORT_PROFILE") : "";
        static const bool profile = !profile_thread.empty();
        if (profile) {
            static std::map<std::string, uint64_t> samples;
            static uint64_t total = 0, next_report = SDL_GetTicks() + 10000;
            auto threads = hle::GuestThreads();
            threads.push_back({"GameMain", game});
            for (const auto& [name, handle] : threads) {
                if (name != (profile_thread == "1" ? std::string("Render") : profile_thread)) continue;
                // Bucket by where the sample is in game code: the leaf if it is game code, else the
                // game call site that led into our code plus the innermost ldport.exe frame.
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                uint64_t rips[kMaxFrames];
                SuspendThread(handle);
                GetThreadContext(handle, &ctx);
                const int n = WalkFrames(ctx, rips);
                ResumeThread(handle);
                if (n <= 0) continue;
                const uint64_t gbase = reinterpret_cast<uint64_t>(g_elf.base());
                const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
                uint64_t game_site = 0, host_fn = 0;
                for (int i = 0; i < n; ++i) {
                    if (rips[i] >= gbase && rips[i] < gbase + g_elf.image_size()) { game_site = rips[i] - gbase; break; }
                    if (!host_fn && rips[i] >= exe && rips[i] < exe + 0x2000000) host_fn = rips[i] - exe;
                }
                char key[96];
                std::snprintf(key, sizeof key, "game+%#llx via ldport.exe+%#llx", (unsigned long long)game_site,
                              (unsigned long long)host_fn);
                ++samples[key];
                ++total;
            }
            if (SDL_GetTicks() >= next_report && total) {
                next_report += 10000;
                std::vector<std::pair<uint64_t, std::string>> top;
                for (const auto& [k, n] : samples) top.push_back({n, k});
                std::sort(top.rbegin(), top.rend());
                std::fprintf(stderr, "[profile] Render thread, %llu samples:\n", (unsigned long long)total);
                for (size_t i = 0; i < top.size() && i < 25; ++i)
                    std::fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * top[i].first / total, top[i].second.c_str());
                samples.clear();
                total = 0;
            }
        }
        // LDPORT_STALL_SAMPLE=1: when no flip has come for over 40 ms, sample where the game's main and render
        // threads are each pass, and when flips resume print the stall's samples (finding hitches).
        static const bool stall_sample = std::getenv("LDPORT_STALL_SAMPLE") != nullptr;
        if (stall_sample) {
            static uint64_t last_flips = 0, last_change_ms = 0;
            static std::map<std::string, int> stall;
            static int stall_samples = 0;
            const uint64_t flips = hle::VideoOutFlipCount(), now = SDL_GetTicks();
            if (flips != last_flips) {
                if (stall_samples && !stall.empty()) {
                    std::vector<std::pair<int, std::string>> top;
                    for (const auto& [k, n] : stall) top.push_back({n, k});
                    std::sort(top.rbegin(), top.rend());
                    std::fprintf(stderr, "[stall] %llu ms without a flip before flip %llu, %d samples:\n",
                                 (unsigned long long)(now - last_change_ms), (unsigned long long)flips, stall_samples);
                    for (size_t i = 0; i < top.size() && i < 40; ++i)
                        std::fprintf(stderr, "  %3d  %s\n", top[i].first, top[i].second.c_str());
                }
                stall.clear();
                stall_samples = 0;
                last_flips = flips;
                last_change_ms = now;
            } else if (last_change_ms && now - last_change_ms > 40) {
                auto threads = hle::GuestThreads();
                threads.push_back({"GameMain", game});
                const uint64_t gbase = reinterpret_cast<uint64_t>(g_elf.base());
                const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
                for (const auto& [name, handle] : threads) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    uint64_t rips[kMaxFrames];
                    SuspendThread(handle);
                    GetThreadContext(handle, &ctx);
                    const int n = WalkFrames(ctx, rips);
                    ResumeThread(handle);
                    if (n <= 0) continue;
                    // The game call site plus the innermost two port frames.
                    uint64_t game_site = 0, host[2] = {0, 0};
                    int h = 0;
                    for (int i = 0; i < n; ++i) {
                        if (rips[i] >= gbase && rips[i] < gbase + g_elf.image_size()) { game_site = rips[i] - gbase; break; }
                        if (h < 2 && rips[i] >= exe && rips[i] < exe + 0x2000000) host[h++] = rips[i] - exe;
                    }
                    char key[160];
                    std::snprintf(key, sizeof key, "%s: game+%#llx via ldport.exe+%#llx <- +%#llx", name.c_str(),
                                  (unsigned long long)game_site, (unsigned long long)host[0], (unsigned long long)host[1]);
                    ++stall[key];
                }
                ++stall_samples;
            }
        }
        // Timer in the title bar (time since start, frame number, fps) so problems can be reported
        // by time and matched to logs and screenshots, which use the same frame numbers.
        static uint64_t next_title = 0, title_frames = 0, title_ms = 0;
        if (const uint64_t now = SDL_GetTicks(); now >= next_title) {
            const uint64_t frames = hle::VideoOutFlipCount();
            const double fps = now > title_ms ? double(frames - title_frames) * 1000.0 / double(now - title_ms) : 0.0;
            char title[128];
            std::snprintf(title, sizeof title, "LEGO Dimensions  |  %02llu:%02llu.%01llu  |  frame %llu  |  %.0f fps",
                          (unsigned long long)(now / 60000), (unsigned long long)(now / 1000 % 60),
                          (unsigned long long)(now / 100 % 10), (unsigned long long)frames, fps);
            std::string full = title;
            if (const std::string url = toypad::WebUrl(); !url.empty()) full += "  |  Toy Pad: " + url;
            else full += "  |  F2: Toy Pad page";
            hle::VideoOutSetTitle(full.c_str());
            next_title = now + 250;
            title_frames = frames;
            title_ms = now;
        }
        if (SDL_GetTicks() >= next_status) {
            next_status += 5000;
            const auto& s = gpu::Gpu().stats();
            std::fprintf(stderr, "[status] %llus: %llu flips, %llu packets, %llu draws, %llu eop interrupts\n",
                         (unsigned long long)(SDL_GetTicks() / 1000), (unsigned long long)hle::VideoOutFlipCount(),
                         (unsigned long long)s.packets, (unsigned long long)s.draws,
                         (unsigned long long)s.eop_interrupts);
            // LDPORT_WATCH="312c490+a0,312c490+aa": follow image+offset, then each +o as (ptr + o);
            // the last step prints the u64 at that address.
            if (const char* watch = std::getenv("LDPORT_WATCH")) {
                std::string w = watch;
                size_t start = 0;
                while (start < w.size()) {
                    const size_t comma = std::min(w.find(',', start), w.size());
                    const std::string chain = w.substr(start, comma - start);
                    start = comma + 1;
                    uint64_t addr = reinterpret_cast<uint64_t>(g_elf.base()) + std::strtoull(chain.c_str(), nullptr, 16);
                    for (size_t plus = chain.find('+'); plus != std::string::npos; plus = chain.find('+', plus + 1))
                        addr = TryRead64(addr) + std::strtoull(chain.c_str() + plus + 1, nullptr, 16);
                    std::fprintf(stderr, "[watch] %s = %llx\n", chain.c_str(), (unsigned long long)TryRead64(addr));
                }
            }
            // Frames stopped: show where every guest thread is, once per stall.
            static uint64_t last_flips = ~0ull;
            static bool dumped = false;
            const uint64_t flips = hle::VideoOutFlipCount();
            // LDPORT_THREAD_DUMP=<frame>: one dump once that frame has passed (for "running but stuck").
            static const uint64_t dump_at = std::getenv("LDPORT_THREAD_DUMP") ? std::strtoull(std::getenv("LDPORT_THREAD_DUMP"), nullptr, 10) : 0;
            static bool dumped_at = false;
            const bool scheduled = dump_at && flips >= dump_at && !dumped_at;
            if (scheduled) dumped_at = true;
            if ((flips == last_flips && !dumped) || hle::g_thread_dump_requested.exchange(false) || scheduled) {
                dumped = true;
                std::fprintf(stderr, "[stall] 'GameMain' %s\n", WhereIs(game).c_str());
                for (const auto& [name, handle] : hle::GuestThreads())
                    if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT)
                        std::fprintf(stderr, "[stall] '%s' %s\n", name.c_str(), WhereIs(handle).c_str());
            }
            if (flips != last_flips) dumped = false;
            last_flips = flips;
        }
    }
#else
    {
        struct sigaction sa{};
        sa.sa_sigaction = CrashSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
    }
    pthread_t game{};
    {
        pthread_attr_t pa;
        pthread_attr_init(&pa);
        pthread_attr_setstacksize(&pa, kGuestMainStack);
        if (pthread_create(&game, &pa, GuestMain, nullptr) != 0) {
            std::fprintf(stderr, "could not start the game thread\n");
            return 1;
        }
        pthread_attr_destroy(&pa);
        pthread_setname_np(game, "GameMain");
    }
    static std::atomic<bool> game_done{false};
    std::thread([game] {
        pthread_join(game, nullptr);
        game_done = true;
    }).detach();
    uint64_t next_status = SDL_GetTicks() + 5000;
    while (!game_done) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 10)) {
            if (event.type == SDL_EVENT_QUIT) hle::GuestExit(0);
            input::OnSdlEvent(event);
        }
        hle::VideoOutMainThreadTick();
        static uint64_t next_title = 0, title_frames = 0, title_ms = 0;
        if (const uint64_t now = SDL_GetTicks(); now >= next_title) {
            const uint64_t frames = hle::VideoOutFlipCount();
            const double fps = now > title_ms ? double(frames - title_frames) * 1000.0 / double(now - title_ms) : 0.0;
            char title[128];
            std::snprintf(title, sizeof title, "LEGO Dimensions  |  %02llu:%02llu.%01llu  |  frame %llu  |  %.0f fps",
                          (unsigned long long)(now / 60000), (unsigned long long)(now / 1000 % 60),
                          (unsigned long long)(now / 100 % 10), (unsigned long long)frames, fps);
            std::string full = title;
            if (const std::string url = toypad::WebUrl(); !url.empty()) full += "  |  Toy Pad: " + url;
            hle::VideoOutSetTitle(full.c_str());
            next_title = now + 250;
            title_frames = frames;
            title_ms = now;
        }
        if (SDL_GetTicks() >= next_status) {
            next_status += 5000;
            const auto& st = gpu::Gpu().stats();
            std::fprintf(stderr, "[status] %llus: %llu flips, %llu packets, %llu draws, %llu eop interrupts\n",
                         (unsigned long long)(SDL_GetTicks() / 1000), (unsigned long long)hle::VideoOutFlipCount(),
                         (unsigned long long)st.packets, (unsigned long long)st.draws,
                         (unsigned long long)st.eop_interrupts);
        }
    }
#endif
    std::printf("entry returned\n");
    SDL_Quit();
    return 0;
}
