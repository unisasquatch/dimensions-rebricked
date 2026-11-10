#include "gpu/vk/formats.h"

#include <cstdlib>

namespace vkr {
namespace {

// GCN surface/data formats shared by colour buffers and textures.
enum : uint32_t {
    k8 = 1, k16 = 2, k8_8 = 3, k32 = 4, k16_16 = 5, k10_11_11 = 6, k11_11_10 = 7, k10_10_10_2 = 8,
    k2_10_10_10 = 9, k8_8_8_8 = 10, k32_32 = 11, k16_16_16_16 = 12, k32_32_32 = 13, k32_32_32_32 = 14,
    k5_6_5 = 16, k1_5_5_5 = 17, k5_5_5_1 = 18, k4_4_4_4 = 19,
    kBc1 = 35, kBc2 = 36, kBc3 = 37, kBc4 = 38, kBc5 = 39, kBc6 = 40, kBc7 = 41,
};
// Number types (CB) / num formats (T#).
enum : uint32_t { kUnorm = 0, kSnorm = 1, kUscaled = 2, kSscaled = 3, kUint = 4, kSint = 5, kSrgb = 6, kFloat = 7 };

VkFormat Pick(uint32_t nt, VkFormat unorm, VkFormat snorm, VkFormat uint_, VkFormat sint, VkFormat srgb, VkFormat flt) {
    switch (nt) {
        case kUnorm: case kUscaled: return unorm;
        case kSnorm: case kSscaled: return snorm;
        case kUint: return uint_;
        case kSint: return sint;
        case kSrgb: return srgb ? srgb : unorm;
        case kFloat: return flt;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat Common(uint32_t f, uint32_t nt) {
    constexpr VkFormat U = VK_FORMAT_UNDEFINED;
    switch (f) {
        case k8: return Pick(nt, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8_SRGB, U);
        case k16: return Pick(nt, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, U, VK_FORMAT_R16_SFLOAT);
        case k8_8: return Pick(nt, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SRGB, U);
        case k32: return Pick(nt, U, U, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, U, VK_FORMAT_R32_SFLOAT);
        case k16_16: return Pick(nt, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, U, VK_FORMAT_R16G16_SFLOAT);
        case k10_11_11: case k11_11_10: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case k10_10_10_2: case k2_10_10_10:
            return Pick(nt, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                        VK_FORMAT_A2B10G10R10_SINT_PACK32, U, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        case k8_8_8_8: return Pick(nt, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SRGB, U);
        case k32_32: return Pick(nt, U, U, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, U, VK_FORMAT_R32G32_SFLOAT);
        case k16_16_16_16: return Pick(nt, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, U, VK_FORMAT_R16G16B16A16_SFLOAT);
        case k32_32_32: return Pick(nt, U, U, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT, U, VK_FORMAT_R32G32B32_SFLOAT);
        case k32_32_32_32: return Pick(nt, U, U, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, U, VK_FORMAT_R32G32B32A32_SFLOAT);
        case k5_6_5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case k1_5_5_5: case k5_5_5_1: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case k4_4_4_4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    }
    return U;
}

}  // namespace

// LDPORT_FP32_HDR=1: four-channel half-float render targets are kept as 32-bit floats, so additive
// blending can't overflow to Inf (NVIDIA blends fp16 targets to Inf; the PS4 evidently doesn't).
VkFormat ColorFormat(uint32_t cb_format, uint32_t number_type) {
    const VkFormat f = Common(cb_format, number_type);
    static const bool fp32_hdr = std::getenv("LDPORT_FP32_HDR") != nullptr;
    return fp32_hdr && f == VK_FORMAT_R16G16B16A16_SFLOAT ? VK_FORMAT_R32G32B32A32_SFLOAT : f;
}

uint32_t ColorComponents(uint32_t f) {
    switch (f) {
        case k8: case k16: case k32: return 1;
        case k8_8: case k16_16: case k32_32: return 2;
        case k10_11_11: case k11_11_10: case k5_6_5: case k32_32_32: return 3;
        default: return 4;
    }
}

void ColorSwap(uint32_t swap, uint32_t n, uint8_t map[4]) {
    for (uint8_t i = 0; i < 4; ++i) map[i] = i;
    // Memory component i receives shader component map[i] (AMD CB COMP_SWAP tables).
    if (n == 4) {
        static const uint8_t t[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
        for (int i = 0; i < 4; ++i) map[i] = t[swap & 3][i];
    } else if (n == 3) {
        static const uint8_t t[4][3] = {{0, 1, 2}, {2, 1, 0}, {0, 1, 2}, {2, 1, 0}};
        for (int i = 0; i < 3; ++i) map[i] = t[swap & 3][i];
    } else if (n == 2) {
        static const uint8_t t[4][2] = {{0, 1}, {0, 3}, {1, 0}, {3, 0}};
        for (int i = 0; i < 2; ++i) map[i] = t[swap & 3][i];
    } else {
        static const uint8_t t[4] = {0, 1, 2, 3};  // X, Y, Z, W
        map[0] = t[swap & 3];
    }
}

VkFormat DepthFormat(uint32_t z_format, bool stencil) {
    if (z_format == 1) return stencil ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D16_UNORM;  // Z_16
    if (z_format == 3) return stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;  // Z_32_FLOAT
    return stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
}

VkFormat TextureFormat(uint32_t f, uint32_t nf, bool& block) {
    block = f >= kBc1 && f <= kBc7;
    const bool srgb = nf == kSrgb;
    switch (f) {
        case kBc1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case kBc2: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        case kBc3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        case kBc4: return nf == kSnorm ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK;
        case kBc5: return nf == kSnorm ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK;
        case kBc6: return nf == kSnorm ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case kBc7: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    }
    // Texture num_format 9 is FLOAT for T#s (CB uses 7).
    return Common(f, nf == 9 ? kFloat : nf);
}

uint32_t TextureElementBytes(uint32_t f) {
    switch (f) {
        case k8: return 1;
        case k16: case k8_8: case k5_6_5: case k1_5_5_5: case k5_5_5_1: case k4_4_4_4: return 2;
        case k32: case k16_16: case k10_11_11: case k11_11_10: case k10_10_10_2: case k2_10_10_10: case k8_8_8_8: return 4;
        case k32_32: case k16_16_16_16: return 8;
        case k32_32_32: return 12;
        case k32_32_32_32: return 16;
        case kBc1: case kBc4: return 8;
        case kBc2: case kBc3: case kBc5: case kBc6: case kBc7: return 16;
    }
    return 4;
}

}  // namespace vkr
