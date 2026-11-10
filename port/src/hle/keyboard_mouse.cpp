#include "hle/keyboard_mouse.h"
#include "hle/port_settings.h"
#include "ui/overlay.h"
#include "hle/toypad_web.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bitset>
#include <mutex>

namespace input {
namespace {

// scePad button bits.
constexpr uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                   kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
                   kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;

// Key -> button: TT's PC layout for its LEGO games (LEGO Marvel Super Heroes, player 1), each key on the PS4
// button with the same job (J jump = Cross, H attack/use = Square, K special = Circle, U tag = Triangle).
struct KeyBinding { SDL_Scancode key; uint32_t button; };
constexpr KeyBinding kKeys[] = {
    {SDL_SCANCODE_J, kCross},            // jump / menus: confirm
    {SDL_SCANCODE_H, kSquare},           // attack / use (hold to aim)
    {SDL_SCANCODE_K, kCircle},           // special / build (hold) / menus: back
    {SDL_SCANCODE_U, kTriangle},         // tag / switch character, enter vehicle
    {SDL_SCANCODE_SPACE, kR1},           // next character
    {SDL_SCANCODE_LCTRL, kL1},           // previous character
    {SDL_SCANCODE_RCTRL, kL1},
    {SDL_SCANCODE_LSHIFT, kR2},
    {SDL_SCANCODE_RETURN, kOptions},     // pause
    {SDL_SCANCODE_ESCAPE, kOptions},
    {SDL_SCANCODE_M, kTouchPad},         // map / Toy Pad menu
    {SDL_SCANCODE_TAB, kTouchPad},
    {SDL_SCANCODE_UP, kUp}, {SDL_SCANCODE_DOWN, kDown}, {SDL_SCANCODE_LEFT, kLeft}, {SDL_SCANCODE_RIGHT, kRight},
    {SDL_SCANCODE_BACKSPACE, kCircle},   // menus: back
    {SDL_SCANCODE_C, kL3},
    {SDL_SCANCODE_V, kR3},
};
// Camera keys (right stick): P up, ; down, L left, ' right.
constexpr SDL_Scancode kCamUp = SDL_SCANCODE_P, kCamDown = SDL_SCANCODE_SEMICOLON, kCamLeft = SDL_SCANCODE_L,
                       kCamRight = SDL_SCANCODE_APOSTROPHE;
constexpr uint32_t kLeftClick = kSquare;  // attack
constexpr uint32_t kRightClick = kL2;     // aim

// Mouse pixels per frame for a full stick deflection on the camera.
constexpr float kMouseFullDeflection = 18.0f;

std::mutex g_mutex;
std::bitset<SDL_SCANCODE_COUNT> g_keys;
bool g_left = false, g_right = false;
float g_dx = 0, g_dy = 0;
bool g_captured = false;
uint64_t g_last_input_ms = 0;

void SetCapture(SDL_Window* window, bool on) {
    if (!window || g_captured == on) return;
    SDL_SetWindowRelativeMouseMode(window, on);
    g_captured = on;
}

}  // namespace

void OnSdlEvent(const SDL_Event& e) {
    if (ui::ProcessEvent(e)) return;  // the Toy Pad picker is open and took it
    SDL_Window* window = SDL_GetWindowFromEvent(&e);
    std::lock_guard lock(g_mutex);
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.scancode == SDL_SCANCODE_F10 && e.type == SDL_EVENT_KEY_DOWN) {
                SetCapture(window, !g_captured);
                break;
            }
            // F11 or Alt+Enter: windowed <-> borderless (exclusive fullscreen is in the Options menu).
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.scancode == SDL_SCANCODE_F11 ||
                 (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                const uint32_t mode = port::Get().display.load() ? 0u : uint32_t(port::DisplayMode::Borderless);
                port::Get().display = mode;  // remembered, and shown in the Options menu
                port::Save();
                port::RequestDisplayMode(mode);
                break;
            }
            // Port menu: F2 starts / stops the Toy Pad web page (address shown in the title bar).
            if (e.key.scancode == SDL_SCANCODE_F2 && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (toypad::WebRunning()) toypad::StopWeb();
                else toypad::StartWeb();
                break;
            }
            if (e.key.scancode < SDL_SCANCODE_COUNT) g_keys[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
            g_last_input_ms = SDL_GetTicks();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (down && !g_captured) {  // the first click only captures the mouse
                SetCapture(window, true);
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT) g_left = down;
            if (e.button.button == SDL_BUTTON_RIGHT) g_right = down;
            g_last_input_ms = SDL_GetTicks();
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (g_captured) {
                g_dx += e.motion.xrel;
                g_dy += e.motion.yrel;
                g_last_input_ms = SDL_GetTicks();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            SetCapture(window, false);
            g_keys.reset();
            g_left = g_right = false;
            break;
        default:
            break;
    }
}

PadState Poll() {
    std::lock_guard lock(g_mutex);
    PadState p;
    for (const auto& b : kKeys)
        if (g_keys[b.key]) p.buttons |= b.button;
    if (g_left) p.buttons |= kLeftClick;
    if (g_right) {
        p.buttons |= kRightClick;
        p.l2 = 255;
    }
    if (g_keys[SDL_SCANCODE_LSHIFT]) p.r2 = 255;
    p.lx = float(g_keys[SDL_SCANCODE_D]) - float(g_keys[SDL_SCANCODE_A]);
    p.ly = float(g_keys[SDL_SCANCODE_S]) - float(g_keys[SDL_SCANCODE_W]);
    if (p.lx != 0 && p.ly != 0) {  // diagonals at full speed, not 1.41x
        p.lx *= 0.7071f;
        p.ly *= 0.7071f;
    }
    p.rx = std::clamp(g_dx / kMouseFullDeflection + float(g_keys[kCamRight]) - float(g_keys[kCamLeft]), -1.0f, 1.0f);
    p.ry = std::clamp(g_dy / kMouseFullDeflection + float(g_keys[kCamDown]) - float(g_keys[kCamUp]), -1.0f, 1.0f);
    g_dx = g_dy = 0;
    p.active = SDL_GetTicks() - g_last_input_ms < 5000;
    return p;
}

}  // namespace input
