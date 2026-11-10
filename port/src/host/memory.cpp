#include "host/memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <cstring>

namespace host {

uint8_t* AllocateImage(size_t size) {
#ifdef _WIN32
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

void FreeImage(uint8_t* base, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size);
#endif
}

bool SetProtection(uint8_t* addr, size_t size, uint32_t prot) {
#ifdef _WIN32
    DWORD flags = PAGE_NOACCESS;
    if (prot & kExec) flags = (prot & kWrite) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (prot & kWrite) flags = PAGE_READWRITE;
    else if (prot & kRead) flags = PAGE_READONLY;
    DWORD old;
    return VirtualProtect(addr, size, flags, &old) != 0;
#else
    int flags = 0;
    if (prot & kRead) flags |= PROT_READ;
    if (prot & kWrite) flags |= PROT_WRITE;
    if (prot & kExec) flags |= PROT_EXEC;
    return mprotect(addr, size, flags) == 0;
#endif
}

bool SafeCopy(void* dst, const void* src, size_t n) {
    if (!n) return true;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t at = reinterpret_cast<uintptr_t>(src), end = at + n; at < end;
         at = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
            return false;
    }
    std::memcpy(dst, src, n);
    return true;
#else
    // The kernel does the reading, so an unmapped page is an EFAULT here rather than a SIGSEGV.
    iovec local{dst, n}, remote{const_cast<void*>(src), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
#endif
}

}  // namespace host
