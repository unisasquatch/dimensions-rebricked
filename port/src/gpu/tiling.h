#pragma once
#include <cstdint>
#include <vector>

// PS4 (GCN "Liverpool") texture memory layouts. A texture's T# names a GB_TILE_MODE index;
// these are the ones the game uses, from the Gnm tile-mode table.
namespace gpu {

enum TileIndex : uint32_t {
    kTileDisplayLinearAligned = 8,
    kTileThin1D = 13,
};

// One mip level, in elements (pixels, or 4x4 blocks for BCn formats).
struct MipLayout {
    uint64_t offset;      // bytes from the texture base
    uint64_t size;        // bytes
    uint32_t width, height;     // real size in elements
    uint32_t pitch, padded_height;  // allocated size in elements
};

// Mip chain layout for a 2D texture.
// width/height are in pixels; `pitch0` is the T# pitch in pixels; `block` for BCn (4x4 elements).
std::vector<MipLayout> LayoutMips(uint32_t tile_index, uint32_t width, uint32_t height, uint32_t pitch0,
                                  uint32_t levels, uint32_t bytes_per_element, bool block, bool pow2_pad);

// Copies one level from guest memory to a tightly packed linear buffer (width x height elements).
// Returns false for tile modes this port doesn't handle yet.
bool Detile(uint32_t tile_index, const uint8_t* src, uint8_t* dst, const MipLayout& mip, uint32_t bytes_per_element);
// The same for element rows y0..y1 only (dst is still the level's start), so one level can be split into strips.
bool DetileRows(uint32_t tile_index, const uint8_t* src, uint8_t* dst, const MipLayout& mip, uint32_t bytes_per_element,
                uint32_t y0, uint32_t y1);

bool IsTileModeSupported(uint32_t tile_index);

}  // namespace gpu
