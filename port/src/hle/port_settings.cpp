#include "hle/port_settings.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>

#include "hle/hle.h"

namespace port {
namespace {

std::string Path() {
    const std::string& root = hle::DataRoot();
    return (root.empty() ? std::string("run") : root) + "/settings.ini";
}

std::atomic<int> g_display_request{-1};  // -1 none, else a DisplayMode
std::mutex g_exe_mutex;  // Load runs again when the window opens
std::string g_legotoypad_exe;

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    std::ifstream in(Path());
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const long v = std::strtol(line.c_str() + eq + 1, nullptr, 10);
        if (key == "scale" && v >= 0 && v <= long(kLastResolutionMode)) {
            // Older files kept native resolutions here (1 = 1080p, 2 = 4K, 3 = 6K): now Resolution, upscaler off.
            if (v >= 1 && v <= 3) Get().resolution = v == 1 ? 1u : v == 2 ? 3u : 4u, Get().scale = 0;
            else Get().scale = uint32_t(v);
        }
        else if (key == "resolution" && v >= 0 && v <= long(kLastResolution)) Get().resolution = uint32_t(v);
        else if (key == "aspect" && v >= 0 && v <= 2) Get().aspect = uint32_t(v);
        else if (key == "display" && v >= 0 && v <= 2) Get().display = uint32_t(v);
        else if (key == "aa" && v >= 0 && v <= 2) Get().aa = uint32_t(v);
        else if (key == "vsync" && v >= 0 && v <= 2) Get().vsync = uint32_t(v);
        else if (key == "ultrawide" && v >= 0 && v <= 2) Get().ultrawide = uint32_t(v);
        else if (key == "fullscreen" && v) Get().display = uint32_t(DisplayMode::Borderless);  // older files
        else if (key == "toypad" && v >= 0 && v <= 2) Get().toypad = uint32_t(v);
        else if (key == "legotoypad_port" && v > 0 && v <= 65535) Get().legotoypad_port = uint32_t(v);
        else if (key == "legotoypad_start" && v >= 0 && v <= 1) Get().legotoypad_start = uint32_t(v);
        else if (key == "skip_cutscenes" && v >= 0 && v <= 1) Get().skip_cutscenes = uint32_t(v);
        else if (key == "keyboard_player" && v >= 0 && v <= 1) Get().keyboard_player = uint32_t(v);
        else if (key == "local_players" && v >= 2 && v <= 4) Get().local_players = uint32_t(v);
        else if (key == "button_icons" && v >= 0 && v <= 5) Get().button_icons = uint32_t(v);
        else if (key == "toypad_web" && v >= 0 && v <= 1) Get().toypad_web = uint32_t(v);
        else if (key == "streams_volume" && v >= 0 && v <= 400) Get().streams_volume = uint32_t(v);
        else if (key == "effects_volume" && v >= 0 && v <= 400) Get().effects_volume = uint32_t(v);
        else if (key == "legotoypad_exe") {  // a path: the whole rest of the line
            std::string path = line.substr(eq + 1);
            while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
            std::lock_guard lock(g_exe_mutex);
            g_legotoypad_exe = path;
        }
    }
}

void Save() {
    std::ofstream out(Path());
    out << "# LEGO Dimensions port settings (also in the game's Options menu)\n"
        << "resolution=" << Get().resolution.load() << "  # 0 automatic, 1 1080p, 2 1440p, 3 4K, 4 6K\n"
        << "scale=" << Get().scale.load() << "  # upscaler: 0 off, 4-7 FSR 1, 8-11 FSR 2, 12-15 DLSS, 16 DLAA\n"
        << "aspect=" << Get().aspect.load() << "\n"
        << "display=" << Get().display.load() << "  # 0 windowed, 1 borderless, 2 fullscreen\n"
        << "aa=" << Get().aa.load() << "  # 0 off, 1 FXAA, 2 TAA\n"
        << "vsync=" << Get().vsync.load() << "  # 0 fast (mailbox), 1 on, 2 off\n"
        << "ultrawide=" << Get().ultrawide.load() << "  # 0 off, 1 21:9, 2 32:9 (after a restart)\n"
        << "toypad=" << Get().toypad.load() << "  # 0 this PC's virtual pad (a physical one joins in), 1 physical only, 2 LegoToypad\n"
        << "legotoypad_port=" << Get().legotoypad_port.load() << "  # LegoToypad.ini [Listener] Port\n"
        << "legotoypad_start=" << Get().legotoypad_start.load() << "  # 1 starts LegoToypad with the game\n"
        << "legotoypad_exe=" << LegoToypadExe() << "\n"
        << "toypad_web=" << Get().toypad_web.load() << "  # 1 serves the virtual Toy Pad page on the local network\n"
        << "skip_cutscenes=" << Get().skip_cutscenes.load() << "  # 1 lets every cutscene be skipped\n"
        << "keyboard_player=" << Get().keyboard_player.load() << "  # 1 keyboard is player 1 alone, controllers are players 2-4\n"
        << "local_players=" << Get().local_players.load() << "  # 2-4 players can drop in (after a restart)\n"
        << "button_icons=" << Get().button_icons.load()
        << "  # 0 follow the device in use, 1 PlayStation, 2 Xbox, 3 Xbox 360, 4 Nintendo, 5 keyboard\n"
        << "streams_volume=" << Get().streams_volume.load() << "  # music and dialogue, percent (0-400)\n"
        << "effects_volume=" << Get().effects_volume.load() << "  # sound effects, percent (0-400)\n";
}

std::string LegoToypadExe() {
    std::lock_guard lock(g_exe_mutex);
    return g_legotoypad_exe;
}

void RequestDisplayMode(uint32_t mode) { g_display_request = int(mode); }

bool TakeDisplayModeRequest(uint32_t& mode) {
    const int r = g_display_request.exchange(-1);
    if (r < 0) return false;
    mode = uint32_t(r);
    return true;
}

float FsrRatio(uint32_t mode) {
    switch (mode) {
        case 4: case 8: case 12: return 1.5f;
        case 5: case 9: return 1.7f;
        case 13: return 1.72f;  // DLSS Balanced
        case 6: case 10: case 14: return 2.0f;
        case 7: case 11: case 15: return 3.0f;
        case 16: return 1.0f;  // DLAA
        default: return 0.0f;
    }
}

float ScaleForMode(uint32_t mode, float output_height) {
    if (const float r = FsrRatio(mode); r > 0.0f) {
        // Render height = picture height / ratio in whole lines (2160 / 1.5 = 1440), as a scale of 1080.
        return std::max(0.25f, std::round(output_height / r) / 1080.0f);
    }
    return mode >= 1 && mode <= 3 ? float(mode) : 1.0f;
}

uint32_t ResolutionHeight(uint32_t resolution) {
    static constexpr uint32_t kHeights[] = {0, 1080, 1440, 2160, 3240};
    return resolution <= kLastResolution ? kHeights[resolution] : 0;
}

const char* ResolutionName(uint32_t resolution) {
    static constexpr const char* kNames[] = {"Automatic", "1080p", "1440p", "4K (2160p)", "6K (supersampled)"};
    return resolution <= kLastResolution ? kNames[resolution] : "Automatic";
}

namespace {
std::atomic<float> g_auto_scale{1.0f};
std::atomic<uint32_t> g_picture_height{1080};
}
void SetAutoScale(float scale) { g_auto_scale = scale; }
float AutoScale() { return g_auto_scale.load(); }
void SetPictureHeight(uint32_t height) { if (height) g_picture_height = height; }
uint32_t PictureHeight() { return g_picture_height.load(); }

uint32_t UpscalerRenderHeight(uint32_t mode) {
    const uint32_t target = ResolutionHeight(Get().resolution.load()), pic = PictureHeight();
    const float out_h = float(target ? std::min(target, pic) : pic);
    return uint32_t(std::lround(ScaleForMode(mode, out_h) * 1080.0f));
}

float NativeScale(uint32_t resolution) {
    const uint32_t h = ResolutionHeight(resolution);
    return h ? float(h) / 1080.0f : AutoScale();
}

const char* UpscalerName(uint32_t mode) { return UpscalerMode(mode) ? ResolutionModeName(mode) : "Off"; }

const char* ResolutionModeName(uint32_t mode) {
    switch (mode) {
        case 2: return "4K (2160p)";
        case 3: return "6K (supersampled)";
        case 4: return "FSR 1 Quality";
        case 5: return "FSR 1 Balanced";
        case 6: return "FSR 1 Performance";
        case 7: return "FSR 1 Ultra Performance";
        case 8: return "FSR 2 Quality";
        case 9: return "FSR 2 Balanced";
        case 10: return "FSR 2 Performance";
        case 11: return "FSR 2 Ultra Performance";
        case 12: return "DLSS Quality";
        case 13: return "DLSS Balanced";
        case 14: return "DLSS Performance";
        case 15: return "DLSS Ultra Performance";
        case 16: return "DLAA";
        default: return "1080p";
    }
}

const char* AntiAliasingName(uint32_t aa) { return aa == 2 ? "TAA" : aa == 1 ? "FXAA" : "Off"; }

const char* UltrawideName(uint32_t mode) { return mode == 1 ? "21:9" : mode == 2 ? "32:9" : "Off"; }

uint32_t BootUltrawide() {
    static const uint32_t boot = Get().ultrawide.load();
    return boot;
}

uint32_t BootLocalPlayers() {
    static const uint32_t boot = [] {
        if (const char* env = std::getenv("LDPORT_MAX_PLAYERS")) {
            const long n = std::strtol(env, nullptr, 10);
            if (n >= 2 && n <= 4) return uint32_t(n);
        }
        return std::clamp(Get().local_players.load(), 2u, 4u);
    }();
    return boot;
}

uint32_t UltrawideWidth(uint32_t mode) { return mode == 1 ? 2560 : mode == 2 ? 3840 : 1920; }

const char* VsyncName(uint32_t vsync) { return vsync == 1 ? "On" : vsync == 2 ? "Off" : "Fast"; }

const char* DisplayModeName(uint32_t mode) {
    return mode == 2 ? "Fullscreen" : mode == 1 ? "Borderless" : "Windowed";
}

}  // namespace port
