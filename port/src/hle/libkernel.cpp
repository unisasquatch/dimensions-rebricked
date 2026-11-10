// libkernel: process basics, time, files and direct memory.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <mutex>
#include <random>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "hle/clock.h"
#include "hle/hle.h"
#include "hle/sce_error.h"
#include "host/guest_memory.h"
#include "host/thread_tls.h"

namespace {

using namespace std::chrono;

SYSV int* Error() { return &errno; }
SYSV void StackChkFail() { hle::Fatal("stack smashing detected in game code"); }
uint64_t g_stack_chk_guard = 0x2b992ddfa23249d6ull;

struct TlsIndex { uint64_t module, offset; };
SYSV void* TlsGetAddr(const TlsIndex* index) { return host::GuestTlsBlock() + index->offset; }

// --- time ---
const auto g_start = steady_clock::now();
SYSV uint64_t GetProcessTime() { return hle::GuestNowNs() / 1000; }
struct GuestTimeval { int64_t sec, usec; };
SYSV int Gettimeofday(GuestTimeval* tv) {
    const auto us = (duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() +
                      hle::GuestWallOffsetNs()) / 1000;
    tv->sec = us / 1000000;
    tv->usec = us % 1000000;
    return 0;
}
SYSV int Usleep(uint32_t usec) {
    if (hle::FastForwarding()) {  // game time only moves per flip: just let others run
        std::this_thread::yield();
        return 0;
    }
    std::this_thread::sleep_for(microseconds(usec));
    return 0;
}
SYSV int UuidCreate(uint8_t* uuid) {
    static std::mt19937_64 rng{std::random_device{}()};
    for (int i = 0; i < 16; i += 8) {
        const uint64_t v = rng();
        std::memcpy(uuid + i, &v, 8);
    }
    return 0;
}

// --- files (FreeBSD open flags) ---
constexpr int kOWrOnly = 0x1, kORdWr = 0x2, kOAppend = 0x8, kOCreat = 0x200, kOTrunc = 0x400, kOExcl = 0x800;

int HostErrorToSce() {
    switch (errno) {
        case ENOENT: return hle::SceError(hle::ENOENT_BSD);
        case EACCES: return hle::SceError(hle::EACCES_BSD);
        case EEXIST: return hle::SceError(hle::EEXIST_BSD);
        case EBADF: return hle::SceError(hle::EBADF_BSD);
        case EINVAL: return hle::SceError(hle::EINVAL_BSD);
        default: return hle::SceError(hle::EIO_BSD);
    }
}

#ifdef _WIN32
SYSV int Open(const char* path, int flags, int /*mode*/) {
    int host = _O_BINARY;
    if (flags & kORdWr) host |= _O_RDWR;
    else if (flags & kOWrOnly) host |= _O_WRONLY;
    else host |= _O_RDONLY;
    if (flags & kOAppend) host |= _O_APPEND;
    if (flags & kOCreat) host |= _O_CREAT;
    if (flags & kOTrunc) host |= _O_TRUNC;
    if (flags & kOExcl) host |= _O_EXCL;
    const std::string host_path = hle::HostPath(path);
    const int fd = _open(host_path.c_str(), host, _S_IREAD | _S_IWRITE);
    if (std::getenv("LDPORT_TRACE_IO")) std::fprintf(stderr, "[io] kernel open %s flags %#x -> %d\n", path, flags, fd);
    return fd < 0 ? HostErrorToSce() : fd;
}
SYSV int Close(int fd) { return _close(fd) < 0 ? HostErrorToSce() : 0; }
SYSV int64_t Read(int fd, void* buf, uint64_t n) {
    const int r = _read(fd, buf, unsigned(n));
    return r < 0 ? HostErrorToSce() : r;
}

// Positional I/O without moving the file pointer, so threads can share a descriptor.
int64_t PositionalIo(int fd, void* buf, uint64_t n, int64_t offset, bool write) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) return hle::SceError(hle::EBADF_BSD);
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    DWORD done = 0;
    const BOOL ok = write ? WriteFile(h, buf, DWORD(n), &done, &ov) : ReadFile(h, buf, DWORD(n), &done, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF) return hle::SceError(hle::EIO_BSD);
    return done;
}
SYSV int64_t Pread(int fd, void* buf, uint64_t n, int64_t offset) { return PositionalIo(fd, buf, n, offset, false); }
SYSV int64_t Pwrite(int fd, const void* buf, uint64_t n, int64_t offset) {
    return PositionalIo(fd, const_cast<void*>(buf), n, offset, true);
}
SYSV int Unlink(const char* path) { return _unlink(hle::HostPath(path).c_str()) < 0 ? HostErrorToSce() : 0; }
#else
SYSV int Open(const char* path, int flags, int /*mode*/) {
    int host = O_CLOEXEC;
    if (flags & kORdWr) host |= O_RDWR;
    else if (flags & kOWrOnly) host |= O_WRONLY;
    else host |= O_RDONLY;
    if (flags & kOAppend) host |= O_APPEND;
    if (flags & kOCreat) host |= O_CREAT;
    if (flags & kOTrunc) host |= O_TRUNC;
    if (flags & kOExcl) host |= O_EXCL;
    const std::string host_path = hle::HostPath(path);
    const int fd = open(host_path.c_str(), host, 0644);
    if (std::getenv("LDPORT_TRACE_IO")) std::fprintf(stderr, "[io] kernel open %s flags %#x -> %d\n", path, flags, fd);
    return fd < 0 ? HostErrorToSce() : fd;
}
SYSV int Close(int fd) { return close(fd) < 0 ? HostErrorToSce() : 0; }
SYSV int64_t Read(int fd, void* buf, uint64_t n) {
    const ssize_t r = read(fd, buf, n);
    return r < 0 ? HostErrorToSce() : r;
}
SYSV int64_t Pread(int fd, void* buf, uint64_t n, int64_t offset) {
    const ssize_t r = pread(fd, buf, n, offset);
    return r < 0 ? HostErrorToSce() : r;
}
SYSV int64_t Pwrite(int fd, const void* buf, uint64_t n, int64_t offset) {
    const ssize_t r = pwrite(fd, buf, n, offset);
    return r < 0 ? HostErrorToSce() : r;
}
SYSV int Unlink(const char* path) { return unlink(hle::HostPath(path).c_str()) < 0 ? HostErrorToSce() : 0; }
#endif
SYSV int Rename(const char* from, const char* to) {
    return std::rename(hle::HostPath(from).c_str(), hle::HostPath(to).c_str()) < 0 ? HostErrorToSce() : 0;
}

// --- direct memory: "physical" offsets are handed out by a bump allocator and backed
// by host memory when mapped ---
constexpr uint64_t kDirectMemorySize = 0x13C000000ull;  // 5056 MB, the retail game budget
constexpr uint64_t kDirectMemoryVirtualBase = 0x8000000000ull;  // 512 GB: well inside 40-bit GPU addresses
std::mutex g_dmem_mutex;
uint64_t g_dmem_next = 0;
struct DirectBlock { uint64_t start, size; int type; };
std::map<uint64_t, DirectBlock> g_dmem_blocks;  // start -> block
std::map<uint64_t, void*> g_mappings;           // host address -> reservation base
std::map<uint64_t, uint64_t> g_lengths;         // host address -> length (munmap needs it)

SYSV uint64_t GetDirectMemorySize() { return kDirectMemorySize; }
SYSV int AllocateDirectMemory(int64_t, int64_t, uint64_t len, uint64_t align, int type, int64_t* out) {
    std::lock_guard lock(g_dmem_mutex);
    align = align ? align : 0x4000;
    const uint64_t start = (g_dmem_next + align - 1) & ~(align - 1);
    if (start + len > kDirectMemorySize) return hle::SceError(hle::EAGAIN_BSD);
    g_dmem_next = start + len;
    g_dmem_blocks[start] = {start, len, type};
    *out = int64_t(start);
    return 0;
}
SYSV int ReleaseDirectMemory(int64_t start, uint64_t) {
    std::lock_guard lock(g_dmem_mutex);
    g_dmem_blocks.erase(uint64_t(start));
    return 0;
}
SYSV int MapDirectMemory(void** addr, uint64_t len, int /*prot*/, int /*flags*/, int64_t, uint64_t align) {
    align = align < 0x10000 ? 0x10000 : align;
    // GPU registers such as CB_COLOR_BASE hold 40-bit addresses, so direct memory has to live
    // below 1 TB as it does on the PS4. Mappings are placed at increasing fixed addresses from
    // 512 GB, skipping anything Windows already uses there.
    static uint64_t next = kDirectMemoryVirtualBase;
    uint8_t* base = nullptr;
    {
        std::lock_guard lock(g_dmem_mutex);
        for (int tries = 0; tries < 4096 && !base; ++tries) {
            const uint64_t at = (next + align - 1) & ~(align - 1);
#ifdef _WIN32
            base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            void* m = mmap(reinterpret_cast<void*>(at), len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
            base = m == MAP_FAILED ? nullptr : static_cast<uint8_t*>(m);
#endif
            next = at + ((len + 0xFFFF) & ~uint64_t(0xFFFF)) + (base ? 0 : 0x1000000);
        }
    }
    if (!base) return hle::SceError(hle::ENOMEM_BSD);
    auto* aligned = base;
    {
        std::lock_guard lock(g_dmem_mutex);
        g_mappings[reinterpret_cast<uint64_t>(aligned)] = base;
        g_lengths[reinterpret_cast<uint64_t>(aligned)] = len;
        host::RegisterGuestRange(aligned, len);
    }
    *addr = aligned;
    return 0;
}
SYSV int Munmap(void* addr, uint64_t) {
    {
        std::lock_guard lock(g_dmem_mutex);
        if (!g_mappings.count(reinterpret_cast<uint64_t>(addr))) return hle::SceError(hle::EINVAL_BSD);
    }
    // The GPU may still be reading this memory (frames in flight, queued submissions): finish that
    // work and drop the renderer's Vulkan import before the pages go away.
    host::ReleaseGuestRange(addr);
    std::lock_guard lock(g_dmem_mutex);
    auto it = g_mappings.find(reinterpret_cast<uint64_t>(addr));
    if (it == g_mappings.end()) return hle::SceError(hle::EINVAL_BSD);
    host::UnregisterGuestRange(reinterpret_cast<void*>(it->first));
#ifdef _WIN32
    VirtualFree(it->second, 0, MEM_RELEASE);
#else
    munmap(it->second, g_lengths[it->first]);
    g_lengths.erase(it->first);
#endif
    g_mappings.erase(it);
    return 0;
}
struct DirectMemoryInfo { int64_t start, end; int type; };
SYSV int DirectMemoryQuery(int64_t offset, int flags, DirectMemoryInfo* info, uint64_t) {
    std::lock_guard lock(g_dmem_mutex);
    auto it = g_dmem_blocks.upper_bound(uint64_t(offset));
    if (it != g_dmem_blocks.begin()) {
        auto prev = std::prev(it);
        if (uint64_t(offset) < prev->second.start + prev->second.size) it = prev;
    }
    // flags & 1: find the next block at or after offset.
    if (it == g_dmem_blocks.end() || (!(flags & 1) && uint64_t(offset) < it->second.start))
        return hle::SceError(hle::EACCES_BSD);
    info->start = int64_t(it->second.start);
    info->end = int64_t(it->second.start + it->second.size);
    info->type = it->second.type;
    return 0;
}

}  // namespace

#define KERNEL(name, fn) HLE_EXPORT("libkernel", name, reinterpret_cast<const void*>(&fn))
KERNEL("__error", Error);
KERNEL("__stack_chk_fail", StackChkFail);
HLE_EXPORT("libkernel", "__stack_chk_guard", &g_stack_chk_guard);
KERNEL("__tls_get_addr", TlsGetAddr);
KERNEL("sceKernelGetProcessTime", GetProcessTime);
KERNEL("sceKernelGettimeofday", Gettimeofday);
KERNEL("sceKernelUsleep", Usleep);
KERNEL("sceKernelUuidCreate", UuidCreate);
KERNEL("sceKernelOpen", Open);
KERNEL("sceKernelClose", Close);
KERNEL("sceKernelRead", Read);
KERNEL("sceKernelPread", Pread);
KERNEL("sceKernelPwrite", Pwrite);
KERNEL("sceKernelUnlink", Unlink);
KERNEL("sceKernelRename", Rename);
KERNEL("sceKernelGetDirectMemorySize", GetDirectMemorySize);
KERNEL("sceKernelAllocateDirectMemory", AllocateDirectMemory);
KERNEL("sceKernelReleaseDirectMemory", ReleaseDirectMemory);
KERNEL("sceKernelMapDirectMemory", MapDirectMemory);
KERNEL("sceKernelMunmap", Munmap);
KERNEL("sceKernelDirectMemoryQuery", DirectMemoryQuery);
