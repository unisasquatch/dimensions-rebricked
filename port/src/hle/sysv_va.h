#pragma once
#include <cstdint>
#include <string>

namespace hle {

// The System V x86-64 va_list. Guest variadic calls arrive through SYSV_VARIADIC_THUNK,
// which spills the argument registers and builds one of these.
struct SysvVaList {
    uint32_t gp_offset;
    uint32_t fp_offset;
    uint8_t* overflow_arg_area;
    uint8_t* reg_save_area;
};

uint64_t NextGp(SysvVaList* va);
double NextFp(SysvVaList* va);

// printf/scanf with PS4 (LP64) semantics: `long` is 64-bit.
std::string FormatGuest(const char* fmt, SysvVaList* va);
int ScanGuest(const char* input, const char* fmt, SysvVaList* va);

}  // namespace hle

// Defines extern "C" `name(named..., ...)` that forwards to `impl(named..., SysvVaList*)`.
// `va_reg` is the register that carries the va_list pointer: the one after the named args.
#define SYSV_VARIADIC_THUNK(name, impl, named_gp, va_reg)                                   \
    extern "C" __attribute__((naked, sysv_abi)) void name() {                              \
        __asm__ volatile(                                                                  \
            "subq $232, %rsp\n"                                                           \
            "movq %rdi, 0(%rsp)\n movq %rsi, 8(%rsp)\n movq %rdx, 16(%rsp)\n"            \
            "movq %rcx, 24(%rsp)\n movq %r8, 32(%rsp)\n movq %r9, 40(%rsp)\n"            \
            "movaps %xmm0, 48(%rsp)\n movaps %xmm1, 64(%rsp)\n"                          \
            "movaps %xmm2, 80(%rsp)\n movaps %xmm3, 96(%rsp)\n"                          \
            "movaps %xmm4, 112(%rsp)\n movaps %xmm5, 128(%rsp)\n"                        \
            "movaps %xmm6, 144(%rsp)\n movaps %xmm7, 160(%rsp)\n"                        \
            "movl $" #named_gp "*8, 176(%rsp)\n movl $48, 180(%rsp)\n"                   \
            "leaq 240(%rsp), %rax\n movq %rax, 184(%rsp)\n movq %rsp, 192(%rsp)\n"       \
            "leaq 176(%rsp), %" va_reg "\n"                                              \
            "call " #impl "\n"                                                            \
            "addq $232, %rsp\n"                                                           \
            "ret\n");                                                                     \
    }
