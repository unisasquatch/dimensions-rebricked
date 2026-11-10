#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

#include "hle/hle.h"
#include "host/memory.h"
#include "loader/nid.h"

namespace hle {
namespace {

std::map<std::string, const void*>& Table() {
    static std::map<std::string, const void*> table;  // "library/nid" -> address
    return table;
}

struct ConstantStub {
    const char* library;
    const char* name;
    int64_t ret;
    std::atomic<bool> logged{false};
};
std::deque<ConstantStub>& Constants() {
    static std::deque<ConstantStub> stubs;  // deque: addresses stay stable as it grows
    return stubs;
}

extern "C" SYSV int64_t ConstantStubHandler(uint32_t index) {
    ConstantStub& s = Constants()[index];
    if (!s.logged.exchange(true))
        std::fprintf(stderr, "[offline] %s::%s -> %#llx\n", s.library, s.name, static_cast<long long>(s.ret));
    return s.ret;
}

// Emits `mov edi, index; mov rax, ConstantStubHandler; jmp rax`.
const void* EmitConstantStub(uint32_t index) {
    constexpr size_t kArena = 0x40000, kStub = 32;
    static uint8_t* arena = nullptr;
    static size_t used = 0;
    if (!arena) {
        arena = host::AllocateImage(kArena);
        host::SetProtection(arena, kArena, host::kRead | host::kWrite | host::kExec);
    }
    if (used + kStub > kArena) Fatal("constant stub arena full");
    uint8_t* p = arena + used;
    used += kStub;
    p[0] = 0xBF;
    std::memcpy(p + 1, &index, 4);
    p[5] = 0x48;
    p[6] = 0xB8;
    const uint64_t handler = reinterpret_cast<uint64_t>(&ConstantStubHandler);
    std::memcpy(p + 7, &handler, 8);
    p[15] = 0xFF;
    p[16] = 0xE0;
    return p;
}

std::mutex g_mount_mutex;
// Guest prefix -> host directories, top layer first (e.g. /app0 = patch over base game).
std::map<std::string, std::vector<std::string>>& Mounts() {
    static std::map<std::string, std::vector<std::string>> mounts;
    return mounts;
}
std::string g_data;

}  // namespace

void Register(const char* library, const char* name, const void* address) {
    Table()[std::string(library) + "/" + loader::ComputeNid(name)] = address;
}

void RegisterConstant(const char* library, const char* name, int64_t ret) {
    auto& stubs = Constants();
    stubs.emplace_back();
    stubs.back().library = library;
    stubs.back().name = name;
    stubs.back().ret = ret;
    Register(library, name, EmitConstantStub(uint32_t(stubs.size() - 1)));
}

const void* Lookup(const std::string& library, const std::string& nid) {
    auto it = Table().find(library + "/" + nid);
    return it == Table().end() ? nullptr : it->second;
}

void SetPathRoots(const std::vector<std::string>& app0_layers, const std::string& data) {
    g_data = data;
    std::lock_guard lock(g_mount_mutex);
    Mounts()["/app0"] = app0_layers;
    Mounts()["/hostapp"] = app0_layers;
    Mounts()["/data"] = {data};
    Mounts()["/download0"] = {data + "/download0"};
}

void Mount(const std::string& guest_prefix, const std::string& host_dir) {
    std::error_code ec;
    std::filesystem::create_directories(host_dir, ec);
    std::lock_guard lock(g_mount_mutex);
    Mounts()[guest_prefix] = {host_dir};
}

void Unmount(const std::string& guest_prefix) {
    std::lock_guard lock(g_mount_mutex);
    Mounts().erase(guest_prefix);
}

const std::string& DataRoot() { return g_data; }

#ifndef _WIN32
namespace {
// Linux file names are case-sensitive and the game's are not (its data was made for Windows tools and a
// case-insensitive console file system): a path that does not exist as written is matched name by name
// ignoring case, against cached directory listings.
std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::mutex g_case_mutex;
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_dir_cache;  // dir -> lower -> name

std::string MatchCase(const std::string& path) {
    std::error_code ec;
    if (path.empty() || std::filesystem::exists(path, ec)) return path;
    std::string out;
    size_t at = 0;
    if (path[0] == '/') {
        out = "";
        at = 1;
    }
    std::lock_guard lock(g_case_mutex);
    while (at <= path.size()) {
        size_t next = path.find('/', at);
        if (next == std::string::npos) next = path.size();
        const std::string part = path.substr(at, next - at);
        at = next + 1;
        if (part.empty()) continue;
        const std::string dir = out.empty() ? "/" : out;
        std::string candidate = out + "/" + part;
        if (!std::filesystem::exists(candidate, ec)) {
            auto [it, inserted] = g_dir_cache.try_emplace(dir);
            if (inserted)
                for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                    it->second[Lower(e.path().filename().string())] = e.path().filename().string();
            auto found = it->second.find(Lower(part));
            if (found == it->second.end()) return path;  // really missing: let the caller create / fail on it
            candidate = out + "/" + found->second;
        }
        out = candidate;
    }
    return out;
}
}  // namespace
#endif

std::vector<std::string> HostPathLayers(const char* guest_path) {
    const std::string p = guest_path;
    std::lock_guard lock(g_mount_mutex);
    const std::pair<const std::string, std::vector<std::string>>* best = nullptr;
    for (const auto& m : Mounts()) {
        const bool match = p.rfind(m.first, 0) == 0 && (p.size() == m.first.size() || p[m.first.size()] == '/');
        if (match && (!best || m.first.size() > best->first.size())) best = &m;
    }
    if (!best) {
        std::fprintf(stderr, "[path] unmapped guest path %s\n", guest_path);
        return {g_data + "/unmapped" + p};
    }
    std::vector<std::string> out;
    for (const auto& layer : best->second) out.push_back(layer + p.substr(best->first.size()));
#ifndef _WIN32
    for (auto& path : out) path = MatchCase(path);
#endif
    return out;
}

std::string HostPath(const char* guest_path) {
    const auto layers = HostPathLayers(guest_path);
    std::error_code ec;
    for (const auto& path : layers)
        if (std::filesystem::exists(path, ec)) return path;
    return layers.front();  // new files go to the top layer
}

void Fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[fatal] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace hle
