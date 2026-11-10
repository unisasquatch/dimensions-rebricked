#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkr {

// Render targets and textures are kept in "memory order": Vulkan channel 0 is the first
// component in memory. Shader exports are swizzled to match CB_COLOR_INFO.COMP_SWAP, and texture
// views apply the T#'s dst_sel, so a render target can be sampled exactly as the game expects.

// CB_COLOR_INFO FORMAT + NUMBER_TYPE to a Vulkan format (VK_FORMAT_UNDEFINED if unsupported).
VkFormat ColorFormat(uint32_t cb_format, uint32_t number_type);
// Shader-output component order for CB_COLOR_INFO.COMP_SWAP with `components` channels: out[i] = in[map[i]].
void ColorSwap(uint32_t comp_swap, uint32_t components, uint8_t map[4]);
uint32_t ColorComponents(uint32_t cb_format);

// DB_Z_INFO.FORMAT (+ stencil) to a Vulkan depth format.
VkFormat DepthFormat(uint32_t z_format, bool stencil);

// T# DATA_FORMAT + NUM_FORMAT to a Vulkan format; `block` set for BCn compressed formats.
VkFormat TextureFormat(uint32_t data_format, uint32_t num_format, bool& block);
// Bytes per element (per 4x4 block for BCn).
uint32_t TextureElementBytes(uint32_t data_format);

}  // namespace vkr
