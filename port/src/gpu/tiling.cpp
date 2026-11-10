#include "gpu/tiling.h"

#include <algorithm>
#include <cstring>

namespace gpu {
namespace {

constexpr uint32_t kMicroTile = 8;  // micro tiles are 8x8 elements

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t NextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Element index inside a thin (non-displayable) micro tile: Z order, bits x0 y0 x1 y1 x2 y2.
uint32_t ThinMicroIndex(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

}  // namespace

bool IsTileModeSupported(uint32_t tile_index) {
    return tile_index == kTileDisplayLinearAligned || tile_index == kTileThin1D;
}

std::vector<MipLayout> LayoutMips(uint32_t tile_index, uint32_t width, uint32_t height, uint32_t pitch0,
                                  uint32_t levels, uint32_t bpe, bool block, bool pow2_pad) {
    std::vector<MipLayout> mips;
    uint64_t offset = 0;
    const uint32_t div = block ? 4 : 1;
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t pw = level == 0 ? std::max(pitch0, width) : w;
        uint32_t ph = h;
        if (pow2_pad && level > 0) {
            pw = std::max(1u, NextPow2(width) >> level);
            ph = std::max(1u, NextPow2(height) >> level);
        }
        MipLayout m;
        m.width = (w + div - 1) / div;
        m.height = (h + div - 1) / div;
        const uint32_t pe = (pw + div - 1) / div, he = (ph + div - 1) / div;
        if (tile_index == kTileThin1D) {
            m.pitch = AlignUp(pe, kMicroTile);
            m.padded_height = AlignUp(he, kMicroTile);
        } else {  // linear aligned: rows padded to 64 elements (and at least 256 bytes)
            m.pitch = AlignUp(pe, std::max(64u, 256u / bpe));
            m.padded_height = he;
        }
        m.offset = offset;
        m.size = uint64_t(m.pitch) * m.padded_height * bpe;
        offset += (m.size + 255) & ~uint64_t(255);
        mips.push_back(m);
    }
    return mips;
}

namespace {

// Thin1D rows y0..y1 with the element size known at compile time (the copies become plain moves). Within a
// micro tile the Z order puts elements x = 0..7 of a row at x0 | x1 << 2 | x2 << 4 from the row's base.
template <uint32_t B>
void ThinRows(const uint8_t* src, uint8_t* dst, const MipLayout& m, uint32_t y0, uint32_t y1) {
    static constexpr uint32_t kX[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    const uint32_t tiles_per_row = m.pitch / kMicroTile;
    const size_t tile_bytes = size_t(kMicroTile) * kMicroTile * B;
    const size_t row = size_t(m.width) * B;
    const uint32_t full_tiles = m.width / kMicroTile, rest = m.width % kMicroTile;
    for (uint32_t y = y0; y < y1; ++y) {
        const uint32_t ty = y / kMicroTile, iy = y % kMicroTile;
        const uint32_t ybase = ((iy & 1) << 1) | ((iy & 2) << 2) | ((iy & 4) << 3);
        const uint8_t* tsrc = src + size_t(ty) * tiles_per_row * tile_bytes + size_t(ybase) * B;
        uint8_t* d = dst + size_t(y) * row;
        for (uint32_t tx = 0; tx < full_tiles; ++tx, tsrc += tile_bytes, d += kMicroTile * B)
            for (uint32_t xi = 0; xi < kMicroTile; ++xi) std::memcpy(d + xi * B, tsrc + kX[xi] * B, B);
        for (uint32_t xi = 0; xi < rest; ++xi) std::memcpy(d + xi * B, tsrc + kX[xi] * B, B);
    }
}

}  // namespace

bool DetileRows(uint32_t tile_index, const uint8_t* src, uint8_t* dst, const MipLayout& m, uint32_t bpe, uint32_t y0,
                uint32_t y1) {
    y1 = std::min(y1, m.height);
    const size_t row = size_t(m.width) * bpe;
    if (tile_index == kTileDisplayLinearAligned) {
        for (uint32_t y = y0; y < y1; ++y) std::memcpy(dst + y * row, src + size_t(y) * m.pitch * bpe, row);
        return true;
    }
    if (tile_index == kTileThin1D) {
        switch (bpe) {
            case 1: ThinRows<1>(src, dst, m, y0, y1); return true;
            case 2: ThinRows<2>(src, dst, m, y0, y1); return true;
            case 4: ThinRows<4>(src, dst, m, y0, y1); return true;
            case 8: ThinRows<8>(src, dst, m, y0, y1); return true;
            case 16: ThinRows<16>(src, dst, m, y0, y1); return true;
            default: break;
        }
        const uint32_t tiles_per_row = m.pitch / kMicroTile;
        const size_t tile_bytes = size_t(kMicroTile) * kMicroTile * bpe;
        for (uint32_t y = y0; y < y1; ++y) {
            const uint32_t ty = y / kMicroTile, iy = y % kMicroTile;
            for (uint32_t x = 0; x < m.width; ++x) {
                const size_t tile = size_t(ty) * tiles_per_row + x / kMicroTile;
                const size_t s = tile * tile_bytes + size_t(ThinMicroIndex(x % kMicroTile, iy)) * bpe;
                std::memcpy(dst + y * row + size_t(x) * bpe, src + s, bpe);
            }
        }
        return true;
    }
    return false;
}

bool Detile(uint32_t tile_index, const uint8_t* src, uint8_t* dst, const MipLayout& m, uint32_t bpe) {
    return DetileRows(tile_index, src, dst, m, bpe, 0, m.height);
}

}  // namespace gpu
