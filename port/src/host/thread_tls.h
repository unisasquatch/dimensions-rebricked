#pragma once
#include <cstdint>

namespace host {

// PS4 code reads its thread control block (TCB) with `mov rax, fs:[0]`; the loader patches
// those reads to `mov rax, gs:[GuestTcbDisplacement()]`, a slot in the Windows TEB.
bool InitGuestTls();
uint32_t GuestTcbDisplacement();

// Builds this thread's static TLS block (x86-64 variant II: data sits just below the TCB,
// and TCB[0] points to itself) from the module's initialisation image.
// The image is remembered so later guest threads can call EnsureGuestThreadTls().
void* CreateGuestThreadTls(const uint8_t* init_image, uint64_t filesz, uint64_t memsz, uint64_t align);
void EnsureGuestThreadTls();

// Start of this thread's static TLS block (what __tls_get_addr offsets are relative to).
uint8_t* GuestTlsBlock();

}  // namespace host
