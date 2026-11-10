// The few MSVC C runtime names the port uses, mapped to POSIX on Linux (the Steam Deck build).
#pragma once

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#define _O_BINARY 0
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_APPEND O_APPEND
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_EXCL O_EXCL
#define _S_IREAD S_IRUSR
#define _S_IWRITE S_IWUSR
#define _S_IFDIR S_IFDIR

#define _open open
#define _close close
#define _read read
#define _write write
#define _unlink unlink
#define _fileno fileno
#define _lseeki64 lseek
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _stat64 stat  // both the struct and the function
#define _fstat64 fstat
inline long long _telli64(int fd) { return lseek(fd, 0, SEEK_CUR); }

inline int _gmtime64_s(struct tm* out, const long long* t) {
    const time_t v = time_t(*t);
    return gmtime_r(&v, out) ? 0 : 1;
}
inline int _localtime64_s(struct tm* out, const long long* t) {
    const time_t v = time_t(*t);
    return localtime_r(&v, out) ? 0 : 1;
}
inline void* _aligned_malloc(size_t size, size_t align) {
    void* p = nullptr;
    return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) == 0 ? p : nullptr;
}
#define _aligned_free free
typedef long long __time64_t;
#endif
