// A small web server for the virtual Toy Pad: open the page on a phone, tablet or the PC itself
// and place or remove figures while playing. Started from the port's menu key (F2) or with
// LDPORT_TOYPAD_WEB=1; serves on every network interface, port 8642 by default.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace toypad {

// Every known figure: names (hand-written for characters), franchise and whether an icon exists, from
// <data>/toypad/icons/figures.json when made (port/tools/toypad_icons.py), else the built-in list.
struct FigureInfo {
    uint32_t id;
    std::string name, franchise;
    bool icon;
};
std::vector<FigureInfo> FigureList();

bool StartWeb(int port = 8642);
void StopWeb();
bool WebRunning();
// The address to open (this machine's LAN address), or empty when stopped.
std::string WebUrl();

}  // namespace toypad
