// The port's settings as items (see settings_items.h).
#include "ui/settings_items.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gpu/vk/renderer.h"
#include "gpu/vk/dlss.h"
#include "hle/button_icons.h"
#include "hle/options_menu.h"
#include "hle/toypad_web.h"
#include "hle/port_settings.h"
#include "hle/toypad_source.h"
#include "loader/game_patches.h"

namespace ui::settings {
namespace {

port::Settings& S() { return port::Get(); }

uint32_t Cycle(uint32_t value, int step, uint32_t first, uint32_t last) {
    const int n = int(last - first + 1);
    return first + uint32_t(((int(value - first) + step) % n + n) % n);
}

// Resolution first (the picture's height), then the upscaler that reaches it (port_settings.h).
std::string ResolutionValue() {
    const uint32_t r = S().resolution.load();
    if (r) return port::ResolutionName(r);
    return std::string("Automatic (") + (port::AutoScale() >= 2.0f ? "4K" : "1080p") + ")";
}
void ResolutionChange(int step) {
    S().resolution = Cycle(S().resolution.load(), step, 0, port::kLastResolution);
    if (!port::UpscalerMode(S().scale.load())) vkr::SetRenderScale(port::NativeScale(S().resolution.load()));
}

// Upscaler: off, FSR 1 / FSR 2 / DLSS (Quality .. Ultra Performance), DLAA. In menu order: 0, 4, 5, ... 16.
std::string UpscalerValue() {
    const uint32_t mode = S().scale.load();
    if (!port::UpscalerMode(mode)) return "Off";
    std::string v = port::UpscalerName(mode);
    v += " (renders " + std::to_string(port::UpscalerRenderHeight(mode)) + "p)";
    if (port::DlssMode(mode) && vkr::DlssTried() && !vkr::dlss::Available()) v += " - no DLSS here, FSR 2";
    return v;
}
void UpscalerChange(int step) {
    const uint32_t mode = port::UpscalerMode(S().scale.load()) ? S().scale.load() : 3u;  // 3: "off" in the cycle
    const uint32_t next = Cycle(mode, step, 3, port::kLastResolutionMode);
    S().scale = next == 3 ? 0u : next;
    if (!port::UpscalerMode(S().scale.load())) vkr::SetRenderScale(port::NativeScale(S().resolution.load()));
    // Upscaling: the presenter sets the render scale from the resolution and the upscaler's ratio.
}

const char* const kAspect[] = {"16:9", "Stretch", "Zoom"};
std::string AspectValue() { return kAspect[S().aspect.load() % 3]; }
void AspectChange(int step) { S().aspect = Cycle(S().aspect.load(), step, 0, 2); }

std::string DisplayValue() { return port::DisplayModeName(S().display.load()); }
void DisplayChange(int step) {
    S().display = Cycle(S().display.load(), step, 0, 2);
    port::RequestDisplayMode(S().display.load());
}

std::string VsyncValue() { return port::VsyncName(S().vsync.load()); }
void VsyncChange(int step) { S().vsync = Cycle(S().vsync.load(), step, 0, 2); }  // the presenter rebuilds its swapchain

std::string AaValue() {
    const uint32_t m = S().scale.load();
    if (port::DlssMode(m) && !(vkr::DlssTried() && !vkr::dlss::Available())) return m == 16 ? "DLAA" : "DLSS";
    return port::Fsr2Mode(m) ? "FSR 2" : port::AntiAliasingName(S().aa.load());
}
void AaChange(int step) { S().aa = Cycle(S().aa.load(), step, 0, 2); }

std::string UltrawideValue() {
    std::string v = port::UltrawideName(S().ultrawide.load());
    if (S().ultrawide.load() != port::BootUltrawide()) v += " (after a restart)";
    return v;
}
void UltrawideChange(int step) { S().ultrawide = Cycle(S().ultrawide.load(), step, 0, 2); }

std::string ToyPadValue() {
    const toypad::Source src = toypad::CurrentSource();
    std::string v = toypad::SourceName(src);
    if (src == toypad::Source::kLegoToypad) v += toypad::LegoToypadConnected() ? " (connected)" : " (waiting)";
    return v;
}
void ToyPadChange(int step) {
    toypad::SetSource(toypad::Source(Cycle(uint32_t(toypad::CurrentSource()), step, 0, 2)));  // saved, at once
}

std::string IconsValue() {
    const uint32_t mode = S().button_icons.load();
    if (mode) return icons::SetName(int(mode) - 1);
    return std::string("Automatic (") + icons::SetName(icons::Wanted()) + ")";
}
void IconsChange(int step) { S().button_icons = Cycle(S().button_icons.load(), step, 0, 5); }  // next frame

std::string PlayersValue() {
    std::string v = std::to_string(S().local_players.load()) + " players";
    if (S().local_players.load() != port::BootLocalPlayers()) v += " (after a restart)";
    return v;
}
void PlayersChange(int step) { S().local_players = Cycle(S().local_players.load(), step, 2, 4); }  // at startup

std::string KeyboardValue() { return S().keyboard_player.load() ? "Player 1 only" : "Player 1 + 1st controller"; }
void KeyboardChange(int) { S().keyboard_player = S().keyboard_player.load() ? 0u : 1u; }  // pads re-mapped

std::string BrightnessValue() {
    const int v = port::GameBrightness();
    return v < 0 ? std::string("-") : std::to_string(v);
}
void BrightnessChange(int step) { port::ChangeGameBrightness(step); }

std::string MotionBlurValue() {
    const int v = port::GameMotionBlur();
    return v < 0 ? std::string("-") : v ? "On" : "Off";
}
void MotionBlurChange(int) { port::ToggleGameMotionBlur(); }

std::string LanValue() { return toypad::WebWanted() ? "On" : "Off"; }
void LanChange(int) { toypad::SetWebWanted(!toypad::WebWanted()); }

// Shown under LAN Toy Pad while it is on: where to point a phone or tablet.
std::string AddressValue() {
    std::string url = toypad::WebUrl();
    if (url.rfind("http://", 0) == 0) url.erase(0, 7);
    if (!url.empty()) return url;
    return toypad::CurrentSource() == toypad::Source::kThisPc ? "not started (port in use?)" : "needs Toy Pad: This PC";
}
void AddressChange(int) {}

std::string SkipValue() { return S().skip_cutscenes.load() ? "On" : "Off"; }
void SkipChange(int) {
    S().skip_cutscenes = S().skip_cutscenes.load() ? 0u : 1u;
    loader::SetSkipAnyCutscene(S().skip_cutscenes.load() != 0);  // in effect at once
}

}  // namespace

const char* PageTitle(Page page) { return page == Page::Gameplay ? "Gameplay" : "Display Settings"; }

const std::vector<Item>& Items(Page page) {
    // Display Settings: Resolution, then the upscaler; Anti-aliasing only where it does something (upscaler off or
    // FSR 1: FSR 2, DLSS and DLAA anti-alias the picture themselves).
    auto display_list = [](bool with_aa) {
        std::vector<Item> v = {
            {"Resolution", ResolutionValue, ResolutionChange,
             "The picture's resolution: 1080p, 1440p, 4K, or 6K (supersampled, the sharpest). Automatic is 4K on a "
             "4K display and 1080p otherwise. With an upscaler on it is the resolution the upscaler reaches (at most "
             "your display's).",
             "The picture's resolution; choose an upscaler next"},
            {"Upscaler", UpscalerValue, UpscalerChange,
             "Off renders at the resolution above. FSR 1, FSR 2 (AMD) and DLSS (NVIDIA RTX) render a smaller picture "
             "and upscale it to that resolution: Quality is the sharpest, Ultra Performance the fastest. FSR 2 and "
             "DLSS build detail from several frames and anti-alias too; DLAA is DLSS at the full resolution.",
             "Off, or render smaller and upscale: FSR 1, FSR 2, DLSS, DLAA"},
            {"Aspect Ratio", AspectValue, AspectChange,
             "16:9 keeps the game's shape with black bars; Stretch fills the window; Zoom fills it by cropping.",
             "16:9 with bars, Stretch to fill, or Zoom and crop"},
            {"Display Mode", DisplayValue, DisplayChange,
             "Windowed, Borderless (a full-screen window) or Fullscreen (exclusive). F11 / Alt+Enter switches "
             "between windowed and borderless.",
             "Windowed, Borderless or Fullscreen (F11 switches)"},
            {"VSync", VsyncValue, VsyncChange,
             "Fast: no tearing, lowest delay. On: classic vsync. Off: no vsync, may tear. The game runs at 60 fps "
             "in every mode.",
             "Fast: no tearing, low delay. On: classic. Off: may tear"},
        };
        if (with_aa)
            v.push_back({"Anti-aliasing", AaValue, AaChange,
                         "FXAA smooths jagged edges on the final picture. TAA (temporal) also steadies shimmering edges "
                         "by blending frames; the HUD is left untouched.",
                         "Smooths jagged edges: FXAA, or TAA across frames"});
        if (std::getenv("LDPORT_ULTRAWIDE_ROW"))  // in progress: hidden
            v.push_back({"Ultrawide", UltrawideValue, UltrawideChange,
                         "For 21:9 and 32:9 screens: the 3D world fills the width and shows more at the sides; menus, "
                         "the HUD and movies stay 16:9 in the middle with black bars. Takes effect after restarting "
                         "the game.",
                         "For 21:9 and 32:9 screens (after a restart)"});
        v.push_back({"Brightness", BrightnessValue, BrightnessChange,
                     "The game's own brightness (0-100, 50 is as released), in steps of 5.",
                     "The game's brightness (50 is as released)"});
        v.push_back({"Motion Blur", MotionBlurValue, MotionBlurChange,
                     "The game's own motion blur on fast camera moves.", "The game's motion blur on fast camera moves"});
        return v;
    };
    static const std::vector<Item> display_aa = display_list(true), display_no_aa = display_list(false);
    // Gameplay, with the LAN Toy Pad's address row under it while it is on.
    static const Item kToyPad = {
        "Toy Pad", ToyPadValue, ToyPadChange,
        "Where figures come from. This PC: the pause menu's Toy Pad and the LAN Toy Pad page; a physical Toy Pad "
        "plugged in joins in. Physical Toy Pad: only the real pad. LegoToypad app: harrysof's LegoToypad "
        "(github.com/harrysof/LegoToypad) places them, and starts with the game when installed. Changing it lifts "
        "the figures placed from this PC.",
        "Where figures come from: this PC, a real pad or LegoToypad"};
    static const Item kLan = {
        "LAN Toy Pad", LanValue, LanChange,
        "A virtual Toy Pad web page on your local network: open the address below in a browser on a phone, tablet "
        "or another computer on the same network to place and remove figures while you play. Works while Toy Pad "
        "is This PC. Windows may ask to allow the game through its firewall the first time.",
        "Place figures from a phone or tablet on your network"};
    static const Item kAddress = {"Address", AddressValue, AddressChange,
                                  "Open this in a browser on the same network as this PC.",
                                  "Open this in a browser on the same network"};
    static const Item kIcons = {
        "Button Icons", IconsValue, IconsChange,
        "The button pictures in prompts and menus. Automatic follows what you last pressed: PlayStation, Xbox (One / "
        "Series; Xbox 360 pads get the 360 set), Nintendo (Switch layout: B at the bottom, A on the right) or the "
        "keyboard keys. Or choose one set to always show. The controller icons come from your game files.",
        "The button pictures in prompts; Automatic follows your pad"};
    static const std::vector<Item> gameplay_common = {
        {"Local Players", PlayersValue, PlayersChange,
         "How many players can join. 2 is the game as released. 3 or 4 turns on the game's own four-way split "
         "screen everywhere (it normally keeps it for a few modes): each player needs a controller and a character "
         "on the Toy Pad, and joins by pressing Options. Takes effect after restarting the game.",
         "Up to 4 players in split screen (after a restart)"},
        {"Keyboard", KeyboardValue, KeyboardChange,
         "Player 1 + 1st controller: the keyboard and the first controller both play player 1. Player 1 only: the "
         "keyboard is player 1 and the first controller is player 2.",
         "Whether the first controller is player 1 or player 2"},
        {"Skip Any Cutscene", SkipValue, SkipChange,
         "On: every cutscene can be skipped with the game's usual skip button, even the first time you see it. "
         "Off: the game's own rule (mostly only cutscenes you have already watched).",
         "Skip cutscenes even the first time you see them"},
    };
    static const std::vector<Item> gameplay_lan_off = [] {
        std::vector<Item> v = gameplay_common;
        v.insert(v.end(), {kToyPad, kLan, kIcons});
        return v;
    }();
    static const std::vector<Item> gameplay_lan_on = [] {
        std::vector<Item> v = gameplay_common;
        v.insert(v.end(), {kToyPad, kLan, kAddress, kIcons});
        return v;
    }();
    if (page == Page::Gameplay) return toypad::WebWanted() ? gameplay_lan_on : gameplay_lan_off;
    const uint32_t up = S().scale.load();
    return port::UpscalerMode(up) && up >= 8 ? display_no_aa : display_aa;
}

void Change(Page page, int index, int step) {
    const auto& items = Items(page);
    if (index < 0 || index >= int(items.size()) || !step) return;
    items[size_t(index)].change(step);
    port::Save();
    std::fprintf(stderr, "[settings] %s: %s\n", items[size_t(index)].name, items[size_t(index)].value().c_str());
}

std::string Label(Page page, int index) {
    const auto& items = Items(page);
    if (index < 0 || index >= int(items.size())) return {};
    return std::string(items[size_t(index)].name) + ": " + items[size_t(index)].value();
}

}  // namespace ui::settings
