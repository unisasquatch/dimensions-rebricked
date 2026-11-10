// libScePad: the PS4 controllers, backed by SDL gamepads (DualSense, DualShock 4, Xbox pads and the Steam
// Deck's built-in controls all map to the same layout).
//
// Local co-op: up to four players. Player N is PS4 user N and pad handle N. Each controller keeps the player
// slot it got when it connected (the lowest free one, or the one it had before if it comes back), and its
// light / player LED shows that number. Players 2-4 are logged in while their controller is connected (see
// PadSlotPresent and the user service in system_services.cpp); the game opens their pad on the login event and
// they join in game with Options, as on a PS4. The keyboard and mouse always belong to player 1; with
// keyboard_player=1 in settings.ini the first controller becomes player 2 instead of sharing player 1.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>

#include "hle/button_icons.h"
#include "hle/hle.h"
#include "hle/keyboard_mouse.h"
#include "hle/options_menu.h"
#include "hle/port_settings.h"
#include "hle/videoout.h"
#include "ui/overlay.h"

namespace {

constexpr int kPadErrorInvalidArg = int(0x80920001);
constexpr int kPadTypeStandard = 0;

constexpr uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                   kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
                   kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;

#pragma pack(push, 1)
struct PadData {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_pad[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    uint8_t touch_num, touch_reserved[3];
    uint32_t touch_reserved1;
    uint8_t touches[2][8];
    bool connected;
    uint8_t pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserve[2];
    uint8_t device_unique_len;
    uint8_t device_unique[12];
};
#pragma pack(pop)
static_assert(sizeof(PadData) == 120);

constexpr int kPlayers = 4;

std::mutex g_mutex;
// One controller slot per player (with g_mutex).
struct Slot {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID id = 0;
    std::string identity;      // GUID + device path of the controller that last held this slot
    uint64_t lost_ns = 0;      // when that controller went away (0: never had one / still here)
    bool virtual_pad = false;  // debug remote: a controller that isn't there (/debug/pad, /debug/press?p=)
};
Slot g_slots[kPlayers];

std::string Identity(SDL_JoystickID id) {
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    const char* path = SDL_GetGamepadPathForID(id);
    return std::string(guid) + "|" + (path ? path : "");
}

int FirstControllerSlot() { return port::Get().keyboard_player.load() ? 1 : 0; }

// Matches SDL's gamepads to player slots: drops the ones that went away and gives new ones a slot. Cheap
// enough to run a few times a second from the game's pad and user-service polling. With g_mutex.
void Reconcile() {
    static uint64_t next_ns = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (now < next_ns) return;
    next_ns = now + 200'000'000ull;
    // The Keyboard setting changed: hand the controllers out again from the new first slot.
    static int first_slot = FirstControllerSlot();
    if (first_slot != FirstControllerSlot()) {
        first_slot = FirstControllerSlot();
        for (Slot& s : g_slots) {
            if (s.pad) SDL_CloseGamepad(s.pad);
            s.pad = nullptr;
            s.id = 0;
            s.lost_ns = 0;
        }
    }
    for (int i = 0; i < kPlayers; ++i) {
        Slot& s = g_slots[i];
        if (s.pad && !SDL_GamepadConnected(s.pad)) {
            std::fprintf(stderr, "[pad] player %d controller disconnected\n", i + 1);
            SDL_CloseGamepad(s.pad);
            s.pad = nullptr;
            s.id = 0;
            s.lost_ns = now;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int n = 0; ids && n < count; ++n) {
        const SDL_JoystickID id = ids[n];
        bool known = false;
        for (const Slot& s : g_slots) known = known || (s.pad && s.id == id);
        if (known) continue;
        const std::string identity = Identity(id);
        // The slot this controller had before, else the lowest free one (from player 2 when the keyboard
        // has player 1 to itself).
        int slot = -1;
        for (int i = FirstControllerSlot(); i < kPlayers && slot < 0; ++i)
            if (!g_slots[i].pad && g_slots[i].lost_ns && g_slots[i].identity == identity) slot = i;
        for (int i = FirstControllerSlot(); i < kPlayers && slot < 0; ++i)
            if (!g_slots[i].pad) slot = i;
        if (slot < 0) continue;  // four players already
        Slot& s = g_slots[slot];
        s.pad = SDL_OpenGamepad(id);
        if (!s.pad) continue;
        s.id = id;
        s.identity = identity;
        s.lost_ns = 0;
        s.virtual_pad = false;
        SDL_SetGamepadPlayerIndex(s.pad, slot);
        const char* name = SDL_GetGamepadName(s.pad);
        std::fprintf(stderr, "[pad] player %d: %s\n", slot + 1, name ? name : "controller");
    }
    SDL_free(ids);
}

// Player slot -> its controller (nullptr when it has none). With g_mutex.
SDL_Gamepad* Gamepad(int slot) {
    Reconcile();
    return slot >= 0 && slot < kPlayers ? g_slots[slot].pad : nullptr;
}

// Is someone playing in this slot? Player 1 always is (keyboard); the others while a controller is there, and
// for a couple of seconds after it goes away (a Bluetooth blip shouldn't drop the player). With g_mutex.
bool Present(int slot) {
    if (slot == 0) return true;
    if (slot < 0 || slot >= kPlayers) return false;
    Reconcile();
    const Slot& s = g_slots[slot];
    if (s.pad || s.virtual_pad) return true;
    return s.lost_ns && SDL_GetTicksNS() - s.lost_ns < 2'000'000'000ull;
}

uint8_t Stick(int16_t v) { return uint8_t(std::clamp((int(v) + 32768) >> 8, 0, 255)); }
uint8_t Trigger(int16_t v) { return uint8_t(std::clamp(int(v) >> 7, 0, 255)); }

SYSV int Init() {
    icons::Prepare();  // builds the other button icon sets from the game's files, in the background
    return 0;
}
// The handle is the user id: users 1-4 are players 1-4.
SYSV int Open(int user, int type, int index, const void*) {
    const bool ok = type == kPadTypeStandard && index == 0 && user >= 1 && user <= kPlayers;
    std::fprintf(stderr, "[pad] scePadOpen(user %d, type %d, index %d) -> %d\n", user, type, index, ok ? user : -1);
    return ok ? user : kPadErrorInvalidArg;
}
SYSV int Close(int) { return 0; }

constexpr std::pair<const char*, uint32_t> kButtonNames[] = {
    {"cross", kCross}, {"circle", kCircle}, {"square", kSquare}, {"triangle", kTriangle},
    {"options", kOptions}, {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
    {"l1", kL1}, {"r1", kR1}, {"l2", kL2}, {"r2", kR2}, {"l3", kL3}, {"r3", kR3}, {"touchpad", kTouchPad}};

// Testing aid: LDPORT_INPUT="4500:cross,5000:options,5200:options@2" holds a button for a few frames at a flip
// count; "@N" sends it on player N's pad (default player 1).
struct ScriptedPress { uint64_t frame; uint32_t bits; int slot; };
const std::vector<ScriptedPress>& Script() {
    static std::vector<ScriptedPress> script = [] {
        std::vector<ScriptedPress> s;
        const char* env = std::getenv("LDPORT_INPUT");
        if (!env) return s;
        std::string text = env;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(',', pos);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(pos, end - pos);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) {
                std::string button = item.substr(colon + 1);
                int slot = 0;
                if (const size_t at = button.find('@'); at != std::string::npos) {
                    slot = std::clamp(std::atoi(button.c_str() + at + 1) - 1, 0, kPlayers - 1);
                    button.resize(at);
                }
                for (const auto& [name, bit] : kButtonNames)
                    if (button == name) s.push_back({std::strtoull(item.c_str(), nullptr, 10), bit, slot});
            }
            pos = end + 1;
        }
        return s;
    }();
    return script;
}
// Live presses from the debug web endpoint (/debug/press): held for a few flips from now, per player.
std::atomic<uint32_t> g_live_bits[kPlayers]{};
std::atomic<uint64_t> g_live_until[kPlayers]{};

uint32_t ScriptedButtons(int slot) {
    constexpr uint64_t kHoldFrames = 6;
    const uint64_t now = hle::VideoOutFlipCount();
    uint32_t bits = now < g_live_until[slot].load() ? g_live_bits[slot].load() : 0;
    for (const auto& p : Script())
        if (p.slot == slot && now >= p.frame && now < p.frame + kHoldFrames) bits |= p.bits;
    return bits;
}

// Keyboard/mouse on top of the controller: buttons combine; a stick takes keyboard/mouse input when there is any.
void MergeKeyboardMouse(PadData* out) {
    const input::PadState k = input::Poll();
    out->buttons |= k.buttons;
    auto axis = [](float v) { return uint8_t(std::clamp(int(128 + v * 127.5f), 0, 255)); };
    if (k.lx != 0 || k.ly != 0) {
        out->left_x = axis(k.lx);
        out->left_y = axis(k.ly);
    }
    if (k.rx != 0 || k.ry != 0) {
        out->right_x = axis(k.rx);
        out->right_y = axis(k.ry);
    }
    out->l2 = std::max(out->l2, k.l2);
    out->r2 = std::max(out->r2, k.r2);
    // For the button icons: a key, or the mouse really moved (a nudge of the desk doesn't switch them).
    if (k.buttons || k.lx != 0 || k.ly != 0 || std::abs(k.rx) > 0.5f || std::abs(k.ry) > 0.5f)
        icons::NoteInput(icons::kKeyboard);
}

// The button icon set for a kind of controller (Steam Deck and generic pads have Xbox-style labels).
int IconSet(SDL_Gamepad* g) {
    switch (SDL_GetGamepadType(g)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return icons::kPlayStation;
        case SDL_GAMEPAD_TYPE_XBOX360: return icons::kXbox360;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_GAMEPAD_TYPE_GAMECUBE: return icons::kNintendo;
        default: return icons::kXbox;
    }
}

// `note_icons`: the button prompts follow player 1's device only (co-op players don't switch them).
void ReadGamepad(SDL_Gamepad* g, PadData* out, bool note_icons) {
    static constexpr std::pair<SDL_GamepadButton, uint32_t> kMap[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kCross}, {SDL_GAMEPAD_BUTTON_EAST, kCircle},
        {SDL_GAMEPAD_BUTTON_WEST, kSquare}, {SDL_GAMEPAD_BUTTON_NORTH, kTriangle},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3},
        {SDL_GAMEPAD_BUTTON_START, kOptions}, {SDL_GAMEPAD_BUTTON_TOUCHPAD, kTouchPad},
        {SDL_GAMEPAD_BUTTON_BACK, kTouchPad},  // Xbox View / Deck menu button stands in for the touch pad
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
    };
    bool used = false;  // this controller was used (for the button icons): a button, or a stick / trigger well in
    for (const auto& [button, bit] : kMap)
        if (SDL_GetGamepadButton(g, button)) {
            out->buttons |= bit;
            used = true;
        }
    for (const SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                    SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
        used = used || std::abs(int(SDL_GetGamepadAxis(g, a))) > 16000;
    if (used && note_icons) icons::NoteInput(IconSet(g));
    out->left_x = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
    out->left_y = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    out->right_x = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
    out->right_y = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    out->l2 = Trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    out->r2 = Trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (out->l2 > 0x20) out->buttons |= kL2;
    if (out->r2 > 0x20) out->buttons |= kR2;
}

int ReadStateInner(int handle, PadData* out) {
    if (handle < 1 || handle > kPlayers) return kPadErrorInvalidArg;
    const int slot = handle - 1;
    std::memset(out, 0, sizeof(*out));
    out->left_x = out->left_y = out->right_x = out->right_y = 0x80;
    out->orientation[3] = 1.0f;
    out->timestamp = SDL_GetTicksNS() / 1000;

    std::lock_guard lock(g_mutex);
    // Player 1: keyboard and mouse always count as a connected pad, so the game runs without a controller.
    // Players 2-4: connected while their controller is (or the debug remote's stand-in).
    const bool connected = Present(slot);
    out->connected = connected;
    out->connected_count = connected ? 1 : 0;
    if (!connected || ui::BlocksGameInput()) return 0;  // the Toy Pad picker / settings list has the controllers
    out->buttons |= ScriptedButtons(slot);
    if (SDL_Gamepad* g = Gamepad(slot)) ReadGamepad(g, out, slot == 0);
    if (slot == 0) MergeKeyboardMouse(out);
    return 0;
}

// The Options pages' Display Settings / Gameplay lists are shown in the game's own menu (options_menu.cpp):
// every pad's input goes through it first. Any player can be on that page.
SYSV int ReadState(int handle, PadData* out) {
    const int r = ReadStateInner(handle, out);
    if (r != 0) return r;
    port::MenuPadInput(handle - 1, out->buttons, out->left_x);
    return r;
}

struct LightBar { uint8_t r, g, b, reserved; };
SYSV int SetLightBar(int handle, const LightBar* c) {
    std::lock_guard lock(g_mutex);
    if (SDL_Gamepad* g = Gamepad(handle - 1)) SDL_SetGamepadLED(g, c->r, c->g, c->b);
    return 0;
}
SYSV int ResetLightBar(int) { return 0; }

struct Vibration { uint8_t large, small; };
SYSV int SetVibration(int handle, const Vibration* v) {
    std::lock_guard lock(g_mutex);
    if (SDL_Gamepad* g = Gamepad(handle - 1))
        SDL_RumbleGamepad(g, uint16_t(v->large * 257), uint16_t(v->small * 257), 0);
    return 0;
}

}  // namespace

namespace hle {
// Debug remote: presses a pad button (cross, circle, square, triangle, options, up, down, left, right, l1, r1,
// l2, r2, l3, r3, touchpad) on player `player`'s pad (1-4) for `frames` flips. Pressing on a player 2-4 who
// has no controller plugs in a stand-in one (as if a controller had connected). False for an unknown name.
bool PressPadButton(const std::string& name, uint32_t frames, int player) {
    if (player < 1 || player > kPlayers) return false;
    const int slot = player - 1;
    for (const auto& [n, bit] : kButtonNames)
        if (name == n) {
            if (slot > 0) {
                std::lock_guard lock(g_mutex);
                if (!g_slots[slot].pad && !g_slots[slot].virtual_pad) {
                    g_slots[slot].virtual_pad = true;
                    std::fprintf(stderr, "[pad] player %d: debug stand-in controller\n", player);
                }
            }
            g_live_bits[slot] = bit;
            g_live_until[slot] = hle::VideoOutFlipCount() + frames;
            return true;
        }
    return false;
}
bool PressPadButton(const std::string& name, uint32_t frames) { return PressPadButton(name, frames, 1); }

// Debug remote: plugs a stand-in controller in for player 2-4 (or takes it out).
bool SetDebugPad(int player, bool connected) {
    if (player < 2 || player > kPlayers) return false;
    std::lock_guard lock(g_mutex);
    Slot& s = g_slots[player - 1];
    if (s.virtual_pad && !connected) s.lost_ns = 0;  // drop out at once
    s.virtual_pad = connected;
    std::fprintf(stderr, "[pad] player %d: debug stand-in controller %s\n", player, connected ? "in" : "out");
    return true;
}

// The user service: is player `slot` (0-3) here? Player 1 always is; 2-4 while a controller is.
bool PadSlotPresent(int slot) {
    std::lock_guard lock(g_mutex);
    return Present(slot);
}

// Which players are here: bit N = player N+1 (for the debug remote's status).
uint32_t PadSlotsMask() {
    std::lock_guard lock(g_mutex);
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i)
        if (Present(i)) mask |= 1u << i;
    return mask;
}
}  // namespace hle

#define PAD(name, fn) HLE_EXPORT("libScePad", name, reinterpret_cast<const void*>(&fn))
PAD("scePadInit", Init);
PAD("scePadOpen", Open);
PAD("scePadClose", Close);
PAD("scePadReadState", ReadState);
PAD("scePadSetLightBar", SetLightBar);
PAD("scePadResetLightBar", ResetLightBar);
PAD("scePadSetVibration", SetVibration);
