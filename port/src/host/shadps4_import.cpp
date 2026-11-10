#include "host/shadps4_import.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace host {
namespace {

// Title IDs of LEGO Dimensions on PS4. Other regions are recognised by their save contents instead (see IsSaveDir).
const std::set<std::string> kTitleIds = {"CUSA01176"};
// Files the game writes into every save directory (Slot00..Slot03).
const char* const kSaveFiles[] = {"GAME01", "V2GAME01", "OPTSC01", "FEOPTS01", "ZOPTS01"};

fs::path U8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string U8(const fs::path& p) {
    const std::u8string s = fs::path(p).make_preferred().u8string();
    return std::string(s.begin(), s.end());
}

// The few param.sfo keys we report: TITLE_ID, MAINTITLE, SUBTITLE, SAVEDATA_DIRECTORY.
std::map<std::string, std::string> ReadParamSfo(const fs::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0u; };
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? uint16_t(d[o] | d[o + 1] << 8) : uint16_t(0); };
    if (d.size() < 20 || u32(0) != 0x46535000) return out;  // "\0PSF"
    const uint32_t keys = u32(8), data = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= d.size(); ++i) {
        const size_t e = 20 + i * 16;
        const size_t ko = keys + u16(e), fmt = u16(e + 2), len = u32(e + 4), dof = data + u32(e + 12);
        if (ko >= d.size() || dof + len > d.size()) continue;
        std::string key;
        for (size_t k = ko; k < d.size() && d[k]; ++k) key += char(d[k]);
        if (fmt != 0x0204 && fmt != 0x0004) continue;  // utf-8 strings only
        std::string val(reinterpret_cast<const char*>(&d[dof]), len);
        val.erase(std::find(val.begin(), val.end(), '\0'), val.end());
        out[key] = val;
    }
    return out;
}

struct SaveDir {
    fs::path path;
    std::string title_id;  // from param.sfo, or the folder above when it looks like one
    std::string user;      // the folder above the title folder (shadPS4's user id)
    std::string subtitle;
    fs::file_time_type newest{};
};

bool HasSaveFile(const fs::path& dir) {
    std::error_code ec;
    for (const char* f : kSaveFiles)
        if (fs::is_regular_file(dir / f, ec)) return true;
    return false;
}

bool LooksLikeTitleId(const std::string& s) {
    return s.size() == 9 && std::all_of(s.begin(), s.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }) &&
           std::all_of(s.begin() + 4, s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// A LEGO Dimensions save directory: its param.sfo names the game, or it holds the files the game writes.
bool IsSaveDir(const fs::path& dir, SaveDir& out) {
    std::error_code ec;
    std::map<std::string, std::string> sfo;
    if (fs::is_regular_file(dir / "sce_sys" / "param.sfo", ec)) sfo = ReadParamSfo(dir / "sce_sys" / "param.sfo");
    const std::string title = sfo.count("TITLE_ID") ? sfo["TITLE_ID"] : "";
    const std::string name = sfo.count("MAINTITLE") ? sfo["MAINTITLE"] : "";
    const bool named = kTitleIds.count(title) || (name.find("LEGO") != std::string::npos && name.find("Dimensions") != std::string::npos);
    if (!named && !HasSaveFile(dir)) return false;
    // A save directory of another LEGO game that happens to share file names is still not ours.
    if (!named && !title.empty() && !kTitleIds.count(title) && !name.empty()) return false;
    out.path = dir;
    const std::string parent = U8(dir.parent_path().filename());
    out.title_id = !title.empty() ? title : LooksLikeTitleId(parent) ? parent : "";
    out.user = LooksLikeTitleId(parent) ? U8(dir.parent_path().parent_path().filename()) : "";
    out.subtitle = sfo.count("SUBTITLE") ? sfo["SUBTITLE"] : "";
    for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (e.is_regular_file(ec2)) out.newest = std::max(out.newest, e.last_write_time(ec2));
    }
    return true;
}

void Find(const fs::path& dir, int depth, std::vector<SaveDir>& found) {
    std::error_code ec;
    SaveDir s;
    if (IsSaveDir(dir, s)) {
        found.push_back(s);
        return;
    }
    if (depth == 0) return;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2) || e.is_symlink(ec2)) continue;
        const std::string n = U8(e.path().filename());
        if (n.rfind("sce_", 0) == 0 || n == "shader" || n == "game_data" || n == "log") continue;
        Find(e.path(), depth - 1, found);
    }
}

std::vector<fs::path> DefaultRoots() {
    std::vector<fs::path> roots;
    auto env = [](const char* n) -> std::string { const char* v = std::getenv(n); return v ? v : ""; };
#ifdef _WIN32
    if (!env("APPDATA").empty()) roots.push_back(U8Path(env("APPDATA")) / "shadPS4");
#else
    const std::string home = env("HOME");
    const std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) roots.push_back(fs::path(xdg) / "shadPS4");
    if (!home.empty()) {
        roots.push_back(fs::path(home) / ".local/share/shadPS4");
        roots.push_back(fs::path(home) / ".var/app/net.shadps4.shadPS4/data/shadPS4");  // Flatpak
        roots.push_back(fs::path(home) / "Library/Application Support/shadPS4");      // macOS
    }
#endif
    return roots;
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

// Copies a save directory, leaving out shadPS4's / the console's sce_* folders (sce_sys, sce_backup).
void CopySave(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    for (const auto& e : fs::directory_iterator(from)) {
        const std::string n = U8(e.path().filename());
        if (n.rfind("sce_", 0) == 0) continue;
        if (e.is_directory()) fs::copy(e.path(), to / e.path().filename(), fs::copy_options::recursive);
        else if (e.is_regular_file()) fs::copy_file(e.path(), to / e.path().filename());
    }
}

#ifdef _WIN32
// The arguments after --import-shadps4, in UTF-8 (argv is in the ANSI code page, which can't hold every path).
std::vector<std::string> WideArgsAfterFlag() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    bool after = false;
    for (int i = 1; i < n; ++i) {
        if (!after) {
            after = std::wstring(w[i]) == L"--import-shadps4";
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

}  // namespace

int ImportShadps4(const std::vector<std::string>& ansi_args) {
#ifdef _WIN32
    const std::vector<std::string> wide = WideArgsAfterFlag();
    const std::vector<std::string>& args = wide.empty() ? ansi_args : wide;
#else
    const std::vector<std::string>& args = ansi_args;
#endif
    std::string source, data = "./savedata", user;
    bool dry = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) data = args[++i];
        else if (args[i] == "--user" && i + 1 < args.size()) user = args[++i];
        else if (args[i] == "--dry-run") dry = true;
        else if (source.empty() && args[i].rfind("--", 0) != 0) source = args[i];
    }
    if (source.empty()) {
        std::fprintf(stderr,
                     "usage: ldport --import-shadps4 <shadPS4 folder | savedata folder | save folder | auto> --data <data folder>\n"
                     "                              [--user <shadPS4 user id>] [--dry-run]\n");
        return 1;
    }

    std::vector<fs::path> roots;
    if (source == "auto") roots = DefaultRoots();
    else roots.push_back(U8Path(source));
    std::vector<SaveDir> found;
    for (const fs::path& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) {
            if (source != "auto") {
                std::fprintf(stderr, "[import] %s is not a folder\n", U8(r).c_str());
                return 1;
            }
            continue;
        }
        std::printf("[import] looking in %s\n", U8(r).c_str());
        Find(r, 6, found);
    }
    if (!user.empty())
        found.erase(std::remove_if(found.begin(), found.end(), [&](const SaveDir& s) { return s.user != user; }), found.end());
    if (found.empty()) {
        std::fprintf(stderr, "[import] no LEGO Dimensions saves found%s\n", user.empty() ? "" : " for that user id");
        return 2;
    }

    // shadPS4 can hold several users (and regions); each save directory name is imported once, from the newest copy.
    std::map<std::string, SaveDir> pick;
    for (const SaveDir& s : found) {
        const std::string name = U8(s.path.filename());
        auto [it, inserted] = pick.try_emplace(name, s);
        if (!inserted && s.newest > it->second.newest) it->second = s;
    }
    for (const SaveDir& s : found) {
        const bool used = pick[U8(s.path.filename())].path == s.path;
        std::printf("[import] %s %s%s%s%s%s\n", used ? "found  " : "skipped", U8(s.path).c_str(),
                    s.title_id.empty() ? "" : "  title ", s.title_id.c_str(), s.user.empty() ? "" : "  user ", s.user.c_str());
        if (!used) std::printf("[import]           (a newer %s is imported; pick a user with --user <id>)\n", U8(s.path.filename()).c_str());
    }

    const fs::path data_dir = U8Path(data), saves = data_dir / "savedata";
    std::error_code ec;
    bool have_saves = false;
    for (const auto& e : fs::directory_iterator(saves, ec)) { (void)e; have_saves = true; break; }
    fs::path backup = data_dir / "savedata-backups" / Stamp();
    for (int n = 2; fs::exists(backup, ec); ++n) backup = data_dir / "savedata-backups" / (Stamp() + "-" + std::to_string(n));
    if (dry) {
        std::printf("[import] dry run: would %sreplace %zu save folder(s) in %s\n",
                    have_saves ? ("back up the current saves to " + U8(backup) + " and ").c_str() : "", pick.size(), U8(saves).c_str());
        return 0;
    }
    try {
        if (have_saves) {
            fs::create_directories(backup);
            fs::copy(saves, backup, fs::copy_options::recursive);
            std::printf("[import] backed up the current saves to %s\n", U8(backup).c_str());
        }
        // Copy into a staging folder outside savedata/ (the game lists every folder in there as a save), then swap.
        const fs::path stage = data_dir / "savedata-import.tmp";
        fs::remove_all(stage);
        for (const auto& [name, s] : pick) CopySave(s.path, stage / U8Path(name));
        fs::create_directories(saves);
        for (const auto& [name, s] : pick) {
            const fs::path to = saves / U8Path(name);
            fs::remove_all(to);
            fs::rename(stage / U8Path(name), to);
            std::printf("[import] imported %s\n", name.c_str());
        }
        fs::remove_all(stage);
    } catch (const fs::filesystem_error& e) {
        std::fprintf(stderr, "[import] failed: %s\n", e.what());
        if (have_saves) std::fprintf(stderr, "[import] the saves from before are in %s\n", U8(backup).c_str());
        return 1;
    }
    std::printf("[import] done: %zu save folder(s) in %s\n", pick.size(), U8(saves).c_str());
    return 0;
}

}  // namespace host
