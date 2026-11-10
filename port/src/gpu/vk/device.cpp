#include "gpu/vk/device.h"
#include "hle/port_settings.h"
#include "ui/overlay.h"
#include "gpu/vk/renderer.h"
#include "gpu/vk/dlss.h"

#include <cmath>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace vkr {

// Presentation timing (diagnostics).
double g_fence_ms = 0, g_acquire_ms = 0, g_present_ms = 0;
uint64_t g_presents = 0;
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

bool HasExtension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

bool Device::Init(SDL_Window* window) {
    const bool validation = std::getenv("LDPORT_VK_VALIDATION") != nullptr;

    uint32_t sdl_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    std::vector<const char*> exts(sdl_exts, sdl_exts + sdl_count);
    std::vector<const char*> layers;
    if (validation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    dlss::AddInstanceExtensions(exts);  // NVIDIA DLSS (only those this system has)

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LEGO Dimensions port";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateInstance failed\n");
        return false;
    }
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = OnValidation;
        if (create) create(instance_, &mci, nullptr, &messenger_);
    }
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        std::fprintf(stderr, "[vulkan] surface: %s\n", SDL_GetError());
        return false;
    }

    // Prefer a discrete GPU with a queue that can draw and present.
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qfs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            const bool better = !physical_ || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (better) {
                physical_ = pd;
                queue_family_ = q;
            }
            break;
        }
    }
    if (!physical_) {
        std::fprintf(stderr, "[vulkan] no suitable GPU\n");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    external_memory_host_ = HasExtension(physical_, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);

    std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (external_memory_host_) dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (props.vendorID == 0x10DE) dlss::AddDeviceExtensions(physical_, dev_exts);  // NVIDIA: DLSS's

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;  // GLSL discard compiles to demote
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.geometryShader = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.imageCubeArray = VK_TRUE;
    f2.features.shaderImageGatherExtended = VK_TRUE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateDevice failed\n");
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    std::fprintf(stderr, "[vulkan] %s, API %u.%u, external host memory %s%s\n", props.deviceName,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 external_memory_host_ ? "yes" : "no", validation ? ", validation on" : "");
    return true;
}

uint32_t Device::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool WaitFence(VkDevice device, VkFence fence, const char* what) {
    constexpr uint64_t kReportNs = 5'000'000'000ull;
    bool reported = false;
    for (;;) {
        const VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, kReportNs);
        if (r == VK_SUCCESS) return true;
        if (r != VK_TIMEOUT) {
            std::fprintf(stderr, "[vulkan] waiting for %s failed (%d): device lost?\n", what, int(r));
            return false;
        }
        if (!reported) std::fprintf(stderr, "[vulkan] GPU still busy with %s after 5 s (hung shader?)\n", what);
        reported = true;
    }
}

Device& GetDevice() {
    static Device d;
    return d;
}

// --- Presenter ---

bool Presenter::Init(Device& device, SDL_Window* window) {
    dev_ = &device;
    window_ = window;
    VkDevice d = dev_->device();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev_->queue_family();
    vkCreateCommandPool(d, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(d, &cai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(d, &fci, nullptr, &fence_);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(d, &sci, nullptr, &acquired_);
    fsr_.Init(device);  // FSR 1 resolution modes; without it they fall back to the bilinear blit
    fxaa_.Init(device);
    return CreateSwapchain();
}

bool Presenter::CreateSwapchain() {
    VkDevice d = dev_->device();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev_->physical(), dev_->surface(), &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;  // minimised
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent_ = {uint32_t(w), uint32_t(h)};
    }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = dev_->surface();
    sci.minImageCount = count;
    sci.imageFormat = format_;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // VSync (Options menu): Fast = mailbox (no tearing, never blocks: with FIFO a present waits for a free image
    // at vblank and every later submission - the next frame's rendering - waits behind it), On = FIFO,
    // Off = immediate (may tear). The game clock holds the game to 60 Hz either way. LDPORT_PRESENT=fifo|mailbox|
    // immediate overrides; a mode the driver lacks falls back to FIFO.
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        vsync_ = port::Get().vsync.load();
        VkPresentModeKHR want = vsync_ == 1 ? VK_PRESENT_MODE_FIFO_KHR
                              : vsync_ == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
        if (const char* e = std::getenv("LDPORT_PRESENT")) {
            const std::string s = e;
            want = s == "fifo" ? VK_PRESENT_MODE_FIFO_KHR : s == "immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                             : VK_PRESENT_MODE_MAILBOX_KHR;
        }
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical(), dev_->surface(), &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical(), dev_->surface(), &n, modes.data());
        if (std::find(modes.begin(), modes.end(), want) != modes.end()) sci.presentMode = want;
        std::fprintf(stderr, "[present] mode %s\n", sci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                     : sci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    }
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(d, &sci, nullptr, &next) != VK_SUCCESS) return false;
    DestroySwapchain();
    swapchain_ = next;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(d, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(d, swapchain_, &n, images_.data());
    views_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(d, &vci, nullptr, &views_[i]);
    }
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    rendered_.resize(n);
    for (auto& s : rendered_) vkCreateSemaphore(d, &semi, nullptr, &s);
    return true;
}

void Presenter::DestroySwapchain() {
    VkDevice d = dev_->device();
    vkDeviceWaitIdle(d);
    for (VkSemaphore s : rendered_) vkDestroySemaphore(d, s, nullptr);
    rendered_.clear();
    for (VkImageView v : views_) vkDestroyImageView(d, v, nullptr);
    views_.clear();
    if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Presenter::EnsureStaging(uint32_t width, uint32_t height) {
    if (source_ && source_w_ == width && source_h_ == height) return true;
    VkDevice d = dev_->device();
    vkDeviceWaitIdle(d);
    if (staging_) {
        vkDestroyBuffer(d, staging_, nullptr);
        vkFreeMemory(d, staging_memory_, nullptr);
        if (source_view_) vkDestroyImageView(d, source_view_, nullptr);
        source_view_ = VK_NULL_HANDLE;
        vkDestroyImage(d, source_, nullptr);
        vkFreeMemory(d, source_memory_, nullptr);
    }
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(d, &bci, nullptr, &staging_);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, staging_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(d, &mai, nullptr, &staging_memory_);
    vkBindBufferMemory(d, staging_, staging_memory_, 0);
    vkMapMemory(d, staging_memory_, 0, bytes, 0, &staging_map_);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(d, &ici, nullptr, &source_);
    vkGetImageMemoryRequirements(d, source_, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(d, &mai, nullptr, &source_memory_);
    vkBindImageMemory(d, source_, source_memory_, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = source_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(d, &vci, nullptr, &source_view_);
    source_w_ = width;
    source_h_ = height;
    return true;
}

namespace {

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

void Presenter::PresentPixels(const void* pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!swapchain_ && !CreateSwapchain()) return;
    WaitFence(dev_->device(), fence_, "the previous present");
    EnsureStaging(width, height);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(static_cast<uint8_t*>(staging_map_) + size_t(y) * width * 4,
                    static_cast<const uint8_t*>(pixels) + size_t(y) * pitch * 4, size_t(width) * 4);
    Present(width, height, [&](VkCommandBuffer cmd) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging_, source_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    });
}

// LDPORT_DUPCHECK=1 (experiment): each present copies a 512x512 block from the middle of the window image;
// once its fence has passed it is compared with the previous one, counting presents that show nothing new.
static const bool g_dupcheck = std::getenv("LDPORT_DUPCHECK") != nullptr;
constexpr uint32_t kProbe = 512;

void Presenter::CheckProbe() {
    if (!probe_pending_) return;
    probe_pending_ = false;
    const size_t bytes = size_t(kProbe) * kProbe * 4;
    std::vector<uint8_t> cur(static_cast<const uint8_t*>(probe_map_), static_cast<const uint8_t*>(probe_map_) + bytes);
    static std::vector<uint8_t> prev;
    static uint32_t n = 0, same = 0, run = 0, longest = 0;
    static uint64_t sum_diff = 0;
    if (prev.size() == bytes) {
        uint64_t diff = 0;
        for (size_t i = 0; i < bytes; i += 4)
            for (int c = 0; c < 3; ++c) diff += uint64_t(std::abs(int(cur[i + c]) - int(prev[i + c])));
        if (diff == 0) {
            ++same;
            longest = std::max(longest, ++run);
        } else {
            run = 0;
        }
        sum_diff += diff;
        static double seq[40];  // the last 40 presents' change per pixel, oldest first
        if (n >= 560) seq[n - 560] = double(diff) / (kProbe * kProbe);
        if (++n == 600) {
            std::fprintf(stderr, "[dupcheck] 600 presents: %u identical to the one before (%.0f%%), longest run %u; mean change %.2f/px\n",
                         same, same / 6.0, longest, double(sum_diff) / 600 / (kProbe * kProbe));
            std::fprintf(stderr, "[dupcheck] last 40:");
            for (double v : seq) std::fprintf(stderr, " %.1f", v);
            std::fprintf(stderr, "\n");
            n = same = longest = 0;
            sum_diff = 0;
        }
    }
    prev.swap(cur);
}

void Presenter::PresentImage(VkImage image, uint32_t width, uint32_t height) {
    if (!swapchain_ && !CreateSwapchain()) return;
    WriteCapture();
    const auto t_fence = std::chrono::steady_clock::now();
    WaitFence(dev_->device(), fence_, "the previous present");
    g_fence_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_fence).count();
    CheckProbe();
    EnsureStaging(width, height);
    Present(width, height, [&](VkCommandBuffer cmd) {
        // A raw copy keeps the bytes in memory order (32-bit formats are copy-compatible).
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {width, height, 1};
        vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, source_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    });
}

void Presenter::Present(uint32_t width, uint32_t height, const std::function<void(VkCommandBuffer)>& fill_source) {
    VkDevice d = dev_->device();
    if (port::Get().vsync.load() != vsync_) CreateSwapchain();  // VSync changed in the Options menu
    uint32_t index = 0;
    const auto t_acquire = std::chrono::steady_clock::now();
    VkResult r = vkAcquireNextImageKHR(d, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    g_acquire_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_acquire).count();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        CreateSwapchain();
        return;
    }
    vkResetFences(d, 1, &fence_);
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    Barrier(cmd_, source_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    fill_source(cmd_);
    Barrier(cmd_, source_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage target = images_[index];
    Barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Aspect ratio (Options menu): Fit letterboxes to the game's 16:9, Stretch fills the window,
    // Zoom fills it by cropping the picture's edges.
    const uint32_t aspect = port::Get().aspect.load();
    const float sx = float(extent_.width) / width, sy = float(extent_.height) / height;
    int32_t src_x0 = 0, src_y0 = 0, src_x1 = int32_t(width), src_y1 = int32_t(height);
    int32_t x = 0, y = 0, w = int32_t(extent_.width), h = int32_t(extent_.height);
    if (aspect == 2) {  // Zoom: crop the source to the window's shape
        const float s = std::max(sx, sy);
        const int32_t cw = int32_t(extent_.width / s), ch = int32_t(extent_.height / s);
        src_x0 = (int32_t(width) - cw) / 2;
        src_y0 = (int32_t(height) - ch) / 2;
        src_x1 = src_x0 + cw;
        src_y1 = src_y0 + ch;
    } else if (aspect != 1) {  // Fit
        const float s = std::min(sx, sy);
        w = int32_t(width * s);
        h = int32_t(height * s);
        x = (int32_t(extent_.width) - w) / 2;
        y = (int32_t(extent_.height) - h) / 2;
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {src_x0, src_y0, 0};
    blit.srcOffsets[1] = {src_x1, src_y1, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    // Resolution modes with FSR 1: the render scale follows the picture's height on screen, and the picture
    // is upscaled by EASU + RCAS instead of a bilinear blit (Fit and Stretch; Zoom crops, so it blits).
    const uint32_t res_mode = port::Get().scale.load();
    const bool fsr_mode = port::FsrRatio(res_mode) > 0.0f;
    if (h > 0) port::SetPictureHeight(uint32_t(h));
    if (fsr_mode && h > 0) {
        // The target is the chosen Resolution, at most the picture's height on screen (upscalers output there).
        const uint32_t target = port::ResolutionHeight(port::Get().resolution.load());
        const float out_h = target ? std::min(float(target), float(h)) : float(h);
        const float want = port::ScaleForMode(res_mode, out_h);
        if (std::fabs(want - CurrentRenderScale()) > 0.004f) SetRenderScale(want);
    }
    if (port::Fsr2Mode(res_mode)) SetUpscaleOutput(aspect != 2 ? uint32_t(w) : 0, aspect != 2 ? uint32_t(h) : 0);
    // Anti-aliasing (Options menu): FXAA on the picture at its rendered size, before any upscaling.
    VkImage pic = source_;
    VkImageView pic_view = source_view_;
    VkImageLayout pic_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (port::Get().aa.load() == 1 && fxaa_.ready()) {
        Barrier(cmd_, source_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        fxaa_.Record(cmd_, source_view_, source_w_, source_h_, width, height);
        pic = fxaa_.output();
        pic_view = fxaa_.output_view();
        pic_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (fsr_mode && fsr_.ready() && aspect != 2 && (w > int32_t(width) || h > int32_t(height))) {
        if (pic == source_) Barrier(cmd_, source_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        fsr_.Record(cmd_, pic_view, source_w_, source_h_, width, height, uint32_t(w), uint32_t(h), 0.2f);
        VkImageCopy up{};
        up.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        up.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        up.dstOffset = {x, y, 0};
        up.extent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyImage(cmd_, fsr_.output(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
    } else {
        vkCmdBlitImage(cmd_, pic, pic_layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (ui::PickerOpen() && index < views_.size()) {  // the Toy Pad picker over the game
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ui::Record(cmd_, views_[index], extent_, format_);
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    if (capture_ == 1) {
        if (!capture_buf_ || capture_extent_.width != extent_.width || capture_extent_.height != extent_.height) {
            if (capture_buf_) {
                vkDestroyBuffer(d, capture_buf_, nullptr);
                vkFreeMemory(d, capture_mem_, nullptr);
            }
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = VkDeviceSize(extent_.width) * extent_.height * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(d, &bci, nullptr, &capture_buf_);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, capture_buf_, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(d, &mai, nullptr, &capture_mem_);
            vkBindBufferMemory(d, capture_buf_, capture_mem_, 0);
            capture_extent_ = extent_;
        }
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buf_, 1, &c);
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        capture_ = 2;
    }
    if (g_dupcheck && extent_.width >= kProbe && extent_.height >= kProbe) {
        if (!probe_buf_) {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = VkDeviceSize(kProbe) * kProbe * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(d, &bci, nullptr, &probe_buf_);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, probe_buf_, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX)
                mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(d, &mai, nullptr, &probe_mem_);
            vkBindBufferMemory(d, probe_buf_, probe_mem_, 0);
            vkMapMemory(d, probe_mem_, 0, VK_WHOLE_SIZE, 0, &probe_map_);
        }
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {int32_t(extent_.width - kProbe) / 2, int32_t(extent_.height - kProbe) / 2, 0};
        c.imageExtent = {kProbe, kProbe, 1};
        vkCmdCopyImageToBuffer(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe_buf_, 1, &c);
        Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        probe_pending_ = true;
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    vkQueueSubmit(dev_->queue(), 1, &si, fence_);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const auto t_present = std::chrono::steady_clock::now();
    r = vkQueuePresentKHR(dev_->queue(), &pi);
    g_present_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_present).count();
    if (++g_presents % 120 == 0) {
        std::fprintf(stderr, "[present] per frame: fence wait %.1f ms, acquire %.1f ms, present %.1f ms\n", g_fence_ms / 120, g_acquire_ms / 120, g_present_ms / 120);
        g_fence_ms = g_acquire_ms = g_present_ms = 0;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) CreateSwapchain();
}

bool Presenter::CaptureNext(uint32_t timeout_ms) {
    capture_ = 1;
    for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
        if (capture_ == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void Presenter::WriteCapture() {
    if (capture_ != 2) return;
    WaitFence(dev_->device(), fence_, "a capture");
    void* p = nullptr;
    const VkDeviceSize bytes = VkDeviceSize(capture_extent_.width) * capture_extent_.height * 4;
    vkMapMemory(dev_->device(), capture_mem_, 0, bytes, 0, &p);
    std::filesystem::create_directories("run/shots");
    char name[96];
    std::snprintf(name, sizeof name, "run/shots/present_%ux%u.raw", capture_extent_.width, capture_extent_.height);
    std::ofstream(name, std::ios::binary).write(static_cast<const char*>(p), std::streamsize(bytes));
    vkUnmapMemory(dev_->device(), capture_mem_);
    capture_ = 0;
}

Presenter& GetPresenter() {
    static Presenter p;
    return p;
}

}  // namespace vkr
