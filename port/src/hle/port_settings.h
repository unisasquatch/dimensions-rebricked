// The port's own settings (shown in the game's Options menu, see options_menu.cpp), saved to
// <data>/settings.ini. Read from any thread; changes take effect where each one is used.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace port {

enum class Aspect : uint32_t { Fit = 0, Stretch = 1, Zoom = 2 };
// Window: a normal window, a borderless window covering the display, or exclusive fullscreen at the
// display's desktop mode.
enum class DisplayMode : uint32_t { Windowed = 0, Borderless = 1, Fullscreen = 2 };

struct Settings {
    // Resolution mode: 0 automatic, 1 = 1080p, 2 = 4K, 3 = 6K (supersampled), 4-7 = FSR 1 Quality / Balanced /
    // Performance / Ultra Performance (rendered below the window's resolution, upscaled by FSR), 8-11 = the same
    // with FSR 2 (temporal: jittered frames, motion vectors and depth; it also anti-aliases), 12-15 = the same with
    // NVIDIA DLSS, 16 = DLAA (DLSS at the full resolution). DLSS modes use FSR 2 where DLSS can't run.
    // Shown as two rows: Resolution (below) and Upscaler (this: 0 off, 4-16 the FSR / DLSS modes; 1-3 are older
    // files' native resolutions, moved to `resolution` when loaded).
    std::atomic<uint32_t> scale{0};
    // The picture's height: 0 automatic (the display decides: 4K on a 2160p display, else 1080p), 1 = 1080p,
    // 2 = 1440p, 3 = 4K (2160p), 4 = 6K (3240p, supersampled). With the upscaler off the game renders at it; an
    // upscaler renders at it / its ratio and upscales to the display (at most the display's own height).
    std::atomic<uint32_t> resolution{0};
    std::atomic<uint32_t> aspect{0};         // Aspect
    std::atomic<uint32_t> display{0};        // DisplayMode
    std::atomic<uint32_t> aa{0};             // anti-aliasing: 0 off, 1 FXAA, 2 TAA (temporal)
    std::atomic<uint32_t> vsync{0};          // 0 fast (mailbox), 1 on (FIFO), 2 off (immediate, may tear)
    // Ultrawide: 0 off (16:9), 1 = 21:9, 2 = 32:9. The game builds its screen at startup, so a change takes
    // effect after a restart (BootUltrawide is the value the game started with).
    std::atomic<uint32_t> ultrawide{0};
    // Where the game's figures come from (ToyPadSource; hle/toypad_source.h). LegoToypad is harrysof's app,
    // which connects to 127.0.0.1:legotoypad_port; legotoypad_start launches it with the game.
    std::atomic<uint32_t> toypad{0};
    std::atomic<uint32_t> legotoypad_port{9191};
    std::atomic<uint32_t> legotoypad_start{1};
    // 1: the virtual Toy Pad web page (hle/toypad_web.h) serves on the local network while figures come from this
    // PC, so a phone or tablet can place them. LDPORT_TOYPAD_WEB=0/1 overrides it for one run.
    std::atomic<uint32_t> toypad_web{1};
    // Local co-op: 0 the keyboard and the first controller are both player 1; 1 the keyboard is player 1 on its
    // own and controllers are players 2-4.
    std::atomic<uint32_t> keyboard_player{0};
    // Local co-op: how many players can drop in (2-4). The game allows 2 except in some modes; 3 or 4 unlocks
    // its own 4-player split screen everywhere (loader/game_patches.cpp). Takes effect after a restart
    // (BootLocalPlayers is the value the game started with).
    std::atomic<uint32_t> local_players{2};
    // 1: every cutscene can be skipped with the game's own prompt and button, not only ones already watched
    // (loader/game_patches.cpp, SetSkipAnyCutscene). Off by default.
    std::atomic<uint32_t> skip_cutscenes{0};
    // Button icons (hle/button_icons.h): 0 follow the last device used, 1 PlayStation, 2 Xbox, 3 Xbox 360,
    // 4 Nintendo, 5 keyboard.
    std::atomic<uint32_t> button_icons{0};
    // Audio balance, in percent (0-400), on top of the game's own levels (hle/libscengs2.cpp): streams are the
    // music and dialogue, effects every sound effect (bricks, footsteps, menu clicks). The game mixes for 7.1; folded
    // down to stereo, effects it spreads over every speaker add up louder than the music and dialogue.
    // Defaults chosen by ear (Nov 9): music and dialogue +4 dB, effects -4.4 dB.
    std::atomic<uint32_t> streams_volume{160};
    std::atomic<uint32_t> effects_volume{60};
};

Settings& Get();
void Load();  // once, at startup
void Save();
// settings.ini's legotoypad_exe: the LegoToypad program (empty: <install>/LegoToypad/LegoToypad.exe).
std::string LegoToypadExe();

// Main thread: applies a window change asked for from elsewhere (the Options menu runs on a game thread).
void RequestDisplayMode(uint32_t mode);
bool TakeDisplayModeRequest(uint32_t& mode);
const char* DisplayModeName(uint32_t mode);

// Resolution modes: the FSR 1 upscale ratio (output / render height) of a mode, 0 when it is not an FSR mode.
float FsrRatio(uint32_t mode);
// Temporal upscaling modes (FSR 2 or DLSS): jittered frames, the camera's motion vectors and depth.
inline bool Fsr2Mode(uint32_t mode) { return mode >= 8 && mode <= 16; }
inline bool DlssMode(uint32_t mode) { return mode >= 12 && mode <= 16; }
constexpr uint32_t kLastResolutionMode = 16;
// Render scale (x the game's 1080p) for a mode, given the on-screen picture height for FSR modes.
float ScaleForMode(uint32_t mode, float output_height);
const char* ResolutionModeName(uint32_t mode);
// Resolution (Settings::resolution): its height (0 for automatic), name, and the native render scale (x 1080p)
// it gives with the upscaler off. AutoScale is what automatic chose at startup.
uint32_t ResolutionHeight(uint32_t resolution);
const char* ResolutionName(uint32_t resolution);
float NativeScale(uint32_t resolution);
void SetAutoScale(float scale);
float AutoScale();
// The game picture's height on screen, as the presenter last drew it (what upscalers output to).
void SetPictureHeight(uint32_t height);
uint32_t PictureHeight();
// The height an upscaler mode renders at for the current Resolution and picture.
uint32_t UpscalerRenderHeight(uint32_t mode);
constexpr uint32_t kLastResolution = 4;
// Upscaler modes in menu order: off, then the FSR / DLSS modes.
inline bool UpscalerMode(uint32_t mode) { return mode >= 4 && mode <= kLastResolutionMode; }
const char* UpscalerName(uint32_t mode);
const char* AntiAliasingName(uint32_t aa);
const char* VsyncName(uint32_t vsync);
const char* UltrawideName(uint32_t mode);
// The ultrawide mode this run started with (read once, at the game's startup patches).
uint32_t BootUltrawide();
// The game's screen width for a mode at its 1080 lines: 1920, 2560 (21:9) or 3840 (32:9).
uint32_t UltrawideWidth(uint32_t mode);
// The local player limit this run started with (2-4; LDPORT_MAX_PLAYERS overrides the setting).
uint32_t BootLocalPlayers();

}  // namespace port
