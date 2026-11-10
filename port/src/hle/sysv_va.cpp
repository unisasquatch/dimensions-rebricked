#include "hle/sysv_va.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hle {
namespace {

constexpr uint32_t kGpEnd = 48;
constexpr uint32_t kFpEnd = 176;

// Appends one host-formatted conversion.
template <typename T>
void Append(std::string& out, const std::string& spec, T value) {
    const int n = std::snprintf(nullptr, 0, spec.c_str(), value);
    if (n <= 0) return;
    const size_t at = out.size();
    out.resize(at + n + 1);
    std::snprintf(out.data() + at, n + 1, spec.c_str(), value);
    out.resize(at + n);
}

// x87 80-bit long double (PS4) to double.
double LongDoubleFromBytes(const uint8_t* p) {
    uint64_t mantissa;
    uint16_t se;
    std::memcpy(&mantissa, p, 8);
    std::memcpy(&se, p + 8, 2);
    const int exp = se & 0x7FFF;
    const double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mantissa == 0) return 0.0 * sign;
    if (exp == 0x7FFF) return (mantissa << 1) ? NAN : sign * INFINITY;
    return sign * std::ldexp(double(mantissa), exp - 16383 - 63);
}

enum class Length { kNone, kChar, kShort, kWide, kLongDouble };

Length ParseLength(const char*& f) {
    if (f[0] == 'h' && f[1] == 'h') { f += 2; return Length::kChar; }
    if (f[0] == 'l' && f[1] == 'l') { f += 2; return Length::kWide; }
    switch (*f) {
        case 'h': ++f; return Length::kShort;
        case 'l': case 'z': case 'j': case 't': case 'q': ++f; return Length::kWide;
        case 'L': ++f; return Length::kLongDouble;
    }
    return Length::kNone;
}

}  // namespace

uint64_t NextGp(SysvVaList* va) {
    uint64_t v;
    if (va->gp_offset < kGpEnd) {
        std::memcpy(&v, va->reg_save_area + va->gp_offset, 8);
        va->gp_offset += 8;
    } else {
        std::memcpy(&v, va->overflow_arg_area, 8);
        va->overflow_arg_area += 8;
    }
    return v;
}

double NextFp(SysvVaList* va) {
    double v;
    if (va->fp_offset < kFpEnd) {
        std::memcpy(&v, va->reg_save_area + va->fp_offset, 8);
        va->fp_offset += 16;
    } else {
        std::memcpy(&v, va->overflow_arg_area, 8);
        va->overflow_arg_area += 8;
    }
    return v;
}

std::string FormatGuest(const char* f, SysvVaList* va) {
    std::string out;
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        ++f;
        if (*f == '%') { out += '%'; ++f; continue; }

        std::string spec = "%";
        while (*f && std::strchr("-+ #0'", *f)) {
            if (*f != '\'') spec += *f;  // thousands grouping isn't supported by the host CRT
            ++f;
        }
        if (*f == '*') { spec += std::to_string(int32_t(NextGp(va))); ++f; }
        while (std::isdigit(uint8_t(*f))) spec += *f++;
        if (*f == '.') {
            spec += *f++;
            if (*f == '*') { spec += std::to_string(int32_t(NextGp(va))); ++f; }
            while (std::isdigit(uint8_t(*f))) spec += *f++;
        }
        const Length len = ParseLength(f);
        const char conv = *f ? *f++ : '\0';

        switch (conv) {
            case 'd': case 'i': {
                const uint64_t v = NextGp(va);
                if (len == Length::kWide) Append(out, spec + "lld", int64_t(v));
                else if (len == Length::kShort) Append(out, spec + "d", int(int16_t(v)));
                else if (len == Length::kChar) Append(out, spec + "d", int(int8_t(v)));
                else Append(out, spec + "d", int32_t(v));
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                const uint64_t v = NextGp(va);
                const std::string c(1, conv);
                if (len == Length::kWide) Append(out, spec + "ll" + c, uint64_t(v));
                else if (len == Length::kShort) Append(out, spec + c, unsigned(uint16_t(v)));
                else if (len == Length::kChar) Append(out, spec + c, unsigned(uint8_t(v)));
                else Append(out, spec + c, uint32_t(v));
                break;
            }
            case 'c': Append(out, spec + "c", int(NextGp(va))); break;
            case 's': {
                const auto* s = reinterpret_cast<const char*>(NextGp(va));
                if (len == Length::kWide) out += "(wide string)";
                else Append(out, spec + "s", s ? s : "(null)");
                break;
            }
            case 'p': Append(out, "0x%llx", static_cast<unsigned long long>(NextGp(va))); break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v;
                if (len == Length::kLongDouble) {
                    // Long doubles are passed in memory, 16-byte aligned.
                    auto addr = (reinterpret_cast<uintptr_t>(va->overflow_arg_area) + 15) & ~uintptr_t(15);
                    v = LongDoubleFromBytes(reinterpret_cast<uint8_t*>(addr));
                    va->overflow_arg_area = reinterpret_cast<uint8_t*>(addr + 16);
                } else {
                    v = NextFp(va);
                }
                Append(out, spec + conv, v);
                break;
            }
            case 'n': {
                auto* p = reinterpret_cast<void*>(NextGp(va));
                if (len == Length::kWide) *static_cast<int64_t*>(p) = int64_t(out.size());
                else *static_cast<int32_t*>(p) = int32_t(out.size());
                break;
            }
            default:
                out += '%';
                if (conv) out += conv;
        }
    }
    return out;
}

int ScanGuest(const char* input, const char* f, SysvVaList* va) {
    int assigned = 0;
    const char* pos = input;
    while (*f) {
        // One host sscanf per directive: literal text, then a single conversion, then %n.
        std::string host;
        while (*f && !(f[0] == '%' && f[1] != '%')) {
            if (f[0] == '%') { host += "%%"; f += 2; } else host += *f++;
        }
        if (!*f) {
            int used = -1;
            std::sscanf(pos, (host + "%n").c_str(), &used);
            break;
        }

        ++f;  // '%'
        const bool suppress = *f == '*';
        if (suppress) ++f;
        std::string spec = "%";
        if (suppress) spec += '*';
        while (std::isdigit(uint8_t(*f))) spec += *f++;
        const Length len = ParseLength(f);
        const char conv = *f;
        if (conv == '[') {
            spec += *f++;
            if (*f == '^') spec += *f++;
            if (*f == ']') spec += *f++;
            while (*f && *f != ']') spec += *f++;
            if (*f) spec += *f++;
        } else {
            ++f;
            if (std::strchr("diuoxXn", conv)) {
                if (len == Length::kWide) spec += "ll";
                else if (len == Length::kShort) spec += "h";
                else if (len == Length::kChar) spec += "hh";
            } else if (std::strchr("fFeEgGaA", conv) && len == Length::kWide) {
                spec += "l";
            }
            spec += conv;
        }

        int used = -1;
        if (conv == 'n' && !suppress) {
            *reinterpret_cast<int32_t*>(NextGp(va)) = int32_t(pos - input);
            continue;
        }
        if (suppress) {
            std::sscanf(pos, (host + spec + "%n").c_str(), &used);
        } else {
            void* target = reinterpret_cast<void*>(NextGp(va));
            const int got = std::sscanf(pos, (host + spec + "%n").c_str(), target, &used);
            if (got == EOF && assigned == 0) return EOF;
            if (got < 1) return assigned;
            ++assigned;
        }
        if (used < 0) return assigned;
        pos += used;
    }
    return assigned;
}

}  // namespace hle
