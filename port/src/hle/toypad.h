#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace toypad {

class RealToyPad;
using Packet = std::array<uint8_t, 32>;
using TagData = std::array<uint8_t, 0x2D * 4>;  // NTAG213: 45 pages of 4 bytes

// Pad zones as the game numbers them.
enum Zone : uint8_t { kCenter = 1, kLeft = 2, kRight = 3 };

// One pad's light, as the game last set it. An effect runs from `from` to `to`, starting `elapsed_ms` ago:
// Flash alternates `to` (on_ms) and `from` (off_ms); Fade blends between them over on_ms per step.
// `steps` state changes in all (-1: forever); an odd count ends on `to`, an even one on `from`.
struct Light {
    enum Fx : uint8_t { kSteady, kFlash, kFade };
    Fx fx = kSteady;
    std::array<uint8_t, 3> rgb{};             // the colour now
    std::array<uint8_t, 3> from{}, to{};
    uint32_t on_ms = 0, off_ms = 0;
    int steps = 0;
    int64_t elapsed_ms = 0;
};
// The colour an effect shows `t_ms` after it started, and whether it has finished.
std::array<uint8_t, 3> LightColourAt(const Light& l, int64_t t_ms, bool* done = nullptr);

// The LEGO Dimensions Toy Pad as seen over its two interrupt endpoints.
// Protocol reference: RPCS3's emulated toypad (rpcs3/Emu/Io/Dimensions.cpp, GPL-2.0);
// this is an independent implementation of the documented behaviour.
class VirtualToyPad {
public:
    static constexpr int kSlots = 7;

    explicit VirtualToyPad(std::string figure_dir);
    // Where figure tags (and the web page's icons, in icons/) are kept.
    const std::string& FigureDir() const { return figure_dir_; }

    // Endpoint 0x01: a command from the game. The reply is queued for endpoint 0x81.
    void Write(const uint8_t* packet);
    // Endpoint 0x81: next queued packet (figure events first, then command replies).
    std::optional<Packet> Read();

    // A physical pad is plugged in: both pads act as one. The real pad does the handshake and the lights (the
    // emulator follows along so the boards show them); figure indices are one shared space, rewritten in the
    // real pad's figure events, and tag reads/writes go to whichever pad holds that figure.
    void WriteMerged(const uint8_t* packet, RealToyPad& real);
    std::optional<Packet> ReadMerged(RealToyPad& real);

    // Places a figure (by ID: characters < 1000, vehicles/gadgets >= 1000) on a zone.
    // Returns the slot used, or -1 if the zone is full.
    int Place(Zone zone, uint32_t figure_id);
    // Lifts a virtual figure off. Figures on the physical pad can't be removed from here.
    bool Remove(int slot);
    // Lifts every figure placed from this PC (the physical pad's stay): the Toy Pad source changed.
    void RemoveAllVirtual();

    // Figures from another app (LegoToypad, see toypad_source.cpp), which numbers its figures 0-6 itself and
    // sends whole tag dumps. `save_path` non-empty: the game's tag writes go to that file; otherwise they last
    // until the figure is lifted (LegoToypad's built-in tags have no file behind them). Returns the slot, or -1.
    int PlaceExternal(Zone zone, int ext_index, const TagData& tag, std::string save_path);
    struct ExternalTag { Zone zone; TagData tag; std::string save_path; };
    // Lifts the app's figure `ext_index`, returning its tag (with the game's writes) for a move.
    std::optional<ExternalTag> RemoveExternal(int ext_index);
    // The game has finished the handshake: figure events reach it now (not re-sent from scratch later).
    bool Ready();
    // The lights as the game last commanded them, in the 40-byte "GET_LED" form LegoToypad reads: 'L', a serial
    // that changes with every new command, version 2, 3 regions, then per region (pad, mode 0 off / 1 solid /
    // 2 flash / 3 fade, r, g, b, the colour shown before (fades start from it), on ticks, off ticks, count
    // (0 = forever), ticks per fade step).
    static constexpr int kLedSnapshotSize = 40;
    void LedSnapshot(uint8_t out[kLedSnapshotSize]);
    std::string Describe();
    // `id` is 0 for a physical figure until the game has read its ID page.
    struct SlotInfo { int slot; Zone zone; uint32_t id; bool physical; };
    std::vector<SlotInfo> Slots();
    // The three pads' lights: [0] centre, [1] left, [2] right (zone - 1).
    std::array<Light, 3> Lights();

private:
    struct Figure {
        bool present = false;
        Zone zone = kCenter;
        uint32_t id = 0;
        TagData tag{};
        bool physical = false;   // on the real pad, as its index `real_index`
        uint8_t real_index = 0;
        int ext_index = -1;      // placed by another app (LegoToypad), under its own index
        std::string save_path;   // that app's tag file, if any (empty: writes stay in memory)
    };

    Packet Command(const uint8_t* packet);  // the emulator's reply to a command (mutex_ held)
    bool IsPhysical(int slot) const { return slot >= 0 && slot < kSlots && figures_[slot].present && figures_[slot].physical; }
    void TranslateFromReal(Packet& p);
    void DropPhysical();
    Packet ReplySeed(const uint8_t* payload, uint8_t seq);
    Packet ReplyChallenge(const uint8_t* payload, uint8_t seq);
    Packet ReplyReadPages(uint8_t index, uint8_t page, uint8_t seq);
    Packet ReplyWritePage(uint8_t index, uint8_t page, const uint8_t* data, uint8_t seq);
    Packet ReplyModel(const uint8_t* payload, uint8_t seq);
    void QueueFigureEvent(const Figure& f, int slot, bool removed);
    TagData LoadOrCreateTag(uint32_t figure_id);
    void SaveTag(const Figure& f);

    uint32_t NextRandom();
    void LedCommand(uint8_t cmd, const uint8_t* payload);
    void StartLight(int pad, Light::Fx fx, const uint8_t* rgb, uint32_t on_ms, uint32_t off_ms, uint8_t count);

    struct LightState { Light light; int64_t start_ms = 0; };
    LightState lights_[3];
    // The last command per pad as sent (for LedSnapshot): mode, rgb, from rgb, on, off, count, step.
    struct LedWire { uint8_t mode = 0, rgb[3] = {}, from[3] = {}, on = 0, off = 0, count = 0, step = 0; };
    LedWire led_wire_[3];
    uint8_t led_serial_ = 0;
    void SetWire(int pad, uint8_t mode, const uint8_t* rgb, uint8_t on, uint8_t off, uint8_t count, uint8_t step);

    std::mutex mutex_;
    std::string figure_dir_;
    Figure figures_[kSlots];
    std::deque<Packet> events_, replies_;
    // Figure events are held until the game has finished the wake/seed/challenge handshake: a real
    // pad answers commands first, and the game discards events that arrive mid-handshake.
    bool ready_ = false;
    bool merged_ = false;  // a physical pad was in use (its figures are dropped when it goes)
    // Real-pad page reads in flight, by sequence number: the slot and first page, to learn the figure's ID.
    std::map<uint8_t, std::pair<int, uint8_t>> pending_reads_;
    std::map<uint8_t, int> pending_models_;  // real-pad model queries in flight: seq -> slot
    uint32_t rng_[4] = {};
};

VirtualToyPad& Instance();

}  // namespace toypad
