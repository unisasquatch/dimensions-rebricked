#pragma once
#include <cstdint>

union SDL_Event;

// Keyboard and mouse as a PS4 controller, laid out like TT's PC LEGO games (LEGO Star Wars: The
// Skywalker Saga): WASD to move, mouse for the camera, Space to jump, left click to attack, E to
// interact/build, Q to switch character. The mapping table is in keyboard_mouse.cpp.
namespace input {

// Main thread: feed every SDL event (keys, mouse buttons and motion, focus changes).
void OnSdlEvent(const SDL_Event& event);

struct PadState {
    uint32_t buttons = 0;              // scePad button bits
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1; y down is positive, as on the PS4
    uint8_t l2 = 0, r2 = 0;
    bool active = false;               // any key or mouse input recently
};

// Game thread: current keyboard/mouse state as a pad. Consumes the mouse motion since the last call.
PadState Poll();

}  // namespace input
