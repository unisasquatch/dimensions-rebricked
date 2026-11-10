#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/gcn/decoder.h"

namespace gcn {

enum class Stage : uint8_t { Vertex, Pixel };

// Texture bindings in descriptor set 0: vertex shader images from 0, pixel shader images from here.
constexpr uint32_t kPixelBindingBase = 16;

// Texture dimensions, from T# word 3 bits 28-31.
enum class ImageType : uint8_t { Tex1D = 8, Tex2D = 9, Tex3D = 10, Cube = 11, Tex1DArray = 12, Tex2DArray = 13 };

// A place where the shader touches memory. The CPU resolves each site before every draw
// (ResolveSites) and writes what it found into the per-draw data block at `data_offset`.
struct Site {
    enum Kind : uint8_t { Smrd, Buffer, Image } kind;
    uint32_t inst_index;   // index into ShaderProgram::code
    uint32_t data_offset;  // in dwords
    // Smrd: dwords loaded. Buffer: 8 dwords (device address lo/hi, stride, records, dfmt, nfmt, dst_sel, pad).
    // Image: binding index; the T#/S# raw words go to the renderer via ResolvedSite.
    uint32_t dwords = 0;
    uint32_t binding = 0;
    bool depth_compare = false;
};

// Inputs that decide the generated code besides the instructions themselves.
struct ShaderKey {
    Stage stage;
    uint32_t user_sgprs = 0;
    uint32_t ps_input_ena = 0;          // SPI_PS_INPUT_ENA (pixel)
    std::vector<ImageType> image_types;  // per image site, from the T# at draw time
    // Pixel: per render target, which shader component each memory-order channel receives.
    uint8_t mrt_map[8][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
                             {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};
    // Pixel: render targets with a 16-bit float format; exports to them saturate at +-65504 like
    // the hardware's conversion (Vulkan's would produce Inf).
    uint8_t fp16_targets = 0;
    // Pixel: SPI_PS_INPUT_CNTL_0..31 - which vertex output feeds each pixel input (bits 0-4, bit 5 =
    // use DEFAULT_VAL bits 8-9 instead), FLAT_SHADE bit 10. Without it input n reads output n.
    bool ps_input_map = false;
    uint32_t ps_input_cntl[32] = {};
    // Resolution scale (LDPORT_SCALE): render targets are `scale` times the game's size. The shader
    // sees the game's coordinates: gl_FragCoord is divided by it, and for the image sites in
    // `scaled_images` (bindings that are render targets) sizes are divided and texel fetches scaled.
    float scale = 1.0f;
    uint32_t scaled_images = 0;
};

struct ShaderProgram {
    Stage stage;
    std::vector<Inst> code;  // fetch shader inlined at its s_swappc call
    std::vector<Site> sites;
    std::vector<uint32_t> resolve_steps;  // scalar ALU + site instructions up to the last site
    uint32_t data_dwords = 0;  // per-draw data block size (user SGPRs + sites)
    uint32_t image_sites = 0;
    uint32_t outputs_mask = 0;  // VS: params written; PS: render targets written
    uint32_t inputs_mask = 0;   // PS: attributes read
    bool writes_depth = false;
    bool has_fetch = false;  // VS: reads vertex buffers (a fetch shader); full-screen passes make their own
    uint64_t content_hash = 0;  // the machine code's hash (stable across runs, unlike id)
    uint64_t id = 0;  // unique per build, for caches keyed by program
    uint64_t address = 0;  // guest address of the shader code (diagnostics)
};

// Decodes the shader (and the fetch shader it calls, if any) and lays out its sites.
ShaderProgram Build(Stage stage, const uint32_t* code, const uint32_t* fetch, uint32_t user_sgprs);

// GLSL 4.60 for Vulkan: per-draw data via a buffer reference in push constants, images as
// combined image samplers in descriptor set 0 (pixel shader images offset by kPixelBindingBase).
std::string GenerateGlsl(const ShaderProgram& program, const ShaderKey& key);

// Debug: "<pixel shader address>:<vgpr>,<vgpr>,<vgpr>" makes that shader output those registers (as floats)
// instead of its colour (LDPORT_DEBUG_PS at start, or the debug remote). Setting it asks the renderer to
// regenerate its shaders (ShaderDebugChanged).
std::string DebugPsSpec();
void SetDebugPsSpec(const std::string& spec);
bool TakeShaderDebugChanged();

// What the CPU found for each site at draw time.
struct ResolvedImage {
    uint32_t tsharp[8];
    uint32_t ssharp[4];
};
// Runs the scalar part of the program on the CPU with this draw's user data, filling the per-draw
// data block (`data`, data_dwords long) and the T#/S# for each image site. `to_device` maps a
// guest address to a GPU device address (0 if unmapped).
void ResolveSites(const ShaderProgram& program, const uint32_t* user_data, uint32_t* data,
                  std::vector<ResolvedImage>& images, uint64_t (*to_device)(uint64_t guest, uint64_t bytes));

}  // namespace gcn
