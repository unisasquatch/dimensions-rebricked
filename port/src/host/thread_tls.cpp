#include "host/thread_tls.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace host {
namespace {

constexpr uint64_t kTcbSize = 0x100;
#ifdef _WIN32
constexpr uint32_t kTebTlsSlots = 0x1480;  // TEB.TlsSlots[64] on x64
DWORD g_slot = TLS_OUT_OF_INDEXES;
#endif

struct InitImage { const uint8_t* data; uint64_t filesz, memsz, align; };
InitImage g_image;
thread_local uint8_t* t_block = nullptr;

}  // namespace

bool InitGuestTls() {
#ifdef _WIN32
    g_slot = TlsAlloc();
    return g_slot < 64;  // only the inline TlsSlots array is addressable as gs:[disp]
#else
    return true;  // Linux: each guest thread's gs base points at its TCB (glibc keeps fs)
#endif
}

uint32_t GuestTcbDisplacement() {
#ifdef _WIN32
    return kTebTlsSlots + g_slot * 8;
#else
    return 0;
#endif
}

void* CreateGuestThreadTls(const uint8_t* init_image, uint64_t filesz, uint64_t memsz, uint64_t align) {
    const uint64_t tls_size = (memsz + align - 1) & ~(align - 1);
    auto* block = static_cast<uint8_t*>(std::calloc(1, tls_size + kTcbSize + align));
    auto* tcb = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(block) + tls_size + align - 1) & ~uintptr_t(align - 1));
    std::memcpy(tcb - tls_size, init_image, filesz);
    *reinterpret_cast<void**>(tcb) = tcb;
#ifdef _WIN32
    TlsSetValue(g_slot, tcb);
#else
    // The patched guest reads are gs:[0], which the TCB's self pointer answers.
    syscall(SYS_arch_prctl, ARCH_SET_GS, reinterpret_cast<unsigned long>(tcb));
#endif
    g_image = {init_image, filesz, memsz, align};
    t_block = tcb - tls_size;
    return tcb;
}

void EnsureGuestThreadTls() {
    if (!t_block) CreateGuestThreadTls(g_image.data, g_image.filesz, g_image.memsz, g_image.align);
}

uint8_t* GuestTlsBlock() {
    EnsureGuestThreadTls();
    return t_block;
}

}  // namespace host
