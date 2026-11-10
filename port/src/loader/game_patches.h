#pragma once
#include <cstddef>
#include <cstdint>

namespace loader {

// Applies the port's small code patches to the loaded image (see game_patches.cpp). Returns how many.
size_t ApplyGamePatches(uint8_t* base, size_t image_size);

// "Skip any cutscene": lets every cutscene be skipped (on) or restores the game's check (off). Any thread, any
// time after ApplyGamePatches; does nothing when the check wasn't found. Returns whether it is in place.
bool SetSkipAnyCutscene(bool on);

}  // namespace loader
