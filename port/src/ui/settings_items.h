// The port's settings as a list of items, shared by the game's own Options pages (hle/options_menu.cpp shows
// them as native rows) and the ImGui settings list (ui/overlay.cpp).
#pragma once
#include <string>
#include <vector>

namespace ui::settings {

enum class Page { Display, Gameplay };

struct Item {
    const char* name;
    std::string (*value)();       // the current value as shown ("FSR 2 Quality (1440p)")
    void (*change)(int step);     // -1 / +1: the previous / next value (applied, not yet saved)
    const char* help;             // a paragraph (the ImGui list)
    const char* short_help;       // one line (the game's Options page)
};

const char* PageTitle(Page page);
// The page's items, in order (the Ultrawide row only with LDPORT_ULTRAWIDE_ROW).
const std::vector<Item>& Items(Page page);
// Changes item `index` by `step`, saves settings.ini and logs it.
void Change(Page page, int index, int step);
// "Name: Value", the way the game labels its own rows ("Music: On").
std::string Label(Page page, int index);

}  // namespace ui::settings
