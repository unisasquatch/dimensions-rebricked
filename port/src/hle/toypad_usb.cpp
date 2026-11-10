#include "hle/toypad_usb.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#pragma comment(lib, "setupapi.lib")
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#endif

namespace toypad {
namespace {

#ifdef _WIN32
using Handle = HANDLE;
const Handle kNoHandle = INVALID_HANDLE_VALUE;
void CloseH(Handle h) { CloseHandle(h); }
unsigned long LastError() { return GetLastError(); }

// GUID_DEVINTERFACE_HID
const GUID kHidInterface = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

std::string FindPadPath() {
    HDEVINFO set = SetupDiGetClassDevsW(&kHidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::string found;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &kHidInterface, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::string buf(need, '\0');
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, need, nullptr, nullptr)) continue;
        std::string path = detail->DevicePath;
        std::string lower = path;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("vid_0e6f&pid_0241") != std::string::npos) found = path;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
Handle OpenPad(const std::string& path) {
    return CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}
// Reads one input report into out32 (without the report ID byte): its length, 0 for nothing, < 0 when gone.
int ReadReport(Handle h, uint8_t* out32) {
    uint8_t buf[33];
    DWORD n = 0;
    if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) return -1;
    if (n < 2) return 0;
    std::memcpy(out32, buf + 1, std::min<size_t>(32, n - 1));  // byte 0 is the HID report ID
    return int(n - 1);
}
bool WriteReport(Handle h, const uint8_t* packet) {
    uint8_t buf[33] = {0};  // report ID 0, then the packet
    std::memcpy(buf + 1, packet, 32);
    DWORD n = 0;
    return WriteFile(h, buf, sizeof buf, &n, nullptr) != 0;
}
#else
// Linux: /dev/hidrawN (needs read/write access: the Steam Deck package installs a udev rule for the pad).
using Handle = int;
const Handle kNoHandle = -1;
void CloseH(Handle h) { close(h); }
unsigned long LastError() { return (unsigned long)errno; }

std::string FindPadPath() {
    DIR* dir = opendir("/sys/class/hidraw");
    if (!dir) return {};
    std::string found;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "hidraw", 6) != 0) continue;
        std::ifstream uevent(std::string("/sys/class/hidraw/") + e->d_name + "/device/uevent");
        for (std::string line; std::getline(uevent, line);) {
            for (char& c : line) c = char(std::toupper(static_cast<unsigned char>(c)));
            if (line.rfind("HID_ID=", 0) == 0 && line.find(":00000E6F:00000241") != std::string::npos) {
                found = std::string("/dev/") + e->d_name;
                break;
            }
        }
        if (!found.empty()) break;
    }
    closedir(dir);
    return found;
}
Handle OpenPad(const std::string& path) { return open(path.c_str(), O_RDWR | O_CLOEXEC); }
int ReadReport(Handle h, uint8_t* out32) {
    uint8_t buf[64];
    const ssize_t n = read(h, buf, sizeof buf);  // no report IDs on this device: the packet as is
    if (n < 0 && errno == EINTR) return 0;
    if (n <= 0) return -1;
    std::memcpy(out32, buf, std::min<size_t>(32, size_t(n)));
    return int(n);
}
bool WriteReport(Handle h, const uint8_t* packet) {
    uint8_t buf[33] = {0};  // report number 0, then the packet
    std::memcpy(buf + 1, packet, 32);
    return write(h, buf, sizeof buf) == ssize_t(sizeof buf);
}
#endif

struct State {
    std::mutex mutex;
    Handle handle = kNoHandle;
    std::deque<std::array<uint8_t, 32>> incoming;
    std::atomic<bool> connected{false};
    std::chrono::steady_clock::time_point next_try{};
};
State g;

void ReaderLoop(Handle h) {
    for (;;) {
        std::array<uint8_t, 32> p{};
        const int n = ReadReport(h, p.data());
        if (n < 0) {
            std::fprintf(stderr, "[toypad] physical Toy Pad disconnected (error %lu)\n", LastError());
            std::lock_guard lock(g.mutex);
            g.connected = false;
            CloseH(h);
            if (g.handle != kNoHandle) {
                CloseH(g.handle);
                g.handle = kNoHandle;
            }
            return;
        }
        if (n < 1) continue;
        std::lock_guard lock(g.mutex);
        if (g.incoming.size() < 256) g.incoming.push_back(p);
    }
}

bool TryOpen() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g.next_try) return false;
    g.next_try = now + std::chrono::seconds(3);
    const std::string path = FindPadPath();
    if (path.empty()) return false;
    // Two handles: a synchronous handle serialises its I/O, so a blocking read would hold up every write.
    Handle w = OpenPad(path), r = OpenPad(path);
    if (w == kNoHandle || r == kNoHandle) {
        static bool told = false;
        if (!told)
            std::fprintf(stderr, "[toypad] physical Toy Pad found at %s but can't be opened (error %lu)\n", path.c_str(),
                         LastError());
        told = true;
        if (w != kNoHandle) CloseH(w);
        if (r != kNoHandle) CloseH(r);
        return false;
    }
    g.handle = w;
    g.incoming.clear();
    g.connected = true;
    std::thread(ReaderLoop, r).detach();
    std::fprintf(stderr, "[toypad] physical Toy Pad connected: the game talks to it directly\n");
    return true;
}

}  // namespace

RealToyPad* RealToyPad::Get() {
    static const bool virtual_only = [] {
        const char* e = std::getenv("LDPORT_TOYPAD");
        return e && std::strcmp(e, "virtual") == 0;
    }();
    if (virtual_only) return nullptr;
    static RealToyPad pad;
    std::lock_guard lock(g.mutex);
    if (g.connected || TryOpen()) return &pad;
    return nullptr;
}

bool RealToyPad::Write(const uint8_t* packet) {
    Handle h;
    {
        std::lock_guard lock(g.mutex);
        h = g.handle;
    }
    if (h == kNoHandle) return false;
    return WriteReport(h, packet);
}

std::optional<std::array<uint8_t, 32>> RealToyPad::Read() {
    std::lock_guard lock(g.mutex);
    if (g.incoming.empty()) return std::nullopt;
    auto p = g.incoming.front();
    g.incoming.pop_front();
    return p;
}

bool RealToyPad::Connected() const { return g.connected; }

}  // namespace toypad
