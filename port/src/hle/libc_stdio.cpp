// libc stdio. Guest FILE* values point at GuestFile objects that wrap a host FILE*.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hle/hle.h"
#include "host/compat.h"
#include "hle/sysv_va.h"

namespace {

struct GuestFile {
    FILE* host;
    uint8_t opaque[0x80];  // the game only passes these by address, but keep room for inline peeks
};

GuestFile g_stdin{stdin, {}}, g_stdout{stdout, {}}, g_stderr{stderr, {}};

FILE* H(GuestFile* f) { return f->host; }

SYSV GuestFile* Fopen(const char* path, const char* mode) {
    const std::string host_path = hle::HostPath(path);
    FILE* f = std::fopen(host_path.c_str(), mode);
    if (!f) return nullptr;
    return new GuestFile{f, {}};
}
SYSV int Fclose(GuestFile* f) {
    const int r = std::fclose(H(f));
    if (f != &g_stdin && f != &g_stdout && f != &g_stderr) delete f;
    return r;
}
SYSV size_t Fread(void* p, size_t s, size_t n, GuestFile* f) { return std::fread(p, s, n, H(f)); }
SYSV size_t Fwrite(const void* p, size_t s, size_t n, GuestFile* f) { return std::fwrite(p, s, n, H(f)); }
SYSV int Fseek(GuestFile* f, int64_t off, int whence) { return _fseeki64(H(f), off, whence); }
SYSV int64_t Ftell(GuestFile* f) { return _ftelli64(H(f)); }
SYSV int Fflush(GuestFile* f) { return std::fflush(f ? H(f) : nullptr); }
SYSV int Ferror(GuestFile* f) { return std::ferror(H(f)); }
SYSV void Clearerr(GuestFile* f) { std::clearerr(H(f)); }
SYSV int Fileno(GuestFile* f) { return _fileno(H(f)); }
SYSV int Getc(GuestFile* f) { return std::getc(H(f)); }
SYSV int Fputc(int c, GuestFile* f) { return std::fputc(c, H(f)); }
SYSV int Fputs(const char* s, GuestFile* f) { return std::fputs(s, H(f)); }
SYSV int Puts(const char* s) { return std::puts(s); }
SYSV int Putchar(int c) { return std::putchar(c); }

int WriteFormatted(FILE* f, const std::string& s) {
    return int(std::fwrite(s.data(), 1, s.size(), f));
}
size_t CopyOut(char* buf, size_t n, const std::string& s) {
    if (n) {
        const size_t len = s.size() < n - 1 ? s.size() : n - 1;
        std::memcpy(buf, s.data(), len);
        buf[len] = '\0';
    }
    return s.size();
}

}  // namespace

// Variadic entry points: the thunks build a SysvVaList and call the *_impl functions.
extern "C" SYSV int guest_vprintf_impl(const char* fmt, hle::SysvVaList* va) {
    return WriteFormatted(stdout, hle::FormatGuest(fmt, va));
}
extern "C" SYSV int guest_vfprintf_impl(GuestFile* f, const char* fmt, hle::SysvVaList* va) {
    return WriteFormatted(H(f), hle::FormatGuest(fmt, va));
}
extern "C" SYSV int guest_vsprintf_impl(char* buf, const char* fmt, hle::SysvVaList* va) {
    return int(CopyOut(buf, SIZE_MAX, hle::FormatGuest(fmt, va)));
}
extern "C" SYSV int guest_vsnprintf_impl(char* buf, size_t n, const char* fmt, hle::SysvVaList* va) {
    return int(CopyOut(buf, n, hle::FormatGuest(fmt, va)));
}
extern "C" SYSV int guest_vsscanf_impl(const char* s, const char* fmt, hle::SysvVaList* va) {
    return hle::ScanGuest(s, fmt, va);
}

SYSV_VARIADIC_THUNK(guest_printf, guest_vprintf_impl, 1, "rsi")
SYSV_VARIADIC_THUNK(guest_fprintf, guest_vfprintf_impl, 2, "rdx")
SYSV_VARIADIC_THUNK(guest_sprintf, guest_vsprintf_impl, 2, "rdx")
SYSV_VARIADIC_THUNK(guest_snprintf, guest_vsnprintf_impl, 3, "rcx")
SYSV_VARIADIC_THUNK(guest_sscanf, guest_vsscanf_impl, 2, "rdx")

#define STDIO(name, fn) HLE_EXPORT("libc", name, reinterpret_cast<const void*>(&fn))
STDIO("fopen", Fopen);
STDIO("fclose", Fclose);
STDIO("fread", Fread);
STDIO("fwrite", Fwrite);
STDIO("fseek", Fseek);
STDIO("ftell", Ftell);
STDIO("fflush", Fflush);
STDIO("ferror", Ferror);
STDIO("clearerr", Clearerr);
STDIO("fileno", Fileno);
STDIO("getc", Getc);
STDIO("fputc", Fputc);
STDIO("fputs", Fputs);
STDIO("puts", Puts);
STDIO("putchar", Putchar);
STDIO("printf", guest_printf);
STDIO("fprintf", guest_fprintf);
STDIO("sprintf", guest_sprintf);
STDIO("snprintf", guest_snprintf);
STDIO("sscanf", guest_sscanf);
// A guest va_list argument is a pointer to the System V structure.
STDIO("vprintf", guest_vprintf_impl);
STDIO("vsprintf", guest_vsprintf_impl);
STDIO("vsnprintf", guest_vsnprintf_impl);
HLE_EXPORT("libc", "_Stdin", &g_stdin);
HLE_EXPORT("libc", "_Stdout", &g_stdout);
HLE_EXPORT("libc", "_Stderr", &g_stderr);
