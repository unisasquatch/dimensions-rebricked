// The Toy Pad source switch, and the listener LegoToypad talks to (see toypad_source.h).
//
// LegoToypad's protocol (the "Toypad listener" of its Cemu / RPCS3 / shadPS4 builds; documented in the app's
// main.cpp, https://github.com/harrysof/LegoToypad, MIT): the app connects to 127.0.0.1:<port>, usually once per
// action, and sends commands with a 5-byte header (command, pad 1 centre / 2 left / 3 right, index 0-6, source
// pad, source index):
//   1 LOAD    + the tag's 180 bytes (NTAG213, 45 pages) + u16 LE path length + UTF-8 path. A path names the tag's
//             file: the game's writes go there (when it exists), as the listener keeps it.
//   2 REMOVE  the figure at `index`
//   3 MOVE    the figure at (source pad, source index) to (pad, index)
//   4 GET_LED answered with 40 bytes: the lights as last commanded (VirtualToyPad::LedSnapshot)
// Indices are the app's: a figure keeps the one it was loaded under. A figure lifted and another placed straight
// after are 500 ms apart, as on a real pad (the game misses a placement that arrives with the removal).
#include "hle/toypad_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "hle/hle.h"
#include "hle/port_settings.h"
#include "hle/toypad.h"
#include "hle/toypad_web.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace toypad {
namespace {

using namespace std::chrono;
namespace fs = std::filesystem;

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void CloseSocket(Socket s) {
    shutdown(s, SHUT_RDWR);  // wakes a thread blocked in accept()
    close(s);
}
#endif

constexpr int kSlots = VirtualToyPad::kSlots;
constexpr auto kPickupDelay = milliseconds(500);

std::mutex g_mutex;                 // the listener's start / stop
Socket g_listen = kNoSocket;
std::atomic<uint32_t> g_generation{0};  // a stopped listener's thread sees a newer generation and leaves
std::atomic<int64_t> g_last_request_ms{0};
std::atomic<int> g_override{-1};  // LDPORT_TOYPAD_SOURCE, for this run only (settings.ini is read again later)

fs::path U8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string Utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

int64_t NowMs() { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }

bool RecvAll(Socket s, void* out, size_t n) {
    char* p = static_cast<char*>(out);
    while (n) {
        const int got = int(recv(s, p, int(std::min<size_t>(n, 1 << 16)), 0));
        if (got <= 0) return false;
        p += got;
        n -= size_t(got);
    }
    return true;
}

bool SendAll(Socket s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
        const int sent = int(send(s, p, int(n), 0));
        if (sent <= 0) return false;
        p += sent;
        n -= size_t(sent);
    }
    return true;
}

// A path as the app sent it, for this OS: under Wine its paths are Windows paths, with the Linux root as Z:.
fs::path HostPath(std::string p) {
#ifndef _WIN32
    if (p.size() > 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p.erase(0, 2);
    std::replace(p.begin(), p.end(), '\\', '/');
#endif
    return U8Path(p);
}

// Waits between a figure lifted and the next placed, when the game is watching (before its handshake, events
// aren't sent: it is told what is on the pad when the handshake completes).
void PickupDelay() {
    if (Instance().Ready()) std::this_thread::sleep_for(kPickupDelay);
}

// One command. False: the connection is done (closed, or a command we can't size, so the next header is lost).
bool HandleCommand(Socket s) {
    uint8_t h[5];
    if (!RecvAll(s, h, sizeof h)) return false;
    g_last_request_ms = NowMs();
    auto& pad = Instance();
    const uint8_t cmd = h[0], zone = h[1], index = h[2], from_zone = h[3], from_index = h[4];
    if (cmd == 4) {  // GET_LED
        uint8_t snap[VirtualToyPad::kLedSnapshotSize];
        pad.LedSnapshot(snap);
        return SendAll(s, snap, sizeof snap);
    }
    const bool where_ok = zone >= 1 && zone <= 3 && index < kSlots;
    if (cmd == 1) {  // LOAD
        TagData tag{};
        uint8_t len[2];
        if (!RecvAll(s, tag.data(), tag.size()) || !RecvAll(s, len, 2)) return false;
        std::string path(size_t(len[0] | len[1] << 8), '\0');
        if (!path.empty() && !RecvAll(s, path.data(), path.size())) return false;
        if (!where_ok) {
            std::fprintf(stderr, "[legotoypad] LOAD to pad %u index %u ignored\n", zone, index);
            return true;
        }
        std::string save;
        if (!path.empty()) {  // the file holds the figure's progress: it wins over the bytes sent
            TagData file{};
            const fs::path host = HostPath(path);
            if (std::ifstream in(host, std::ios::binary); in.read(reinterpret_cast<char*>(file.data()), file.size())) {
                tag = file;
                save = Utf8(host);
            }
        }
        if (pad.RemoveExternal(index)) PickupDelay();
        const int slot = pad.PlaceExternal(Zone(zone), index, tag, save);
        std::fprintf(stderr, "[legotoypad] LOAD pad %u index %u -> slot %d%s%s\n", zone, index, slot + 1,
                     save.empty() ? "" : ", tag file ", save.c_str());
        if (slot < 0) std::fprintf(stderr, "[legotoypad] the pad is full (7 figures)\n");
        return true;
    }
    if (cmd == 2) {  // REMOVE
        const bool removed = where_ok && pad.RemoveExternal(index);
        std::fprintf(stderr, "[legotoypad] REMOVE pad %u index %u%s\n", zone, index, removed ? "" : " (nothing there)");
        return true;
    }
    if (cmd == 3) {  // MOVE
        if (!where_ok || from_zone < 1 || from_zone > 3 || from_index >= kSlots) {
            std::fprintf(stderr, "[legotoypad] MOVE %u/%u -> %u/%u ignored\n", from_zone, from_index, zone, index);
            return true;
        }
        auto moved = pad.RemoveExternal(from_index);
        if (!moved) {
            std::fprintf(stderr, "[legotoypad] MOVE from index %u: nothing there\n", from_index);
            return true;
        }
        if (from_index != index) pad.RemoveExternal(index);  // whatever was at the destination comes off too
        PickupDelay();
        const int slot = pad.PlaceExternal(Zone(zone), index, moved->tag, moved->save_path);
        std::fprintf(stderr, "[legotoypad] MOVE %u/%u -> %u/%u (slot %d)\n", from_zone, from_index, zone, index, slot + 1);
        return true;
    }
    std::fprintf(stderr, "[legotoypad] unknown command %#04x: connection closed\n", cmd);
    return false;
}

void ListenLoop(Socket listener, uint32_t generation) {
    while (g_generation == generation) {
        const Socket c = accept(listener, nullptr, nullptr);
        if (c == kNoSocket) {
            if (g_generation != generation) break;
            std::this_thread::sleep_for(milliseconds(50));
            continue;
        }
#ifdef _WIN32
        const DWORD timeout = 2000;
#else
        const timeval timeout{2, 0};
#endif
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        while (g_generation == generation && HandleCommand(c)) {
        }
        CloseSocket(c);
    }
}

void StartListener() {
    std::lock_guard lock(g_mutex);
    if (g_listen != kNoSocket) return;
#ifdef _WIN32
    static const bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa;
#endif
    const uint16_t port = uint16_t(port::Get().legotoypad_port.load());
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only, like the emulators' listeners
#ifndef _WIN32
    const int yes = 1;  // restartable straight after a previous run
    if (s != kNoSocket) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    if (s == kNoSocket || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
        std::fprintf(stderr,
                     "[legotoypad] can't listen on 127.0.0.1:%u (is an emulator with a Toypad listener running?)\n",
                     unsigned(port));
        if (s != kNoSocket) CloseSocket(s);
        return;
    }
    g_listen = s;
    std::thread(ListenLoop, s, ++g_generation).detach();
    std::fprintf(stderr, "[legotoypad] listening on 127.0.0.1:%u for the LegoToypad app\n", unsigned(port));
}

void StopListener() {
    std::lock_guard lock(g_mutex);
    if (g_listen == kNoSocket) return;
    ++g_generation;
    CloseSocket(g_listen);  // accept() fails and the thread leaves
    g_listen = kNoSocket;
    std::fprintf(stderr, "[legotoypad] listener stopped\n");
}

// The LegoToypad program: settings.ini's legotoypad_exe (relative: to the install folder), else the installer's
// <install>/LegoToypad/LegoToypad.exe. The install folder is the one above the data folder.
fs::path LegoToypadPath() {
    const fs::path data = U8Path(hle::DataRoot().empty() ? std::string("run") : hle::DataRoot());
    const fs::path root = fs::absolute(data).parent_path();
    const std::string set = port::LegoToypadExe();
    if (!set.empty()) {
        fs::path p = HostPath(set);
        return p.is_absolute() ? p : root / p;
    }
    return root / "LegoToypad" / "LegoToypad.exe";
}

#ifdef _WIN32
bool Running(const fs::path& exe) {
    const std::wstring name = exe.filename().wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof e};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, name.c_str()) == 0;
    CloseHandle(snap);
    return found;
}
#else
bool Running(const fs::path& exe) {  // a process whose command line names the program (under Wine or not)
    const std::string name = exe.filename().string();
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (dirent* d = readdir(proc)) {
        if (found || d->d_name[0] < '0' || d->d_name[0] > '9') continue;
        std::ifstream f(std::string("/proc/") + d->d_name + "/cmdline", std::ios::binary);
        std::string cmd((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        found = cmd.find(name) != std::string::npos && cmd.find("ldport") == std::string::npos;
    }
    closedir(proc);
    return found;
}
#endif

Source FromEnv(const char* e, Source fallback) {
    if (!e) return fallback;
    const std::string v = e;
    if (v == "pc" || v == "0") return Source::kThisPc;
    if (v == "physical" || v == "1") return Source::kPhysical;
    if (v == "legotoypad" || v == "2") return Source::kLegoToypad;
    return fallback;
}

void Apply(Source s) {
    if (s == Source::kLegoToypad) {
        StartListener();
        if (port::Get().legotoypad_start.load()) LaunchLegoToypad();
    } else {
        StopListener();
    }
}

}  // namespace

Source CurrentSource() {
    const int o = g_override.load();
    return Source(o >= 0 ? uint32_t(o) : std::min<uint32_t>(port::Get().toypad.load(), 2));
}

const char* SourceName(Source s) {
    switch (s) {
        case Source::kPhysical: return "Physical Toy Pad";
        case Source::kLegoToypad: return "LegoToypad app";
        default: return "This PC";
    }
}

bool WebWanted() {
    if (const char* e = std::getenv("LDPORT_TOYPAD_WEB")) return std::strcmp(e, "0") != 0;  // this run only
    return port::Get().toypad_web.load() != 0;
}

void SetWebWanted(bool on) {
    port::Get().toypad_web = on ? 1u : 0u;
    if (on && CurrentSource() == Source::kThisPc && !WebRunning()) StartWeb();
    if (!on && !std::getenv("LDPORT_DEBUG_HTTP")) StopWeb();  // the debug remote shares the server
}

void StartSource() {
    if (const char* e = std::getenv("LDPORT_TOYPAD_SOURCE")) g_override = int(FromEnv(e, CurrentSource()));
    const Source s = CurrentSource();
    std::fprintf(stderr, "[toypad] figures from: %s\n", SourceName(s));
    if (s == Source::kThisPc && WebWanted()) StartWeb();
    Apply(s);
}

void SetSource(Source s) {
    const Source old = CurrentSource();
    if (s == old) return;
    g_override = -1;
    port::Get().toypad = uint32_t(s);
    port::Save();
    std::fprintf(stderr, "[toypad] figures from: %s\n", SourceName(s));
    Instance().RemoveAllVirtual();  // the old source's figures leave the pad
    if (s == Source::kThisPc && WebWanted() && !WebRunning()) StartWeb();
    Apply(s);
}

bool LegoToypadConnected() {
    const int64_t last = g_last_request_ms.load();
    return last && NowMs() - last < 3000;
}

bool LaunchLegoToypad() {
    const fs::path exe = LegoToypadPath();
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        std::fprintf(stderr, "[legotoypad] not started: %s not found (download it from "
                             "https://github.com/harrysof/LegoToypad/releases)\n", Utf8(exe).c_str());
        return false;
    }
    if (Running(exe)) return true;
#ifdef _WIN32
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    const std::wstring dir = exe.parent_path().wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, dir.c_str(), &si,
                        &pi)) {
        std::fprintf(stderr, "[legotoypad] could not start %s (error %lu)\n", Utf8(exe).c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    // A Windows program: Wine runs it (Proton users can add it to Steam instead and leave legotoypad_start=0).
    const bool windows_exe = exe.extension() == ".exe" || exe.extension() == ".EXE";
    if (windows_exe && std::system("command -v wine >/dev/null 2>&1") != 0) {
        std::fprintf(stderr, "[legotoypad] not started: it needs Wine (or add it to Steam as a non-Steam game, "
                             "with Proton)\n");
        return false;
    }
    const std::string path = exe.string(), dir = exe.parent_path().string();
    const pid_t pid = fork();
    if (pid == 0) {  // forks again and leaves, so the app is nobody's child (no zombie when it closes)
        setsid();
        if (fork() != 0) _exit(0);
        if (chdir(dir.c_str()) != 0) _exit(127);
        signal(SIGCHLD, SIG_DFL);
        if (windows_exe) execlp("wine", "wine", path.c_str(), static_cast<char*>(nullptr));
        else execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
    if (pid < 0) {
        std::fprintf(stderr, "[legotoypad] could not start %s\n", path.c_str());
        return false;
    }
#endif
    std::fprintf(stderr, "[legotoypad] started %s\n", Utf8(exe).c_str());
    return true;
}

}  // namespace toypad
