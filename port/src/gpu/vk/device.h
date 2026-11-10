#pragma once
#include "gpu/vk/fsr.h"
#include <vulkan/vulkan.h>

#include <cstdint>
#include <atomic>
#include <functional>
#include <vector>

struct SDL_Window;

namespace vkr {

// The Vulkan instance, device and the window's swapchain. Created on the main thread
// (SDL surface rules); used afterwards from the game's GPU submission thread.
class Device {
public:
    bool Init(SDL_Window* window);

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return queue_family_; }
    VkSurfaceKHR surface() const { return surface_; }
    bool has_external_memory_host() const { return external_memory_host_; }

    // Index of a memory type allowed by `type_bits` that has all of `flags`, or UINT32_MAX.
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    bool external_memory_host_ = false;
};

Device& GetDevice();

// Waits for a fence, reporting (once per wait) if the GPU takes over 5 seconds: a hung shader
// otherwise just looks like a frozen game. Returns false if the device was lost.
bool WaitFence(VkDevice device, VkFence fence, const char* what);

// Shows frames in the window: copies a source into the swapchain image, scaled to fit.
class Presenter {
public:
    bool Init(Device& device, SDL_Window* window);
    // Uploads `pixels` (B8G8R8A8, `pitch` pixels per row) and presents it. Blocks for vsync.
    void PresentPixels(const void* pixels, uint32_t width, uint32_t height, uint32_t pitch);
    // Presents the top-left width x height of a rendered image (kept in VK_IMAGE_LAYOUT_GENERAL).
    // Its bytes are copied as-is, so a memory-order RGBA8 target shows correctly on a BGRA swapchain
    // when the game's display format is A8R8G8B8 (memory B, G, R, A).
    void PresentImage(VkImage image, uint32_t width, uint32_t height);
    // Debug: saves the next presented window image (overlay included) to run/shots/present_<w>x<h>.raw
    // (BGRA). Returns once written, or false after `timeout_ms`.
    bool CaptureNext(uint32_t timeout_ms);

private:
    // Records fill_source (which writes `source_` in TRANSFER_DST layout), scales it into the
    // swapchain image and presents.
    void Present(uint32_t width, uint32_t height, const std::function<void(VkCommandBuffer)>& fill_source);
    bool CreateSwapchain();
    void DestroySwapchain();
    bool EnsureStaging(uint32_t width, uint32_t height);

    Device* dev_ = nullptr;
    SDL_Window* window_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;  // for the overlay
    std::atomic<int> capture_{0};      // 1 requested, 2 recorded (written after its fence), 0 idle
    VkBuffer capture_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory capture_mem_ = VK_NULL_HANDLE;
    VkExtent2D capture_extent_{};
    void WriteCapture();
    void CheckProbe();  // LDPORT_DUPCHECK
    VkBuffer probe_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory probe_mem_ = VK_NULL_HANDLE;
    void* probe_map_ = nullptr;
    bool probe_pending_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;  // one per swapchain image

    // Upload path for raw display buffers (until the renderer draws them).
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* staging_map_ = nullptr;
    VkImage source_ = VK_NULL_HANDLE;
    VkImageView source_view_ = VK_NULL_HANDLE;  // sampled by FSR
    Fsr1 fsr_;
    Fxaa fxaa_;
    uint32_t vsync_ = 0;  // the setting the swapchain was made with
    VkDeviceMemory source_memory_ = VK_NULL_HANDLE;
    uint32_t source_w_ = 0, source_h_ = 0;
};

Presenter& GetPresenter();

}  // namespace vkr
