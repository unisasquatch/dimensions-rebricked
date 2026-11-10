// A physical LEGO Dimensions Toy Pad (USB VID 0E6F, PID 0241) over the host's HID driver. When one is plugged
// in, the game's USB transfers go to it instead of the emulated pad (LDPORT_TOYPAD=virtual keeps the emulated
// one). Its packets are the same 32 bytes the game sends and receives on the console.
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace toypad {

class RealToyPad {
public:
    // The pad if one is connected (opened on first call, retried every few seconds while absent).
    static RealToyPad* Get();
    // Endpoint 0x01: a packet from the game.
    bool Write(const uint8_t* packet);
    // Endpoint 0x81: the next packet the pad sent, if any.
    std::optional<std::array<uint8_t, 32>> Read();
    bool Connected() const;
};

}  // namespace toypad
