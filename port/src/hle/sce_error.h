#pragma once
#include <cstdint>

namespace hle {

// PS4 (FreeBSD) errno values; they differ from the Windows CRT's for several codes.
constexpr int EPERM_BSD = 1, ENOENT_BSD = 2, EIO_BSD = 5, EBADF_BSD = 9, ENOMEM_BSD = 12, EACCES_BSD = 13,
              EBUSY_BSD = 16, EEXIST_BSD = 17, EINVAL_BSD = 22, EAGAIN_BSD = 35, ETIMEDOUT_BSD = 60;

// sceKernel* functions return 0x8002xxxx error codes built from errno values.
constexpr int SceError(int bsd_errno) { return int(0x80020000u | uint32_t(bsd_errno)); }

}  // namespace hle
