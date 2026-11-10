#pragma once
#include <cstdint>
#include <vector>

namespace port {

// Patches the game's Options page to show the port's settings rows (see options_menu.cpp).
// Call after loading the image, before it is made executable.
void InstallOptionsMenu(uint8_t* base);

// The pad reader passes every pad's state through here (slot 0-3) before the game sees it. Cross on the
// Options pages' "Display Settings" / "Gameplay" rows turns the page into that settings list; while one is
// shown, Left / Right / Cross change the highlighted setting and Circle returns, and the game only gets Up /
// Down (its own cursor). Buttons and the left stick's X are changed in place.
void MenuPadInput(int slot, uint32_t& buttons, uint8_t& left_x);

// The game's own Brightness (0-100) and Motion Blur settings, moved from its pause Options page into Display
// Settings: read (-1 while the game's options don't exist yet) and changed the way that page changes them (in
// effect at once; the game saves them with its other options).
int GameBrightness();
void ChangeGameBrightness(int step);
int GameMotionBlur();
void ToggleGameMotionBlur();

}  // namespace port
