// Controller-aware button icons.
//
// The PS4 game only ever draws its own PlayStation icon sheet (framework-data/gui/buttons/ps4/buttonicons_dx11.tsh,
// a 608x608 BC3 texture with a named rectangle per button). The game's archives also carry the Xbox One, Xbox 360
// and Wii U sheets of TT's other builds, and framework-data/gui/buttons/buttonconfig.csv says which of their icons
// stands for each PlayStation button (Cross -> A on Xbox, B on Nintendo, ...). At startup this module reads those
// from the player's own game files and builds one replacement sheet per icon set in the PlayStation sheet's exact
// layout; the keyboard set is drawn here (keycaps with the port's key bindings). The renderer recognises the
// game's sheet when it uploads it and sends the sheet of the set in use instead, and uploads it again when the
// set changes. Nothing is written to the game's files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace icons {

enum Set : int { kPlayStation = 0, kXbox = 1, kXbox360 = 2, kNintendo = 3, kKeyboard = 4, kSetCount = 5 };
const char* SetName(int set);

// Starts building the sheets on a background thread (once; cheap to call again). The same thread then makes the
// Toy Pad figures' list, portraits and picker atlas in <data>/toypad/icons from the game's files, when they aren't
// there yet.
void Prepare();

// Goes up each time the Toy Pad figure files are made this run: the picker reloads them when it changes.
int FiguresGeneration();

// The pad layer: the player just used this kind of device (any player, buttons or a stick pushed well over).
void NoteInput(int set);

// The set the player should see: the button_icons setting, else the last device used (PlayStation until then).
int Wanted();

// Renderer side. A texture this size, in BC3 with one level, may be the game's sheet (false until the sheets are
// built). IsGameSheet checks its linear level-0 blocks against the game's file.
bool Ready();
bool SheetSize(uint32_t width, uint32_t height);
bool IsGameSheet(const uint8_t* blocks, size_t bytes);
// The set the renderer should put on the sheet now: Wanted(), or PlayStation while that set isn't available.
int Applicable();
// RGBA8 pixels (width x height x 4, top row first) for a set; nullptr for PlayStation (the game's own sheet).
const std::vector<uint8_t>* Pixels(int set);

}  // namespace icons
