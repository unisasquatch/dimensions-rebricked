// NVIDIA DLSS (Super Resolution / DLAA) through the NGX SDK (third_party/dlss, NVIDIA RTX SDKs licence).
// Windows on NVIDIA RTX only; everywhere else Available() is false and the DLSS modes use FSR 2.
// This software contains source code provided by NVIDIA Corporation (the SDK's headers and helpers).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace vkr::dlss {

// Extensions NGX needs, added to the lists before the instance / device are created (supported ones only).
void AddInstanceExtensions(std::vector<const char*>& exts);
void AddDeviceExtensions(VkPhysicalDevice phys, std::vector<const char*>& exts);

// Starts NGX on first use; false (once logged) when DLSS can't run on this system.
bool Init(VkInstance instance, VkPhysicalDevice phys, VkDevice device);
bool Available();

struct Image {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageAspectFlags aspect;
};
struct Frame {
    Image color, depth, motion, output;  // inputs in SHADER_READ_ONLY_OPTIMAL, output in GENERAL
    uint32_t render_w, render_h;
    float jitter_x, jitter_y;            // render pixels
    float mv_scale_x, mv_scale_y;        // motion vectors * scale = render pixels, current -> previous
    bool reset;
    bool depth_inverted;
    int quality;                         // 0 Quality, 1 Balanced, 2 Performance, 3 Ultra Performance, 4 DLAA
};
// Records DLSS into `cmd`. Re-creates the feature when sizes, quality or depth direction change.
bool Evaluate(VkCommandBuffer cmd, const Frame& f);
void Shutdown(VkDevice device);

}  // namespace vkr::dlss
