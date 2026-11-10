// libSceFios2: Sony's file I/O library, which the game uses for its asset streaming.
// File handles are host descriptors; the one asynchronous call completes immediately.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include "host/compat.h"
#include <sys/stat.h>

#include "hle/hle.h"

namespace {

namespace fs = std::filesystem;

// Error values: 0x80820005 (bad path) and 0x80820002 (no free handle) are confirmed by the game,
// which handles bad-path silently; the others are guesses in the same range.
constexpr int kFiosErrBadPath = int(0x80820005);
constexpr int kFiosErrEof = int(0x80820009);
constexpr int kFiosErrBadHandle = int(0x8082000B);

constexpr uint32_t kOpenRead = 0x1, kOpenWrite = 0x2, kOpenAppend = 0x4, kOpenCreate = 0x8, kOpenTruncate = 0x10;
constexpr uint32_t kStatDirectory = 0x1, kStatReadOnly = 0x2;

struct FiosBuffer { void* data; uint64_t length; };
struct FiosOpenParams {
    uint32_t open_flags : 16;
    uint32_t op_flags : 16;
    uint32_t reserved;
    FiosBuffer buffer;
};
struct FiosStat {
    int64_t file_offset, file_size, access_date, modification_date;  // the game reads stat_flags at 0x20
    uint32_t stat_flags, reserved;
    int64_t uid, gid, dev, ino, mode;
};
static_assert(offsetof(FiosStat, stat_flags) == 0x20);
struct FiosDirEntry {
    int64_t file_size;
    uint32_t stat_flags;
    uint16_t name_length, full_path_length, offset_to_name;
    uint16_t reserved[3];
    char full_path[1024];
};

struct Dir {
    std::string guest_path;
    std::vector<fs::directory_entry> entries;
    size_t next = 0;
};
struct Op { int64_t result, count; };

std::mutex g_mutex;
// LDPORT_TRACE_IO=1 logs every file open, read and seek.
const bool g_trace = std::getenv("LDPORT_TRACE_IO") != nullptr;
int g_next_dh = 1, g_next_op = 1;
std::map<int, Dir> g_dirs;
std::map<int, Op> g_ops;

SYSV int Initialize(const void*) { return 0; }

SYSV int FHOpenSync(const void*, int* fh, const char* path, const FiosOpenParams* params) {
    const uint32_t f = params ? params->open_flags : kOpenRead;
    int host = _O_BINARY;
    host |= (f & kOpenWrite) ? ((f & kOpenRead) ? _O_RDWR : _O_WRONLY) : _O_RDONLY;
    if (f & kOpenAppend) host |= _O_APPEND;
    if (f & kOpenCreate) host |= _O_CREAT;
    if (f & kOpenTruncate) host |= _O_TRUNC;
    const int fd = _open(hle::HostPath(path).c_str(), host, _S_IREAD | _S_IWRITE);
    if (g_trace) std::fprintf(stderr, "[io] fios open %s flags %#x -> %d\n", path, f, fd);
    if (fd < 0) return kFiosErrBadPath;
    *fh = fd;
    return 0;
}
SYSV int FHCloseSync(const void*, int fh) {
    if (g_trace) std::fprintf(stderr, "[io] fios close fd %d\n", fh);
    return _close(fh) < 0 ? kFiosErrBadHandle : 0;
}
SYSV int64_t FHReadSync(const void*, int fh, void* buf, int64_t len) {
    int64_t total = 0;
    while (total < len) {  // _read takes 32-bit sizes
        const unsigned chunk = unsigned(std::min<int64_t>(len - total, 0x40000000));
        const int r = _read(fh, static_cast<uint8_t*>(buf) + total, chunk);
        if (r < 0) return kFiosErrBadHandle;
        if (r == 0) break;
        total += r;
    }
    if (g_trace) std::fprintf(stderr, "[io] fios read fd %d len %lld -> %lld\n", fh, (long long)len, (long long)total);
    return total;
}
SYSV int64_t FHWriteSync(const void*, int fh, const void* buf, int64_t len) {
    const int r = _write(fh, buf, unsigned(len));
    return r < 0 ? kFiosErrBadHandle : r;
}
SYSV int64_t FHSeek(int fh, int64_t offset, int whence) {
    const int64_t r = _lseeki64(fh, offset, whence);
    if (g_trace) std::fprintf(stderr, "[io] fios seek fd %d %lld/%d -> %lld\n", fh, (long long)offset, whence, (long long)r);
    return r;
}

void FillStat(const struct _stat64& st, int64_t offset, FiosStat* out) {
    std::memset(out, 0, sizeof(*out));
    out->file_offset = offset;
    out->file_size = st.st_size;
    out->access_date = st.st_atime;
    out->modification_date = st.st_mtime;
    if (st.st_mode & _S_IFDIR) out->stat_flags |= kStatDirectory;
    if (!(st.st_mode & _S_IWRITE)) out->stat_flags |= kStatReadOnly;
}
SYSV int FHStatSync(const void*, int fh, FiosStat* out) {
    struct _stat64 st;
    if (_fstat64(fh, &st) < 0) return kFiosErrBadHandle;
    FillStat(st, _telli64(fh), out);
    if (g_trace) std::fprintf(stderr, "[io] fios stat fd %d -> size %lld flags %#x\n", fh, (long long)out->file_size, out->stat_flags);
    return 0;
}

int OpenDir(int* dh, const char* path) {
    // Merge every overlay layer; an entry in a higher layer hides the same name below it.
    Dir dir{path};
    bool found = false;
    std::set<std::string> seen;
    for (const auto& layer : hle::HostPathLayers(path)) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(layer, ec)) {
            if (seen.insert(e.path().filename().string()).second) dir.entries.push_back(e);
        }
        found |= !ec;
    }
    if (!found) return kFiosErrBadPath;
    std::lock_guard lock(g_mutex);
    *dh = g_next_dh++;
    g_dirs[*dh] = std::move(dir);
    return 0;
}
SYSV int DHOpenSync(const void*, int* dh, const char* path, FiosBuffer) { return OpenDir(dh, path); }
SYSV int DHOpen(const void*, int* dh, const char* path, FiosBuffer) {
    const int r = OpenDir(dh, path);
    std::lock_guard lock(g_mutex);
    g_ops[g_next_op] = {r, 0};
    return g_next_op++;
}
SYSV int DHReadSync(const void*, int dh, FiosDirEntry* out) {
    std::lock_guard lock(g_mutex);
    auto it = g_dirs.find(dh);
    if (it == g_dirs.end()) return kFiosErrBadHandle;
    Dir& d = it->second;
    if (d.next >= d.entries.size()) return kFiosErrEof;
    const fs::directory_entry& e = d.entries[d.next++];

    std::memset(out, 0, sizeof(*out));
    const std::string name = e.path().filename().string();
    std::string full = d.guest_path;
    if (!full.empty() && full.back() != '/') full += '/';
    out->offset_to_name = uint16_t(full.size());
    full += name;
    std::snprintf(out->full_path, sizeof(out->full_path), "%s", full.c_str());
    out->name_length = uint16_t(name.size());
    out->full_path_length = uint16_t(full.size());
    std::error_code ec;
    if (e.is_directory(ec)) out->stat_flags |= kStatDirectory;
    else out->file_size = int64_t(e.file_size(ec));
    return 0;
}
SYSV int DHCloseSync(const void*, int dh) {
    std::lock_guard lock(g_mutex);
    return g_dirs.erase(dh) ? 0 : kFiosErrBadHandle;
}

SYSV int OpWait(int op) {
    std::lock_guard lock(g_mutex);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? kFiosErrBadHandle : int(it->second.result);
}
SYSV int64_t OpGetActualCount(int op) {
    std::lock_guard lock(g_mutex);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? 0 : it->second.count;
}
SYSV int OpDelete(int op) {
    std::lock_guard lock(g_mutex);
    g_ops.erase(op);
    return 0;
}

SYSV int DirectoryCreateSync(const void*, const char* path) {
    std::error_code ec;
    fs::create_directories(hle::HostPath(path), ec);
    return ec ? kFiosErrBadPath : 0;
}
SYSV int DirectoryDeleteSync(const void*, const char* path) {
    std::error_code ec;
    return fs::remove(hle::HostPath(path), ec) ? 0 : kFiosErrBadPath;
}
SYSV int FileDeleteSync(const void*, const char* path) {
    std::error_code ec;
    return fs::remove(hle::HostPath(path), ec) ? 0 : kFiosErrBadPath;
}
SYSV int RenameSync(const void*, const char* from, const char* to) {
    std::error_code ec;
    fs::rename(hle::HostPath(from), hle::HostPath(to), ec);
    return ec ? kFiosErrBadPath : 0;
}

}  // namespace

#define FIOS(name, fn) HLE_EXPORT("libSceFios2", name, reinterpret_cast<const void*>(&fn))
FIOS("sceFiosInitialize", Initialize);
FIOS("sceFiosFHOpenSync", FHOpenSync);
FIOS("sceFiosFHCloseSync", FHCloseSync);
FIOS("sceFiosFHReadSync", FHReadSync);
FIOS("sceFiosFHWriteSync", FHWriteSync);
FIOS("sceFiosFHSeek", FHSeek);
FIOS("sceFiosFHStatSync", FHStatSync);
FIOS("sceFiosDHOpenSync", DHOpenSync);
FIOS("sceFiosDHOpen", DHOpen);
FIOS("sceFiosDHReadSync", DHReadSync);
FIOS("sceFiosDHCloseSync", DHCloseSync);
FIOS("sceFiosOpWait", OpWait);
FIOS("sceFiosOpGetActualCount", OpGetActualCount);
FIOS("sceFiosOpDelete", OpDelete);
FIOS("sceFiosDirectoryCreateSync", DirectoryCreateSync);
FIOS("sceFiosDirectoryDeleteSync", DirectoryDeleteSync);
FIOS("sceFiosFileDeleteSync", FileDeleteSync);
FIOS("sceFiosRenameSync", RenameSync);
