// Controller-aware button icons: see button_icons.h.
#include "hle/button_icons.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "hle/hle.h"
#include "hle/port_settings.h"

// Keycap labels: the TrueType rasteriser that comes with Dear ImGui (public domain), a private copy here.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace icons {
namespace {

namespace fs = std::filesystem;

// --- small helpers ---

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[1] << 8 | p[0]); }
float BeFloat(const uint8_t* p) {
    const uint32_t u = Be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint64_t Fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// --- raw deflate (RFC 1951), for the archives' "DFLT" chunks ---

class Inflater {
public:
    Inflater(const uint8_t* in, size_t n, std::vector<uint8_t>& out) : in_(in), n_(n), out_(out) {}

    bool Run() {
        for (;;) {
            const uint32_t last = Bits(1);
            const uint32_t type = Bits(2);
            bool ok = false;
            if (type == 0) ok = Stored();
            else if (type == 1) ok = Fixed();
            else if (type == 2) ok = Dynamic();
            if (!ok || error_) return false;
            if (last) return true;
        }
    }

private:
    struct Huffman {
        uint16_t count[16];
        uint16_t symbol[320];
    };

    uint32_t Bits(int k) {
        while (bit_count_ < k) {
            if (pos_ >= n_) {
                error_ = true;
                return 0;
            }
            bit_buf_ |= uint32_t(in_[pos_++]) << bit_count_;
            bit_count_ += 8;
        }
        const uint32_t v = bit_buf_ & ((1u << k) - 1);
        bit_buf_ >>= k;
        bit_count_ -= k;
        return v;
    }

    static void Build(Huffman& h, const uint8_t* lengths, int n) {
        std::memset(h.count, 0, sizeof h.count);
        for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
        uint16_t offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= int(Bits(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
            if (error_) return -1;
        }
        return -1;
    }

    bool Stored() {
        bit_buf_ = 0;
        bit_count_ = 0;
        if (pos_ + 4 > n_) return false;
        const uint16_t len = Le16(in_ + pos_), nlen = Le16(in_ + pos_ + 2);
        pos_ += 4;
        if (uint16_t(~nlen) != len || pos_ + len > n_) return false;
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;
        return true;
    }

    bool Codes(const Huffman& lit, const Huffman& dist) {
        static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lit);
            if (sym < 0) return false;
            if (sym < 256) {
                out_.push_back(uint8_t(sym));
                continue;
            }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            const size_t len = kLenBase[sym] + Bits(kLenExtra[sym]);
            const int dsym = Decode(dist);
            if (dsym < 0 || dsym >= 30) return false;
            const size_t d = kDistBase[dsym] + Bits(kDistExtra[dsym]);
            if (error_ || d > out_.size()) return false;
            const size_t from = out_.size() - d;
            for (size_t i = 0; i < len; ++i) out_.push_back(out_[from + i]);
        }
    }

    bool Fixed() {
        uint8_t lengths[288 + 30];
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
        Huffman lit, dist;
        Build(lit, lengths, 288);
        Build(dist, lengths + 288, 30);
        return Codes(lit, dist);
    }

    bool Dynamic() {
        static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = int(Bits(5)) + 257, ndist = int(Bits(5)) + 1, ncode = int(Bits(4)) + 4;
        if (nlen > 286 || ndist > 30) return false;
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = uint8_t(Bits(3));
        Huffman lencode, lit, dist;
        Build(lencode, lengths, 19);
        uint8_t all[320] = {};
        for (int i = 0; i < nlen + ndist;) {
            const int sym = Decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) {
                all[i++] = uint8_t(sym);
                continue;
            }
            int repeat = 0;
            uint8_t value = 0;
            if (sym == 16) {
                if (i == 0) return false;
                value = all[i - 1];
                repeat = 3 + int(Bits(2));
            } else if (sym == 17) {
                repeat = 3 + int(Bits(3));
            } else {
                repeat = 11 + int(Bits(7));
            }
            if (i + repeat > nlen + ndist) return false;
            while (repeat--) all[i++] = value;
        }
        Build(lit, all, nlen);
        Build(dist, all + nlen, ndist);
        return Codes(lit, dist);
    }

    const uint8_t* in_;
    size_t n_, pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool error_ = false;
    std::vector<uint8_t>& out_;
};

// TT's "DFLT" chunks: u32 stored size, u32 size (little-endian), then raw deflate whose first byte has bit 2
// cleared (a dynamic block reads as stored until it is set back), or the bytes as they are when both sizes match.
bool Unpack(const std::vector<uint8_t>& raw, size_t size, std::vector<uint8_t>& out) {
    if (raw.size() < 12 || std::memcmp(raw.data(), "DFLT", 4) != 0) {
        out = raw;
        return true;
    }
    out.clear();
    out.reserve(size);
    size_t p = 0;
    while (p + 12 <= raw.size() && std::memcmp(raw.data() + p, "DFLT", 4) == 0) {
        const uint32_t stored = Le32(raw.data() + p + 4), real = Le32(raw.data() + p + 8);
        if (p + 12 + stored > raw.size()) return false;
        const uint8_t* chunk = raw.data() + p + 12;
        if (stored == real) {
            out.insert(out.end(), chunk, chunk + stored);
        } else {
            std::vector<uint8_t> copy(chunk, chunk + stored);
            const size_t before = out.size();
            bool ok = false;
            for (const uint8_t first : {uint8_t(copy[0] | 4), copy[0], uint8_t(copy[0] ^ 4)}) {
                copy[0] = first;
                out.resize(before);
                if (Inflater(copy.data(), copy.size(), out).Run() && out.size() - before == real) {
                    ok = true;
                    break;
                }
            }
            if (!ok) return false;
        }
        p += 12 + stored;
    }
    if (out.size() < size) return false;
    out.resize(size);
    return true;
}

// --- the game's archives (<name>.HDR index + <name>.DAT data) ---

struct FileRef {
    fs::path dat;
    uint64_t offset = 0;
    uint32_t stored = 0, size = 0;
    bool packed = false;
};

struct Archive {
    std::string stem;
    fs::path dat;
    bool patch = false;
    // Old tables (little-endian, the base game's GAME / STREAMn): per-file hash of its path, entries by index.
    std::vector<uint32_t> hashes;
    std::vector<FileRef> entries;
    // New tables (".CC40TAD", big-endian, the update's PATCH / PATCHSTREAMn): only the button files, by path.
    std::map<std::string, FileRef> named;
};

// The path hash in the old tables: FNV-1a over the upper-case path (backslashes, no leading one), with TT's
// prime 1677619 in place of FNV's 16777619.
uint32_t PathHash(const std::string& path) {
    uint32_t h = 0x811C9DC5u;
    for (const char c : Upper(path)) h = (h ^ uint8_t(c)) * 1677619u;
    return h;
}

// Old table: i32 version (-7), u32 files, files x {u32 offset >> 8, u32 stored, u32 size, u32 low offset byte << 24 |
// compression}, u32 name entries, name entries x 12 bytes, u32 hash-table offset (from after it), the names, and the
// sorted u32 path hashes, one per file.
bool ReadOldTable(const std::vector<uint8_t>& t, Archive& a) {
    if (t.size() < 8) return false;
    const uint32_t files = Le32(t.data() + 4);
    size_t p = 8;
    if (p + uint64_t(files) * 16 + 4 > t.size()) return false;
    a.entries.resize(files);
    for (uint32_t i = 0; i < files; ++i, p += 16) {
        const uint32_t off = Le32(t.data() + p), extra = Le32(t.data() + p + 12);
        FileRef& f = a.entries[i];
        f.dat = a.dat;
        f.offset = (uint64_t(off) << 8) + (extra >> 24);
        f.stored = Le32(t.data() + p + 4);
        f.size = Le32(t.data() + p + 8);
        f.packed = (extra & 0xFFFFFF) != 0;
    }
    const uint32_t names = Le32(t.data() + p);
    p += 4;
    const uint64_t names_at = p + uint64_t(names) * 12;
    if (names_at + 4 > t.size()) return false;
    const uint64_t hashes_at = names_at + 4 + Le32(t.data() + names_at);
    if (hashes_at + uint64_t(files) * 4 > t.size()) return false;
    a.hashes.resize(files);
    for (uint32_t i = 0; i < files; ++i) a.hashes[i] = Le32(t.data() + hashes_at + 4 * i);
    return true;
}

// New table (whole HDR file): u32 size, ".CC40TAD", i32 version, u32, u32 files, u32 name entries, u32 names size,
// the names, u32, name entries {u32 name offset, u16 parent, u16 file, u16, u16}, i32, u32, u32, then file entries
// {u32 high, u32 offset, u32 stored, u32 size | 0x80000000 when compressed}. Paths join names with '/'.
bool ReadNewTable(const std::vector<uint8_t>& d, Archive& a) {
    if (d.size() < 32) return false;
    const uint32_t files = Be32(d.data() + 20), names = Be32(d.data() + 24), names_size = Be32(d.data() + 28);
    const uint64_t names_at = 32;
    uint64_t p = names_at + names_size + 4;
    if (p + uint64_t(names) * 12 + 8 + uint64_t(files) * 16 > d.size()) return false;
    const uint64_t table_at = p + uint64_t(names) * 12 + 8;
    std::vector<std::string> full(names);
    for (uint32_t i = 0; i < names; ++i, p += 12) {
        const uint32_t name_off = Be32(d.data() + p);
        const uint16_t parent = Be16(d.data() + p + 4), file = Be16(d.data() + p + 6);
        std::string name;
        if (name_off < names_size) {
            const char* s = reinterpret_cast<const char*>(d.data() + names_at + name_off);
            name.assign(s, strnlen(s, names_size - name_off));
        }
        const std::string& base = parent > 0 && parent < i ? full[parent] : std::string();
        full[i] = base.empty() ? name : base + "/" + name;
        if (!file || file >= files || name.find('.') == std::string::npos) continue;
        const std::string path = Lower(full[i]);
        // The button sheets, and the Toy Pad figures' list and portraits (see BuildFigures).
        const bool figures = path.size() >= 20 && (path.compare(path.size() - 20, 20, "chars/collection.txt") == 0 ||
                                                   path.find("icons/characters/charactericontpage_dx11") != std::string::npos);
        if (path.rfind("framework-data/gui/buttons/", 0) != 0 && !figures) continue;
        const uint8_t* e = d.data() + table_at + uint64_t(file) * 16;
        FileRef f;
        f.dat = a.dat;
        const uint32_t hi = Be32(e), size = Be32(e + 12);
        f.offset = Be32(e + 4) + (hi < 16 ? uint64_t(hi) << 32 : 0);
        f.stored = Be32(e + 8);
        f.size = size & 0x7FFFFFFF;
        f.packed = (size & 0x80000000u) != 0;
        a.named[path] = f;
    }
    return true;
}

// Every archive in the game's layers (update first), with its data file.
std::vector<Archive> OpenArchives() {
    std::vector<fs::path> layers;
    for (const std::string& l : hle::HostPathLayers("/app0/data")) layers.push_back(l);
    std::vector<Archive> out;
    std::error_code ec;
    for (const fs::path& layer : layers) {
        if (!fs::is_directory(layer, ec)) continue;
        for (const auto& chunk : fs::directory_iterator(layer, ec)) {
            if (!chunk.is_directory(ec)) continue;
            for (const auto& f : fs::directory_iterator(chunk.path(), ec)) {
                if (Upper(f.path().extension().string()) != ".HDR") continue;
                const std::string stem = Upper(f.path().stem().string());
                bool seen = false;
                for (const Archive& a : out) seen = seen || a.stem == stem;
                if (seen) continue;  // a higher layer has it
                // The data file sits in one of the chunk folders, of any layer.
                fs::path dat;
                for (const fs::path& l2 : layers) {
                    for (const auto& c2 : fs::directory_iterator(l2, ec)) {
                        for (const char* ext : {".DAT", ".dat"}) {
                            const fs::path cand = c2.path() / (f.path().stem().string() + ext);
                            if (dat.empty() && fs::exists(cand, ec)) dat = cand;
                        }
                    }
                }
                if (dat.empty()) continue;
                Archive a;
                a.stem = stem;
                a.dat = dat;
                a.patch = stem.rfind("PATCH", 0) == 0;
                const std::vector<uint8_t> hdr = ReadFile(f.path());
                bool ok = false;
                if (hdr.size() >= 12 && std::memcmp(hdr.data() + 4, ".CC40TAD", 8) == 0) ok = ReadNewTable(hdr, a);
                else if (hdr.size() > 4) ok = ReadOldTable(std::vector<uint8_t>(hdr.begin() + 4, hdr.end()), a);
                if (ok) out.push_back(std::move(a));
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Archive& x, const Archive& y) { return x.patch > y.patch; });
    return out;
}

// A file by its path ("framework-data/gui/buttons/ps4/buttonicons_dx11.tsh"); the update's copy wins.
std::vector<uint8_t> Extract(const std::vector<Archive>& archives, const std::string& path) {
    const std::string lower = Lower(path);
    std::string back = path;
    std::replace(back.begin(), back.end(), '/', '\\');
    const uint32_t hash = PathHash(back);
    for (const Archive& a : archives) {
        const FileRef* ref = nullptr;
        if (auto it = a.named.find(lower); it != a.named.end()) ref = &it->second;
        if (!ref && !a.hashes.empty()) {
            const auto it = std::find(a.hashes.begin(), a.hashes.end(), hash);
            if (it != a.hashes.end()) ref = &a.entries[size_t(it - a.hashes.begin())];
        }
        if (!ref) continue;
        std::ifstream in(ref->dat, std::ios::binary);
        if (!in) continue;
        in.seekg(std::streamoff(ref->offset));
        std::vector<uint8_t> raw(ref->stored);
        if (!in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) continue;
        std::vector<uint8_t> data;
        if (!ref->packed) {
            raw.resize(std::min<size_t>(raw.size(), ref->size));
            return raw;
        }
        if (Unpack(raw, ref->size, data)) return data;
        std::fprintf(stderr, "[icons] %s in %s does not unpack\n", path.c_str(), a.stem.c_str());
    }
    return {};
}

// --- icon sheets (.tsh: a table of named rectangles, then a DDS image) ---

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // pixels, top-left origin
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Sheet {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;      // decoded, top row first
    std::vector<uint8_t> blocks;    // level 0 as stored (the PlayStation sheet's, to recognise it on the GPU)
    std::map<std::string, Rect> sprites;  // by name (lower case)
};

uint32_t Expand(uint32_t v, int bits) { return bits == 5 ? (v << 3 | v >> 2) : (v << 2 | v >> 4); }

void DecodeDxt(const uint8_t* blocks, uint32_t w, uint32_t h, bool bc3, std::vector<uint8_t>& out) {
    out.assign(size_t(w) * h * 4, 0);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = bc3 ? 16 : 8;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = blocks + (size_t(by) * bw + bx) * bs;
            uint8_t alpha[16];
            std::fill(alpha, alpha + 16, uint8_t(255));
            if (bc3) {
                const uint32_t a0 = b[0], a1 = b[1];
                uint8_t table[8] = {uint8_t(a0), uint8_t(a1)};
                if (a0 > a1) {
                    for (int k = 1; k < 7; ++k) table[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
                } else {
                    for (int k = 1; k < 5; ++k) table[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5);
                    table[6] = 0;
                    table[7] = 255;
                }
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
                for (int k = 0; k < 16; ++k) alpha[k] = table[(bits >> (3 * k)) & 7];
                b += 8;
            }
            const uint32_t c0 = Le16(b), c1 = Le16(b + 2), idx = Le32(b + 4);
            uint8_t col[4][4];
            const auto unpack = [](uint32_t c, uint8_t* o) {
                o[0] = uint8_t(Expand((c >> 11) & 31, 5));
                o[1] = uint8_t(Expand((c >> 5) & 63, 6));
                o[2] = uint8_t(Expand(c & 31, 5));
                o[3] = 255;
            };
            unpack(c0, col[0]);
            unpack(c1, col[1]);
            for (int ch = 0; ch < 3; ++ch) {
                if (c0 > c1 || bc3) {
                    col[2][ch] = uint8_t((2 * col[0][ch] + col[1][ch]) / 3);
                    col[3][ch] = uint8_t((col[0][ch] + 2 * col[1][ch]) / 3);
                } else {
                    col[2][ch] = uint8_t((col[0][ch] + col[1][ch]) / 2);
                    col[3][ch] = 0;
                }
            }
            col[2][3] = 255;
            col[3][3] = (c0 > c1 || bc3) ? 255 : 0;
            for (int k = 0; k < 16; ++k) {
                const uint32_t x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* o = &out[(size_t(y) * w + x) * 4];
                std::memcpy(o, col[(idx >> (2 * k)) & 3], 4);
                if (bc3) o[3] = alpha[k];
            }
        }
    }
}

// contents.txt: "index: hash name" per sprite, giving the names of the sheets' hashed entries.
void ReadNames(const std::vector<uint8_t>& text, std::map<uint32_t, std::string>& names) {
    std::string s(text.begin(), text.end());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        char hash[32] = {}, name[64] = {};
        if (std::sscanf(s.substr(pos, end - pos).c_str(), "%*d: %31s %63s", hash, name) == 2)
            names[uint32_t(std::strtoul(hash, nullptr, 16))] = Lower(name);
        pos = end + 1;
    }
}

// Sprite records start at 0x65 after a u32 count (big-endian), 56 bytes each: u0, v0, u1, v1 (floats, v up),
// x, y, width, height before trimming, a tag, four trims, the name hash. The DDS image follows them.
bool ParseSheet(const std::vector<uint8_t>& d, const std::map<uint32_t, std::string>& names, Sheet& sheet) {
    const uint8_t kDds[4] = {'D', 'D', 'S', ' '};
    const auto at = std::search(d.begin(), d.end(), kDds, kDds + 4);
    if (at == d.end() || d.size() < 0x65) return false;
    const size_t dds = size_t(at - d.begin());
    if (dds + 128 > d.size()) return false;
    const uint32_t count = Be32(d.data() + 0x61);
    if (0x65 + size_t(count) * 56 > dds) return false;
    sheet.h = Le32(d.data() + dds + 12);
    sheet.w = Le32(d.data() + dds + 16);
    const bool bc3 = std::memcmp(d.data() + dds + 84, "DXT5", 4) == 0;
    const bool bc1 = std::memcmp(d.data() + dds + 84, "DXT1", 4) == 0;
    if (!bc3 && !bc1) return false;
    const size_t bytes = size_t((sheet.w + 3) / 4) * ((sheet.h + 3) / 4) * (bc3 ? 16 : 8);
    if (dds + 128 + bytes > d.size()) return false;
    sheet.blocks.assign(d.begin() + dds + 128, d.begin() + dds + 128 + bytes);
    DecodeDxt(sheet.blocks.data(), sheet.w, sheet.h, bc3, sheet.rgba);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = d.data() + 0x65 + size_t(i) * 56;
        Rect rect;
        rect.x0 = BeFloat(r) * float(sheet.w);
        rect.x1 = BeFloat(r + 8) * float(sheet.w);
        rect.y0 = (1.0f - BeFloat(r + 12)) * float(sheet.h);
        rect.y1 = (1.0f - BeFloat(r + 4)) * float(sheet.h);
        const auto name = names.find(Be32(r + 52));
        if (name != names.end() && rect.w() > 0 && rect.h() > 0) sheet.sprites[name->second] = rect;
    }
    return !sheet.sprites.empty();
}

// --- drawing into a sheet ---

struct Color {
    float r, g, b, a;
};

struct Canvas {
    uint32_t w, h;
    std::vector<uint8_t>& px;
    // Source-over a colour at `coverage` (0-1) onto a pixel.
    void Blend(int x, int y, Color c, float coverage) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h) || coverage <= 0) return;
        uint8_t* p = &px[(size_t(y) * w + x) * 4];
        const float sa = c.a * std::min(1.0f, coverage), da = p[3] / 255.0f;
        const float oa = sa + da * (1 - sa);
        if (oa <= 0) return;
        const float dc[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f}, sc[3] = {c.r, c.g, c.b};
        for (int i = 0; i < 3; ++i) p[i] = uint8_t(std::clamp((sc[i] * sa + dc[i] * da * (1 - sa)) / oa, 0.0f, 1.0f) * 255 + 0.5f);
        p[3] = uint8_t(oa * 255 + 0.5f);
    }
    // Fills the pixels whose 4x4 sub-samples are inside the shape.
    void Fill(const Rect& box, const std::function<bool(float, float)>& inside, const std::function<Color(float, float)>& color) {
        const int x0 = int(std::floor(box.x0)), x1 = int(std::ceil(box.x1)), y0 = int(std::floor(box.y0)),
                  y1 = int(std::ceil(box.y1));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) hits += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
                if (hits) Blend(x, y, color(x + 0.5f, y + 0.5f), hits / 16.0f);
            }
    }
    void Clear(const Rect& r) {
        for (int y = std::max(0, int(std::floor(r.y0))); y < std::min(int(h), int(std::ceil(r.y1))); ++y)
            for (int x = std::max(0, int(std::floor(r.x0))); x < std::min(int(w), int(std::ceil(r.x1))); ++x)
                std::memset(&px[(size_t(y) * w + x) * 4], 0, 4);
    }
};

bool InRoundRect(const Rect& r, float radius, float x, float y) {
    if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) return false;
    const float cx = std::clamp(x, r.x0 + radius, r.x1 - radius), cy = std::clamp(y, r.y0 + radius, r.y1 - radius);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

Rect Inset(const Rect& r, float l, float t, float rr, float b) { return {r.x0 + l, r.y0 + t, r.x1 - rr, r.y1 - b}; }

// Fits the source picture inside the destination rectangle (centred, aspect kept), area-sampled.
void Blit(Canvas& dst, const Rect& d, const Sheet& src, const Rect& s) {
    dst.Clear(d);
    const float scale = std::min(d.w() / s.w(), d.h() / s.h());
    const float ox = d.x0 + (d.w() - s.w() * scale) / 2, oy = d.y0 + (d.h() - s.h() * scale) / 2;
    const int n = std::clamp(int(std::ceil(1.0f / scale)), 1, 6);
    // Bilinear tap with premultiplied alpha, clamped inside the sprite (neighbours never bleed in).
    const auto tap = [&](float fx, float fy, float* acc) {
        fx = std::clamp(fx - 0.5f, s.x0, std::max(s.x0, s.x1 - 1.0f));
        fy = std::clamp(fy - 0.5f, s.y0, std::max(s.y0, s.y1 - 1.0f));
        const int x0 = int(fx), y0 = int(fy);
        const int x_last = std::min(int(src.w) - 1, std::max(x0, int(std::ceil(s.x1)) - 1));
        const int y_last = std::min(int(src.h) - 1, std::max(y0, int(std::ceil(s.y1)) - 1));
        const float ax = fx - x0, ay = fy - y0;
        const int xs[2] = {x0, std::min(x0 + 1, x_last)}, ys[2] = {y0, std::min(y0 + 1, y_last)};
        const float wx[2] = {1 - ax, ax}, wy[2] = {1 - ay, ay};
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const uint8_t* p = &src.rgba[(size_t(ys[j]) * src.w + xs[i]) * 4];
                const float wgt = wx[i] * wy[j], a = p[3] / 255.0f;
                acc[0] += p[0] * a * wgt;
                acc[1] += p[1] * a * wgt;
                acc[2] += p[2] * a * wgt;
                acc[3] += a * wgt;
            }
    };
    for (int y = int(std::floor(d.y0)); y < int(std::ceil(d.y1)); ++y)
        for (int x = int(std::floor(d.x0)); x < int(std::ceil(d.x1)); ++x) {
            if (x < 0 || y < 0 || x >= int(dst.w) || y >= int(dst.h)) continue;
            float acc[4] = {};
            int taps = 0;
            for (int sy = 0; sy < n; ++sy)
                for (int sx = 0; sx < n; ++sx) {
                    const float px = s.x0 + (x + (sx + 0.5f) / n - ox) / scale, py = s.y0 + (y + (sy + 0.5f) / n - oy) / scale;
                    ++taps;
                    if (px < s.x0 || py < s.y0 || px >= s.x1 || py >= s.y1) continue;
                    tap(px, py, acc);
                }
            const float a = acc[3] / float(taps);
            uint8_t* o = &dst.px[(size_t(y) * dst.w + x) * 4];
            if (a <= 0) {
                std::memset(o, 0, 4);
                continue;
            }
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(acc[c] / acc[3], 0.0f, 255.0f) + 0.5f);
            o[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f);
        }
}

// The keycap label font: a bold system font (none ships with the port).
struct Font {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

Font LoadFont() {
    Font f;
    std::vector<fs::path> paths;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const fs::path fonts = fs::path(windir ? windir : "C:\\Windows") / "Fonts";
    for (const char* n : {"segoeuib.ttf", "arialbd.ttf", "seguisb.ttf", "verdanab.ttf", "arial.ttf"}) paths.push_back(fonts / n);
#else
    for (const char* n : {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                          "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                          "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"})
        paths.push_back(n);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator("/usr/share/fonts", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->path().extension() == ".ttf" && name.find("Bold") != std::string::npos) paths.push_back(it->path());
    }
#endif
    for (const fs::path& p : paths) {
        f.data = ReadFile(p);
        if (f.data.empty()) continue;
        const int offset = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
        if (offset >= 0 && stbtt_InitFont(&f.info, f.data.data(), offset)) {
            f.ok = true;
            std::fprintf(stderr, "[icons] keycap font %s\n", p.string().c_str());
            return f;
        }
    }
    std::fprintf(stderr, "[icons] no font found: keycaps without labels\n");
    return f;
}

// A line of text centred in a box, as large as fits (at most max_height pixels tall).
void Text(Canvas& c, const Font& font, const std::string& text, const Rect& box, float max_height, Color color) {
    if (!font.ok || text.empty()) return;
    float px = max_height;
    float scale = 0, width = 0;
    for (int tries = 0; tries < 2; ++tries) {
        // Size by the capital letters' height (labels are capitals and digits).
        int x0, y0, x1, y1;
        stbtt_GetCodepointBox(&font.info, 'H', &x0, &y0, &x1, &y1);
        scale = px / float(std::max(1, y1));
        width = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&font.info, uint8_t(text[i]), &adv, &lsb);
            width += adv * scale;
            if (i + 1 < text.size()) width += stbtt_GetCodepointKernAdvance(&font.info, uint8_t(text[i]), uint8_t(text[i + 1])) * scale;
        }
        if (width <= box.w()) break;
        px *= box.w() / width;
    }
    int cap_x0, cap_y0, cap_x1, cap_y1;
    stbtt_GetCodepointBox(&font.info, 'H', &cap_x0, &cap_y0, &cap_x1, &cap_y1);
    const float baseline = box.y0 + (box.h() + cap_y1 * scale) / 2;
    float x = box.x0 + (box.w() - width) / 2;
    for (size_t i = 0; i < text.size(); ++i) {
        const int ch = uint8_t(text[i]);
        int gw = 0, gh = 0, gx = 0, gy = 0;
        const float fx = x - std::floor(x);
        unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&font.info, scale, scale, fx, 0, ch, &gw, &gh, &gx, &gy);
        const int ox = int(std::floor(x)) + gx, oy = int(std::lround(baseline)) + gy;
        for (int yy = 0; yy < gh; ++yy)
            for (int xx = 0; xx < gw; ++xx) c.Blend(ox + xx, oy + yy, color, bmp[yy * gw + xx] / 255.0f);
        stbtt_FreeBitmap(bmp, nullptr);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font.info, ch, &adv, &lsb);
        x += adv * scale;
        if (i + 1 < text.size()) x += stbtt_GetCodepointKernAdvance(&font.info, ch, uint8_t(text[i + 1])) * scale;
    }
}

const Color kOutline{0.11f, 0.12f, 0.13f, 1}, kInk{0.13f, 0.14f, 0.16f, 1}, kAccent{0.27f, 0.60f, 1.0f, 1};

// An arrow pointing `dir` (0 up, 1 down, 2 left, 3 right), filling the box.
void Arrow(Canvas& c, const Rect& box, int dir, Color color) {
    const float s = std::min(box.w(), box.h());
    const float cx = (box.x0 + box.x1) / 2, cy = (box.y0 + box.y1) / 2;
    // In the arrow's own frame (pointing up), u across, v along (-0.5 tip .. 0.5 tail).
    const auto inside = [=](float x, float y) {
        float u = (x - cx) / s, v = (y - cy) / s;
        if (dir == 1) v = -v;
        if (dir == 2) std::swap(u, v);
        if (dir == 3) { const float t = u; u = v; v = -t; }
        if (v >= -0.5f && v <= 0.0f && std::fabs(u) <= (v + 0.5f)) return true;  // head
        return v > -0.05f && v <= 0.45f && std::fabs(u) <= 0.17f;                // stem
    };
    c.Fill(box, inside, [=](float, float) { return color; });
}

// A keyboard key: dark rim, light top face a little higher than the bottom lip, label on the face.
void Keycap(Canvas& c, const Font& font, const Rect& r, const std::string& label, int arrow = -1) {
    const float m = std::min(r.w(), r.h());
    const Rect cap = Inset(r, m * 0.03f, m * 0.03f, m * 0.03f, m * 0.03f);
    const float radius = m * 0.17f;
    c.Fill(cap, [&](float x, float y) { return InRoundRect(cap, radius, x, y); }, [](float, float) { return kOutline; });
    const Rect face = Inset(cap, m * 0.07f, m * 0.06f, m * 0.07f, m * 0.17f);
    c.Fill(face, [&](float x, float y) { return InRoundRect(face, radius * 0.75f, x, y); },
           [&](float, float y) {
               const float t = std::clamp((y - face.y0) / std::max(1.0f, face.h()), 0.0f, 1.0f);
               const float v = 0.97f - 0.17f * t;
               return Color{v, v, v + 0.01f, 1};
           });
    const Rect lip = {face.x0, face.y1, face.x1, cap.y1 - m * 0.05f};
    c.Fill(lip, [&](float x, float y) { return InRoundRect(Inset(cap, m * 0.07f, m * 0.3f, m * 0.07f, m * 0.05f), radius * 0.6f, x, y) && y > face.y1; },
           [](float, float) { return Color{0.55f, 0.56f, 0.6f, 1}; });
    const Rect inner = Inset(face, face.w() * 0.1f, face.h() * 0.12f, face.w() * 0.1f, face.h() * 0.12f);
    if (arrow >= 0) {
        const float a = std::min(inner.w(), inner.h()) * 0.9f;
        const float cx = (inner.x0 + inner.x1) / 2, cy = (inner.y0 + inner.y1) / 2;
        Arrow(c, {cx - a / 2, cy - a / 2, cx + a / 2, cy + a / 2}, arrow, kInk);
    } else {
        Text(c, font, label, inner, std::min(inner.h() * 0.78f, face.h() * 0.55f), kInk);
    }
}

// A mouse; `button` 1 left / 2 right lit, `arrow` (0-3) a camera direction beside it.
void Mouse(Canvas& c, const Rect& r, int button, int arrow) {
    Rect area = r;
    Rect arrow_box{};
    if (arrow >= 0) {
        const float m = std::min(r.w(), r.h()) * 0.34f;
        const float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
        switch (arrow) {
            case 0: arrow_box = {cx - m / 2, r.y0, cx + m / 2, r.y0 + m}; area.y0 += m; break;
            case 1: arrow_box = {cx - m / 2, r.y1 - m, cx + m / 2, r.y1}; area.y1 -= m; break;
            case 2: arrow_box = {r.x0, cy - m / 2, r.x0 + m, cy + m / 2}; area.x0 += m; break;
            default: arrow_box = {r.x1 - m, cy - m / 2, r.x1, cy + m / 2}; area.x1 -= m; break;
        }
    }
    float h = area.h() * 0.94f, w = h / 1.45f;
    if (w > area.w() * 0.94f) {
        w = area.w() * 0.94f;
        h = w * 1.45f;
    }
    const float cx = (area.x0 + area.x1) / 2, cy = (area.y0 + area.y1) / 2;
    const Rect body{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    const float rad = w * 0.48f, t = w * 0.075f;
    c.Fill(body, [&](float x, float y) { return InRoundRect(body, rad, x, y); }, [](float, float) { return kOutline; });
    const Rect in = Inset(body, t, t, t, t);
    const float split = in.y0 + in.h() * 0.42f;
    c.Fill(in, [&](float x, float y) { return InRoundRect(in, rad - t, x, y); },
           [&](float x, float y) {
               if (y < split && ((button == 1 && x < cx) || (button == 2 && x > cx))) return kAccent;
               const float v = 0.95f - 0.15f * std::clamp((y - in.y0) / in.h(), 0.0f, 1.0f);
               return Color{v, v, v + 0.01f, 1};
           });
    c.Fill({in.x0, split - t / 2, in.x1, split + t / 2}, [](float, float) { return true; }, [](float, float) { return kOutline; });
    c.Fill({cx - t / 2, in.y0, cx + t / 2, split}, [](float, float) { return true; }, [](float, float) { return kOutline; });
    const Rect wheel{cx - t * 1.1f, in.y0 + in.h() * 0.1f, cx + t * 1.1f, in.y0 + in.h() * 0.3f};
    c.Fill(wheel, [&](float x, float y) { return InRoundRect(wheel, t, x, y); }, [](float, float) { return kOutline; });
    if (arrow >= 0) Arrow(c, arrow_box, arrow, kAccent);
}

// The keyboard and mouse set, matching the bindings in keyboard_mouse.cpp (kKeys, the mouse buttons, WASD and
// the mouse on the sticks). Change both together.
void DrawKeyboard(Canvas& c, const Font& font, const std::string& name, const Rect& r) {
    c.Clear(r);
    static const std::map<std::string, std::string> kLabels = {
        {"cross", "J"},      {"circle", "K"}, {"square", "H"}, {"triangle", "U"}, {"l1", "CTRL"},
        {"r1", "SPACE"},     {"r2", "SHIFT"}, {"l3", "C"},     {"r3", "V"},       {"start", "ESC"},
        {"select", "TAB"},   {"lstickup", "W"}, {"lstickdown", "S"}, {"lstickleft", "A"}, {"lstickright", "D"},
    };
    static const std::map<std::string, int> kDirs = {{"up", 0}, {"down", 1}, {"left", 2}, {"right", 3}};
    if (auto it = kLabels.find(name); it != kLabels.end()) {
        // Wide keys on a wide slot; a one-letter key square.
        Rect k = r;
        if (it->second.size() == 1 && r.w() > r.h()) k = {(r.x0 + r.x1 - r.h()) / 2, r.y0, (r.x0 + r.x1 + r.h()) / 2, r.y1};
        if (it->second.size() == 1 && r.h() > r.w()) k = {r.x0, (r.y0 + r.y1 - r.w()) / 2, r.x1, (r.y0 + r.y1 + r.w()) / 2};
        Keycap(c, font, k, it->second);
        return;
    }
    if (name.rfind("pad", 0) == 0) {  // D-pad: the arrow keys
        const auto dir = kDirs.find(name.substr(3));
        const float s = std::min(r.w(), r.h());
        const Rect k{(r.x0 + r.x1 - s) / 2, (r.y0 + r.y1 - s) / 2, (r.x0 + r.x1 + s) / 2, (r.y0 + r.y1 + s) / 2};
        Keycap(c, font, k, "", dir == kDirs.end() ? 0 : dir->second);
        return;
    }
    if (name == "lstick") {  // W over A S D
        const float k = std::min(r.w() / 3, r.h() / 2);
        const float x0 = (r.x0 + r.x1) / 2 - 1.5f * k, y0 = (r.y0 + r.y1) / 2 - k;
        Keycap(c, font, {x0 + k, y0, x0 + 2 * k, y0 + k}, "W");
        Keycap(c, font, {x0, y0 + k, x0 + k, y0 + 2 * k}, "A");
        Keycap(c, font, {x0 + k, y0 + k, x0 + 2 * k, y0 + 2 * k}, "S");
        Keycap(c, font, {x0 + 2 * k, y0 + k, x0 + 3 * k, y0 + 2 * k}, "D");
        return;
    }
    if (name == "l2") return Mouse(c, r, 2, -1);  // aim: right button
    if (name == "rstick") return Mouse(c, r, 0, -1);
    if (name.rfind("rstick", 0) == 0) {
        const auto dir = kDirs.find(name.substr(6));
        return Mouse(c, r, 0, dir == kDirs.end() ? -1 : dir->second);
    }
    // Anything else (none today): a blank key.
    Keycap(c, font, r, "");
}

// --- building the sets ---

struct State {
    std::atomic<bool> ready{false};
    uint32_t w = 0, h = 0;
    uint64_t game_hash = 0;
    size_t game_bytes = 0;
    std::vector<uint8_t> pixels[kSetCount];  // empty: not available
} g_state;

std::atomic<int> g_last_device{kPlayStation};

// Writes a sheet as a PNG (stored deflate blocks), for checking the sets: LDPORT_ICONS_DUMP=<folder>.
void WritePng(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    static uint32_t crc_table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    });
    const auto crc = [](const uint8_t* p, size_t n) {
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (const uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t p = 0; p < raw.size(); p += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - p);
        z.push_back(p + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
    }
    const uint32_t adler = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));
    std::ofstream out(path, std::ios::binary);
    const auto be = [&](uint32_t v) {
        const uint8_t o[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        out.write(reinterpret_cast<const char*>(o), 4);
    };
    const auto chunk = [&](const char* tag, const std::vector<uint8_t>& data) {
        be(uint32_t(data.size()));
        std::vector<uint8_t> body(tag, tag + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
        be(crc(body.data(), body.size()));
    };
    out.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// --- the Toy Pad figures: list, portraits and the picker's atlas (what port/tools/toypad_icons.py made) ---
//
// <data>/toypad/icons/figures.json lists every figure the game knows (each chars/collection.txt's
// "collect <name> portal_model_id <id>" lines) with a readable name, its franchise and the add-on package it needs;
// <id>.png is its portrait from the game's character icon pages (for the LAN Toy Pad page), and atlas.bin holds
// all the portraits in one RGBA image for the in-game picker. Made from the player's own game files the first time
// the game runs (and again when kFiguresVersion changes).

constexpr int kFiguresVersion = 1;
constexpr uint32_t kIconHeight = 128;
std::atomic<int> g_figures_generation{0};

// TT's obfuscated text: "ZIPX", u32 size, u32 size, then RC4 keyed with the size's 4 little-endian bytes.
std::vector<uint8_t> UnZipx(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || std::memcmp(d.data(), "ZIPX", 4) != 0) return d;
    const uint32_t n = std::min<uint32_t>(Le32(d.data() + 4), uint32_t(d.size() - 12));
    const uint8_t key[4] = {d[4], d[5], d[6], d[7]};
    uint8_t box[256];
    for (int i = 0; i < 256; ++i) box[i] = uint8_t(i);
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + box[i] + key[i & 3]) & 255;
        std::swap(box[i], box[j]);
    }
    std::vector<uint8_t> out(n);
    for (uint32_t k = 0, i = 0, j = 0; k < n; ++k) {
        i = (i + 1) & 255;
        j = (j + box[i]) & 255;
        std::swap(box[i], box[j]);
        out[k] = d[12 + k] ^ box[(box[i] + box[j]) & 255];
    }
    return out;
}

// Year 2 add-on packages by the franchise whose world each adds (their own collection.txt lists only NPCs).
// Ghostbusters is split: the 2016 figures need the 2016 story pack.
const std::map<std::string, std::string> kPackByFranchise = {
    {"Adventure Time", "OPUSADVENTURETIM"}, {"The A-Team", "OPUSATEAM0000001"},
    {"The LEGO Batman Movie", "OPUSBATMANSTORY1"}, {"Beetlejuice", "OPUSBEETLEJUICE1"}, {"E.T.", "OPUSET0000000001"},
    {"Fantastic Beasts", "OPUSFANTASTICBEA"}, {"The Goonies", "OPUSGOONIES00001"}, {"Gremlins", "OPUSGREMLINS0001"},
    {"Harry Potter", "OPUSHARRYPOTTER1"}, {"Knight Rider", "OPUSKNIGHTRIDER1"},
    {"LEGO City: Undercover", "OPUSLEGOCITY0001"}, {"Mission: Impossible", "OPUSMISSIONIMPOS"},
    {"The Powerpuff Girls", "OPUSPOWERPUFF001"}, {"Sonic the Hedgehog", "OPUSSONIC0000001"},
    {"Teen Titans Go!", "OPUSTEENTITANS01"}};

std::string FigurePack(uint32_t id, const std::string& franchise) {
    if (franchise == "Ghostbusters") return (id == 47 || id == 1197 || id == 1198 || id == 1199) ? "OPUSGHOSTBUSTERS" : "";
    const auto it = kPackByFranchise.find(franchise);
    return it == kPackByFranchise.end() ? "" : it->second;
}

// Each character's franchise; vehicles and gadgets take their owner's (or a name keyword's).
const std::map<uint32_t, std::string>& Franchises() {
    static const std::map<uint32_t, std::string> m = [] {
        const std::pair<const char*, std::vector<uint32_t>> table[] = {
            {"DC Comics", {1, 4, 6, 12, 20, 23, 40, 43, 45, 46, 769}}, {"The Lord of the Rings", {2, 18, 19, 29}},
            {"The LEGO Movie", {3, 5, 8, 16, 41}}, {"The Simpsons", {7, 21, 27}}, {"Portal 2", {9}},
            {"Ninjago", {10, 22, 24, 30, 32, 37, 44}}, {"Legends of Chima", {11, 17, 28}}, {"Doctor Who", {13, 15}},
            {"Back to the Future", {14, 31}}, {"Jurassic World", {25, 33}}, {"Midway Arcade", {26}},
            {"Ghostbusters", {34, 35, 39, 47}}, {"Scooby-Doo", {36, 38}}, {"The Wizard of Oz", {42}},
            {"Adventure Time", {48, 50, 51, 63}}, {"Mission: Impossible", {49}}, {"Harry Potter", {52, 53, 67}},
            {"Knight Rider", {54}}, {"The A-Team", {55}}, {"Fantastic Beasts", {56, 62}}, {"Sonic the Hedgehog", {57}},
            {"Gremlins", {59, 60}}, {"E.T.", {61}}, {"The LEGO Batman Movie", {64, 65, 69}}, {"The Goonies", {66}},
            {"LEGO City: Undercover", {68}}, {"Teen Titans Go!", {70, 71, 77}}, {"Beetlejuice", {72}},
            {"The Powerpuff Girls", {74, 75, 76}}};
        std::map<uint32_t, std::string> r;
        for (const auto& [name, ids] : table)
            for (const uint32_t id : ids) r[id] = name;
        return r;
    }();
    return m;
}
const std::pair<const char*, const char*> kVehicleHints[] = {
    {"lbm", "The LEGO Batman Movie"}, {"tardis", "Doctor Who"}, {"k9", "Doctor Who"}, {"dalek", "Doctor Who"},
    {"portal", "Portal 2"}, {"turret", "Portal 2"}, {"ninjago", "Ninjago"}, {"jurassic", "Jurassic World"},
    {"ecto", "Ghostbusters"}, {"ghost", "Ghostbusters"}, {"pirateship", "The Goonies"},
    {"swoopingevil", "Fantastic Beasts"}, {"niffler", "Fantastic Beasts"}, {"lumpy", "Adventure Time"},
    {"marceline", "Adventure Time"}, {"bmo", "Adventure Time"}, {"jakemob", "Adventure Time"}};

void FixSpelling(std::string& s) {
    static const std::pair<const char*, const char*> fixes[] = {{"Delorian", "DeLorean"}, {"Jurrasic", "Jurassic"},
                                                                {"Gimlie", "Gimli's"},     {"Scoobie", "Scooby"},
                                                                {"Coptor", "Copter"},      {"Balista", "Ballista"}};
    for (const auto& [from, to] : fixes)
        for (size_t at; (at = s.find(from)) != std::string::npos;) s.replace(at, std::strlen(from), to);
}

// "GandalfTheGrey" -> "Gandalf The Grey"; underscores become spaces; all-lower-case words get a capital.
std::string Words(const std::string& raw) {
    std::string spaced;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '_') {
            spaced += ' ';
            continue;
        }
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(raw[i - 1])))
            spaced += ' ';
        spaced += c;
    }
    std::string out;
    std::istringstream words(spaced);
    for (std::string w; words >> w;) {
        bool lower = false, upper = false;
        for (const char c : w) {
            lower = lower || std::islower(static_cast<unsigned char>(c));
            upper = upper || std::isupper(static_cast<unsigned char>(c));
        }
        if (lower && !upper) w[0] = char(std::toupper(static_cast<unsigned char>(w[0])));
        out += (out.empty() ? "" : " ") + w;
    }
    FixSpelling(out);
    return out;
}

// Vehicles and gadgets ("TI_BadCopCar_1", three builds each) take the comment's name ("///< Bad Cop Car - Model 1").
std::string NiceName(const std::string& raw, const std::string& comment) {
    static const std::regex vehicle(R"(^ti_(.+?)_(\d)$)", std::regex::icase);
    static const std::regex model(R"(///<\s*(.+?)\s*-\s*Model)");
    std::smatch m;
    if (std::regex_match(raw, m, vehicle)) {
        const std::string base = m[1], build = m[2];
        std::smatch c;
        std::string name = std::regex_search(comment, c, model) ? std::string(c[1]) : Words(base);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        FixSpelling(name);
        return name + " (build " + build + ")";
    }
    std::string r = raw;
    if (Lower(r).rfind("teentitansgo_", 0) == 0) r = r.substr(13);
    return Words(r);
}

std::string JsonString(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        o += c;
    }
    return o + "\"";
}

struct Icon {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// A character icon page (.tsh): 56-byte records, each with a "1TXD" marker at +32 and the name hash at +52, and its
// rectangle (u0, v0, u1, v1, v up, big-endian floats) at +0; the first marker locates them. Then a DXT1/DXT5 DDS.
bool ParseIconPage(const std::vector<uint8_t>& d, const std::map<uint32_t, std::string>& names, Sheet& sheet) {
    const uint8_t kTxd[4] = {'1', 'T', 'X', 'D'}, kDds[4] = {'D', 'D', 'S', ' '};
    const auto txd = std::search(d.begin(), d.end(), kTxd, kTxd + 4);
    const auto dds_at = std::search(d.begin(), d.end(), kDds, kDds + 4);
    if (txd == d.end() || dds_at == d.end()) return false;
    const size_t t = size_t(txd - d.begin()), dds = size_t(dds_at - d.begin());
    if (t < 32 || dds + 128 > d.size()) return false;
    sheet.h = Le32(d.data() + dds + 12);
    sheet.w = Le32(d.data() + dds + 16);
    const bool bc3 = std::memcmp(d.data() + dds + 84, "DXT5", 4) == 0;
    if (!bc3 && std::memcmp(d.data() + dds + 84, "DXT1", 4) != 0) return false;
    const size_t bytes = size_t((sheet.w + 3) / 4) * ((sheet.h + 3) / 4) * (bc3 ? 16 : 8);
    if (dds + 128 + bytes > d.size()) return false;
    DecodeDxt(d.data() + dds + 128, sheet.w, sheet.h, bc3, sheet.rgba);
    for (size_t i = 0; i < names.size(); ++i) {
        const size_t marker = t + 56 * i;
        if (marker + 24 > dds || std::memcmp(d.data() + marker, "1TXD", 4) != 0) break;
        const uint8_t* r = d.data() + marker - 32;
        const float u0 = BeFloat(r), v0 = BeFloat(r + 4), u1 = BeFloat(r + 8), v1 = BeFloat(r + 12);
        const auto name = names.find(Be32(d.data() + marker + 20));
        if (name == names.end() || !(0 <= u0 && u0 < u1 && u1 <= 1 && 0 <= v0 && v0 < v1 && v1 <= 1)) continue;
        Rect rect;
        rect.x0 = u0 * float(sheet.w);
        rect.x1 = u1 * float(sheet.w);
        rect.y0 = (1.0f - v1) * float(sheet.h);
        rect.y1 = (1.0f - v0) * float(sheet.h);
        sheet.sprites[name->second] = rect;
    }
    return !sheet.sprites.empty();
}

void BuildFigures(const std::vector<Archive>& archives) {
    const auto t0 = std::chrono::steady_clock::now();
    // The same folder the Toy Pad reads (toypad.cpp: <data>/toypad), named the same way.
    const fs::path out = fs::path((hle::DataRoot().empty() ? std::string("run") : hle::DataRoot()) + "/toypad/icons");
    const fs::path stamp = out / "made-by-port.txt";
    std::error_code ec;
    if (!std::getenv("LDPORT_REBUILD_FIGURES") && fs::exists(out / "figures.json", ec) && fs::exists(out / "atlas.bin", ec) &&
        ReadFile(stamp) == std::vector<uint8_t>{uint8_t('0' + kFiguresVersion)})
        return;  // made already

    // Every path the archives name; the update's copy of a file wins (OpenArchives puts its archives first).
    std::set<std::string> collections, pages;
    for (const Archive& a : archives) {
        for (const auto& [path, ref] : a.named) {
            if (path.size() >= 20 && path.compare(path.size() - 20, 20, "chars/collection.txt") == 0) collections.insert(path);
            const std::string tsh = "icons/characters/charactericontpage_dx11.tsh";
            if (path.size() >= tsh.size() && path.compare(path.size() - tsh.size(), tsh.size(), tsh) == 0) pages.insert(path);
        }
    }
    struct Figure { std::string raw, name, owner; };
    std::map<uint32_t, Figure> figures;
    static const std::regex collect(R"re(^\s*collect\s+"([^"]+)"\s+portal_model_id\s+(\d+)(.*)$)re", std::regex::icase);
    for (const std::string& path : collections) {
        const std::vector<uint8_t> data = UnZipx(Extract(archives, path));
        std::istringstream lines(std::string(data.begin(), data.end()));
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch m;
            if (!std::regex_match(line, m, collect)) continue;
            const uint32_t id = uint32_t(std::stoul(m[2]));
            if (figures.count(id)) continue;
            const std::string raw = m[1], comment = m[3];
            std::string owner;  // "can_be_upgraded_by <name>", unless commented out ("/can_be...")
            for (size_t at = comment.find("can_be_upgraded_by"); at != std::string::npos;
                 at = comment.find("can_be_upgraded_by", at + 1)) {
                if (at > 0 && comment[at - 1] == '/') continue;
                size_t p = at + 18;
                while (p < comment.size() && std::isspace(static_cast<unsigned char>(comment[p]))) ++p;
                size_t e = p;
                while (e < comment.size() && (std::isalnum(static_cast<unsigned char>(comment[e])) || comment[e] == '_')) ++e;
                if (e > p) {
                    owner = Lower(comment.substr(p, e - p));
                    break;
                }
            }
            figures[id] = {raw, NiceName(raw, comment), owner};
        }
    }
    if (figures.empty()) {
        std::fprintf(stderr, "[icons] no Toy Pad figure list in the game's files: the picker shows names only\n");
        return;
    }
    std::vector<Sheet> sheets;
    for (const std::string& page : pages) {
        std::map<uint32_t, std::string> names;
        const std::string contents = page.substr(0, page.size() - 4) + "_contents.txt";
        ReadNames(Extract(archives, contents), names);
        Sheet s;
        if (!names.empty() && ParseIconPage(Extract(archives, page), names, s)) sheets.push_back(std::move(s));
    }

    std::map<std::string, uint32_t> by_raw;
    for (const auto& [id, f] : figures)
        if (id < 1000) by_raw[Lower(f.raw)] = id;
    fs::create_directories(out, ec);
    std::map<uint32_t, Icon> icons;
    std::string listing = "[\n";
    int with_icon = 0, from_packs = 0;
    for (const auto& [id, f] : figures) {
        const std::string key = Lower(f.raw);
        std::string franchise;
        if (const auto it = Franchises().find(id); it != Franchises().end()) franchise = it->second;
        for (const auto& [hint, name] : kVehicleHints)
            if (franchise.empty() && key.find(hint) != std::string::npos) franchise = name;
        if (franchise.empty())
            if (const auto o = by_raw.find(f.owner); o != by_raw.end())
                if (const auto it = Franchises().find(o->second); it != Franchises().end()) franchise = it->second;
        if (franchise.empty()) franchise = "Other";

        bool has_icon = false;
        for (const Sheet& s : sheets) {
            const auto sp = s.sprites.find(key);
            if (sp == s.sprites.end()) continue;
            const Rect& r = sp->second;
            const int x0 = int(std::lround(r.x0)), y0 = int(std::lround(r.y0));
            const int w = int(std::lround(r.w())), h = int(std::lround(r.h()));
            if (w <= 0 || h <= 0) continue;
            // Nearest-neighbour down to kIconHeight rows (never up).
            Icon icon;
            icon.h = uint32_t(h) > kIconHeight ? kIconHeight : uint32_t(h);
            icon.w = uint32_t(h) > kIconHeight ? std::max(1u, uint32_t(std::lround(double(w) * kIconHeight / h))) : uint32_t(w);
            icon.rgba.assign(size_t(icon.w) * icon.h * 4, 0);
            for (uint32_t y = 0; y < icon.h; ++y) {
                for (uint32_t x = 0; x < icon.w; ++x) {
                    const int sx = x0 + int(uint64_t(x) * w / icon.w), sy = y0 + int(uint64_t(y) * h / icon.h);
                    if (sx < 0 || sy < 0 || uint32_t(sx) >= s.w || uint32_t(sy) >= s.h) continue;
                    std::memcpy(&icon.rgba[(size_t(y) * icon.w + x) * 4], &s.rgba[(size_t(sy) * s.w + sx) * 4], 4);
                }
            }
            WritePng(out / (std::to_string(id) + ".png"), icon.w, icon.h, icon.rgba);
            icons[id] = std::move(icon);
            has_icon = true;
            ++with_icon;
            break;
        }
        const std::string pack = FigurePack(id, franchise);
        from_packs += !pack.empty();
        // One figure per line: the web server reads it line by line.
        listing += (listing.size() > 2 ? ",\n" : "") + std::string("{\"id\": ") + std::to_string(id) +
                   ", \"name\": " + JsonString(f.name) + ", \"kind\": \"" + (id < 1000 ? "character" : "vehicle") +
                   "\", \"icon\": " + (has_icon ? "true" : "false") + ", \"franchise\": " + JsonString(franchise) +
                   ", \"pack\": " + JsonString(pack) + "}";
    }
    listing += "\n]\n";

    // atlas.bin (little-endian): "TPAT", u32 version 1, u32 width, u32 height, u32 count,
    // count x {u32 id, u16 x, u16 y, u16 w, u16 h}, then width * height RGBA bytes.
    constexpr uint32_t kCell = kIconHeight, kColumns = 32;
    const uint32_t rows = uint32_t((icons.size() + kColumns - 1) / kColumns);
    const uint32_t aw = kCell * kColumns, ah = kCell * std::max(1u, rows);
    std::vector<uint8_t> pixels(size_t(aw) * ah * 4, 0), entries;
    const auto put = [&](uint32_t v, int bytes) {
        for (int b = 0; b < bytes; ++b) entries.push_back(uint8_t(v >> (8 * b)));
    };
    uint32_t k = 0;
    for (const auto& [id, icon] : icons) {
        const uint32_t w = std::min(icon.w, kCell), h = std::min(icon.h, kCell);
        const uint32_t x0 = (k % kColumns) * kCell + (kCell - w) / 2, y0 = (k / kColumns) * kCell + (kCell - h) / 2;
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&pixels[(size_t(y0 + y) * aw + x0) * 4], &icon.rgba[size_t(y) * icon.w * 4], size_t(w) * 4);
        put(id, 4), put(x0, 2), put(y0, 2), put(w, 2), put(h, 2);
        ++k;
    }
    {
        std::ofstream a(out / "atlas.bin", std::ios::binary);
        const uint32_t head[5] = {0x54415054u /* "TPAT" */, 1, aw, ah, uint32_t(icons.size())};
        a.write(reinterpret_cast<const char*>(head), sizeof head);
        a.write(reinterpret_cast<const char*>(entries.data()), std::streamsize(entries.size()));
        a.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    }
    std::ofstream(out / "figures.json", std::ios::binary) << listing;
    std::ofstream(stamp, std::ios::binary) << char('0' + kFiguresVersion);
    g_figures_generation.fetch_add(1, std::memory_order_release);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[icons] Toy Pad figures: %zu, %d with portraits (%zu icon pages), %d from add-on packs, in %lld ms\n",
                 figures.size(), with_icon, sheets.size(), from_packs, static_cast<long long>(ms));
}

void BuildButtons(const std::vector<Archive>& archives, std::chrono::steady_clock::time_point t0);

void Build() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Archive> archives = OpenArchives();
    BuildButtons(archives, t0);
    BuildFigures(archives);
}

void BuildButtons(const std::vector<Archive>& archives, std::chrono::steady_clock::time_point t0) {
    const std::string dir = "framework-data/gui/buttons/";
    std::map<uint32_t, std::string> names;
    for (const char* p : {"ps4", "xboxone", "360", "wiiu"})
        ReadNames(Extract(archives, dir + p + "/buttonicons_dx11_contents.txt"), names);
    Sheet ps4;
    if (!ParseSheet(Extract(archives, dir + "ps4/buttonicons_dx11.tsh"), names, ps4)) {
        std::fprintf(stderr, "[icons] the game's PlayStation icon sheet was not found (%zu archives): PlayStation icons only\n",
                     archives.size());
        return;
    }
    // buttonconfig.csv: "Config,Name,Filename,...": per platform, the icon that stands for each PlayStation button.
    std::map<std::string, std::map<std::string, std::string>> config;  // platform -> PS4 name -> its icon
    {
        const std::vector<uint8_t> csv = Extract(archives, dir + "buttonconfig.csv");
        std::string s(csv.begin(), csv.end());
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find('\n', pos);
            if (end == std::string::npos) end = s.size();
            std::string line = s.substr(pos, end - pos);
            pos = end + 1;
            std::vector<std::string> cols;
            for (size_t a = 0, b; a <= line.size(); a = b + 1) {
                b = line.find(',', a);
                if (b == std::string::npos) b = line.size();
                std::string col = line.substr(a, b - a);
                while (!col.empty() && (col.back() == '\r' || col.back() == ' ')) col.pop_back();
                cols.push_back(col);
            }
            if (cols.size() >= 3 && !cols[0].empty() && cols[0] != "Config") config[Upper(cols[0])][Lower(cols[1])] = Lower(cols[2]);
        }
    }
    g_state.w = ps4.w;
    g_state.h = ps4.h;
    g_state.game_bytes = ps4.blocks.size();
    g_state.game_hash = Fnv64(ps4.blocks.data(), ps4.blocks.size());

    struct Source { int set; const char* folder; const char* config; };
    const Source sources[] = {{kXbox, "xboxone", "XBOXONE"}, {kXbox360, "360", "360"}, {kNintendo, "wiiu", "WIIU"}};
    for (const Source& src : sources) {
        Sheet other;
        if (!ParseSheet(Extract(archives, dir + src.folder + "/buttonicons_dx11.tsh"), names, other)) {
            std::fprintf(stderr, "[icons] no %s sheet in the game's files\n", src.folder);
            continue;
        }
        const auto& map = config[src.config];
        std::vector<uint8_t> px(size_t(ps4.w) * ps4.h * 4, 0);
        Canvas c{ps4.w, ps4.h, px};
        int replaced = 0;
        for (const auto& [name, rect] : ps4.sprites) {
            const auto m = map.find(name);
            const auto it = other.sprites.find(m != map.end() ? m->second : name);
            if (it != other.sprites.end()) {
                Blit(c, rect, other, it->second);
                ++replaced;
            } else {
                Blit(c, rect, ps4, rect);  // no counterpart: the PlayStation icon stays
            }
        }
        g_state.pixels[src.set] = std::move(px);
        std::fprintf(stderr, "[icons] %s set: %d of %zu icons from the %s sheet (%ux%u)\n", SetName(src.set), replaced,
                     ps4.sprites.size(), src.folder, other.w, other.h);
    }
    {
        const Font font = LoadFont();
        std::vector<uint8_t> px(size_t(ps4.w) * ps4.h * 4, 0);
        Canvas c{ps4.w, ps4.h, px};
        for (const auto& [name, rect] : ps4.sprites) DrawKeyboard(c, font, name, rect);
        g_state.pixels[kKeyboard] = std::move(px);
    }
    if (const char* dump = std::getenv("LDPORT_ICONS_DUMP")) {
        std::error_code ec;
        fs::create_directories(dump, ec);
        WritePng(fs::path(dump) / "playstation.png", ps4.w, ps4.h, ps4.rgba);
        for (int s = 1; s < kSetCount; ++s)
            if (!g_state.pixels[s].empty()) WritePng(fs::path(dump) / (Lower(SetName(s)) + ".png"), ps4.w, ps4.h, g_state.pixels[s]);
    }
    g_state.ready.store(true, std::memory_order_release);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[icons] button icon sets ready in %lld ms (%zu archives)\n", static_cast<long long>(ms), archives.size());
}

}  // namespace

const char* SetName(int set) {
    switch (set) {
        case kXbox: return "Xbox";
        case kXbox360: return "Xbox 360";
        case kNintendo: return "Nintendo";
        case kKeyboard: return "Keyboard";
        default: return "PlayStation";
    }
}

void Prepare() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Build).detach(); });
}

int FiguresGeneration() { return g_figures_generation.load(std::memory_order_acquire); }

void NoteInput(int set) {
    if (set < 0 || set >= kSetCount) return;
    if (g_last_device.exchange(set) != set) std::fprintf(stderr, "[icons] last device: %s\n", SetName(set));
}

int Wanted() {
    const uint32_t forced = port::Get().button_icons.load();
    if (forced >= 1 && forced <= uint32_t(kSetCount)) return int(forced) - 1;
    return g_last_device.load();
}

bool Ready() { return g_state.ready.load(std::memory_order_acquire); }

bool SheetSize(uint32_t width, uint32_t height) { return Ready() && width == g_state.w && height == g_state.h; }

bool IsGameSheet(const uint8_t* blocks, size_t bytes) {
    return Ready() && bytes >= g_state.game_bytes && Fnv64(blocks, g_state.game_bytes) == g_state.game_hash;
}

int Applicable() {
    const int set = Wanted();
    return Ready() && set != kPlayStation && !g_state.pixels[set].empty() ? set : kPlayStation;
}

const std::vector<uint8_t>* Pixels(int set) {
    if (!Ready() || set <= kPlayStation || set >= kSetCount || g_state.pixels[set].empty()) return nullptr;
    return &g_state.pixels[set];
}

}  // namespace icons
