// ldpkg: unpacks the LEGO Dimensions PS4 packages (fake-signed "fPKG" .pkg files, as shadPS4 users have them) into
// the folder layout the port runs from, and unwraps eboot.bin into eboot.elf.
//
//   ldpkg [options] -o <output folder> <.pkg file or folder of .pkg files>...
//     --layout extracted   (default) elf/eboot.elf, CUSA01176/uroot (1.24 update), CUSA01176-base/uroot (1.00),
//                          addcont/<label>/uroot (add-on packs)
//     --layout install     the installed layout: eboot.elf, update/, base/, addcont/<label>/uroot
//     --threads <n>        files extracted at once (default: CPU count, at most 8)
//     --log <file>         also write everything printed to this file
//     --overwrite          re-extract files that are already there (default: skip a file whose size matches,
//                          so an interrupted run picks up where it stopped)
//   ldpkg --info <.pkg file or folder>...   lists what each package is; exit code 0 when the 1.24 update is among them
//   ldpkg --dlc [--info] -o <game folder> <.pkg file or folder>...   add-on packs only, into an installed game
//   ldpkg --keys <file>                     checks a keys file (exit code 0 when the keys work)
//   ldpkg --self2elf <eboot.bin> <eboot.elf>
//     --keys <file>        the two fPKG RSA keys (see keys.example.inc); needed unless built in from keys.inc
//
// Written for this port from the publicly documented PS4 formats; it contains no code from shadPS4 (GPL-2.0) or
// LibOrbisPkg (LGPL-3.0). The two RSA keys every fPKG tool carries (the "fake" image-key keyset and the package
// derived-key-3 keyset) are not in the repository: --keys loads them, or a local, git-ignored keys.inc builds them in.
//
// The format, briefly:
//   PKG   big-endian header; an entry table (0x10 entry keys, 0x20 image key, 0x1000 param.sfo, ...); the PFS image
//         at pfs_image_offset.
//   keys  dk3   = RSA-decrypt(entry_keys.key[3], derived-key-3 keyset)
//         ivkey = SHA-256(image-key table record (32 bytes) || dk3); image key = AES-128-CBC-decrypt(key = ivkey[16..],
//                 iv = ivkey[..16]) of the 256-byte IMAGE_KEY entry; EKPFS = RSA-decrypt(image key, fake keyset)
//         (when pfs_image_flags bit 61 is set, EKPFS = HMAC-SHA256(EKPFS, seed) first)
//         HMAC-SHA256(EKPFS, le32(1) || seed) = tweak key (16) || data key (16); seed = PFS header + 0x370.
//   PFS   outer image: signed + encrypted, block size 0x10000; AES-128-XTS over 0x1000-byte sectors (sector number =
//         offset in the image / 0x1000) from the first block on (block 0, the header, is plain). Inodes start in
//         block 1 (0x2C8 bytes signed, 0xA8 unsigned); inode 0 is the super-root, whose "uroot" holds pfs_image.dat.
//   PFSC  pfs_image.dat is compressed: header, then a table of (sectors + 1) u64 offsets; a sector whose stored size is
//         the block size is raw, a smaller one is zlib (2-byte header + deflate), a larger one is zeros.
//   inner the decompressed image is a plain PFS whose super-root "uroot" is the game's app0 folder.
//   SELF  eboot.bin is a fake-signed SELF with plain segments: the ELF header and program headers follow the segment
//         table, and each "blocked" segment is copied to its program header's file offset.

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>
#include <wmmintrin.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <intrin.h>
#include <windows.h>
#else
#include <cpuid.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] static void Fail(const std::string& msg) { throw Error(msg); }

// Everything printed also goes to the --log file when there is one.
static FILE* g_log = nullptr;
static void Out(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    fflush(stdout);
    if (g_log) {
        va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fflush(g_log);
    }
}


static u32 Be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
static u64 Be64(const u8* p) { return u64(Be32(p)) << 32 | Be32(p + 4); }
static u16 Le16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 Le32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 Le64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

static std::string U8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
static fs::path FromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// ---------------------------------------------------------------------------------------------------------------
// Files: positional reads (one handle per thread), buffered writes.

class InFile {
public:
    explicit InFile(const fs::path& p) {
#ifdef _WIN32
        h_ = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) Fail("cannot open " + U8(p));
        LARGE_INTEGER sz;
        GetFileSizeEx(h_, &sz);
        size_ = u64(sz.QuadPart);
#else
        fd_ = open(p.c_str(), O_RDONLY);
        if (fd_ < 0) Fail("cannot open " + U8(p));
        size_ = u64(lseek(fd_, 0, SEEK_END));
#endif
        path_ = p;
    }
    ~InFile() {
#ifdef _WIN32
        CloseHandle(h_);
#else
        close(fd_);
#endif
    }
    InFile(const InFile&) = delete;
    InFile& operator=(const InFile&) = delete;

    u64 Size() const { return size_; }
    void Read(u64 off, void* dst, size_t n) const {
        u8* d = static_cast<u8*>(dst);
        while (n) {
            const u32 chunk = u32(std::min<size_t>(n, 1u << 30));
#ifdef _WIN32
            OVERLAPPED ov{};
            ov.Offset = u32(off);
            ov.OffsetHigh = u32(off >> 32);
            DWORD got = 0;
            if (!ReadFile(h_, d, chunk, &got, &ov) || got == 0) Fail("read error in " + U8(path_));
#else
            const ssize_t got = pread(fd_, d, chunk, off_t(off));
            if (got <= 0) Fail("read error in " + U8(path_));
#endif
            d += got;
            off += u64(got);
            n -= size_t(got);
        }
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    u64 size_ = 0;
    fs::path path_;
};

static FILE* OpenWrite(const fs::path& p) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"wb");
#else
    FILE* f = fopen(p.c_str(), "wb");
#endif
    if (!f) Fail("cannot create " + U8(p));
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104).

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset() {
        static const u32 init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h_, init, sizeof h_);
        len_ = 0;
        used_ = 0;
    }
    void Update(const void* data, size_t n) {
        const u8* p = static_cast<const u8*>(data);
        len_ += n;
        while (n) {
            const size_t take = std::min(n, size_t(64) - used_);
            memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ == 64) {
                Block(buf_);
                used_ = 0;
            }
        }
    }
    void Final(u8 out[32]) {
        const u64 bits = len_ * 8;
        const u8 one = 0x80, zero = 0;
        Update(&one, 1);
        while (used_ != 56) Update(&zero, 1);
        u8 lenbe[8];
        for (int i = 0; i < 8; i++) lenbe[i] = u8(bits >> (56 - 8 * i));
        Update(lenbe, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = u8(h_[i] >> (24 - 8 * j));
    }

private:
    static u32 Rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const u8* p) {
        static const u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; i++) w[i] = Be32(p + 4 * i);
        for (int i = 16; i < 64; i++) {
            const u32 s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; i++) {
            const u32 t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const u32 t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    u32 h_[8];
    u64 len_;
    u8 buf_[64];
    size_t used_;
};

static void HmacSha256(const u8* key, size_t keylen, const u8* msg, size_t n, u8 out[32]) {
    u8 k[64] = {};
    if (keylen > 64) {
        Sha256 s;
        s.Update(key, keylen);
        s.Final(k);
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64], inner[32];
    for (int i = 0; i < 64; i++) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    Sha256 s;
    s.Update(ipad, 64);
    s.Update(msg, n);
    s.Final(inner);
    s.Reset();
    s.Update(opad, 64);
    s.Update(inner, 32);
    s.Final(out);
}

// ---------------------------------------------------------------------------------------------------------------
// AES-128 with the AES-NI instructions (every x86-64 CPU the port can run on has them), CBC and XTS decryption.

static bool HasAesNi() {
#ifdef _WIN32
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 25) & 1;
#else
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 25) & 1);
#endif
}

class Aes128 {
public:
    explicit Aes128(const u8 key[16]) {
        enc_[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
#define LDPKG_EXPAND(i, rcon) enc_[i] = Step(enc_[i - 1], _mm_aeskeygenassist_si128(enc_[i - 1], rcon))
        LDPKG_EXPAND(1, 0x01); LDPKG_EXPAND(2, 0x02); LDPKG_EXPAND(3, 0x04); LDPKG_EXPAND(4, 0x08);
        LDPKG_EXPAND(5, 0x10); LDPKG_EXPAND(6, 0x20); LDPKG_EXPAND(7, 0x40); LDPKG_EXPAND(8, 0x80);
        LDPKG_EXPAND(9, 0x1b); LDPKG_EXPAND(10, 0x36);
#undef LDPKG_EXPAND
        dec_[0] = enc_[10];
        for (int i = 1; i < 10; i++) dec_[i] = _mm_aesimc_si128(enc_[10 - i]);
        dec_[10] = enc_[0];
    }
    __m128i Encrypt(__m128i x) const {
        x = _mm_xor_si128(x, enc_[0]);
        for (int i = 1; i < 10; i++) x = _mm_aesenc_si128(x, enc_[i]);
        return _mm_aesenclast_si128(x, enc_[10]);
    }
    __m128i Decrypt(__m128i x) const {
        x = _mm_xor_si128(x, dec_[0]);
        for (int i = 1; i < 10; i++) x = _mm_aesdec_si128(x, dec_[i]);
        return _mm_aesdeclast_si128(x, dec_[10]);
    }
    // Four blocks at once, so the AES unit's pipeline stays full.
    void Decrypt4(__m128i& a, __m128i& b, __m128i& c, __m128i& d) const {
        a = _mm_xor_si128(a, dec_[0]); b = _mm_xor_si128(b, dec_[0]);
        c = _mm_xor_si128(c, dec_[0]); d = _mm_xor_si128(d, dec_[0]);
        for (int i = 1; i < 10; i++) {
            a = _mm_aesdec_si128(a, dec_[i]); b = _mm_aesdec_si128(b, dec_[i]);
            c = _mm_aesdec_si128(c, dec_[i]); d = _mm_aesdec_si128(d, dec_[i]);
        }
        a = _mm_aesdeclast_si128(a, dec_[10]); b = _mm_aesdeclast_si128(b, dec_[10]);
        c = _mm_aesdeclast_si128(c, dec_[10]); d = _mm_aesdeclast_si128(d, dec_[10]);
    }

private:
    static __m128i Step(__m128i key, __m128i gen) {
        gen = _mm_shuffle_epi32(gen, 0xff);
        key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
        key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
        key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
        return _mm_xor_si128(key, gen);
    }
    __m128i enc_[11], dec_[11];
};

static void AesCbcDecrypt(const u8 key[16], const u8 iv[16], const u8* src, u8* dst, size_t n) {
    const Aes128 aes(key);
    __m128i prev = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
    for (size_t i = 0; i + 16 <= n; i += 16) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_xor_si128(aes.Decrypt(c), prev));
        prev = c;
    }
}

// The next XTS tweak: multiply by x in GF(2^128) (little-endian, reduction polynomial 0x87).
static __m128i XtsNext(__m128i t) {
    u64 v[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(v), t);
    const u64 carry = v[1] >> 63;
    v[1] = (v[1] << 1) | (v[0] >> 63);
    v[0] = (v[0] << 1) ^ (carry ? 0x87 : 0);
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(v));
}

struct XtsKeys {
    Aes128 data, tweak;
};

// Decrypts one 0x1000-byte sector in place.
static void XtsDecryptSector(const XtsKeys& k, u64 sector, u8* p) {
    u64 tv[2] = {sector, 0};
    __m128i t = k.tweak.Encrypt(_mm_loadu_si128(reinterpret_cast<const __m128i*>(tv)));
    __m128i* b = reinterpret_cast<__m128i*>(p);
    for (int i = 0; i < 0x1000 / 16; i += 4) {
        const __m128i t0 = t, t1 = XtsNext(t0), t2 = XtsNext(t1), t3 = XtsNext(t2);
        t = XtsNext(t3);
        __m128i x0 = _mm_xor_si128(_mm_loadu_si128(b + i), t0), x1 = _mm_xor_si128(_mm_loadu_si128(b + i + 1), t1);
        __m128i x2 = _mm_xor_si128(_mm_loadu_si128(b + i + 2), t2), x3 = _mm_xor_si128(_mm_loadu_si128(b + i + 3), t3);
        k.data.Decrypt4(x0, x1, x2, x3);
        _mm_storeu_si128(b + i, _mm_xor_si128(x0, t0));
        _mm_storeu_si128(b + i + 1, _mm_xor_si128(x1, t1));
        _mm_storeu_si128(b + i + 2, _mm_xor_si128(x2, t2));
        _mm_storeu_si128(b + i + 3, _mm_xor_si128(x3, t3));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// RSA-2048 private-key decryption (Montgomery exponentiation) and PKCS#1 v1.5 unpadding.

namespace rsa {
// The two fPKG keys (see keys.example.inc) aren't in the repository. They come from --keys <file> at run time, or are
// built in from a local keys.inc (git-ignored) unless LDPKG_NO_BUILTIN_KEYS is defined, as setup's ldpkg.exe is.
#if __has_include("keys.inc") && !defined(LDPKG_NO_BUILTIN_KEYS)
namespace builtin {
#include "keys.inc"
}
#define LDPKG_HAVE_BUILTIN_KEYS 1
#endif
static u8 kFakeN[256], kFakeD[256], kDk3N[256], kDk3D[256];
static bool g_have_keys = false;

constexpr int kLimbs = 64;  // 32-bit limbs
using Num = std::vector<u32>;

static Num FromBe(const u8* p) {
    Num n(kLimbs);
    for (int i = 0; i < kLimbs; i++) n[i] = Be32(p + 4 * (kLimbs - 1 - i));
    return n;
}
static bool Geq(const Num& a, const Num& b) {
    for (int i = kLimbs - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}
static void Sub(Num& a, const Num& b) {
    u64 borrow = 0;
    for (int i = 0; i < kLimbs; i++) {
        const u64 d = u64(a[i]) - b[i] - borrow;
        a[i] = u32(d);
        borrow = (d >> 63) & 1;
    }
}
// a*b*R^-1 mod n (CIOS).
static Num MontMul(const Num& a, const Num& b, const Num& n, u32 ninv) {
    std::vector<u32> t(kLimbs + 2, 0);
    for (int i = 0; i < kLimbs; i++) {
        u64 c = 0;
        for (int j = 0; j < kLimbs; j++) {
            const u64 s = u64(t[j]) + u64(a[j]) * b[i] + c;
            t[j] = u32(s);
            c = s >> 32;
        }
        u64 s = u64(t[kLimbs]) + c;
        t[kLimbs] = u32(s);
        t[kLimbs + 1] = u32(s >> 32);
        const u32 m = t[0] * ninv;
        s = u64(t[0]) + u64(m) * n[0];
        c = s >> 32;
        for (int j = 1; j < kLimbs; j++) {
            s = u64(t[j]) + u64(m) * n[j] + c;
            t[j - 1] = u32(s);
            c = s >> 32;
        }
        s = u64(t[kLimbs]) + c;
        t[kLimbs - 1] = u32(s);
        t[kLimbs] = t[kLimbs + 1] + u32(s >> 32);
    }
    Num r(t.begin(), t.begin() + kLimbs);
    if (t[kLimbs] || Geq(r, n)) Sub(r, n);
    return r;
}

// x^e mod n, all big-endian 256-byte numbers, into out.
static void ModExp(const u8 cipher[256], const u8 nbe[256], const u8 dbe[256], u8 em[256]) {
    const Num n = FromBe(nbe), c = FromBe(cipher);
    u32 ninv = 1;  // -n^-1 mod 2^32 by Newton's iteration
    for (int i = 0; i < 5; i++) ninv *= 2 - n[0] * ninv;
    ninv = 0u - ninv;
    Num r(kLimbs, 0);  // R^2 mod n, R = 2^2048, by doubling 1 4096 times
    r[0] = 1;
    for (int i = 0; i < 2 * 32 * kLimbs; i++) {
        u32 carry = 0;
        for (int j = 0; j < kLimbs; j++) {
            const u32 nc = r[j] >> 31;
            r[j] = (r[j] << 1) | carry;
            carry = nc;
        }
        if (carry || Geq(r, n)) Sub(r, n);
    }
    Num one(kLimbs, 0);
    one[0] = 1;
    const Num x = MontMul(c, r, n, ninv);
    Num acc = MontMul(one, r, n, ninv);
    for (int i = 0; i < 256; i++)
        for (int bit = 7; bit >= 0; bit--) {
            acc = MontMul(acc, acc, n, ninv);
            if ((dbe[i] >> bit) & 1) acc = MontMul(acc, x, n, ninv);
        }
    acc = MontMul(acc, one, n, ninv);
    for (int i = 0; i < kLimbs; i++)
        for (int j = 0; j < 4; j++) em[4 * (kLimbs - 1 - i) + j] = u8(acc[i] >> (24 - 8 * j));
}

// True when (n, d) is a working RSA private key for e = 65537: m^e^d == m for a test message below n.
static bool KeyPairWorks(const u8 nbe[256], const u8 dbe[256]) {
    if (!(nbe[255] & 1) || nbe[0] < 0x80) return false;  // a 2048-bit modulus is odd with its top bit set
    u8 m[256] = {}, e[256] = {}, c[256], back[256];
    for (int i = 1; i < 256; i++) m[i] = u8(i * 37 + 11);
    e[253] = 1, e[255] = 1;
    ModExp(m, nbe, e, c);
    ModExp(c, nbe, dbe, back);
    return std::memcmp(m, back, 256) == 0;
}

// Returns the 32-byte message inside a PKCS#1 v1.5 (type 2) block encrypted to the key (n, d).
static std::vector<u8> Decrypt(const u8 cipher[256], const u8 nbe[256], const u8 dbe[256]) {
    u8 em[256];
    ModExp(cipher, nbe, dbe, em);
    if (em[0] != 0 || em[1] != 2) Fail("RSA decryption failed (not a fake-signed package?)");
    int sep = 2;
    while (sep < 256 && em[sep] != 0) sep++;
    if (sep < 10 || sep >= 256) Fail("RSA decryption failed (bad padding)");
    return std::vector<u8>(em + sep + 1, em + 256);
}

static void UseKeys(const u8* k) {  // 1024 bytes: fake N, fake D, derived-key-3 N, derived-key-3 D
    if (!KeyPairWorks(k, k + 256) || !KeyPairWorks(k + 512, k + 768))
        Fail("the keys don't work: the file needs the fake-package keyset and the derived-key-3 keyset, each a 256-byte "
             "modulus N then private exponent D, in that order (see keys.example.inc)");
    std::memcpy(kFakeN, k, 256), std::memcpy(kFakeD, k + 256, 256);
    std::memcpy(kDk3N, k + 512, 256), std::memcpy(kDk3D, k + 768, 256);
    g_have_keys = true;
}

// A keys file: 1024 raw bytes, or text holding 1024 bytes as 0xNN values (a keys.inc) or plain hex digits.
static void LoadKeys(const fs::path& path) {
    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) Fail("cannot open the keys file " + U8(path));
    std::string raw;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && raw.size() < (1u << 20);) raw.append(buf, n);
    std::fclose(f);
    std::vector<u8> k;
    if (raw.size() == 1024) {
        k.assign(raw.begin(), raw.end());
    } else {
        auto hex = [](char c) { return std::isxdigit(u8(c)) ? (c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10)) : -1; };
        const bool c_style = raw.find("0x") != std::string::npos || raw.find("0X") != std::string::npos;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {  // skip // comments
                while (i < raw.size() && raw[i] != '\n') i++;
            } else if (c_style) {
                if (raw[i] == '0' && i + 3 < raw.size() && (raw[i + 1] == 'x' || raw[i + 1] == 'X') &&
                    hex(raw[i + 2]) >= 0 && hex(raw[i + 3]) >= 0) {
                    k.push_back(u8(hex(raw[i + 2]) * 16 + hex(raw[i + 3])));
                    i += 3;
                }
            } else if (hex(raw[i]) >= 0 && i + 1 < raw.size() && hex(raw[i + 1]) >= 0) {
                k.push_back(u8(hex(raw[i]) * 16 + hex(raw[i + 1])));
                i++;
            }
        }
    }
    if (k.size() != 1024)
        Fail(U8(path) + " doesn't hold the keys: it needs 1024 bytes (four 256-byte numbers), found " +
             std::to_string(k.size()));
    UseKeys(k.data());
}

static void UseBuiltinKeys() {
#ifdef LDPKG_HAVE_BUILTIN_KEYS
    u8 k[1024];
    std::memcpy(k, builtin::kFakeN, 256), std::memcpy(k + 256, builtin::kFakeD, 256);
    std::memcpy(k + 512, builtin::kDk3N, 256), std::memcpy(k + 768, builtin::kDk3D, 256);
    UseKeys(k);
#endif
}
}  // namespace rsa

// ---------------------------------------------------------------------------------------------------------------
// Inflate (RFC 1951), table driven, into a fixed-size buffer.

class Inflater {
public:
    // Decompresses a raw deflate stream; returns the number of bytes written to out (at most cap).
    size_t Run(const u8* in, size_t n, u8* out, size_t cap) {
        in_ = in, n_ = n, pos_ = 0, bits_ = 0, nbits_ = 0, over_ = 0;
        size_t o = 0;
        for (bool last = false; !last;) {
            last = Get(1);
            const u32 type = Get(2);
            if (type == 0) {
                Get(nbits_ & 7);
                const u32 len = Get(16), nlen = Get(16);
                if ((len ^ 0xffff) != nlen) Fail("corrupt deflate data (stored block)");
                if (o + len > cap) Fail("deflate data larger than its sector");
                for (u32 i = 0; i < len; i++) out[o++] = u8(Get(8));
                continue;
            }
            if (type == 1) {
                Fixed();
            } else if (type == 2) {
                Dynamic();
            } else {
                Fail("corrupt deflate data (block type 3)");
            }
            static const u16 lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            static const u8 lextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            static const u16 dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
            static const u8 dextra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            for (;;) {
                const u32 sym = Decode(lit_);
                if (sym < 256) {
                    if (o >= cap) Fail("deflate data larger than its sector");
                    out[o++] = u8(sym);
                } else if (sym == 256) {
                    break;
                } else {
                    const u32 li = sym - 257;
                    if (li >= 29) Fail("corrupt deflate data (length code)");
                    const u32 len = lbase[li] + Get(lextra[li]);
                    const u32 di = Decode(dist_);
                    if (di >= 30) Fail("corrupt deflate data (distance code)");
                    const u32 dist = dbase[di] + Get(dextra[di]);
                    if (dist > o) Fail("corrupt deflate data (distance too far back)");
                    if (o + len > cap) Fail("deflate data larger than its sector");
                    const u8* from = out + o - dist;
                    for (u32 i = 0; i < len; i++) out[o + i] = from[i];
                    o += len;
                }
            }
            if (over_ > 8) Fail("corrupt deflate data (truncated)");
        }
        return o;
    }

private:
    static constexpr int kFast = 10;
    struct Huff {
        u16 count[16];
        u16 symbol[320];
        u16 fast[1 << kFast];  // (symbol << 4) | length, 0 = longer than kFast bits
    };

    void Refill() {
        while (nbits_ <= 56) {
            if (pos_ < n_) bits_ |= u64(in_[pos_++]) << nbits_;
            else over_++;
            nbits_ += 8;
        }
    }
    u32 Get(int n) {
        if (!n) return 0;
        if (nbits_ < n) Refill();
        const u32 v = u32(bits_ & ((1ull << n) - 1));
        bits_ >>= n;
        nbits_ -= n;
        return v;
    }
    static void Build(Huff& h, const u8* lens, int n) {
        memset(h.count, 0, sizeof h.count);
        memset(h.fast, 0, sizeof h.fast);
        for (int i = 0; i < n; i++) h.count[lens[i]]++;
        h.count[0] = 0;
        u16 offs[16];
        offs[1] = 0;
        for (int l = 1; l < 15; l++) offs[l + 1] = u16(offs[l] + h.count[l]);
        for (int i = 0; i < n; i++)
            if (lens[i]) h.symbol[offs[lens[i]]++] = u16(i);
        u32 code = 0;
        int k = 0;
        for (int l = 1; l <= 15; l++) {
            for (int i = 0; i < h.count[l]; i++, k++, code++) {
                if (l > kFast) continue;
                u32 rev = 0;
                for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
                for (u32 j = rev; j < (1u << kFast); j += 1u << l) h.fast[j] = u16(h.symbol[k] << 4 | l);
            }
            code <<= 1;
        }
    }
    u32 Decode(const Huff& h) {
        if (nbits_ < 15) Refill();
        if (const u16 e = h.fast[bits_ & ((1u << kFast) - 1)]) {
            bits_ >>= e & 15;
            nbits_ -= e & 15;
            return e >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= 15; l++) {
            code |= int((bits_ >> (l - 1)) & 1);
            const int count = h.count[l];
            if (code - first < count) {
                bits_ >>= l;
                nbits_ -= l;
                return h.symbol[index + code - first];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        Fail("corrupt deflate data (bad code)");
    }
    void Fixed() {
        u8 l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        Build(lit_, l, 288);
        u8 d[30];
        memset(d, 5, sizeof d);
        Build(dist_, d, 30);
    }
    void Dynamic() {
        const int nlit = int(Get(5)) + 257, ndist = int(Get(5)) + 1, ncode = int(Get(4)) + 4;
        if (nlit > 286 || ndist > 30) Fail("corrupt deflate data (header)");
        static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        u8 cl[19] = {};
        for (int i = 0; i < ncode; i++) cl[order[i]] = u8(Get(3));
        Huff clh;
        Build(clh, cl, 19);
        u8 lens[320] = {};
        for (int i = 0; i < nlit + ndist;) {
            const u32 sym = Decode(clh);
            if (sym < 16) {
                lens[i++] = u8(sym);
                continue;
            }
            u32 rep;
            u8 val = 0;
            if (sym == 16) {
                if (!i) Fail("corrupt deflate data (repeat with no length)");
                val = lens[i - 1];
                rep = 3 + Get(2);
            } else if (sym == 17) {
                rep = 3 + Get(3);
            } else {
                rep = 11 + Get(7);
            }
            if (i + int(rep) > nlit + ndist) Fail("corrupt deflate data (lengths)");
            while (rep--) lens[i++] = val;
        }
        Build(lit_, lens, nlit);
        Build(dist_, lens + nlit, ndist);
    }

    const u8* in_ = nullptr;
    size_t n_ = 0, pos_ = 0;
    u64 bits_ = 0;
    int nbits_ = 0, over_ = 0;
    Huff lit_, dist_;
};

// ---------------------------------------------------------------------------------------------------------------
// PKG: header, entries, param.sfo, keys.

struct SfoInfo {
    std::map<std::string, std::string> str;
    std::string Get(const std::string& k) const {
        const auto it = str.find(k);
        return it == str.end() ? std::string() : it->second;
    }
};

static SfoInfo ParseSfo(const std::vector<u8>& d) {
    SfoInfo s;
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return s;
    const u32 keys = Le32(&d[8]), data = Le32(&d[12]), count = Le32(&d[16]);
    for (u32 i = 0; i < count && 20 + i * 16 + 16 <= d.size(); i++) {
        const u8* e = &d[20 + i * 16];
        const u32 koff = keys + Le16(e), fmt = Le16(e + 2), len = Le32(e + 4), doff = data + Le32(e + 12);
        if (koff >= d.size() || doff + len > d.size()) continue;
        const std::string key(reinterpret_cast<const char*>(&d[koff]));
        if (fmt == 0x0204 || fmt == 0x0004) {
            std::string v(reinterpret_cast<const char*>(&d[doff]), len);
            v.resize(strlen(v.c_str()));
            s.str[key] = v;
        } else if (fmt == 0x0404 && len >= 4) {
            s.str[key] = std::to_string(Le32(&d[doff]));
        }
    }
    return s;
}

struct Package {
    fs::path path;
    u64 size = 0;
    std::string content_id, title_id, label, category, app_ver, version, title;
    u64 pfs_offset = 0, pfs_size = 0, pfs_flags = 0;
    std::vector<u8> entry_keys, image_key, image_key_record;

    std::string Kind() const {
        if (category == "gd") return "base game";
        if (category == "gp") return "update";
        if (category == "ac") return "add-on";
        return "category '" + category + "'";
    }
};

static Package ReadPackage(const fs::path& path) {
    Package p;
    p.path = path;
    InFile f(path);
    p.size = f.Size();
    std::vector<u8> h(0x1000);
    if (p.size < h.size()) Fail(U8(path) + " is not a PS4 package (too small)");
    f.Read(0, h.data(), h.size());
    if (Be32(&h[0]) != 0x7F434E54) Fail(U8(path) + " is not a PS4 package");
    const u32 entries = Be32(&h[0x10]), table = Be32(&h[0x18]);
    p.content_id.assign(reinterpret_cast<const char*>(&h[0x40]), 36);
    p.content_id.resize(strlen(p.content_id.c_str()));
    if (p.content_id.size() >= 16) p.title_id = p.content_id.substr(7, 9);
    if (p.content_id.size() >= 36) p.label = p.content_id.substr(20, 16);
    p.pfs_flags = Be64(&h[0x408]);
    p.pfs_offset = Be64(&h[0x410]);
    p.pfs_size = Be64(&h[0x418]);
    if (entries > 10000 || u64(table) + u64(entries) * 32 > p.size) Fail(U8(path) + ": corrupt entry table");
    std::vector<u8> t(size_t(entries) * 32);
    f.Read(table, t.data(), t.size());
    for (u32 i = 0; i < entries; i++) {
        const u8* e = &t[i * 32];
        const u32 id = Be32(e), off = Be32(e + 16), sz = Be32(e + 20);
        if (u64(off) + sz > p.size || sz > (64u << 20)) continue;
        auto read = [&] {
            std::vector<u8> v(sz);
            f.Read(off, v.data(), sz);
            return v;
        };
        if (id == 0x10) {
            p.entry_keys = read();
        } else if (id == 0x20) {
            p.image_key = read();
            p.image_key_record.assign(e, e + 32);
        } else if (id == 0x1000) {
            const SfoInfo sfo = ParseSfo(read());
            p.category = sfo.Get("CATEGORY");
            p.app_ver = sfo.Get("APP_VER");
            p.version = sfo.Get("VERSION");
            p.title = sfo.Get("TITLE");
        }
    }
    if (p.pfs_offset == 0 || p.pfs_offset + p.pfs_size > p.size)
        Fail(U8(path) + ": the package is incomplete (" + std::to_string(p.size) + " bytes; the PFS image ends at " +
             std::to_string(p.pfs_offset + p.pfs_size) + ")");
    return p;
}

static std::vector<u8> Ekpfs(const Package& p) {
    if (!rsa::g_have_keys) Fail("no package keys: give ldpkg --keys <file> (see keys.example.inc)");
    if (p.entry_keys.size() < 32 + 7 * 32 + 7 * 256 || p.image_key.size() < 256)
        Fail(U8(p.path) + ": the key entries are missing");
    const auto dk3 = rsa::Decrypt(&p.entry_keys[32 + 7 * 32 + 3 * 256], rsa::kDk3N, rsa::kDk3D);
    u8 ivkey[32];
    Sha256 s;
    s.Update(p.image_key_record.data(), 32);
    s.Update(dk3.data(), dk3.size());
    s.Final(ivkey);
    u8 imgkey[256];
    AesCbcDecrypt(ivkey + 16, ivkey, p.image_key.data(), imgkey, 256);
    try {
        return rsa::Decrypt(imgkey, rsa::kFakeN, rsa::kFakeD);
    } catch (const Error&) {
        Fail(U8(p.path) + " is not a fake-signed (fPKG) package: its image key needs console keys");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// PFS images.

struct PfsHeader {
    u16 mode = 0;
    u32 block_size = 0;
    u64 inode_count = 0, inode_blocks = 0;
    u8 seed[16] = {};
    bool Signed() const { return mode & 1; }
    bool Is64() const { return mode & 2; }
    bool Encrypted() const { return mode & 4; }
};
static PfsHeader ParsePfsHeader(const u8* h) {
    PfsHeader r;
    r.mode = Le16(h + 0x1C);
    r.block_size = Le32(h + 0x20);
    r.inode_count = Le64(h + 0x30);
    r.inode_blocks = Le64(h + 0x40);
    memcpy(r.seed, h + 0x370, 16);
    return r;
}

struct Inode {
    u16 mode = 0;
    u64 size = 0, size2 = 0;
    u32 blocks = 0;
    int32_t db[12] = {};
    u32 ib[5] = {};
    bool Dir() const { return mode & 0x4000; }
};
static Inode ParseInode(const u8* p, bool is_signed) {
    Inode n;
    n.mode = Le16(p);
    n.size = Le64(p + 8);
    n.size2 = Le64(p + 16);
    n.blocks = Le32(p + 0x60);
    const int stride = is_signed ? 36 : 4, skip = is_signed ? 32 : 0;
    for (int i = 0; i < 12; i++) n.db[i] = int32_t(Le32(p + 0x64 + i * stride + skip));
    for (int i = 0; i < 5; i++) n.ib[i] = Le32(p + 0x64 + 12 * stride + i * stride + skip);
    return n;
}

struct Dirent {
    u32 ino, type;
    std::string name;
};

// A PFS image read through a block reader; parses inodes and directories.
class Pfs {
public:
    using BlockReader = std::function<void(u64 block, u8* out)>;  // reads one whole block
    Pfs(const PfsHeader& h, BlockReader rd) : h_(h), rd_(std::move(rd)) {
        if (h_.Is64()) Fail("64-bit PFS images are not supported");
        if (h_.block_size < 0x1000 || h_.block_size > (1u << 20)) Fail("corrupt PFS header (block size)");
        const u32 isz = h_.Signed() ? 0x2C8 : 0xA8, per = h_.block_size / isz;
        std::vector<u8> b(h_.block_size);
        for (u64 i = 0; i < h_.inode_blocks && inodes_.size() < h_.inode_count; i++) {
            rd_(1 + i, b.data());
            for (u32 j = 0; j < per && inodes_.size() < h_.inode_count; j++)
                inodes_.push_back(ParseInode(&b[j * isz], h_.Signed()));
        }
        if (inodes_.empty()) Fail("corrupt PFS image (no inodes): wrong decryption keys?");
    }
    const PfsHeader& Header() const { return h_; }
    const Inode& Node(u32 ino) const {
        if (ino >= inodes_.size()) Fail("corrupt PFS image (inode " + std::to_string(ino) + ")");
        return inodes_[ino];
    }
    std::vector<Dirent> List(u32 ino) const {
        const Inode& n = Node(ino);
        if (!n.Dir() || n.db[0] < 1 || n.blocks > 100000) Fail("corrupt PFS image (directory inode)");
        std::vector<Dirent> out;
        std::vector<u8> b(h_.block_size);
        for (u32 k = 0; k < n.blocks; k++) {
            rd_(u64(n.db[0]) + k, b.data());
            for (u32 pos = 0; pos + 16 <= h_.block_size;) {
                const u32 dino = Le32(&b[pos]), type = Le32(&b[pos + 4]), nlen = Le32(&b[pos + 8]), esz = Le32(&b[pos + 12]);
                if (esz == 0) break;
                if (pos + 16 + nlen > h_.block_size) Fail("corrupt PFS directory");
                if (type == 2 || type == 3)
                    out.push_back({dino, type, std::string(reinterpret_cast<const char*>(&b[pos + 16]), nlen)});
                pos += esz;
            }
        }
        return out;
    }
    std::optional<Dirent> Find(u32 dir, const std::string& name) const {
        for (auto& d : List(dir))
            if (d.name == name) return d;
        return std::nullopt;
    }
    // The image block of each of a file's blocks (inodes list them past the 12 direct ones in indirect blocks).
    std::vector<i64> BlockList(u32 ino) const {
        const Inode& n = Node(ino);
        std::vector<i64> list;
        if (n.blocks <= 1 || n.db[1] == -1) {  // contiguous from db[0]
            return list;
        }
        list.reserve(n.blocks);
        for (int i = 0; i < 12 && list.size() < n.blocks; i++) list.push_back(n.db[i]);
        const u32 stride = h_.Signed() ? 36 : 4, skip = h_.Signed() ? 32 : 0, per = h_.block_size / stride;
        std::vector<u8> b(h_.block_size), b2(h_.block_size);
        auto indirect = [&](u64 blk) {
            rd_(blk, b.data());
            for (u32 i = 0; i < per && list.size() < n.blocks; i++) list.push_back(int32_t(Le32(&b[i * stride + skip])));
        };
        if (list.size() < n.blocks) indirect(n.ib[0]);
        if (list.size() < n.blocks) {
            rd_(n.ib[1], b2.data());
            for (u32 j = 0; j < per && list.size() < n.blocks; j++) indirect(Le32(&b2[j * stride + skip]));
        }
        if (list.size() < n.blocks) Fail("file too large for this PFS reader (triple-indirect blocks)");
        bool contiguous = true;
        for (size_t i = 1; i < list.size() && contiguous; i++) contiguous = list[i] == list[i - 1] + 1;
        if (contiguous) list.clear();
        return list;
    }

private:
    PfsHeader h_;
    BlockReader rd_;
    std::vector<Inode> inodes_;
};

// The encrypted outer image inside the package, and pfs_image.dat (the compressed inner image) inside that.
class OuterImage {
public:
    OuterImage(const Package& p) : pkg_(p) {
        InFile f(p.path);
        u8 hdr[0x400];
        f.Read(p.pfs_offset, hdr, sizeof hdr);
        h_ = ParsePfsHeader(hdr);
        if (h_.Encrypted()) {
            std::vector<u8> ekpfs = Ekpfs(p);
            if (p.pfs_flags & 0x2000000000000000ull) {
                u8 k[32];
                HmacSha256(ekpfs.data(), ekpfs.size(), h_.seed, 16, k);
                ekpfs.assign(k, k + 32);
            }
            u8 msg[20], keys[32];
            const u32 index = 1;
            memcpy(msg, &index, 4);
            memcpy(msg + 4, h_.seed, 16);
            HmacSha256(ekpfs.data(), ekpfs.size(), msg, 20, keys);
            xts_.reset(new XtsKeys{Aes128(keys + 16), Aes128(keys)});
        }
    }
    const PfsHeader& Header() const { return h_; }

    // Reads [off, off + n) of the image; f is the calling thread's handle on the package.
    void Read(const InFile& f, u64 off, u8* out, size_t n, std::vector<u8>& scratch) const {
        if (!xts_) {
            f.Read(pkg_.pfs_offset + off, out, n);
            return;
        }
        const u64 a = off & ~u64(0xFFF), e = (off + n + 0xFFF) & ~u64(0xFFF);
        scratch.resize(size_t(e - a));
        f.Read(pkg_.pfs_offset + a, scratch.data(), scratch.size());
        const u64 first = h_.block_size / 0x1000;
        for (u64 s = a; s < e; s += 0x1000)
            if (s / 0x1000 >= first) XtsDecryptSector(*xts_, s / 0x1000, &scratch[size_t(s - a)]);
        memcpy(out, &scratch[size_t(off - a)], n);
    }

private:
    const Package& pkg_;
    PfsHeader h_;
    std::unique_ptr<XtsKeys> xts_;
};

// One thread's view of a package: its own file handle and buffers.
struct Reader {
    explicit Reader(const Package& p) : file(p.path) {}
    InFile file;
    std::vector<u8> scratch, raw, sector;
    Inflater inflater;
};

class PkgImage {
public:
    explicit PkgImage(const Package& p) : outer_(p), meta_(p) {
        Reader& r = meta_;
        const u32 bs = outer_.Header().block_size;
        Pfs outer(outer_.Header(), [&](u64 blk, u8* out) { outer_.Read(r.file, blk * bs, out, bs, r.scratch); });
        std::optional<Dirent> img;
        if (const auto uroot = outer.Find(0, "uroot")) img = outer.Find(uroot->ino, "pfs_image.dat");
        if (!img) img = outer.Find(0, "pfs_image.dat");
        if (!img) Fail(U8(p.path) + ": pfs_image.dat not found (wrong decryption keys?)");
        const Inode& n = outer.Node(img->ino);
        img_start_ = u64(n.db[0]);
        img_size_ = n.size;
        img_blocks_ = outer.BlockList(img->ino);

        // PFSC header and sector table.
        u8 ph[0x30];
        ReadImg(r, 0, ph, sizeof ph);
        if (Le32(ph) != 0x43534650) Fail(U8(p.path) + ": the inner image is not PFSC (decryption failed?)");
        sector_size_ = Le32(ph + 0x0C);
        const u64 table = Le64(ph + 0x18), length = Le64(ph + 0x28);
        if (sector_size_ < 0x1000 || sector_size_ > (1u << 20) || u64(Le64(ph + 0x10)) != sector_size_)
            Fail(U8(p.path) + ": corrupt PFSC header");
        const u64 count = (length + sector_size_ - 1) / sector_size_;
        map_.resize(size_t(count + 1));
        ReadImg(r, table, reinterpret_cast<u8*>(map_.data()), map_.size() * 8);
        for (size_t i = 1; i < map_.size(); i++)
            if (map_[i] < map_[i - 1] || map_[i] > img_size_) Fail(U8(p.path) + ": corrupt PFSC sector table");

        std::vector<u8> sec(sector_size_);
        Sectors(r, 0, 1, sec.data());
        inner_h_ = ParsePfsHeader(sec.data());
        if (inner_h_.block_size != sector_size_ || inner_h_.Encrypted())
            Fail(U8(p.path) + ": unexpected inner PFS (block size " + std::to_string(inner_h_.block_size) + ")");
        // Metadata (inodes, directories, indirect blocks) goes through one shared reader.
        inner_ = std::make_unique<Pfs>(inner_h_, [this](u64 blk, u8* out) {
            std::lock_guard<std::mutex> l(meta_mu_);
            Sectors(meta_, blk, 1, out);
        });
    }
    const Pfs& Inner() const { return *inner_; }
    u32 SectorSize() const { return sector_size_; }

    // Reads [off, off + n) of pfs_image.dat (the compressed stream).
    void ReadImg(Reader& r, u64 off, u8* out, size_t n) const {
        const u32 bs = outer_.Header().block_size;
        if (img_blocks_.empty()) {
            outer_.Read(r.file, img_start_ * bs + off, out, n, r.scratch);
            return;
        }
        while (n) {
            const u64 blk = off / bs, within = off % bs;
            const size_t take = size_t(std::min<u64>(n, bs - within));
            if (blk >= img_blocks_.size()) Fail("read past the end of pfs_image.dat");
            outer_.Read(r.file, u64(img_blocks_[blk]) * bs + within, out, take, r.scratch);
            out += take, off += take, n -= take;
        }
    }
    // Decompresses `count` sectors of the inner image, starting at `first`, into out.
    void Sectors(Reader& r, u64 first, u64 count, u8* out) const {
        if (first + count >= map_.size()) Fail("read past the end of the inner image");
        const u64 a = map_[first], e = map_[first + count];
        r.raw.resize(size_t(e - a));
        ReadImg(r, a, r.raw.data(), r.raw.size());
        for (u64 i = 0; i < count; i++) {
            const u64 so = map_[first + i] - a, sz = map_[first + i + 1] - map_[first + i];
            u8* dst = out + i * sector_size_;
            if (sz == sector_size_) {
                memcpy(dst, &r.raw[size_t(so)], sector_size_);
            } else if (sz > sector_size_ || sz < 2) {
                memset(dst, 0, sector_size_);
            } else {
                const size_t got = r.inflater.Run(&r.raw[size_t(so) + 2], size_t(sz - 2), dst, sector_size_);
                memset(dst + got, 0, sector_size_ - got);
            }
        }
    }

private:
    OuterImage outer_;
    Reader meta_;
    std::mutex meta_mu_;
    u64 img_start_ = 0, img_size_ = 0;
    std::vector<i64> img_blocks_;
    u32 sector_size_ = 0;
    std::vector<u64> map_;
    PfsHeader inner_h_;
    std::unique_ptr<Pfs> inner_;
};

// ---------------------------------------------------------------------------------------------------------------
// Extraction.

struct FileJob {
    fs::path out;
    u32 ino;
    u64 size;
};

static void Walk(const Pfs& pfs, u32 dir, const fs::path& out, std::vector<fs::path>& dirs, std::vector<FileJob>& files) {
    dirs.push_back(out);
    for (const Dirent& d : pfs.List(dir)) {
        if (d.name.empty() || d.name == "." || d.name == ".." || d.name.find_first_of("/\\") != std::string::npos ||
            d.name.find(':') != std::string::npos)
            Fail("unsafe file name in the package: " + d.name);
        const fs::path p = out / FromU8(d.name);
        if (d.type == 3) Walk(pfs, d.ino, p, dirs, files);
        else files.push_back({p, d.ino, pfs.Node(d.ino).size});
    }
}

static std::string Gb(u64 b) {
    char s[32];
    snprintf(s, sizeof s, "%.1f GB", double(b) / 1e9);
    return s;
}

struct Options {
    int threads = 0;
    bool overwrite = false;
};

static void ExtractPackage(const Package& p, const fs::path& dest, const Options& opt) {
    Out("== %s (%s %s) -> %s\n", p.content_id.c_str(), p.Kind().c_str(),
           (p.app_ver.empty() ? p.version : p.app_ver).c_str(), U8(dest).c_str());
    fflush(stdout);
    PkgImage img(p);
    const Pfs& pfs = img.Inner();
    const auto uroot = pfs.Find(0, "uroot");
    if (!uroot || uroot->type != 3) Fail(U8(p.path) + ": no uroot folder in the package");
    std::vector<fs::path> dirs;
    std::vector<FileJob> files;
    Walk(pfs, uroot->ino, dest, dirs, files);
    for (auto& d : dirs) fs::create_directories(d);
    std::sort(files.begin(), files.end(), [](const FileJob& a, const FileJob& b) { return a.size > b.size; });
    u64 total = 0;
    for (auto& f : files) total += f.size;

    std::atomic<size_t> next{0};
    std::atomic<u64> done{0};
    std::atomic<int> skipped{0}, compressed{0};
    std::mutex err_mu;
    std::string err;
    const u32 bs = img.SectorSize();
    auto worker = [&] {
        try {
            Reader r(p);
            constexpr u64 kBatch = 16;
            std::vector<u8> buf(size_t(kBatch) * bs);
            for (;;) {
                {
                    std::lock_guard<std::mutex> l(err_mu);
                    if (!err.empty()) return;
                }
                const size_t i = next++;
                if (i >= files.size()) return;
                const FileJob& job = files[i];
                std::error_code ec;
                if (!opt.overwrite && fs::is_regular_file(job.out, ec) && fs::file_size(job.out, ec) == job.size) {
                    done += job.size;
                    skipped++;
                    continue;
                }
                const Inode& n = pfs.Node(job.ino);
                if (n.size2 != n.size) compressed++;  // not seen in these packages; written as stored
                const std::vector<i64> blocks = pfs.BlockList(job.ino);
                const fs::path part = fs::path(job.out).concat(".part");
                FILE* f = OpenWrite(part);
                u64 left = job.size, blk = 0;
                const u64 nblocks = (job.size + bs - 1) / bs;
                bool ok = true;
                while (left) {
                    u64 count = std::min<u64>(kBatch, nblocks - blk);
                    u64 first;
                    if (blocks.empty()) {
                        first = u64(n.db[0]) + blk;
                    } else {
                        first = u64(blocks[size_t(blk)]);
                        u64 c = 1;
                        while (c < count && blocks[size_t(blk + c)] == blocks[size_t(blk + c - 1)] + 1) c++;
                        count = c;
                    }
                    img.Sectors(r, first, count, buf.data());
                    const size_t w = size_t(std::min<u64>(left, count * bs));
                    if (fwrite(buf.data(), 1, w, f) != w) {
                        ok = false;
                        break;
                    }
                    left -= w;
                    blk += count;
                    done += w;
                }
                if (fclose(f) != 0 || !ok) Fail("cannot write " + U8(part) + " (disk full?)");
                fs::rename(part, job.out);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> l(err_mu);
            if (err.empty()) err = e.what();
        }
    };
    int nthreads = opt.threads > 0 ? opt.threads : int(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    std::atomic<int> running{nthreads};
    std::vector<std::thread> pool;
    for (int i = 0; i < nthreads; i++)
        pool.emplace_back([&] {
            worker();
            running--;
        });

    const auto start = std::chrono::steady_clock::now();
    auto last = start;
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(5)) {
            last = now;
            const double secs = std::chrono::duration<double>(now - start).count();
            const u64 d = done.load();
            Out("   %5.1f%%  %s of %s  (%.0f MB/s)\n", total ? 100.0 * double(d) / double(total) : 100.0, Gb(d).c_str(),
                   Gb(total).c_str(), double(d) / 1e6 / std::max(secs, 0.001));
            fflush(stdout);
        }
    }
    for (auto& t : pool) t.join();
    if (!err.empty()) Fail(err);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Out("   done: %zu files, %s in %.0f s", files.size(), Gb(total).c_str(), secs);
    if (skipped) Out(" (%d already there)", skipped.load());
    if (compressed) Out(" (%d files flagged compressed)", compressed.load());
    Out("\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------------
// SELF -> ELF (fake-signed SELFs, every segment plain).

static void SelfToElf(const fs::path& src, const fs::path& dst) {
    InFile f(src);
    std::vector<u8> d(size_t(f.Size()));
    f.Read(0, d.data(), d.size());
    if (d.size() < 0x20 || memcmp(d.data(), "\x4f\x15\x3d\x1d", 4) != 0) Fail(U8(src) + " is not a SELF");
    const u32 segs = Le16(&d[0x18]);
    const size_t elf = 0x20 + size_t(segs) * 32;
    if (elf + 0x40 > d.size() || memcmp(&d[elf], "\x7f" "ELF", 4) != 0) Fail(U8(src) + ": ELF header not found");
    const u64 phoff = Le64(&d[elf + 0x20]);
    const u16 phentsize = Le16(&d[elf + 0x36]), phnum = Le16(&d[elf + 0x38]);
    const size_t head = size_t(phoff + u64(phentsize) * phnum);
    if (elf + head > d.size() || phentsize < 56) Fail(U8(src) + ": bad program headers");
    struct Ph { u64 off, filesz; };
    std::vector<Ph> ph(phnum);
    size_t size = head;
    for (u32 i = 0; i < phnum; i++) {
        const u8* p = &d[elf + phoff + size_t(i) * phentsize];
        ph[i] = {Le64(p + 8), Le64(p + 32)};
        size = std::max(size, size_t(ph[i].off + ph[i].filesz));
    }
    std::vector<u8> out(size, 0);
    memcpy(out.data(), &d[elf], head);
    for (u32 i = 0; i < segs; i++) {
        const u8* s = &d[0x20 + size_t(i) * 32];
        const u64 flags = Le64(s), off = Le64(s + 8), sz = Le64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & (0x2 | 0x8)) Fail(U8(src) + ": segment " + std::to_string(i) + " is encrypted or compressed (not a fake SELF)");
        const u32 idx = u32((flags >> 20) & 0xFFF);
        if (idx >= phnum || off + sz > d.size()) Fail(U8(src) + ": bad segment " + std::to_string(i));
        if (ph[idx].off + sz > out.size()) out.resize(size_t(ph[idx].off + sz));
        memcpy(&out[size_t(ph[idx].off)], &d[size_t(off)], size_t(sz));
    }
    const fs::path part = fs::path(dst).concat(".part");
    FILE* o = OpenWrite(part);
    const bool ok = fwrite(out.data(), 1, out.size(), o) == out.size();
    if (fclose(o) != 0 || !ok) Fail("cannot write " + U8(part));
    fs::rename(part, dst);
    Out("== %s -> %s (%zu bytes)\n", U8(src).c_str(), U8(dst).c_str(), out.size());
}

// ---------------------------------------------------------------------------------------------------------------

static const char* kTitle = "CUSA01176";

static std::vector<fs::path> Inputs(const std::vector<fs::path>& args) {
    std::vector<fs::path> out;
    for (const auto& a : args) {
        std::error_code ec;
        if (fs::is_directory(a, ec)) {
            std::vector<fs::path> found;
            for (const auto& e : fs::directory_iterator(a)) {
                std::string ext = U8(e.path().extension());
                for (auto& c : ext) c = char(tolower(c));
                if (e.is_regular_file() && ext == ".pkg") found.push_back(e.path());
            }
            std::sort(found.begin(), found.end());
            if (found.empty()) Fail("no .pkg files in " + U8(a));
            out.insert(out.end(), found.begin(), found.end());
        } else if (fs::is_regular_file(a, ec)) {
            out.push_back(a);
        } else {
            Fail(U8(a) + " does not exist");
        }
    }
    return out;
}

static void Usage() {
    Out(
        "ldpkg: unpacks the LEGO Dimensions PS4 packages (fake-signed .pkg) for the PC port.\n\n"
        "  ldpkg [--layout extracted|install] [--threads N] [--overwrite] -o <output folder> <.pkg or folder>...\n"
        "  ldpkg --info <.pkg or folder>...  (exit code 0 when the 1.24 update is there and can be unpacked)\n"
        "  ldpkg --dlc [--info] -o <game folder> <.pkg or folder>...  (add-on packs only, into an installed game)\n"
        "  ldpkg --keys <file>  (checks a keys file; exit code 0 when the keys work)\n"
        "  ldpkg --self2elf <eboot.bin> <eboot.elf>\n\n"
        "Give it the base game (1.00), the 1.24 update and any add-on packs; it sorts them by what they are.\n"
        "  --layout extracted  elf/eboot.elf, CUSA01176/uroot, CUSA01176-base/uroot, addcont/<label>/uroot (default)\n"
        "  --layout install    eboot.elf, update/, base/, addcont/<label>/uroot (an install folder's game/)\n"
        "  --log <file>        also write everything printed to this file\n"
        "  --keys <file>       the two fake-package RSA keys (see keys.example.inc): 1024 raw bytes, or hex text\n");
}

static int Run(std::vector<std::string> args) {
    std::vector<fs::path> inputs;
    fs::path out;
    std::string layout = "extracted";
    Options opt;
    bool info = false, dlc = false;
    fs::path keys;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) Fail(a + " needs a value");
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else if (a == "--self2elf") {
            if (i + 2 >= args.size()) Fail("--self2elf needs <eboot.bin> <eboot.elf>");
            SelfToElf(FromU8(args[i + 1]), FromU8(args[i + 2]));
            return 0;
        } else if (a == "-o" || a == "--out") {
            out = FromU8(next());
        } else if (a == "--layout") {
            layout = next();
            if (layout != "extracted" && layout != "install") Fail("--layout is extracted or install");
        } else if (a == "--threads") {
            opt.threads = std::stoi(next());
        } else if (a == "--overwrite") {
            opt.overwrite = true;
        } else if (a == "--log") {
            const fs::path lp = FromU8(next());
#ifdef _WIN32
            g_log = _wfopen(lp.c_str(), L"w");
#else
            g_log = fopen(lp.c_str(), "w");
#endif
            if (!g_log) Fail("cannot create " + U8(lp));
        } else if (a == "--info") {
            info = true;
        } else if (a == "--dlc") {
            dlc = true;
        } else if (a == "--keys") {
            keys = FromU8(next());
        } else if (!a.empty() && a[0] == '-') {
            Fail("unknown option " + a + " (--help lists them)");
        } else {
            inputs.push_back(FromU8(a));
        }
    }
    if (!keys.empty()) rsa::LoadKeys(keys);
    else rsa::UseBuiltinKeys();
    if (inputs.empty() && !keys.empty()) {
        Out("the keys work\n");
        return 0;
    }
    if (inputs.empty()) {
        Usage();
        return 2;
    }
    if (!HasAesNi()) Fail("this CPU has no AES instructions (AES-NI), which ldpkg needs");

    std::vector<Package> pkgs;
    for (const auto& p : Inputs(inputs)) pkgs.push_back(ReadPackage(p));

    // Sort them: one base game, one update (the newest), any number of add-ons, all of CUSA01176.
    const Package *base = nullptr, *update = nullptr;
    std::vector<const Package*> addons;
    int problems = 0;
    for (const auto& p : pkgs) {
        Out("%-40s %-9s %-8s %-6s %s\n", p.content_id.c_str(), p.Kind().c_str(),
               (p.app_ver.empty() ? p.version : p.app_ver).c_str(), Gb(p.size).c_str(), U8(p.path.filename()).c_str());
        if (p.title_id != kTitle) {
            Out("   skipped: this port is built for %s (LEGO Dimensions, EU); this package is %s\n", kTitle, p.title_id.c_str());
            problems++;
        } else if (dlc && p.category != "ac") {
            Out("   skipped: not an add-on pack (only add-ons are added to an installed game)\n");
        } else if (info && [&] {
                       try {
                           Ekpfs(p);
                           return false;
                       } catch (const Error& e) {
                           Out("   cannot be unpacked: %s\n", e.what());
                           return true;
                       }
                   }()) {
            problems++;
        } else if (p.category == "gd") {
            if (base) Out("   note: two base-game packages; using %s\n", U8(base->path.filename()).c_str());
            else base = &p;
        } else if (p.category == "gp") {
            if (!update || p.app_ver > update->app_ver) update = &p;
        } else if (p.category == "ac") {
            addons.push_back(&p);
        } else {
            Out("   skipped: not a game, update or add-on package\n");
            problems++;
        }
    }
    if (update && update->app_ver != "01.24")
        Out("warning: the update is version %s; the port is built for 01.24 and may not run with it\n", update->app_ver.c_str());
    if (dlc && info) {
        Out("%zu add-on pack(s) can be added\n", addons.size());
        return addons.empty() ? 1 : 0;
    }
    if (info) {
        Out("%s\n", update ? "the 1.24 update is here" : "no update package (the port needs the 1.24 update)");
        return update ? 0 : 1;
    }
    if (out.empty()) Fail("no output folder (-o <folder>)");
    fs::create_directories(out);

    if (dlc) {  // add-ons only: <game folder>/addcont/<label>/uroot, the same in either layout
        for (const Package* a : addons) ExtractPackage(*a, out / "addcont" / a->label / "uroot", opt);
        Out("\n%zu add-on pack(s) added.\n", addons.size());
        if (problems) Out("(%d package(s) skipped, see above.)\n", problems);
        return addons.empty() ? 1 : 0;
    }

    const bool inst = layout == "install";
    const fs::path upd_dir = inst ? out / "update" : out / kTitle / "uroot";
    const fs::path base_dir = inst ? out / "base" : out / (std::string(kTitle) + "-base") / "uroot";
    const fs::path elf = inst ? out / "eboot.elf" : out / "elf" / "eboot.elf";
    if (base) ExtractPackage(*base, base_dir, opt);
    if (update) ExtractPackage(*update, upd_dir, opt);
    for (const Package* a : addons) ExtractPackage(*a, out / "addcont" / a->label / "uroot", opt);

    if (fs::exists(upd_dir / "eboot.bin")) {
        fs::create_directories(elf.parent_path());
        SelfToElf(upd_dir / "eboot.bin", elf);
    }
    const bool complete = fs::exists(elf) && fs::is_directory(upd_dir);
    Out("\n%s\n", complete ? "The game data is ready." : "Not complete yet: the 1.24 update package is still needed.");
    if (!fs::is_directory(base_dir) && complete) Out("(The base game 1.00 package is recommended too.)\n");
    if (problems) Out("(%d package(s) skipped, see above.)\n", problems);
    return complete ? 0 : 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(U8(fs::path(argv[i])));
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    try {
        return Run(args);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        if (g_log) fprintf(g_log, "error: %s\n", e.what());
        return 1;
    }
}
