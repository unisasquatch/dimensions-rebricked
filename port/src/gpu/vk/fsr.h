// AMD FidelityFX Super Resolution 1 (spatial upscaling: EASU, then RCAS sharpening) for the presenter.
// The shader headers (port/third_party/fsr1, MIT licence) are copied next to the executable and compiled
// at start-up.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkr {

class Device;

class Fsr1 {
public:
    bool Init(Device& device);
    bool ready() const { return ready_; }
    // Records the upscale of `source` (sampled through `source_view`, VK_IMAGE_LAYOUT_GENERAL, the top-left
    // in_w x in_h used) to out_w x out_h. The result is left in output() in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    // red and blue swapped so its bytes copy straight into a B8G8R8A8 swapchain image.
    void Record(VkCommandBuffer cmd, VkImageView source_view, uint32_t src_w, uint32_t src_h, uint32_t in_w,
                uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_stops);
    VkImage output() const { return out_; }

private:
    bool EnsureImages(uint32_t w, uint32_t h);
    VkShaderModule Compile(const char* name, const char* body);

    Device* dev_ = nullptr;
    bool ready_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline easu_ = VK_NULL_HANDLE, rcas_ = VK_NULL_HANDLE;
    VkImage mid_ = VK_NULL_HANDLE, out_ = VK_NULL_HANDLE;
    VkDeviceMemory mid_mem_ = VK_NULL_HANDLE, out_mem_ = VK_NULL_HANDLE;
    VkImageView mid_view_ = VK_NULL_HANDLE, out_view_ = VK_NULL_HANDLE;
    uint32_t w_ = 0, h_ = 0;
    PFN_vkCmdPushDescriptorSetKHR push_ = nullptr;
};

// FXAA (luma edge detection and a short blend along the edge), as a compute pass on the presented picture.
class Fxaa {
public:
    bool Init(Device& device);
    bool ready() const { return ready_; }
    // Filters the top-left w x h of `source_view` (VK_IMAGE_LAYOUT_GENERAL) into output(), an RGBA8 image of
    // src_w x src_h left in VK_IMAGE_LAYOUT_GENERAL (same channel order as the source).
    void Record(VkCommandBuffer cmd, VkImageView source_view, uint32_t src_w, uint32_t src_h, uint32_t w, uint32_t h);
    VkImage output() const { return out_; }
    VkImageView output_view() const { return out_view_; }

private:
    Device* dev_ = nullptr;
    bool ready_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkImage out_ = VK_NULL_HANDLE;
    VkDeviceMemory out_mem_ = VK_NULL_HANDLE;
    VkImageView out_view_ = VK_NULL_HANDLE;
    uint32_t w_ = 0, h_ = 0;
    PFN_vkCmdPushDescriptorSetKHR push_ = nullptr;
};

}  // namespace vkr
