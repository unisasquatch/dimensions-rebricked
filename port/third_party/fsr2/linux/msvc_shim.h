// Force-included into AMD FSR 2.2.1's sources for the Linux (Steam Deck) build: the MSVC names it uses.
// The SPIR-V headers in gen/ were produced by AMD's shader compiler during the Windows build (build_fsr2.cmd).
#pragma once
#include <cstddef>
#include <cstdio>
#include <cwchar>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    std::wcsncpy(dst, src, N - 1);
    dst[N - 1] = 0;
    return 0;
}
#define sprintf_s snprintf
