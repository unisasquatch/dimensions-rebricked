#include "hle/toypad.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "hle/hle.h"
#include "hle/toypad_source.h"
#include "hle/toypad_usb.h"

namespace toypad {
namespace {

// TEA key for command payloads (handshake and model queries).
constexpr std::array<uint8_t, 16> kCommandKey = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
                                                 0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};
// Mixed with a tag UID to derive that tag's key.
constexpr std::array<uint8_t, 17> kCharConstant = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
                                                   0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
// "(c) Copyright LEGO 2014" + padding, mixed with a UID to derive the tag password.
constexpr std::array<uint8_t, 25> kPwdConstant = {0x28, 0x63, 0x29, 0x20, 0x43, 0x6F, 0x70, 0x79, 0x72,
                                                  0x69, 0x67, 0x68, 0x74, 0x20, 0x4C, 0x45, 0x47, 0x4F,
                                                  0x20, 0x32, 0x30, 0x31, 0x34, 0xAA, 0xAA};
constexpr int kIdPage = 36, kPwdPage = 43, kBlankMarkerPage = 38;
constexpr uint32_t kFirstVehicleId = 1000;

uint32_t LoadLe(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint32_t LoadBe(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void StoreLe(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void StoreBe(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }

uint8_t Checksum(const uint8_t* p, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; ++i) sum += p[i];
    return uint8_t(sum);
}

using Key = std::array<uint8_t, 16>;

std::array<uint8_t, 8> Tea(const uint8_t* in, const Key& key, bool encrypt) {
    uint32_t v0 = LoadLe(in), v1 = LoadLe(in + 4);
    const uint32_t k0 = LoadLe(&key[0]), k1 = LoadLe(&key[4]), k2 = LoadLe(&key[8]), k3 = LoadLe(&key[12]);
    constexpr uint32_t kDelta = 0x9E3779B9;
    if (encrypt) {
        uint32_t sum = 0;
        for (int i = 0; i < 32; ++i) {
            sum += kDelta;
            v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
            v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
        }
    } else {
        uint32_t sum = kDelta * 32;
        for (int i = 0; i < 32; ++i) {
            v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
            v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
            sum -= kDelta;
        }
    }
    std::array<uint8_t, 8> out;
    StoreLe(&out[0], v0);
    StoreLe(&out[4], v1);
    return out;
}

uint32_t Randomize(const std::vector<uint8_t>& data, int count) {
    uint32_t s = 0;
    for (int i = 0; i < count; ++i) s = LoadLe(&data[i * 4]) + std::rotr(s, 25) + std::rotr(s, 10) - s;
    return s;
}

std::array<uint8_t, 7> Uid(const TagData& tag) { return {tag[0], tag[1], tag[2], tag[4], tag[5], tag[6], tag[7]}; }

uint32_t Scramble(const std::array<uint8_t, 7>& uid, int count) {
    std::vector<uint8_t> v(uid.begin(), uid.end());
    v.insert(v.end(), kCharConstant.begin(), kCharConstant.end());
    v[count * 4 - 1] = 0xAA;
    // The scrambled word is used big-endian when building the key.
    const uint32_t s = Randomize(v, count);
    return (s >> 24) | ((s >> 8) & 0xFF00) | ((s << 8) & 0xFF0000) | (s << 24);
}

Key TagKey(const TagData& tag) {
    const auto uid = Uid(tag);
    Key key{};
    for (int i = 0; i < 4; ++i) StoreBe(&key[i * 4], Scramble(uid, 3 + i));
    return key;
}

uint32_t TagPassword(const std::array<uint8_t, 7>& uid) {
    std::vector<uint8_t> v(uid.begin(), uid.end());
    v.insert(v.end(), kPwdConstant.begin(), kPwdConstant.end());
    return Randomize(v, 8);
}

uint32_t TagFigureId(const TagData& tag) {
    const auto plain = Tea(&tag[kIdPage * 4], TagKey(tag), false);
    const uint32_t id = LoadLe(plain.data());
    return id < kFirstVehicleId ? id : LoadLe(&tag[kIdPage * 4]);
}

}  // namespace

VirtualToyPad::VirtualToyPad(std::string figure_dir) : figure_dir_(std::move(figure_dir)) {
    std::error_code ec;
    std::filesystem::create_directories(figure_dir_, ec);
}

uint32_t VirtualToyPad::NextRandom() {
    uint32_t& a = rng_[0];
    uint32_t& b = rng_[1];
    uint32_t& c = rng_[2];
    uint32_t& d = rng_[3];
    const uint32_t e = a - std::rotl(b, 21);
    a = b ^ std::rotl(c, 19);
    b = c + std::rotl(d, 6);
    c = d + e;
    d = e + a;
    return d;
}

Packet VirtualToyPad::ReplySeed(const uint8_t* payload, uint8_t seq) {
    const auto v = Tea(payload, kCommandKey, false);
    const uint32_t seed = LoadLe(&v[0]);
    rng_[0] = 0xF1EA5EED;
    rng_[1] = rng_[2] = rng_[3] = seed;
    for (int i = 0; i < 42; ++i) NextRandom();

    uint8_t plain[8] = {};
    std::memcpy(plain, &v[4], 4);  // confirmation, echoed back
    const auto enc = Tea(plain, kCommandKey, true);
    Packet r{0x55, 0x09, seq};
    std::memcpy(&r[3], enc.data(), 8);
    r[11] = Checksum(r.data(), 11);
    return r;
}

Packet VirtualToyPad::ReplyChallenge(const uint8_t* payload, uint8_t seq) {
    const auto v = Tea(payload, kCommandKey, false);
    uint8_t plain[8];
    StoreLe(plain, NextRandom());
    std::memcpy(plain + 4, &v[0], 4);  // confirmation, echoed back
    const auto enc = Tea(plain, kCommandKey, true);
    Packet r{0x55, 0x09, seq};
    std::memcpy(&r[3], enc.data(), 8);
    r[11] = Checksum(r.data(), 11);
    return r;
}

Packet VirtualToyPad::ReplyReadPages(uint8_t index, uint8_t page, uint8_t seq) {
    Packet r{0x55, 0x12, seq, 0x00};
    const int slot = index - 1;  // the game numbers slots from 1
    if (slot >= 0 && slot < kSlots && figures_[slot].present && page * 4 + 16 <= int(sizeof(TagData)))
        std::memcpy(&r[4], &figures_[slot].tag[page * 4], 16);
    r[20] = Checksum(r.data(), 20);
    return r;
}

Packet VirtualToyPad::ReplyWritePage(uint8_t index, uint8_t page, const uint8_t* data, uint8_t seq) {
    const int slot = index - 1;
    if (slot >= 0 && slot < kSlots && figures_[slot].present && page < 0x2D) {
        Figure& f = figures_[slot];
        std::memcpy(&f.tag[page * 4], data, 4);
        if (page == kIdPage) f.id = LoadLe(data);
        SaveTag(f);
    }
    Packet r{0x55, 0x02, seq, 0x00};
    r[4] = Checksum(r.data(), 4);
    return r;
}

Packet VirtualToyPad::ReplyModel(const uint8_t* payload, uint8_t seq) {
    const auto v = Tea(payload, kCommandKey, false);
    const int slot = v[0] - 1;
    uint8_t plain[8] = {};
    if (slot >= 0 && slot < kSlots && figures_[slot].present) StoreLe(plain, figures_[slot].id);
    std::memcpy(plain + 4, &v[4], 4);
    const auto enc = Tea(plain, kCommandKey, true);
    Packet r{0x55, 0x0A, seq, 0x00};
    std::memcpy(&r[4], enc.data(), 8);
    r[12] = Checksum(r.data(), 12);
    return r;
}

void VirtualToyPad::Write(const uint8_t* p) {
    std::lock_guard lock(mutex_);
    DropPhysical();  // the physical pad has gone: its figures leave the game too
    replies_.push_back(Command(p));
}

Packet VirtualToyPad::Command(const uint8_t* p) {
    const uint8_t cmd = p[2], seq = p[3];
    static bool seen[256] = {};
    if (!seen[cmd]) {
        seen[cmd] = true;
        std::fprintf(stderr, "[toypad] first %#04x command from the game\n", cmd);
    }
    // Traffic summary every 5 s (commands since the last summary).
    static uint32_t counts[256] = {};
    static auto last = std::chrono::steady_clock::now();
    ++counts[cmd];
    if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(5)) {
        last = now;
        std::fprintf(stderr, "[toypad] last 5 s:");
        for (int c = 0; c < 256; ++c)
            if (counts[c]) std::fprintf(stderr, " %#04x x%u", c, counts[c]);
        std::fprintf(stderr, "\n");
        std::memset(counts, 0, sizeof counts);
    }
    Packet r{};
    switch (cmd) {
        case 0xB0:  // wake: the pad answers with its copyright string
            r = {0x55, 0x0E, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
            ready_ = false;  // figures are reported once the handshake completes (after 0xB3)
            events_.clear();
            break;
        case 0xB1: r = ReplySeed(p + 4, seq); break;
        case 0xB3:
            r = ReplyChallenge(p + 4, seq);
            if (!ready_) {  // handshake done: report the figures already on the pad
                ready_ = true;
                events_.clear();
                for (int slot = 0; slot < kSlots; ++slot)
                    if (figures_[slot].present && !figures_[slot].physical)  // the real pad reports its own
                        QueueFigureEvent(figures_[slot], slot, false);
            }
            break;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC6: case 0xC7: case 0xC8:
            // LED commands: shown on the virtual pad's lights (web page and overlay), then acknowledged.
            LedCommand(cmd, p + 4);
            r = {0x55, 0x01, seq};
            r[3] = Checksum(r.data(), 3);
            break;
        case 0xD2: r = ReplyReadPages(p[4], p[5], seq); break;
        case 0xD3: r = ReplyWritePage(p[4], p[5], p + 6, seq); break;
        case 0xD4: r = ReplyModel(p + 4, seq); break;
        default:
            std::fprintf(stderr, "[toypad] unhandled command %#04x\n", cmd);
            break;
    }
    return r;
}

namespace {
// A command's checksum: the byte after its payload (packet[1] counts the bytes after itself, cmd and seq included).
void Resum(Packet& q) {
    const int n = std::min(q[1] + 2, 31);
    q[n] = Checksum(q.data(), n);
}
}  // namespace

void VirtualToyPad::WriteMerged(const uint8_t* p, RealToyPad& real) {
    std::lock_guard lock(mutex_);
    if (!merged_) std::fprintf(stderr, "[toypad] physical and virtual pads merged\n");
    merged_ = true;
    const uint8_t cmd = p[2], seq = p[3];
    Packet q;
    std::memcpy(q.data(), p, 32);
    switch (cmd) {
        case 0xB0:  // wake: the real pad reports its figures again, under new indices
            for (Figure& f : figures_)
                if (f.physical) f.present = false;
            pending_reads_.clear();
            pending_models_.clear();
            [[fallthrough]];
        case 0xB1: case 0xB3:  // handshake: the real pad answers; the emulator follows (its figure events wait on it)
        case 0xC0: case 0xC2: case 0xC3: case 0xC4: case 0xC6: case 0xC7: case 0xC8:  // lights: both pads
            Command(p);
            break;
        case 0xD2: case 0xD3: {  // read / write tag pages: whichever pad holds the figure
            const int slot = p[4] - 1;
            if (!IsPhysical(slot)) {
                replies_.push_back(Command(p));
                return;
            }
            q[4] = figures_[slot].real_index;
            Resum(q);
            if (cmd == 0xD2) pending_reads_[seq] = {slot, p[5]};
            break;
        }
        case 0xD4: {  // model query: the index is inside the encrypted payload
            auto v = Tea(p + 4, kCommandKey, false);
            const int slot = v[0] - 1;
            if (!IsPhysical(slot)) {
                replies_.push_back(Command(p));
                return;
            }
            v[0] = figures_[slot].real_index;
            const auto enc = Tea(v.data(), kCommandKey, true);
            std::memcpy(&q[4], enc.data(), 8);
            Resum(q);
            pending_models_[seq] = slot;
            break;
        }
        default: break;  // anything else is the real pad's
    }
    real.Write(q.data());
}

std::optional<Packet> VirtualToyPad::ReadMerged(RealToyPad& real) {
    std::lock_guard lock(mutex_);
    if (!replies_.empty()) {
        Packet p = replies_.front();
        replies_.pop_front();
        return p;
    }
    if (auto r = real.Read()) {
        Packet p = *r;
        TranslateFromReal(p);
        return p;
    }
    if (ready_ && !events_.empty()) {
        Packet p = events_.front();
        events_.pop_front();
        return p;
    }
    return std::nullopt;
}

void VirtualToyPad::TranslateFromReal(Packet& p) {
    if (p[0] == 0x56 && p[1] == 0x0B) {  // figure placed / lifted: give it a slot of the shared index space
        const uint8_t zone = p[2], real_index = p[4];
        int slot = -1;
        if (p[5] == 0) {
            for (int i = 0; i < kSlots && slot < 0; ++i)
                if (!figures_[i].present) slot = i;
            if (slot < 0) {
                std::fprintf(stderr, "[toypad] no free index for a physical figure (7 in use)\n");
                return;
            }
            Figure& f = figures_[slot];
            f = {};
            f.present = f.physical = true;
            f.zone = Zone(zone >= 1 && zone <= 3 ? zone : 1);
            f.real_index = real_index;
            std::memcpy(&f.tag[0], &p[6], 3);  // the UID, laid out as on the tag
            std::memcpy(&f.tag[4], &p[9], 4);
        } else {
            for (int i = 0; i < kSlots && slot < 0; ++i)
                if (IsPhysical(i) && figures_[i].real_index == real_index) slot = i;
            if (slot < 0) return;
            figures_[slot].present = false;
        }
        std::fprintf(stderr, "[toypad] physical figure %s, zone %u: real index %u -> %d\n", p[5] ? "lifted" : "placed",
                     zone, real_index, slot + 1);
        p[4] = uint8_t(slot + 1);
        p[13] = Checksum(p.data(), 13);
    } else if (p[0] == 0x55 && p[1] == 0x0A) {  // model reply: the figure's ID, encrypted with the command key
        const auto it = pending_models_.find(p[2]);
        if (it == pending_models_.end()) return;
        const int slot = it->second;
        pending_models_.erase(it);
        if (!IsPhysical(slot) || p[3] != 0) return;
        const uint32_t id = LoadLe(Tea(&p[4], kCommandKey, false).data());
        if (id && id != figures_[slot].id) {
            figures_[slot].id = id;
            std::fprintf(stderr, "[toypad] physical figure at index %d is %u\n", slot + 1, id);
        }
    } else if (p[0] == 0x55 && p[1] == 0x12) {  // page read: keep the pages, to tell which figure it is
        const auto it = pending_reads_.find(p[2]);
        if (it == pending_reads_.end()) return;
        const auto [slot, page] = it->second;
        pending_reads_.erase(it);
        if (!IsPhysical(slot) || p[3] != 0) return;
        Figure& f = figures_[slot];
        for (int i = 0; i < 4 && page + i < 0x2D; ++i) std::memcpy(&f.tag[(page + i) * 4], &p[4 + i * 4], 4);
        if (page <= kIdPage && kIdPage < page + 4 && !f.id) {
            f.id = TagFigureId(f.tag);
            std::fprintf(stderr, "[toypad] physical figure at index %d is %u\n", slot + 1, f.id);
        }
    }
}

void VirtualToyPad::DropPhysical() {
    if (!merged_) return;
    merged_ = false;
    std::fprintf(stderr, "[toypad] physical pad gone: its figures are lifted off\n");
    for (int slot = 0; slot < kSlots; ++slot)
        if (IsPhysical(slot)) {
            QueueFigureEvent(figures_[slot], slot, true);
            figures_[slot].present = false;
        }
}

std::optional<Packet> VirtualToyPad::Read() {
    std::lock_guard lock(mutex_);
    DropPhysical();
    // Command replies first; figure events only once the game has set the pad up.
    auto& queue = !replies_.empty() ? replies_ : events_;
    if (&queue == &events_ && !ready_) return std::nullopt;
    if (queue.empty()) return std::nullopt;
    Packet p = queue.front();
    queue.pop_front();
    return p;
}

void VirtualToyPad::QueueFigureEvent(const Figure& f, int slot, bool removed) {
    const auto uid = Uid(f.tag);
    Packet e{0x56, 0x0B, f.zone, 0x00, uint8_t(slot + 1), uint8_t(removed ? 1 : 0)};
    std::memcpy(&e[6], uid.data(), 7);
    e[13] = Checksum(e.data(), 13);
    events_.push_back(e);
}

TagData VirtualToyPad::LoadOrCreateTag(uint32_t figure_id) {
    TagData tag{};
    const std::string path = figure_dir_ + "/" + std::to_string(figure_id) + ".bin";
    if (std::ifstream in{path, std::ios::binary}; in.read(reinterpret_cast<char*>(tag.data()), tag.size()))
        return tag;

    // A new NTAG213 with a random UID (manufacturer byte 0x04).
    static std::mt19937 rng{std::random_device{}()};
    tag[0] = 0x04;
    for (int i : {1, 2, 4, 5, 6}) tag[i] = uint8_t(rng());
    tag[7] = 0x80;
    tag[3] = uint8_t(figure_id);
    if (figure_id < kFirstVehicleId) {
        // Characters carry their ID encrypted with the tag key, plus a password page.
        uint8_t plain[8];
        StoreLe(plain, figure_id);
        StoreLe(plain + 4, figure_id);
        const auto enc = Tea(plain, TagKey(tag), true);
        std::memcpy(&tag[kIdPage * 4], enc.data(), 8);
        StoreLe(&tag[kPwdPage * 4], TagPassword(Uid(tag)));
    } else {
        // Vehicles and gadgets: plain ID; the game writes its build data to later pages.
        // TODO: confirm the vehicle tag layout against a real tag dump.
        StoreLe(&tag[kIdPage * 4], figure_id);
        tag[kBlankMarkerPage * 4 + 1] = 1;
    }
    return tag;
}

void VirtualToyPad::SaveTag(const Figure& f) {
    if (f.ext_index >= 0) {  // another app's figure: its own file, if it named one
        if (!f.save_path.empty()) {
            const std::u8string path(f.save_path.begin(), f.save_path.end());
            std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out);
            if (out) out.write(reinterpret_cast<const char*>(f.tag.data()), f.tag.size());
        }
        return;
    }
    std::ofstream out(figure_dir_ + "/" + std::to_string(TagFigureId(f.tag)) + ".bin", std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.tag.data()), f.tag.size());
}

int VirtualToyPad::Place(Zone zone, uint32_t figure_id) {
    if (CurrentSource() == Source::kPhysical) {  // the physical pad only: no figures from this PC
        std::fprintf(stderr, "[toypad] Toy Pad source is the physical pad: figure %u not placed\n", figure_id);
        return -1;
    }
    std::lock_guard lock(mutex_);
    // Room on the real pad: one figure in the centre, three on each side.
    int used = 0;
    for (const Figure& f : figures_) used += f.present && f.zone == zone;
    if (used >= (zone == kCenter ? 1 : 3)) return -1;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (figures_[slot].present) continue;
        Figure& f = figures_[slot];
        f = {true, zone, figure_id, LoadOrCreateTag(figure_id)};
        if (TagFigureId(f.tag) != figure_id)  // the game decodes the ID the same way
            std::fprintf(stderr, "[toypad] tag for %u decodes as %u\n", figure_id, TagFigureId(f.tag));
        SaveTag(f);
        QueueFigureEvent(f, slot, false);
        return slot;
    }
    return -1;
}

bool VirtualToyPad::Remove(int slot) {
    std::lock_guard lock(mutex_);
    if (slot < 0 || slot >= kSlots || !figures_[slot].present || figures_[slot].physical) return false;
    QueueFigureEvent(figures_[slot], slot, true);
    figures_[slot].present = false;
    return true;
}

void VirtualToyPad::RemoveAllVirtual() {
    std::lock_guard lock(mutex_);
    for (int slot = 0; slot < kSlots; ++slot)
        if (figures_[slot].present && !figures_[slot].physical) {
            if (ready_) QueueFigureEvent(figures_[slot], slot, true);
            figures_[slot].present = false;
        }
}

int VirtualToyPad::PlaceExternal(Zone zone, int ext_index, const TagData& tag, std::string save_path) {
    std::lock_guard lock(mutex_);
    // The app's own index when it is free (so the game sees the numbers the app shows), else any free slot.
    int slot = ext_index >= 0 && ext_index < kSlots && !figures_[ext_index].present ? ext_index : -1;
    for (int i = 0; i < kSlots && slot < 0; ++i)
        if (!figures_[i].present) slot = i;
    if (slot < 0) return -1;
    Figure& f = figures_[slot];
    f = {};
    f.present = true;
    f.zone = zone;
    f.tag = tag;
    f.id = TagFigureId(tag);
    f.ext_index = ext_index;
    f.save_path = std::move(save_path);
    if (ready_) QueueFigureEvent(f, slot, false);  // before the handshake, the handshake reports it
    return slot;
}

std::optional<VirtualToyPad::ExternalTag> VirtualToyPad::RemoveExternal(int ext_index) {
    std::lock_guard lock(mutex_);
    for (int slot = 0; slot < kSlots; ++slot) {
        Figure& f = figures_[slot];
        if (!f.present || f.physical || f.ext_index != ext_index) continue;
        if (ready_) QueueFigureEvent(f, slot, true);
        f.present = false;
        return ExternalTag{f.zone, f.tag, f.save_path};
    }
    return std::nullopt;
}

bool VirtualToyPad::Ready() {
    std::lock_guard lock(mutex_);
    return ready_;
}

std::vector<VirtualToyPad::SlotInfo> VirtualToyPad::Slots() {
    std::lock_guard lock(mutex_);
    std::vector<SlotInfo> out;
    for (int i = 0; i < kSlots; ++i)
        if (figures_[i].present) out.push_back({i, figures_[i].zone, figures_[i].id, figures_[i].physical});
    return out;
}

// --- lights ---
// One LED "tick" in the effect commands (30 per second). Measured from the game: it re-sends its 16-tick
// "place a figure" fades every ~0.53 s and its 30-tick title-screen fades about every 1.0-1.2 s.
constexpr uint32_t kTickMs = 33;

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::array<uint8_t, 3> LightColourAt(const Light& l, int64_t t, bool* done) {
    if (done) *done = false;
    if (l.fx == Light::kSteady || t < 0) {
        if (done) *done = l.fx == Light::kSteady;
        return l.fx == Light::kSteady ? l.rgb : l.from;
    }
    // Step i shows `to` when i is even and `from` when odd (a fade blends into that colour).
    const int64_t on = std::max<int64_t>(l.on_ms, 1), off = l.fx == Light::kFlash ? std::max<int64_t>(l.off_ms, 1) : on;
    const int64_t cycle = on + off;
    int64_t i = (t / cycle) * 2, rest = t % cycle;
    if (rest >= on) ++i, rest -= on;
    if (l.steps >= 0 && i >= l.steps) {
        if (done) *done = true;
        return (l.steps & 1) ? l.to : l.from;
    }
    const auto& target = (i & 1) ? l.from : l.to;
    if (l.fx == Light::kFlash) return target;
    const auto& source = (i & 1) ? l.to : l.from;
    const int64_t len = (i & 1) ? off : on;
    std::array<uint8_t, 3> c;
    for (int k = 0; k < 3; ++k) c[k] = uint8_t(source[k] + (int(target[k]) - source[k]) * rest / len);
    return c;
}

void VirtualToyPad::StartLight(int pad, Light::Fx fx, const uint8_t* rgb, uint32_t on_ms, uint32_t off_ms,
                               uint8_t count) {
    LightState& s = lights_[pad];
    const int64_t now = SteadyMs();
    Light& l = s.light;
    l.from = LightColourAt(l, now - s.start_ms);  // effects start from the colour showing now
    l.to = {rgb[0], rgb[1], rgb[2]};
    l.fx = fx;
    l.on_ms = on_ms;
    l.off_ms = off_ms;
    l.steps = count == 0xFF ? -1 : count;
    if (fx == Light::kSteady || l.steps == 0) {
        l.fx = Light::kSteady;
        l.rgb = fx == Light::kSteady ? l.to : l.from;
    }
    s.start_ms = now;
}

// Records a pad's command for LedSnapshot (after StartLight, whose `from` is the colour it starts from). A command
// identical to the last one keeps the serial: the game re-sends its fades, and the app would restart them.
void VirtualToyPad::SetWire(int pad, uint8_t mode, const uint8_t* rgb, uint8_t on, uint8_t off, uint8_t count,
                            uint8_t step) {
    const auto& from = lights_[pad].light.from;
    if (mode >= 2 && count == 0) {  // no steps: the effect never starts, the pad keeps its colour
        mode = 1;
        rgb = from.data();
    }
    if (count == 0xFF) count = 0;  // forever, as the app counts
    LedWire w{mode, {rgb[0], rgb[1], rgb[2]}, {from[0], from[1], from[2]}, on, off, count, step};
    LedWire& last = led_wire_[pad];
    if (last.mode == w.mode && std::memcmp(last.rgb, w.rgb, 3) == 0 && last.on == w.on && last.off == w.off &&
        last.count == w.count && last.step == w.step)
        return;
    last = w;
    ++led_serial_;
}

void VirtualToyPad::LedSnapshot(uint8_t out[kLedSnapshotSize]) {
    std::lock_guard lock(mutex_);
    std::memset(out, 0, kLedSnapshotSize);
    out[0] = 'L';
    out[1] = led_serial_;
    out[2] = 2;  // format version
    out[3] = 3;  // regions
    for (int i = 0; i < 3; ++i) {
        const LedWire& w = led_wire_[i];
        uint8_t* r = out + 4 + i * 12;
        r[0] = uint8_t(i + 1);  // 1 centre, 2 left, 3 right
        r[1] = w.mode;
        std::memcpy(r + 2, w.rgb, 3);
        std::memcpy(r + 5, w.from, 3);
        r[8] = w.on;
        r[9] = w.off;
        r[10] = w.count;
        r[11] = w.step;
    }
}

void VirtualToyPad::LedCommand(uint8_t cmd, const uint8_t* p) {
    static int logged = 0;
    if (logged < 300 && cmd != 0xC1) {
        ++logged;
        std::fprintf(stderr, "[toypad] led %#04x:", cmd);
        for (int i = 0; i < 21; ++i) std::fprintf(stderr, " %02x", p[i]);
        std::fprintf(stderr, "\n");
    }
    // Pad 0 is every pad; 1 centre, 2 left, 3 right. The *-all commands list centre, left, right.
    auto each = [&](uint8_t pad, auto&& fn) {
        for (int i = 0; i < 3; ++i)
            if (pad == 0 || pad == i + 1) fn(i);
    };
    switch (cmd) {
        case 0xC0:  // pad, r, g, b
            each(p[0], [&](int i) {
                StartLight(i, Light::kSteady, p + 1, 0, 0, 0);
                SetWire(i, 1, p + 1, 0, 0, 0, 0);
            });
            break;
        case 0xC2:  // fade: pad, ticks per fade, count, r, g, b
            each(p[0], [&](int i) {
                StartLight(i, Light::kFade, p + 3, p[1] * kTickMs, 0, p[2]);
                SetWire(i, 3, p + 3, 0, 0, p[2], p[1]);
            });
            break;
        case 0xC3:  // flash: pad, ticks on, ticks off, count, r, g, b
            each(p[0], [&](int i) {
                StartLight(i, Light::kFlash, p + 4, p[1] * kTickMs, p[2] * kTickMs, p[3]);
                SetWire(i, 2, p + 4, p[1], p[2], p[3], 0);
            });
            break;
        case 0xC4: {  // fade to a random colour: pad, ticks per fade, count
            static std::mt19937 rng{std::random_device{}()};
            const uint8_t rgb[3] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
            each(p[0], [&](int i) {
                StartLight(i, Light::kFade, rgb, p[1] * kTickMs, 0, p[2]);
                SetWire(i, 3, rgb, 0, 0, p[2], p[1]);
            });
            break;
        }
        case 0xC6:  // fade all: 3 x (enable, ticks, count, r, g, b)
            for (int i = 0; i < 3; ++i)
                if (const uint8_t* q = p + i * 6; q[0]) {
                    StartLight(i, Light::kFade, q + 3, q[1] * kTickMs, 0, q[2]);
                    SetWire(i, 3, q + 3, 0, 0, q[2], q[1]);
                }
            break;
        case 0xC7:  // flash all: 3 x (enable, ticks on, ticks off, count, r, g, b)
            for (int i = 0; i < 3; ++i)
                if (const uint8_t* q = p + i * 7; q[0]) {
                    StartLight(i, Light::kFlash, q + 4, q[1] * kTickMs, q[2] * kTickMs, q[3]);
                    SetWire(i, 2, q + 4, q[1], q[2], q[3], 0);
                }
            break;
        case 0xC8:  // colour all: 3 x (enable, r, g, b)
            for (int i = 0; i < 3; ++i)
                if (const uint8_t* q = p + i * 4; q[0]) {
                    StartLight(i, Light::kSteady, q + 1, 0, 0, 0);
                    SetWire(i, 1, q + 1, 0, 0, 0, 0);
                }
            break;
        default: break;  // 0xC1 reads a colour back
    }
}

std::array<Light, 3> VirtualToyPad::Lights() {
    std::lock_guard lock(mutex_);
    const int64_t now = SteadyMs();
    std::array<Light, 3> out;
    for (int i = 0; i < 3; ++i) {
        LightState& s = lights_[i];
        bool done = false;
        const auto c = LightColourAt(s.light, now - s.start_ms, &done);
        if (done) s.light.fx = Light::kSteady;  // a finished effect holds its last colour
        s.light.rgb = c;
        s.light.elapsed_ms = now - s.start_ms;
        out[i] = s.light;
    }
    return out;
}

std::string VirtualToyPad::Describe() {
    std::lock_guard lock(mutex_);
    std::string s;
    for (int i = 0; i < kSlots; ++i) {
        if (!figures_[i].present) continue;
        static const char* kZones[] = {"?", "center", "left", "right"};
        s += "slot " + std::to_string(i) + ": figure " + std::to_string(figures_[i].id) + " on " +
             kZones[figures_[i].zone] + "\n";
    }
    return s.empty() ? "pad is empty\n" : s;
}

VirtualToyPad& Instance() {
    static VirtualToyPad pad(hle::DataRoot() + "/toypad");
    return pad;
}

}  // namespace toypad
