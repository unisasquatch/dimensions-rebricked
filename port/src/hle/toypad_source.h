// Where the game's Toy Pad figures come from, picked in the Options menu (Gameplay > Toy Pad) and saved
// as settings.ini's toypad=:
//   0 this PC (default): the virtual pad (web page at :8642, the pause menu's picker); a physical Toy Pad that is
//     plugged in joins it.
//   1 physical only: the real Toy Pad; figures can't be placed from this PC.
//   2 LegoToypad: harrysof's LegoToypad app (https://github.com/harrysof/LegoToypad, MIT) places figures. It is a
//     client of the "Toypad listener" its emulator builds have (Cemu, RPCS3, shadPS4): a TCP server on
//     127.0.0.1:9191 (LegoToypad.ini [Listener] Port; settings.ini legotoypad_port) that this port provides. A
//     physical Toy Pad joins in here too.
#pragma once
#include <cstdint>

namespace toypad {

enum class Source : uint32_t { kThisPc = 0, kPhysical = 1, kLegoToypad = 2 };

Source CurrentSource();
const char* SourceName(Source s);
// At startup: the saved source (LDPORT_TOYPAD_SOURCE=pc|physical|legotoypad overrides it for one run). The web
// page starts with it when the source is this PC and the LAN Toy Pad setting is on.
void StartSource();
// The LAN Toy Pad setting (settings.ini toypad_web; LDPORT_TOYPAD_WEB=0/1 overrides it for one run), and turning
// it on or off from the Options menu (the page starts or stops at once).
bool WebWanted();
void SetWebWanted(bool on);
// From the Options menu: saved, and in effect at once. The figures placed from this PC (or by LegoToypad) are
// lifted off when the source changes.
void SetSource(Source s);
// LegoToypad has talked to the game in the last few seconds (it polls the lights ~30 times a second).
bool LegoToypadConnected();
// Starts the LegoToypad program (settings.ini legotoypad_exe, else <install>/LegoToypad/LegoToypad.exe) unless it
// is running. False when it isn't installed or can't be started.
bool LaunchLegoToypad();

}  // namespace toypad
