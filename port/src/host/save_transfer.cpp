#include "host/save_transfer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace host {
namespace {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- formats ------------------------------------------------------------------------------------------------------

struct Format {
    const char* key;
    bool big;                  // header and native chunks in big-endian order
    uint32_t header_version;   // first word of every file
    uint32_t game_block;       // GAME / V2GAME block version
    uint32_t dlc_block;        // DLC block version
    uint32_t preamble;         // zeros before the GAME block marker (DLC: always 8)
    uint32_t game_size;        // payload sizes: the game writes fixed-size buffers
    uint32_t v2_size;
    uint32_t dlc_size;
    bool split;                // V2GAME + GAME (false: one GAME01, PS3)
    bool wiiu_names;           // V2GAME0 / GAME0 / DLC0..DLC30
    uint32_t align;            // file length rounded up to this (PS3)
};

const Format kPs4{"ps4", false, 13, 28, 21, 0x440, 100425, 308053, 234817, true, false, 0};
const Format kX360{"x360", true, 13, 28, 21, 0x438, 100417, 308045, 234817, true, false, 0};
const Format kWiiU{"wiiu", true, 12, 27, 20, 0x438, 100381, 308809, 234805, true, true, 0};
const Format kPs3{"ps3", true, 13, 28, 21, 0x438, 0, 308200, 235496, false, false, 1024};

struct Platform {
    const char* key;
    const char* name;
    const Format* format;
    const char* experimental;  // why, or nullptr when verified
};

const Platform kPlatforms[] = {
    {"port", "this PC port", &kPs4, nullptr},
    {"shadps4", "shadPS4 (PS4)", &kPs4, nullptr},
    {"xenia", "Xenia (Xbox 360)", &kX360,
     "the Xbox 360 save layout comes from notes on another port of the game and has not been checked against a real "
     "Xenia save here"},
    {"cemu", "Cemu (Wii U)", &kWiiU,
     "the Wii U save layout comes from notes on another port of the game and has not been checked against a real "
     "Cemu save here"},
    {"rpcs3", "RPCS3 (PS3)", &kPs3,
     "the PS3 layout is unverified, saves copied from a real PS3 are encrypted (only RPCS3's own, decrypted saves "
     "work), and RPCS3's PARAM.PFD is not rewritten"},
};

const Platform* FindPlatform(const std::string& key) {
    for (const Platform& p : kPlatforms)
        if (key == p.key) return &p;
    return nullptr;
}
// The platform a save format belongs to, for messages (the port and shadPS4 share PS4 saves).
const char* FormatName(const Format* f) {
    return f == &kPs4 ? "PS4 (this port / shadPS4)" : f == &kX360 ? "Xbox 360 (Xenia)" : f == &kWiiU ? "Wii U (Cemu)" : "PS3 (RPCS3)";
}

constexpr uint32_t kMarker = 0x4DEE7E53, kSentinel = 0x075BCD15;
constexpr uint32_t kGameV28Chunk = 0x5081EB33;  // only in GAME block 28 (6 counters, zero in a new game)
constexpr uint32_t kDlcV21Chunk = 0x10CA236C;   // only in DLC block 21
constexpr uint32_t kAnchorChunk = 0xD02558B0;   // the two chunks above sit right after this one

// Chunks holding raw console-order data; everything else in the payload is little-endian on every console.
enum class Native { kWords, kPairs, kRecords };
const std::map<uint32_t, Native>& NativeChunks() {
    static const std::map<uint32_t, Native> m = [] {
        std::map<uint32_t, Native> r{{0x97ACD358, Native::kWords}, {0x38EFEFE9, Native::kPairs}};
        for (uint32_t t : {0x35EEAB6Fu, 0x00BD2EA0u, 0x29672BE1u, 0x2DC598C5u, 0x0ABBE129u, 0xEF3A5C65u, 0xD4AAD89Eu,
                           0x1BBDBDF5u, 0x774D4F35u, 0x331B6C77u, 0xCA088DCCu, 0x646F2CD1u, 0x73ED76F1u, 0x60AD7021u,
                           0xD02558B0u})
            r[t] = Native::kRecords;
        return r;
    }();
    return m;
}

uint32_t Get32(const uint8_t* p, bool big) {
    return big ? uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3] : uint32_t(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0];
}
void Put32(uint8_t* p, uint32_t v, bool big) {
    for (int i = 0; i < 4; ++i) p[big ? 3 - i : i] = uint8_t(v >> (8 * i));
}
void Append32(Bytes& out, uint32_t v, bool big = false) {
    uint8_t b[4];
    Put32(b, v, big);
    out.insert(out.end(), b, b + 4);
}

// ~FNV-1 (32-bit, offset basis 0xffffffff), as the game checks it.
uint32_t SaveHash(const uint8_t* p, size_t n) {
    uint32_t h = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) h = (h * 0x01000193u) ^ p[i];
    return ~h;
}

struct Chunk {
    uint32_t tag;
    Bytes data;  // without tag, size and closing tag
};
struct Block {
    uint32_t version = 0;
    std::vector<Chunk> chunks;
};

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string U8(const fs::path& p) {
    const std::u8string s = fs::path(p).make_preferred().u8string();
    return std::string(s.begin(), s.end());
}
fs::path U8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::optional<fs::path> FindFile(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (e.is_regular_file(ec2) && Upper(U8(e.path().filename())) == name) return e.path();
    }
    return std::nullopt;
}

Bytes ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw Error("cannot read " + U8(p));
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Header check; returns the payload. big is set from the file (the header version is a small number).
Bytes ReadSaveFile(const fs::path& p, bool& big, uint32_t& header_version, uint32_t& stored_hash) {
    const Bytes d = ReadAll(p);
    const std::string name = U8(p.filename());
    if (d.size() < 24) throw Error(name + " is not a LEGO Dimensions save");
    big = Get32(d.data(), false) >= 0x100;
    header_version = Get32(d.data(), big);
    if (Get32(d.data() + 4, big) != SaveHash(d.data(), 4))
        throw Error(name + " is not a LEGO Dimensions save (or is encrypted: PS3 console saves must be decrypted first)");
    if (header_version < 12)
        throw Error(name + " comes from a game version without the updates: update the game, load and save, then transfer");
    const uint32_t size = Get32(d.data() + 20, big);
    if (size > d.size() - 24) throw Error(name + " is truncated");
    stored_hash = Get32(d.data() + 16, big);
    return Bytes(d.begin() + 24, d.begin() + 24 + size);
}

Block ParseBlock(const Bytes& payload, const std::string& name) {
    const uint8_t mark[4] = {0x53, 0x7E, 0xEE, 0x4D};
    const auto it = std::search(payload.begin(), payload.end(), mark, mark + 4);
    if (it == payload.end() || std::any_of(payload.begin(), it, [](uint8_t b) { return b != 0; }))
        throw Error(name + ": save structure not recognised");
    size_t pos = size_t(it - payload.begin()) + 4;
    auto need = [&](size_t n) {
        if (pos + n > payload.size()) throw Error(name + ": save structure not recognised");
    };
    need(4);
    Block b;
    b.version = Get32(&payload[pos], false);
    pos += 4;
    for (;;) {
        need(4);
        const uint32_t tag = Get32(&payload[pos], false);
        if (tag == kSentinel) break;
        need(8);
        const uint32_t size = Get32(&payload[pos + 4], false);
        if (size > payload.size()) throw Error(name + ": save structure not recognised");
        need(8 + size_t(size) + 4);
        if (Get32(&payload[pos + 8 + size], false) != tag) throw Error(name + ": save structure not recognised");
        b.chunks.push_back({tag, Bytes(payload.begin() + pos + 8, payload.begin() + pos + 8 + size)});
        pos += 12 + size_t(size);
    }
    if (std::any_of(payload.begin() + pos + 4, payload.end(), [](uint8_t v) { return v != 0; }))
        throw Error(name + ": save structure not recognised");
    return b;
}

// Moves the console-order parts of the native chunks from one byte order to the other.
void ReorderChunk(Chunk& c, bool to_big) {
    const auto it = NativeChunks().find(c.tag);
    if (it == NativeChunks().end()) return;
    Bytes& d = c.data;
    const size_t n = d.size();
    switch (it->second) {
        case Native::kWords:  // a bitmap in 32-bit words
            for (size_t i = 0; i + 4 <= n; i += 4) std::reverse(d.begin() + i, d.begin() + i + 4);
            break;
        case Native::kPairs:  // a count word and a word, then (index, value) pairs: halves swap on big-endian consoles
            if (n >= 8 && !to_big) std::reverse(d.begin(), d.begin() + 4);
            for (size_t i = 0; i + 8 <= n; i += 8) std::rotate(d.begin() + i, d.begin() + i + 4, d.begin() + i + 8);
            if (n >= 8 && to_big) std::reverse(d.begin(), d.begin() + 4);
            break;
        case Native::kRecords:  // 24-byte records: owner, property, u64 value, u64 default (stored as swapped halves)
            for (size_t i = 0; i + 24 <= n; i += 24) {
                std::rotate(d.begin() + i + 8, d.begin() + i + 12, d.begin() + i + 16);
                std::rotate(d.begin() + i + 16, d.begin() + i + 20, d.begin() + i + 24);
            }
            break;
    }
}

// Brings a block to the target's version: the versions differ by one chunk (added in the newer one).
void AdaptBlock(Block& b, uint32_t target, uint32_t new_chunk, uint32_t new_size, bool upgrade, const std::string& name) {
    if (b.version == target || (b.version < target && !upgrade)) return;
    if (b.version == target + 1) {
        b.chunks.erase(std::remove_if(b.chunks.begin(), b.chunks.end(), [&](const Chunk& c) { return c.tag == new_chunk; }),
                       b.chunks.end());
    } else if (b.version + 1 == target) {
        auto at = std::find_if(b.chunks.begin(), b.chunks.end(), [](const Chunk& c) { return c.tag == kAnchorChunk; });
        if (at == b.chunks.end()) throw Error(name + ": save structure not recognised");
        b.chunks.insert(at + 1, Chunk{new_chunk, Bytes(new_size, 0)});
    } else if (b.version > target) {
        throw Error(name + " comes from a newer game version (block v" + std::to_string(b.version) +
                    ") than the target accepts (v" + std::to_string(target) + ")");
    } else {
        throw Error(name + " comes from a game version that is too old (block v" + std::to_string(b.version) +
                    "): update the game, load and save, then transfer");
    }
    b.version = target;
}

Bytes BuildPayload(const Block& b, uint32_t preamble, uint32_t size, const std::string& name) {
    Bytes out(preamble, 0);
    Append32(out, kMarker);
    Append32(out, b.version);
    for (const Chunk& c : b.chunks) {
        Append32(out, c.tag);
        Append32(out, uint32_t(c.data.size()));
        out.insert(out.end(), c.data.begin(), c.data.end());
        Append32(out, c.tag);
    }
    Append32(out, kSentinel);
    if (out.size() > size) throw Error("the save does not fit the target format (" + name + ")");
    out.resize(size, 0);
    return out;
}

Bytes WithHeader(const Format& f, uint32_t payload_hash, const uint8_t* payload, uint32_t size) {
    Bytes out(24);
    Put32(&out[0], f.header_version, f.big);
    Put32(&out[4], SaveHash(&out[0], 4), f.big);
    Put32(&out[8], 0, f.big);
    Put32(&out[12], SaveHash(&out[8], 4), f.big);
    Put32(&out[16], payload_hash, f.big);
    Put32(&out[20], size, f.big);
    out.insert(out.end(), payload, payload + size);
    if (f.align && out.size() % f.align) out.resize(out.size() + f.align - out.size() % f.align, 0);
    return out;
}

// DLC files: DLC01..DLC31 without DLC15 (PS4 / 360 / PS3); the Wii U numbers them from 0 (DLC0..DLC30).
std::vector<int> DlcIndices() {
    std::vector<int> v;
    for (int k = 1; k <= 31; ++k)
        if (k != 15) v.push_back(k);
    return v;
}
std::string DlcName(const Format& f, int k) {
    char b[16];
    std::snprintf(b, sizeof b, f.wiiu_names ? "DLC%d" : "DLC%02d", f.wiiu_names ? k - 1 : k);
    return b;
}
std::string V2Name(const Format& f) { return f.split ? (f.wiiu_names ? "V2GAME0" : "V2GAME01") : "GAME01"; }
std::string GameName(const Format& f) { return f.wiiu_names ? "GAME0" : "GAME01"; }

bool IsSaveFileName(const std::string& upper) {
    for (const char* p : {"V2GAME", "GAME", "DLC", "OPTSC", "OPTS", "FEOPTS", "ZOPTS"}) {
        const size_t n = std::strlen(p);
        if (upper.size() > n && upper.compare(0, n, p) == 0 &&
            std::all_of(upper.begin() + n, upper.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return true;
    }
    return false;
}
bool IsGameOrDlcName(const std::string& upper) {
    return IsSaveFileName(upper) && (upper.rfind("V2GAME", 0) == 0 || upper.rfind("GAME", 0) == 0 || upper.rfind("DLC", 0) == 0);
}

// ---- reading a save slot ------------------------------------------------------------------------------------------

struct Slot {
    fs::path dir;
    const Format* format = nullptr;
    Block game;
    std::map<int, Block> dlcs;  // by PS4 number
    fs::file_time_type time{};
};

bool HasMainFile(const fs::path& dir) {
    for (const char* n : {"V2GAME01", "V2GAME0", "GAME01", "GAME0"})
        if (FindFile(dir, n)) return true;
    return false;
}

Slot ReadSlot(const fs::path& dir) {
    Slot s;
    s.dir = dir;
    auto main = FindFile(dir, "V2GAME01");
    if (!main) main = FindFile(dir, "V2GAME0");
    const bool split = main.has_value();
    if (!main) main = FindFile(dir, "GAME01");
    if (!main) main = FindFile(dir, "GAME0");
    if (!main) throw Error("no LEGO Dimensions save (GAME / V2GAME) in " + U8(dir));
    bool big;
    uint32_t hv, stored;
    const Bytes payload = ReadSaveFile(*main, big, hv, stored);
    if (SaveHash(payload.data(), payload.size()) != stored) throw Error(U8(main->filename()) + " is corrupted (checksum mismatch)");
    s.format = !big ? &kPs4 : hv <= 12 ? &kWiiU : split ? &kX360 : &kPs3;
    s.game = ParseBlock(payload, U8(main->filename()));
    std::error_code ec;
    s.time = fs::last_write_time(*main, ec);
    for (int k : DlcIndices()) {
        const auto p = FindFile(dir, DlcName(*s.format, k));
        if (!p) continue;
        bool b2;
        uint32_t hv2, st2;
        const Bytes dp = ReadSaveFile(*p, b2, hv2, st2);
        if (SaveHash(dp.data(), dp.size()) != st2) throw Error(U8(p->filename()) + " is corrupted (checksum mismatch)");
        s.dlcs[k] = ParseBlock(dp, U8(p->filename()));
    }
    return s;
}

// Save slots under root (root itself when it is one). Folders that hold no readable save are skipped quietly.
void FindSlots(const fs::path& root, int depth, std::vector<fs::path>& out) {
    std::error_code ec;
    if (HasMainFile(root)) {
        out.push_back(root);
        return;
    }
    if (depth == 0) return;
    for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2) && !e.is_symlink(ec2)) FindSlots(e.path(), depth - 1, out);
    }
}

// The one slot a --from folder means: the folder itself, or the only save below it.
fs::path ResolveSlot(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) throw Error(U8(p) + " is not a folder");
    std::vector<fs::path> found;
    FindSlots(p, 8, found);
    if (found.empty()) throw Error("no LEGO Dimensions save found in " + U8(p));
    if (found.size() > 1) {
        std::string list;
        for (const fs::path& f : found) list += "\n  " + U8(f);
        throw Error("that folder holds more than one save; pick one of them:" + list);
    }
    return found[0];
}

// ---- writing ------------------------------------------------------------------------------------------------------

struct OutFile {
    std::string name;
    Bytes data;
};

std::vector<OutFile> Convert(const Slot& s, const Format& to) {
    std::vector<OutFile> out;
    const bool reorder = s.format->big != to.big;
    Block game = s.game;
    if (reorder)
        for (Chunk& c : game.chunks) ReorderChunk(c, to.big);
    // The PS4 does not accept a v27 GAME block (it starts a new story), so a Wii U save gets the v28 chunk, zeroed.
    AdaptBlock(game, to.game_block, kGameV28Chunk, 24, true, V2Name(to));
    const Bytes main = BuildPayload(game, to.preamble, to.v2_size, V2Name(to));
    const uint32_t main_hash = SaveHash(main.data(), main.size());
    out.push_back({V2Name(to), WithHeader(to, main_hash, main.data(), uint32_t(main.size()))});
    if (to.split) out.push_back({GameName(to), WithHeader(to, main_hash, main.data(), to.game_size)});
    for (const auto& [k, src] : s.dlcs) {
        Block b = src;
        if (reorder)
            for (Chunk& c : b.chunks) ReorderChunk(c, to.big);
        // Older DLC blocks stay as they are (real PS4 saves hold v20 DLC files the game never rewrote).
        AdaptBlock(b, to.dlc_block, kDlcV21Chunk, 0, false, DlcName(to, k));
        const Bytes p = BuildPayload(b, 8, to.dlc_size, DlcName(to, k));
        out.push_back({DlcName(to, k), WithHeader(to, SaveHash(p.data(), p.size()), p.data(), uint32_t(p.size()))});
    }
    return out;
}

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

fs::path Canonical(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? p : c;
}

int Transfer(const fs::path& from_arg, const fs::path& to, const Platform& target, const fs::path& data, bool dry) {
    const fs::path from = ResolveSlot(from_arg);
    if (Canonical(from) == Canonical(to)) throw Error("the destination folder cannot be the source folder");
    const Slot s = ReadSlot(from);
    const Format& f = *target.format;
    std::printf("[transfer] source: %s\n[transfer]   %s save, story block v%u, %zu DLC file(s)\n", U8(from).c_str(),
                FormatName(s.format), s.game.version, s.dlcs.size());
    std::printf("[transfer] target: %s (%s)\n", U8(to).c_str(), target.name);
    if (target.experimental) std::printf("[transfer] note: %s is experimental: %s\n", target.name, target.experimental);
    if (s.format == &kX360 || s.format == &kWiiU || s.format == &kPs3)
        std::printf("[transfer] note: reading %s saves is experimental (the layout is not verified here)\n", FormatName(s.format));

    std::vector<OutFile> files;
    if (s.format == &f) {
        // Same console format (port <-> shadPS4): copy the files as they are, options included.
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(from, ec)) {
            const std::string n = Upper(U8(e.path().filename()));
            std::error_code ec2;
            if (e.is_regular_file(ec2) && IsSaveFileName(n)) files.push_back({U8(e.path().filename()), ReadAll(e.path())});
        }
    } else {
        files = Convert(s, f);
        std::printf("[transfer] option files are not converted; the game recreates them with defaults\n");
    }

    std::error_code ec;
    bool dest_has_files = false;
    for (const auto& e : fs::directory_iterator(to, ec)) { (void)e; dest_has_files = true; break; }
    fs::path backup = data / "save-transfer-backups" / (Stamp() + "-" + target.key + "-" + U8(to.filename()));
    for (int n = 2; fs::exists(backup, ec); ++n)
        backup = data / "save-transfer-backups" / (Stamp() + "-" + target.key + "-" + U8(to.filename()) + "-" + std::to_string(n));
    if (dry) {
        if (dest_has_files) std::printf("[transfer] dry run: would back up %s to %s\n", U8(to).c_str(), U8(backup).c_str());
        std::string names;
        size_t dlc = 0;
        for (const OutFile& o : files) {
            if (Upper(o.name).rfind("DLC", 0) == 0) ++dlc;
            else names += (names.empty() ? "" : ", ") + o.name;
        }
        if (dlc) names += " and " + std::to_string(dlc) + " DLC file(s)";
        std::printf("[transfer] dry run: would write %s\n", names.c_str());
        return 0;
    }
    try {
        if (dest_has_files) {
            fs::create_directories(backup);
            fs::copy(to, backup, fs::copy_options::recursive);
            std::printf("[transfer] backed up the destination to %s\n", U8(backup).c_str());
        }
        fs::create_directories(to);
        // The old story and DLC files go (a DLC file left from another save would not match the new story).
        for (const auto& e : fs::directory_iterator(to)) {
            if (e.is_regular_file() && IsGameOrDlcName(Upper(U8(e.path().filename())))) fs::remove(e.path());
        }
        for (const OutFile& o : files) {
            const fs::path tmp = to / U8Path(o.name + ".transfer-tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(o.data.data()), std::streamsize(o.data.size()));
                if (!out) throw Error("cannot write " + U8(tmp));
            }
            fs::rename(tmp, to / U8Path(o.name));
            std::printf("[transfer] wrote %s\n", o.name.c_str());
        }
    } catch (const fs::filesystem_error& e) {
        throw Error(std::string(e.what()) + (dest_has_files ? "\nthe destination's previous files are in " + U8(backup) : ""));
    }
    std::printf("[transfer] done: %zu file(s) in %s\n", files.size(), U8(to).c_str());
    return 0;
}

// ---- default emulator folders -------------------------------------------------------------------------------------

std::string Env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

std::vector<fs::path> DefaultRoots(const std::string& platform) {
    std::vector<fs::path> r;
#ifdef _WIN32
    const fs::path appdata = U8Path(Env("APPDATA"));
    fs::path docs = U8Path(Env("USERPROFILE")) / "Documents";
    PWSTR w = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &w))) docs = fs::path(w);
    CoTaskMemFree(w);
    if (platform == "shadps4") r = {appdata / "shadPS4" / "savedata"};
    if (platform == "xenia") r = {docs / "Xenia" / "content"};
    if (platform == "cemu") r = {appdata / "Cemu" / "mlc01" / "usr" / "save"};
    if (platform == "rpcs3") r = {appdata / "rpcs3" / "dev_hdd0" / "home"};  // RPCS3 is usually portable: pass its folder
#else
    const fs::path home = Env("HOME");
    const fs::path xdg = Env("XDG_DATA_HOME").empty() ? home / ".local/share" : fs::path(Env("XDG_DATA_HOME"));
    const fs::path cfg = Env("XDG_CONFIG_HOME").empty() ? home / ".config" : fs::path(Env("XDG_CONFIG_HOME"));
    if (platform == "shadps4") r = {xdg / "shadPS4/savedata", home / ".var/app/net.shadps4.shadPS4/data/shadPS4/savedata"};
    if (platform == "xenia") r = {xdg / "Xenia/content"};
    if (platform == "cemu") r = {xdg / "Cemu/mlc01/usr/save", home / ".var/app/info.cemu.Cemu/data/Cemu/mlc01/usr/save"};
    if (platform == "rpcs3") r = {cfg / "rpcs3/dev_hdd0/home", home / ".var/app/net.rpcs3.RPCS3/config/rpcs3/dev_hdd0/home"};
#endif
    return r;
}

struct Found {
    fs::path dir;
    const Format* format;
    fs::file_time_type time;
};

std::vector<Found> ListPlatform(const std::string& platform, const fs::path& data) {
    std::vector<fs::path> roots = platform == "port" ? std::vector<fs::path>{data / "savedata"} : DefaultRoots(platform);
    const Platform* p = FindPlatform(platform);
    std::vector<Found> out;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        std::vector<fs::path> dirs;
        FindSlots(root, 8, dirs);
        for (const fs::path& d : dirs) {
            try {
                const Slot s = ReadSlot(d);
                if (s.format == p->format) out.push_back({d, s.format, s.time});
            } catch (const Error&) {
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.dir < b.dir; });
    return out;
}

std::string TimeText(fs::file_time_type t) {
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() +
                                                                                       std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// ---- interactive --------------------------------------------------------------------------------------------------

std::string ReadLine() {
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(in, &mode)) {  // a console: read UTF-16 so any path can be typed or pasted
        std::wstring w;
        wchar_t buf[512];
        DWORD got = 0;
        while (ReadConsoleW(in, buf, 512, &got, nullptr) && got) {
            w.append(buf, got);
            if (w.find(L'\n') != std::wstring::npos) break;
        }
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
        return s;
    }
#endif
    std::string s;
    if (!std::getline(std::cin, s)) throw Error("cancelled");
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '"')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == '"')) ++i;
    return s.substr(i);
}

int Ask(const std::string& question, int count) {
    for (;;) {
        std::printf("%s ", question.c_str());
        std::fflush(stdout);
        const std::string a = Trim(ReadLine());
        if (a.empty() || a == "q" || a == "Q") throw Error("cancelled");
        char* end = nullptr;
        const long n = std::strtol(a.c_str(), &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= count) return int(n);
        std::printf("Type a number from 1 to %d (or q to quit).\n", count);
    }
}

// Picks a save folder of a platform: one found in its default folders, or a typed path.
fs::path PickEmulatorSlot(const Platform& p, const fs::path& data, bool as_target) {
    const std::vector<Found> found = ListPlatform(p.key, data);
    std::printf("\n%s saves found:\n", p.name);
    for (size_t i = 0; i < found.size(); ++i)
        std::printf("  %zu) %s  (saved %s)\n", i + 1, U8(found[i].dir).c_str(), TimeText(found[i].time).c_str());
    if (found.empty()) {
        std::printf("  (none in the default folders");
        for (const fs::path& r : DefaultRoots(p.key)) std::printf(" %s", U8(r).c_str());
        std::printf(")\n");
        if (as_target)
            std::printf("  Tip: start LEGO Dimensions in %s and save once, so it makes the save folder; then pick it here.\n", p.name);
    }
    std::printf("  %zu) type a folder\n", found.size() + 1);
    const int n = Ask("Which one?", int(found.size() + 1));
    if (size_t(n) <= found.size()) return found[size_t(n) - 1].dir;
    std::printf("Folder (the save folder itself, or one above it holding a single save): ");
    std::fflush(stdout);
    const fs::path typed = U8Path(Trim(ReadLine()));
    if (!as_target) return typed;
    std::error_code ec;
    if (fs::is_directory(typed, ec)) {
        std::vector<fs::path> slots;
        FindSlots(typed, 8, slots);
        if (slots.size() == 1) return slots[0];
    }
    return typed;
}

fs::path PickPortSlot(const fs::path& data, bool as_target) {
    std::vector<std::string> names;
    std::printf("\nPort save slots in %s:\n", U8(data / "savedata").c_str());
    for (int i = 0; i < 4; ++i) {
        char n[8];
        std::snprintf(n, sizeof n, "Slot%02d", i);
        const fs::path d = data / "savedata" / n;
        std::string state = "empty";
        if (HasMainFile(d)) {
            std::error_code ec;
            state = "save from " + TimeText(fs::last_write_time(FindFile(d, "V2GAME01").value_or(d / "GAME01"), ec));
        } else if (!as_target) {
            continue;
        }
        names.push_back(n);
        std::printf("  %zu) %s  (%s)\n", names.size(), n, state.c_str());
    }
    if (names.empty()) throw Error("the port has no saves yet in " + U8(data / "savedata"));
    return data / "savedata" / names[size_t(Ask("Which slot?", int(names.size()))) - 1];
}

int Interactive(const fs::path& data) {
    std::printf("LEGO Dimensions save transfer\n");
    std::printf("The source save is never changed; what the destination held is backed up to\n  %s\n\n",
                U8(data / "save-transfer-backups").c_str());
    std::printf("  1) Bring a save INTO this port\n  2) Send a save from this port OUT to an emulator\n");
    const bool into = Ask("Which?", 2) == 1;
    std::printf("\n");
    const Platform* emus[] = {&kPlatforms[1], &kPlatforms[2], &kPlatforms[3], &kPlatforms[4]};
    for (int i = 0; i < 4; ++i)
        std::printf("  %d) %s%s\n", i + 1, emus[i]->name, emus[i]->experimental ? "  [experimental]" : "");
    const Platform& emu = *emus[Ask(into ? "From which emulator?" : "To which emulator?", 4) - 1];
    if (emu.experimental) std::printf("\nNote: %s is experimental: %s.\n", emu.name, emu.experimental);
    fs::path from, to;
    if (into) {
        from = PickEmulatorSlot(emu, data, false);
        to = PickPortSlot(data, true);
    } else {
        from = PickPortSlot(data, false);
        to = PickEmulatorSlot(emu, data, true);
    }
    const Platform& target = into ? kPlatforms[0] : emu;
    std::printf("\n");
    Transfer(from, to, target, data, true);
    std::printf("\nGo ahead? (y/n) ");
    std::fflush(stdout);
    const std::string a = Trim(ReadLine());
    if (a.empty() || (a[0] != 'y' && a[0] != 'Y')) throw Error("cancelled");
    return Transfer(from, to, target, data, false);
}

#ifdef _WIN32
std::vector<std::string> WideArgsAfter(const wchar_t* flag) {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    bool after = false;
    for (int i = 1; i < n; ++i) {
        if (!after) {
            after = std::wstring(w[i]) == flag;
            continue;
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(size_t(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, a.data(), len, nullptr, nullptr);
        out.push_back(a);
    }
    LocalFree(w);
    return out;
}
#endif

void Usage() {
    std::fprintf(stderr,
                 "usage: ldport --save-transfer [--data <data folder>]          (interactive)\n"
                 "       ldport --save-transfer --list [platform] [--data <dir>]\n"
                 "       ldport --save-transfer --detect <folder>\n"
                 "       ldport --save-transfer --from <folder> --to <folder> --format <platform> [--data <dir>] [--dry-run]\n"
                 "       ldport --save-transfer --from <folder> --to-port <SlotNN> --data <dir> [--dry-run]\n"
                 "platforms: port, shadps4, xenia (experimental), cemu (experimental), rpcs3 (experimental)\n");
}

}  // namespace

int SaveTransfer(const std::vector<std::string>& ansi_args) {
#ifdef _WIN32
    const std::vector<std::string> wide = WideArgsAfter(L"--save-transfer");
    const std::vector<std::string>& args = wide.empty() ? ansi_args : wide;
#else
    const std::vector<std::string>& args = ansi_args;
#endif
    std::string from, to, to_port, format, detect, data = "data", list;
    bool dry = false, listing = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has = i + 1 < args.size();
        if (a == "--from" && has) from = args[++i];
        else if (a == "--to" && has) to = args[++i];
        else if (a == "--to-port" && has) to_port = args[++i];
        else if (a == "--format" && has) format = args[++i];
        else if (a == "--detect" && has) detect = args[++i];
        else if (a == "--data" && has) data = args[++i];
        else if (a == "--dry-run") dry = true;
        else if (a == "--list") {
            listing = true;
            if (has && args[i + 1].rfind("--", 0) != 0) list = args[++i];
        } else {
            Usage();
            return 1;
        }
    }
    const fs::path data_dir = U8Path(data);
    try {
        if (!detect.empty()) {
            const Slot s = ReadSlot(ResolveSlot(U8Path(detect)));
            std::printf("%s: %s save, story block v%u, %zu DLC file(s)\n", U8(s.dir).c_str(), FormatName(s.format),
                        s.game.version, s.dlcs.size());
            return 0;
        }
        if (listing) {
            size_t total = 0;
            for (const Platform& p : kPlatforms) {
                if (!list.empty() && list != p.key) continue;
                for (const Found& f : ListPlatform(p.key, data_dir)) {
                    std::printf("%-8s %s  (saved %s)\n", p.key, U8(f.dir).c_str(), TimeText(f.time).c_str());
                    ++total;
                }
            }
            if (!total) std::printf("no LEGO Dimensions saves found in the default folders\n");
            return total ? 0 : 2;
        }
        if (!from.empty()) {
            if (!to_port.empty()) {
                to = U8(data_dir / "savedata" / U8Path(to_port));
                format = "port";
            }
            const Platform* p = FindPlatform(format);
            if (to.empty() || !p) {
                Usage();
                return 1;
            }
            return Transfer(U8Path(from), U8Path(to), *p, data_dir, dry);
        }
        if (!to.empty() || !format.empty()) {
            Usage();
            return 1;
        }
        return Interactive(data_dir);
    } catch (const Error& e) {
        std::fprintf(stderr, "[transfer] %s\n", e.what());
        return std::string(e.what()).rfind("no LEGO Dimensions save", 0) == 0 ? 2 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[transfer] failed: %s\n", e.what());
        return 1;
    }
}

}  // namespace host
