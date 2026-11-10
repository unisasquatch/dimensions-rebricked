// libc: strings, memory, math, stdlib and Sony/Dinkumware internals.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "hle/hle.h"
#include "host/compat.h"

#define LIBC_FORWARD(ret, name, params, args) HLE_FORWARD("libc", ret, name, params, args)

// --- memory and strings ---
LIBC_FORWARD(void*, memcpy, (void* d, const void* s, size_t n), (d, s, n));
LIBC_FORWARD(void*, memmove, (void* d, const void* s, size_t n), (d, s, n));
LIBC_FORWARD(void*, memset, (void* d, int c, size_t n), (d, c, n));
LIBC_FORWARD(int, memcmp, (const void* a, const void* b, size_t n), (a, b, n));
LIBC_FORWARD(const void*, memchr, (const void* s, int c, size_t n), (s, c, n));
LIBC_FORWARD(int, memcpy_s, (void* d, size_t dn, const void* s, size_t n), (d, dn, s, n));
LIBC_FORWARD(size_t, strlen, (const char* s), (s));
LIBC_FORWARD(size_t, strnlen_s, (const char* s, size_t n), (s, n));
LIBC_FORWARD(char*, strcpy, (char* d, const char* s), (d, s));
LIBC_FORWARD(char*, strncpy, (char* d, const char* s, size_t n), (d, s, n));
LIBC_FORWARD(char*, strcat, (char* d, const char* s), (d, s));
LIBC_FORWARD(char*, strncat, (char* d, const char* s, size_t n), (d, s, n));
LIBC_FORWARD(int, strcmp, (const char* a, const char* b), (a, b));
LIBC_FORWARD(int, strncmp, (const char* a, const char* b, size_t n), (a, b, n));
LIBC_FORWARD(const char*, strchr, (const char* s, int c), (s, c));
LIBC_FORWARD(const char*, strrchr, (const char* s, int c), (s, c));
LIBC_FORWARD(const char*, strstr, (const char* a, const char* b), (a, b));
LIBC_FORWARD(size_t, strspn, (const char* a, const char* b), (a, b));
LIBC_FORWARD(size_t, strcspn, (const char* a, const char* b), (a, b));
LIBC_FORWARD(char*, strtok, (char* s, const char* d), (s, d));

// `long` is 64-bit on the PS4.
SYSV static int64_t g_strtol(const char* s, char** end, int base) { return std::strtoll(s, end, base); }
HLE_EXPORT("libc", "strtol", reinterpret_cast<const void*>(&g_strtol));

// --- math (floating-point arguments use xmm registers in both ABIs) ---
LIBC_FORWARD(double, asin, (double x), (x));
LIBC_FORWARD(double, atan, (double x), (x));
LIBC_FORWARD(double, atan2, (double y, double x), (y, x));
LIBC_FORWARD(double, exp, (double x), (x));
LIBC_FORWARD(double, pow, (double x, double y), (x, y));
LIBC_FORWARD(double, tan, (double x), (x));
LIBC_FORWARD(double, ldexp, (double x, int e), (x, e));
LIBC_FORWARD(float, acosf, (float x), (x));
LIBC_FORWARD(float, atanf, (float x), (x));
LIBC_FORWARD(float, atan2f, (float y, float x), (y, x));
LIBC_FORWARD(float, expf, (float x), (x));
LIBC_FORWARD(float, exp2f, (float x), (x));
LIBC_FORWARD(float, fmodf, (float x, float y), (x, y));
LIBC_FORWARD(float, ldexpf, (float x, int e), (x, e));
LIBC_FORWARD(float, modff, (float x, float* i), (x, i));
LIBC_FORWARD(float, powf, (float x, float y), (x, y));
LIBC_FORWARD(float, tanf, (float x), (x));

// --- Dinkumware internals behind inline <cmath>/<string> code ---
namespace {

constexpr short kFinite = -1, kInfCode = 1, kNanCode = 2;

SYSV short FDtest(const float* x) {
    switch (std::fpclassify(*x)) {
        case FP_INFINITE: return kInfCode;
        case FP_NAN: return kNanCode;
        case FP_ZERO: return 0;
        default: return kFinite;
    }
}
// qoff: 0 = sine, 1 = cosine (odd values add a quarter turn).
SYSV double Sin(double x, unsigned qoff) { return (qoff & 1) ? std::cos(x) : std::sin(x); }
SYSV float FSin(float x, unsigned qoff) { return (qoff & 1) ? std::cos(x) : std::sin(x); }
// base: -1 = log2, 0 = natural, 1 = log10.
SYSV double Log(double x, int base) { return base < 0 ? std::log2(x) : base > 0 ? std::log10(x) : std::log(x); }
SYSV float FLog(float x, int base) { return base < 0 ? std::log2(x) : base > 0 ? std::log10(x) : std::log(x); }
SYSV double Stod(const char* s, char** end, long pten) {
    return std::strtod(s, end) * (pten ? std::pow(10.0, pten) : 1.0);
}
SYSV int64_t Stoll(const char* s, char** end, int base) { return std::strtoll(s, end, base); }
SYSV uint64_t Stoul(const char* s, char** end, int base) { return std::strtoull(s, end, base); }

// Dinkumware ctype tables, indexed from -1 (EOF) to 255.
constexpr short kXD = 0x01, kUP = 0x02, kSP = 0x04, kPU = 0x08, kLO = 0x10, kDI = 0x20, kCN = 0x40, kBB = 0x80;
struct CtypeTables {
    short ctype[257], lower[257], upper[257];
    CtypeTables() {
        for (int c = -1; c < 256; ++c) {
            short f = 0;
            if (c >= 0 && c < 128) {
                if (std::isxdigit(c)) f |= kXD;
                if (std::isupper(c)) f |= kUP;
                if (c == ' ') f |= kSP;
                if (std::ispunct(c)) f |= kPU;
                if (std::islower(c)) f |= kLO;
                if (std::isdigit(c)) f |= kDI;
                if (c >= '\t' && c <= '\r') f |= kCN;
                else if (std::iscntrl(c)) f |= kBB;
            }
            ctype[c + 1] = f;
            lower[c + 1] = short(c >= 'A' && c <= 'Z' ? c + 32 : c);
            upper[c + 1] = short(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    }
};
const CtypeTables& Tables() {
    static CtypeTables t;
    return t;
}
SYSV const short* Getpctype() { return Tables().ctype + 1; }
SYSV const short* Getptolower() { return Tables().lower + 1; }
SYSV const short* Getptoupper() { return Tables().upper + 1; }

}  // namespace

HLE_EXPORT("libc", "_FDtest", reinterpret_cast<const void*>(&FDtest));
HLE_EXPORT("libc", "_Sin", reinterpret_cast<const void*>(&Sin));
HLE_EXPORT("libc", "_FSin", reinterpret_cast<const void*>(&FSin));
HLE_EXPORT("libc", "_Log", reinterpret_cast<const void*>(&Log));
HLE_EXPORT("libc", "_FLog", reinterpret_cast<const void*>(&FLog));
HLE_EXPORT("libc", "_Stod", reinterpret_cast<const void*>(&Stod));
HLE_EXPORT("libc", "_Stoll", reinterpret_cast<const void*>(&Stoll));
HLE_EXPORT("libc", "_Stoul", reinterpret_cast<const void*>(&Stoul));
HLE_EXPORT("libc", "_Getpctype", reinterpret_cast<const void*>(&Getpctype));
HLE_EXPORT("libc", "_Getptolower", reinterpret_cast<const void*>(&Getptolower));
HLE_EXPORT("libc", "_Getptoupper", reinterpret_cast<const void*>(&Getptoupper));

// --- stdlib ---
namespace {

using GuestCompare = SYSV int (*)(const void*, const void*);
thread_local GuestCompare t_compare;
int HostCompare(const void* a, const void* b) { return t_compare(a, b); }

SYSV void Qsort(void* base, size_t n, size_t size, GuestCompare cmp) {
    GuestCompare saved = t_compare;  // qsort may be re-entered from a comparator
    t_compare = cmp;
    std::qsort(base, n, size, HostCompare);
    t_compare = saved;
}

SYSV void* Malloc(size_t n) { return std::malloc(n); }
SYSV void Free(void* p) { std::free(p); }

// Sony's allocator statistics; the game only reads sizes for its memory HUD.
struct MallocManagedSize {
    uint16_t size, version;
    uint32_t reserved;
    size_t max_system, current_system, max_inuse, current_inuse;
};
SYSV int MallocStatsFast(MallocManagedSize* s) {
    s->max_system = s->current_system = s->max_inuse = s->current_inuse = 0;
    return 0;
}

// FreeBSD's rand(): RAND_MAX is 0x7fffffff, not the Windows CRT's 0x7fff. The game scales
// rand() by 2^-30 and loops until it gets a big enough jump, so the range matters.
std::mutex g_rand_mutex;
uint64_t g_rand_state = 1;
SYSV int Rand() {
    std::lock_guard lock(g_rand_mutex);
    int64_t x = int64_t(g_rand_state % 0x7ffffffe) + 1;  // Park-Miller minimal standard
    const int64_t hi = x / 127773, lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    g_rand_state = uint64_t(--x);
    return int(x);
}
SYSV void Srand(unsigned seed) {
    std::lock_guard lock(g_rand_mutex);
    g_rand_state = seed;
}

// PS4 struct tm (FreeBSD layout): nine ints, then tm_gmtoff and tm_zone.
struct GuestTm {
    int sec, min, hour, mday, mon, year, wday, yday, isdst;
    int64_t gmtoff;
    const char* zone;
};
SYSV GuestTm* Gmtime(const int64_t* t) {
    thread_local GuestTm out;
    std::tm host{};
    const __time64_t v = *t;
    if (_gmtime64_s(&host, &v) != 0) return nullptr;
    out = {host.tm_sec, host.tm_min, host.tm_hour, host.tm_mday, host.tm_mon, host.tm_year,
           host.tm_wday, host.tm_yday, host.tm_isdst, 0, "UTC"};
    return &out;
}

// Process exit: guest atexit handlers run in reverse registration order.
struct ExitHandler {
    SYSV void (*fn)(void*);
    void* arg;
    bool with_arg;
};
std::mutex g_exit_mutex;
std::vector<ExitHandler> g_exit_handlers;

}  // namespace

namespace hle {

void AddExitHandler(SYSV void (*fn)(void*), void* arg, bool with_arg) {
    std::lock_guard lock(g_exit_mutex);
    g_exit_handlers.push_back({fn, arg, with_arg});
}

[[noreturn]] void GuestExit(int status) {
    std::fprintf(stderr, "[libc] exit(%d)\n", status);
    for (;;) {
        ExitHandler h;
        {
            std::lock_guard lock(g_exit_mutex);
            if (g_exit_handlers.empty()) break;
            h = g_exit_handlers.back();
            g_exit_handlers.pop_back();
        }
        if (h.with_arg) h.fn(h.arg);
        else reinterpret_cast<SYSV void (*)()>(h.fn)();
    }
    std::fflush(nullptr);
    std::_Exit(status);
}

}  // namespace hle

namespace {

SYSV int Atexit(SYSV void (*fn)()) {
    hle::AddExitHandler(reinterpret_cast<SYSV void (*)(void*)>(fn), nullptr, false);
    return 0;
}
SYSV void Exit(int status) { hle::GuestExit(status); }
SYSV void QuickExit(int status) { std::fflush(nullptr); std::_Exit(status); }
SYSV void Abort() { hle::Fatal("guest called abort()"); }
SYSV void CatchReturnFromMain(int status) { hle::GuestExit(status); }

// The game's CRT calls this before main to set up `environ`; there is no environment to copy.
SYSV void InitEnv() {}

// Linked by the game to prove it was built against this libc; any non-zero value works.
uint32_t g_need_libc = 1;

}  // namespace

HLE_EXPORT("libc", "qsort", reinterpret_cast<const void*>(&Qsort));
HLE_EXPORT("libc", "malloc", reinterpret_cast<const void*>(&Malloc));
HLE_EXPORT("libc", "free", reinterpret_cast<const void*>(&Free));
HLE_EXPORT("libc", "malloc_stats_fast", reinterpret_cast<const void*>(&MallocStatsFast));
HLE_EXPORT("libc", "rand", reinterpret_cast<const void*>(&Rand));
HLE_EXPORT("libc", "srand", reinterpret_cast<const void*>(&Srand));
HLE_EXPORT("libc", "gmtime", reinterpret_cast<const void*>(&Gmtime));
HLE_EXPORT("libc", "atexit", reinterpret_cast<const void*>(&Atexit));
HLE_EXPORT("libc", "exit", reinterpret_cast<const void*>(&Exit));
HLE_EXPORT("libc", "_Exit", reinterpret_cast<const void*>(&QuickExit));
HLE_EXPORT("libc", "abort", reinterpret_cast<const void*>(&Abort));
HLE_EXPORT("libc", "catchReturnFromMain", reinterpret_cast<const void*>(&CatchReturnFromMain));
HLE_EXPORT("libc", "_init_env", reinterpret_cast<const void*>(&InitEnv));
HLE_EXPORT("libc", "Need_sceLibc", &g_need_libc);

// --- setjmp/longjmp: the guest jmp_buf holds the System V callee-saved registers ---
extern "C" __attribute__((naked, sysv_abi)) void guest_setjmp() {
    __asm__ volatile(
        "movq %rbx, 0(%rdi)\n movq %rbp, 8(%rdi)\n movq %r12, 16(%rdi)\n"
        "movq %r13, 24(%rdi)\n movq %r14, 32(%rdi)\n movq %r15, 40(%rdi)\n"
        "leaq 8(%rsp), %rdx\n movq %rdx, 48(%rdi)\n"  // caller's rsp
        "movq (%rsp), %rdx\n movq %rdx, 56(%rdi)\n"   // return address
        "xorl %eax, %eax\n ret\n");
}
extern "C" __attribute__((naked, sysv_abi)) void guest_longjmp() {
    __asm__ volatile(
        "movl %esi, %eax\n testl %eax, %eax\n jnz 1f\n movl $1, %eax\n 1:\n"
        "movq 0(%rdi), %rbx\n movq 8(%rdi), %rbp\n movq 16(%rdi), %r12\n"
        "movq 24(%rdi), %r13\n movq 32(%rdi), %r14\n movq 40(%rdi), %r15\n"
        "movq 48(%rdi), %rsp\n jmpq *56(%rdi)\n");
}
HLE_EXPORT("libc", "setjmp", reinterpret_cast<const void*>(&guest_setjmp));
HLE_EXPORT("libc", "longjmp", reinterpret_cast<const void*>(&guest_longjmp));
