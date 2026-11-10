#include "gpu/vk/renderer.h"

#include "gpu/capture.h"
#include "hle/button_icons.h"
#include "hle/hle.h"

#include <shaderc/shaderc.h>
#include <ffx_fsr2.h>
#include <vk/ffx_fsr2_vk.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include "hle/port_settings.h"
#include "host/memory.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <vector>

#include "gpu/gcn/translator.h"
#include "gpu/tiling.h"
#include "gpu/vk/device.h"
#include "gpu/vk/dlss.h"
#include "gpu/vk/formats.h"
#include "host/guest_memory.h"

namespace vkr {
namespace {

using gpu::Registers;
namespace reg = gpu::reg;

constexpr VkDeviceSize kRingSize = 64ull << 20;  // per frame in flight: draw data and generated indices
// Frames the GPU may still be working on while the next is recorded (LDPORT_FRAMES=1: one at a time).
constexpr uint32_t kMaxFrames = 2;
uint32_t FramesInFlight() {
    static const uint32_t n = std::getenv("LDPORT_FRAMES") && std::atoi(std::getenv("LDPORT_FRAMES")) == 1 ? 1 : kMaxFrames;
    return n;
}
constexpr uint32_t kMaxBindings = 32;

// Context registers used here beyond gpu/registers.h.
constexpr uint32_t kDbRenderControl = 0x000, kDbStencilClear = 0x00A, kDbDepthClear = 0x00B, kDbStencilInfo = 0x011,
                   kCbBlendRed = 0x105, kDbStencilControl = 0x10B, kDbStencilRefMask = 0x10C,
                   kDbStencilRefMaskBf = 0x10D, kPaScGenericScissorTl = 0x090, kPaScScreenScissorTl = 0x00C,
                   kPaClVteCntl = 0x206, kVgtIndxOffset = 0x102;

VkCompareOp CompareOp(uint32_t f);

// FNV-1a, byte by byte. Kept for what is stored on disk (the SPIR-V cache is named by GLSL hash).
uint64_t Hash(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Eight bytes per step: for per-draw keys and texture change detection, which run thousands of
// times a frame (the byte-wise hash was a large share of recording time).
uint64_t FastHash(const void* data, size_t bytes, uint64_t h = 0x9E3779B97F4A7C15ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (; bytes >= 8; p += 8, bytes -= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; bytes; ++p, --bytes) h = (h ^ *p) * 1099511628211ull;
    return h;
}

uint32_t ProgramDwords(const gcn::ShaderProgram& p) {
    uint32_t n = 0;
    for (const auto& in : p.code) n += in.size;
    return n;
}

struct Import {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    uint64_t base = 0, size = 0;
};

// Resolution scale: LDPORT_SCALE=2 renders every target at twice the game's size (1080p -> 2160p).
// Fractional scales (1.333 = 1440p, 0.667 = 720p) feed the upscalers.
std::atomic<float> g_render_scale{0.0f}, g_wanted_scale{0.0f};
float RenderScale() {
    float s = g_render_scale.load(std::memory_order_relaxed);
    if (s <= 0.0f) {
        const char* e = std::getenv("LDPORT_SCALE");
        const double v = e ? std::strtod(e, nullptr) : 1.0;
        s = float(v >= 0.25 && v <= 4.0 ? v : 1.0);
        g_render_scale = s;
    }
    return s;
}
#ifdef _WIN32
// Debug write watch on the camera block's page (LDPORT_WATCH_CAMERA): logs the writing code, then lifts itself.
std::atomic<uint64_t> g_watch_page{0}, g_watch_block{0}, g_watch_rearm{0};
DWORD g_watch_old = PAGE_READWRITE;
int g_watch_arms = 0;
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watch_rearm.load()) {  // past the other write: watch again
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_watch_rearm.load()), 0x1000, PAGE_READONLY, &old);
        g_watch_page = g_watch_rearm.load();
        g_watch_rearm = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint64_t page = g_watch_page.load();
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !page || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t addr = er->ExceptionInformation[1];
    if ((addr & ~uint64_t(0xFFF)) != page) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000, g_watch_old, &old);
    g_watch_page = 0;
    const uint64_t block = g_watch_block.load();
    if (addr < block || addr >= block + 160) {  // another write on the page: step over it, then re-protect
        g_watch_rearm = page;
        info->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const CONTEXT* c = info->ContextRecord;
    std::fprintf(stderr, "[watch] write to %llx (page +%llx) at rip %llx; stack:", (unsigned long long)addr,
                 (unsigned long long)(addr & 0xFFF), (unsigned long long)c->Rip);
    const uint64_t* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
    for (int i = 0; i < 160; ++i) {
        const uint64_t v = sp[i];
        if ((v >> 36) == (c->Rip >> 36) && v != c->Rip) std::fprintf(stderr, " %llx", (unsigned long long)v);
    }
    std::fprintf(stderr, "\n[watch] regs rdi %llx rsi %llx rdx %llx rcx %llx r8 %llx r9 %llx rax %llx rbx %llx\n",
                 (unsigned long long)c->Rdi, (unsigned long long)c->Rsi, (unsigned long long)c->Rdx,
                 (unsigned long long)c->Rcx, (unsigned long long)c->R8, (unsigned long long)c->R9,
                 (unsigned long long)c->Rax, (unsigned long long)c->Rbx);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

// FSR 2 modes: the size the presenter shows the picture at (0 when FSR 2 can't be used).
std::atomic<uint32_t> g_up_w{0}, g_up_h{0};
std::atomic<bool> g_dlss_tried{false};  // NGX started (or tried) for a DLSS mode
std::atomic<int> g_f2_debug{0};  // debug: bit 0 flips the jitter's x sign for FSR 2, bit 1 its y, bit 2 no jitter
// A size at the render scale (at least 1).
uint32_t Scaled(uint32_t v) { return std::max(1u, uint32_t(std::lround(double(v) * RenderScale()))); }

struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;  // the game's size
    uint32_t pw = 0, ph = 0;          // the image's size (scaled)
    uint64_t address = 0;
    bool depth = false;
};

struct Program {
    gcn::ShaderProgram program;
    uint64_t code_hash = 0;
    uint64_t checked_frame = ~0ull;  // shader bytes are re-verified once per frame
};

struct Dummy {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

}  // namespace

struct Renderer::Impl {
    Device* dev = nullptr;
    VkDevice d = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // the frame being recorded (cmds[slot])
    VkFence fence = VK_NULL_HANDLE;        // its fence (fences[slot])
    VkCommandBuffer cmds[kMaxFrames]{};
    VkFence fences[kMaxFrames]{};
    uint32_t slot = 0;
    // Texture memory: sub-allocated from 64 MB blocks instead of one vkAllocateMemory per texture (a new area
    // creates ~100 textures in one frame, and each allocation cost ~0.1 ms). Large textures get their own.
    struct TexAlloc {
        uint32_t block = ~0u;  // ~0u: not pooled
        VkDeviceSize offset = 0, size = 0;
    };
    struct PoolBlock {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t type = 0;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size, coalesced
    };
    std::vector<PoolBlock> pool_blocks;
    static constexpr VkDeviceSize kPoolBlock = 64ull << 20, kPoolMax = 16ull << 20;
    bool PoolAllocate(const VkMemoryRequirements& req, TexAlloc& out, VkDeviceMemory& memory) {
        if (req.size > kPoolMax) return false;
        const uint32_t type = dev->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 256);
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t b = 0; b < pool_blocks.size(); ++b) {
                PoolBlock& pb = pool_blocks[b];
                if (pb.type != type) continue;
                for (auto it = pb.free.begin(); it != pb.free.end(); ++it) {
                    const VkDeviceSize start = (it->first + align - 1) & ~(align - 1);
                    const VkDeviceSize end = it->first + it->second;
                    if (start + req.size > end) continue;
                    const VkDeviceSize f_off = it->first;
                    pb.free.erase(it);
                    if (start > f_off) pb.free[f_off] = start - f_off;  // the alignment gap stays free
                    if (end > start + req.size) pb.free[start + req.size] = end - (start + req.size);
                    out = {b, start, req.size};
                    memory = pb.memory;
                    return true;
                }
            }
            if (pass == 1) break;
            VkMemoryRequirements breq = req;
            breq.size = kPoolBlock;
            breq.memoryTypeBits = 1u << type;
            PoolBlock nb;
            nb.memory = Allocate(breq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
            if (!nb.memory) return false;
            nb.type = type;
            nb.free[0] = kPoolBlock;
            pool_blocks.push_back(std::move(nb));
            std::fprintf(stderr, "[renderer] texture pool: %zu blocks (%llu MB)\n", pool_blocks.size(),
                         (unsigned long long)(pool_blocks.size() * (kPoolBlock >> 20)));
        }
        return false;
    }
    void PoolFree(const TexAlloc& a) {
        if (a.block >= pool_blocks.size()) return;
        auto& free = pool_blocks[a.block].free;
        VkDeviceSize off = a.offset, size = a.size;
        auto next = free.lower_bound(off);
        if (next != free.end() && off + size == next->first) {
            size += next->second;
            next = free.erase(next);
        }
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == off) {
                prev->second += size;
                return;
            }
        }
        free[off] = size;
    }
    // A sampled texture image; its memory comes from the pool when it fits (memory is then null: the block owns it).
    VkImage CreateTextureImage(VkImageType type, VkFormat format, uint32_t w, uint32_t h, uint32_t depth,
                               uint32_t layers, uint32_t mips, VkImageCreateFlags flags, VkDeviceMemory& memory,
                               TexAlloc& alloc) {
        alloc = {};
        memory = VK_NULL_HANDLE;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.flags = flags;
        ici.imageType = type;
        ici.format = format;
        ici.extent = {w, h, depth};
        ici.mipLevels = mips;
        ici.arrayLayers = layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(d, &ici, nullptr, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, image, &req);
        static const bool no_pool = std::getenv("LDPORT_NO_TEX_POOL") != nullptr;
        VkDeviceMemory block_memory = VK_NULL_HANDLE;
        if (!no_pool && PoolAllocate(req, alloc, block_memory)) {
            vkBindImageMemory(d, image, block_memory, alloc.offset);
            return image;
        }
        alloc = {};
        memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        if (!memory) {  // never bind a null allocation: the driver faults on it later
            vkDestroyImage(d, image, nullptr);
            return VK_NULL_HANDLE;
        }
        vkBindImageMemory(d, image, memory, 0);
        return image;
    }

    // Objects replaced while a frame may still use them, destroyed once the slot they were retired in
    // has come round again (its fence waited: every frame that could use them is done).
    struct Retired {
        std::vector<VkImageView> views;
        std::vector<std::pair<VkImage, VkDeviceMemory>> images;
        std::vector<TexAlloc> allocs;  // pooled texture memory, freed after the images
    } retired[kMaxFrames];
    void DestroyRetired(uint32_t s) {
        for (VkImageView v : retired[s].views) vkDestroyImageView(d, v, nullptr);
        for (auto [image, memory] : retired[s].images) {
            vkDestroyImage(d, image, nullptr);
            if (memory) vkFreeMemory(d, memory, nullptr);
        }
        for (const TexAlloc& a : retired[s].allocs) PoolFree(a);
        retired[s] = {};
    }
    // Waits for every frame still on the GPU (dumps, syncs).
    void WaitAll(const char* what) {
        for (uint32_t i = 0; i < FramesInFlight(); ++i) WaitFence(d, fences[i], what);
    }
    bool recording = false;
    bool rendering = false;
    PFN_vkCmdPushDescriptorSetKHR push_descriptor = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_pointer_props = nullptr;
    VkDeviceSize import_alignment = 4096;

    // Per-frame ring: draw data blocks and generated index buffers.
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ring_memory = VK_NULL_HANDLE;
    uint8_t* ring_map = nullptr;
    VkDeviceAddress ring_address = 0;
    VkDeviceSize ring_used = 0, ring_end = 0;  // this slot's part of the ring

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, shadow_sampler = VK_NULL_HANDLE;
    std::map<gcn::ImageType, Dummy> dummies;
    shaderc_compiler_t compiler = nullptr;

    std::unordered_map<uint32_t, Import> imports;  // by guest range id
    std::map<std::tuple<uint64_t, VkFormat, uint32_t, uint32_t>, Target> targets;
    std::unordered_map<uint64_t, Program> programs;          // by (address ^ fetch) key
    std::unordered_map<uint64_t, VkShaderModule> modules;    // by GLSL hash
    std::unordered_map<uint64_t, VkPipeline> pipelines;      // by state hash

    // Current dynamic-rendering attachments.
    std::array<const Target*, 8> bound_color{};
    const Target* bound_depth = nullptr;
    VkExtent2D bound_extent{};

    uint64_t frame = 0;
    uint64_t draws = 0, skipped = 0, draws_at_flip = 0, frame_draws = 0;
    uint64_t gpu_wait_us = 0;  // time spent waiting for the previous frame (diagnostics)
    uint64_t record_us = 0;    // CPU time recording draws (includes those waits)
    // Frame pacing: the longest gap between flips, and how many frames missed a 60 Hz refresh.
    std::chrono::steady_clock::time_point last_flip{};
    double worst_flip_ms = 0;
    uint32_t late_flips = 0;
    VkQueryPool timestamps = VK_NULL_HANDLE;  // GPU time per frame (diagnostics), this slot's
    VkQueryPool timestamp_pools[kMaxFrames]{};
    bool timestamps_valid_slot[kMaxFrames]{};
    // LDPORT_FLASH_COUNT=1: each displayed frame is shrunk to 16x9 on the GPU and read back a frame
    // later (no extra waits), to count flash-coloured frames without disturbing timing.
    const bool flash_count = std::getenv("LDPORT_FLASH_COUNT") != nullptr;
    VkImage probe_img = VK_NULL_HANDLE;
    VkDeviceMemory probe_img_mem = VK_NULL_HANDLE, probe_buf_mem = VK_NULL_HANDLE;
    VkBuffer probe_buf = VK_NULL_HANDLE;
    const uint8_t* probe_map = nullptr;
    bool probe_pending = false;
    uint64_t probe_frames = 0, probe_flashes = 0, probe_frame_id = 0;
    void ProbeRecord(uint64_t display_address, uint64_t frame_id) {
        const Target* t = FindTargetByAddress(display_address);
        if (!t || t->format != VK_FORMAT_R8G8B8A8_UNORM || !recording) return;
        if (!probe_img) {
            probe_img = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 16, 9, 1, 1, 1,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, probe_img_mem);
            ToGeneral(probe_img, VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = 16 * 9 * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(d, &bci, nullptr, &probe_buf);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, probe_buf, &req);
            probe_buf_mem = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
            vkBindBufferMemory(d, probe_buf, probe_buf_mem, 0);
            void* m;
            vkMapMemory(d, probe_buf_mem, 0, VK_WHOLE_SIZE, 0, &m);
            probe_map = static_cast<const uint8_t*>(m);
        }
        EndRendering();
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(t->pw), int32_t(t->ph), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {16, 9, 1};
        vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, probe_img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        FullBarrier();
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {16, 9, 1};
        vkCmdCopyImageToBuffer(cmd, probe_img, VK_IMAGE_LAYOUT_GENERAL, probe_buf, 1, &c);
        FullBarrier();
        probe_pending = true;
        probe_frame_id = frame_id;
    }
    void ProbeRead() {  // after the frame's fence has signalled
        if (!probe_pending) return;
        probe_pending = false;
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int y = 2; y < 7; ++y)  // inside the letterbox bars
            for (int x = 0; x < 16; ++x, ++n) {
                const uint8_t* px = probe_map + (y * 16 + x) * 4;
                r += px[0]; g += px[1]; b += px[2];
            }
        r /= n; g /= n; b /= n;
        ++probe_frames;
        const bool flash = (g < 15 && (b > 150 || r > 150)) || (g > 150 && r < 60 && b < 60);
        if (flash) {
            ++probe_flashes;
            std::fprintf(stderr, "[flash] frame %llu colour %u,%u,%u\n", (unsigned long long)probe_frame_id, r, g, b);
        }
        if (probe_frames % 600 == 0)
            std::fprintf(stderr, "[flash] %llu flash-coloured of %llu frames\n", (unsigned long long)probe_flashes,
                         (unsigned long long)probe_frames);
    }
    // Vulkan pipeline cache, persisted to <data>/shadercache/pipelines.bin (saved when new
    // pipelines were made, at most every 10 s).
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    bool pipeline_cache_dirty = false;
    std::chrono::steady_clock::time_point pipeline_cache_saved = std::chrono::steady_clock::now();
    std::string PipelineCachePath() const {
        return (hle::DataRoot().empty() ? std::string("run") : hle::DataRoot()) + "/shadercache/pipelines.bin";
    }
    void LoadPipelineCache() {
        std::vector<char> blob;
        if (std::ifstream in{PipelineCachePath(), std::ios::binary | std::ios::ate}) {
            blob.resize(size_t(in.tellg()));
            in.seekg(0);
            in.read(blob.data(), std::streamsize(blob.size()));
        }
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pci.initialDataSize = blob.size();
        pci.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(d, &pci, nullptr, &pipeline_cache) != VK_SUCCESS) {
            pci.initialDataSize = 0;  // stale or foreign data: start empty
            pci.pInitialData = nullptr;
            vkCreatePipelineCache(d, &pci, nullptr, &pipeline_cache);
        }
        std::fprintf(stderr, "[renderer] pipeline cache: %zu bytes loaded\n", blob.size());
    }
    void SavePipelineCache(bool force) {
        if (!pipeline_cache || !pipeline_cache_dirty) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - pipeline_cache_saved < std::chrono::seconds(10)) return;
        // Serialising a large cache takes a few hundred ms: do it on a worker so the game doesn't
        // hitch (the cache is internally synchronised). A forced save (exit) runs here.
        static std::atomic<bool> saving{false};
        if (saving.exchange(true)) return;  // the previous save is still running
        pipeline_cache_dirty = false;
        pipeline_cache_saved = now;
        auto save = [d = d, cache = pipeline_cache, path = PipelineCachePath()] {
            size_t size = 0;
            vkGetPipelineCacheData(d, cache, &size, nullptr);
            std::vector<char> blob(size);
            if (vkGetPipelineCacheData(d, cache, &size, blob.data()) == VK_SUCCESS) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
                const std::string tmp = path + ".tmp";
                std::ofstream(tmp, std::ios::binary).write(blob.data(), std::streamsize(size));
                std::filesystem::rename(tmp, path, ec);
            }
            saving = false;
        };
        if (force) save();
        else std::thread(save).detach();
    }
    // LDPORT_PASS_TIMING=1: GPU time per render pass, summed per target, reported every 120 frames.
    const bool pass_timing = std::getenv("LDPORT_PASS_TIMING") != nullptr;
    static constexpr uint32_t kMaxPasses = 1024;
    VkQueryPool pass_ts = VK_NULL_HANDLE;
    VkQueryPool pass_pools[kMaxFrames]{};
    uint32_t pass_count = 0, recorded_passes_slot[kMaxFrames]{};
    struct PassInfo { uint64_t target; uint32_t draws; };
    std::vector<PassInfo> passes, recorded_slot[kMaxFrames];
    struct PassTotal { double ms = 0; uint64_t passes = 0, draws = 0; };
    std::map<uint64_t, PassTotal> pass_totals;
    bool timestamps_valid = false;
    float timestamp_period_ns = 1.0f;
    uint64_t gpu_frame_us = 0;

    // --- helpers ---
    VkDeviceMemory Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool device_address) {
        VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = device_address ? &flags_info : nullptr;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = dev->FindMemoryType(req.memoryTypeBits, flags);
        VkDeviceMemory m = VK_NULL_HANDLE;
        const VkResult r = vkAllocateMemory(d, &mai, nullptr, &m);
        static uint64_t count = 0;
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[renderer] memory allocation %llu failed (%d), %llu bytes\n", (unsigned long long)count,
                         int(r), (unsigned long long)req.size);
            return VK_NULL_HANDLE;
        }
        if (++count % 500 == 0) std::fprintf(stderr, "[renderer] %llu memory allocations\n", (unsigned long long)count);
        return m;
    }

    VkImage CreateImage(VkImageType type, VkFormat format, uint32_t w, uint32_t h, uint32_t depth, uint32_t layers,
                        uint32_t mips, VkImageUsageFlags usage, VkImageCreateFlags flags, VkDeviceMemory& memory) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.flags = flags;
        ici.imageType = type;
        ici.format = format;
        ici.extent = {w, h, depth};
        ici.mipLevels = mips;
        ici.arrayLayers = layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(d, &ici, nullptr, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, image, &req);
        memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        if (!memory) {  // never bind a null allocation: the driver faults on it later
            vkDestroyImage(d, image, nullptr);
            return VK_NULL_HANDLE;
        }
        vkBindImageMemory(d, image, memory, 0);
        return image;
    }

    VkImageView CreateView(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                           uint32_t layers, uint32_t mips = 1) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = type;
        vci.format = format;
        vci.subresourceRange = {aspect, 0, mips, 0, layers};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(d, &vci, nullptr, &view);
        return view;
    }

    void FullBarrier() {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void ToGeneral(VkImage image, VkImageAspectFlags aspect, uint32_t layers = 1, uint32_t mips = 1) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = image;
        b.subresourceRange = {aspect, 0, mips, 0, layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void BeginFrame() {
        if (recording) return;
        static bool hang_logged = false;
        slot = (slot + 1) % FramesInFlight();
        cmd = cmds[slot];
        fence = fences[slot];
        timestamps = timestamp_pools[slot];
        pass_ts = pass_pools[slot];
        const auto wait_start = std::chrono::steady_clock::now();
        // This slot's previous frame (FramesInFlight() frames ago) must be done before its command
        // buffer, ring part and upload part are reused. The flash probe reads back the frame before.
        if (!WaitFence(d, fence, "the previous frame") && !hang_logged) { hang_logged = true; LogFrameShaders(); }
        if (flash_count) WaitAll("the probed frame");
        DestroyRetired(slot);  // this slot's previous frame is done, and so is every frame before it
        gpu_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count());
        vkResetFences(d, 1, &fence);
        if (flash_count) ProbeRead();
        const VkDeviceSize ring_part = kRingSize / FramesInFlight(), upload_part = kUploadSize / FramesInFlight();
        ring_used = ring_part * slot;
        ring_end = ring_used + ring_part;
        upload_used = upload_part * slot;
        upload_end = upload_used + upload_part;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        // GPU time of the previous frame (its fence has signalled), then start timing this one.
        uint64_t ts[2] = {};
        if (timestamps_valid_slot[slot] && vkGetQueryPoolResults(d, timestamps, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            gpu_frame_us += uint64_t(double(ts[1] - ts[0]) * timestamp_period_ns / 1000.0);
        vkCmdResetQueryPool(cmd, timestamps, 0, 2);
        if (pass_timing) {
            const uint32_t recorded_passes = recorded_passes_slot[slot];
            const auto& recorded = recorded_slot[slot];
            if (recorded_passes) {
                std::vector<uint64_t> r(recorded_passes * 2);
                if (vkGetQueryPoolResults(d, pass_ts, 0, recorded_passes * 2, r.size() * 8, r.data(), 8,
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    for (uint32_t i = 0; i < recorded_passes; ++i) {
                        PassTotal& pt = pass_totals[recorded[i].target];
                        pt.ms += double(r[i * 2 + 1] - r[i * 2]) * timestamp_period_ns / 1e6;
                        ++pt.passes;
                        pt.draws += recorded[i].draws;
                    }
            }
            vkCmdResetQueryPool(cmd, pass_ts, 0, kMaxPasses * 2);
            pass_count = 0;
            passes.clear();
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
        timestamps_valid_slot[slot] = true;
        recording = true;
        // The previous frame may still be running: its work on the shared targets comes first.
        if (FramesInFlight() > 1) FullBarrier();
    }

    void EndRendering() {
        if (!rendering) return;
        vkCmdEndRendering(cmd);
        if (pass_timing && pass_count && pass_count <= kMaxPasses)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_ts, (pass_count - 1) * 2 + 1);
        rendering = false;
        FullBarrier();  // later draws may sample what was just rendered
    }

    void SubmitFrame() {
        EndRendering();
        if (!recording) return;
        if (pass_timing) {
            recorded_slot[slot] = passes;
            recorded_passes_slot[slot] = std::min(pass_count, kMaxPasses);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(dev->queue(), 1, &si, fence);
        recording = false;
    }

    // Bump allocation in the per-frame ring; returns a host pointer and the device address.
    uint8_t* RingAlloc(VkDeviceSize bytes, VkDeviceAddress& address, VkDeviceSize& offset) {
        bytes = (bytes + 255) & ~VkDeviceSize(255);
        if (ring_used + bytes > ring_end) return nullptr;
        offset = ring_used;
        address = ring_address + ring_used;
        ring_used += bytes;
        return ring_map + offset;
    }

    // --- guest memory ---
    // The last few imports hit: nearly every buffer of a frame is in one of them, and the guest range
    // lookup takes a lock. (unordered_map elements keep their address when it grows.)
    Import* recent_imports[4] = {};
    uint32_t recent_next = 0;
    uint64_t recent_generation = 0;
    Import* ImportFor(uint64_t guest) {
        if (const uint64_t g = host::GuestRangeGeneration(); g != recent_generation) {  // a range went away
            recent_generation = g;
            for (Import*& c : recent_imports) c = nullptr;
        }
        for (Import* c : recent_imports)
            if (c && guest >= c->base && guest < c->base + c->size) return c;
        host::GuestRange range;
        if (!host::FindGuestRange(guest, range)) return nullptr;
        auto it = imports.find(range.id);
        if (it != imports.end()) {
            if (!it->second.buffer) return nullptr;
            recent_imports[recent_next++ % 4] = &it->second;
            return &it->second;
        }

        Import& im = imports[range.id];
        im.base = range.base;
        im.size = (range.size + import_alignment - 1) & ~(import_alignment - 1);
        void* host = reinterpret_cast<void*>(range.base);
        VkMemoryHostPointerPropertiesEXT props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (!host_pointer_props ||
            host_pointer_props(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &props) != VK_SUCCESS)
            return nullptr;

        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.pNext = &ext;
        bci.size = im.size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        vkCreateBuffer(d, &bci, nullptr, &im.buffer);

        VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = host;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.pNext = &imp;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &flags;
        mai.allocationSize = im.size;
        mai.memoryTypeIndex = dev->FindMemoryType(props.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (vkAllocateMemory(d, &mai, nullptr, &im.memory) != VK_SUCCESS) {
            std::fprintf(stderr, "[renderer] could not import guest memory %llx (+%llx)\n",
                         (unsigned long long)im.base, (unsigned long long)im.size);
            vkDestroyBuffer(d, im.buffer, nullptr);
            im.buffer = VK_NULL_HANDLE;
            return nullptr;
        }
        vkBindBufferMemory(d, im.buffer, im.memory, 0);
        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = im.buffer;
        im.address = vkGetBufferDeviceAddress(d, &bai);
        std::fprintf(stderr, "[renderer] imported guest memory %llx (+%llx MB)\n", (unsigned long long)im.base,
                     (unsigned long long)(im.size >> 20));
        return &im;
    }

    // --- shaders ---
    const gcn::ShaderProgram* GetProgram(gcn::Stage stage, uint64_t address, uint64_t fetch, uint32_t user_sgprs) {
        if (address < 0x10000) return nullptr;
        const uint64_t key = address ^ (fetch * 31) ^ uint64_t(stage);
        auto it = programs.find(key);
        if (it != programs.end()) {
            // Shader memory can be reused for different code; re-check the bytes once per frame.
            if (it->second.checked_frame == frame) return &it->second.program;
            const uint32_t n = ProgramDwords(it->second.program);
            if (FastHash(reinterpret_cast<const void*>(address), n * 4) == it->second.code_hash) {
                it->second.checked_frame = frame;
                return &it->second.program;
            }
        }
        Program p;
        p.program = gcn::Build(stage, reinterpret_cast<const uint32_t*>(address),
                               fetch ? reinterpret_cast<const uint32_t*>(fetch) : nullptr, user_sgprs);
        p.code_hash = FastHash(reinterpret_cast<const void*>(address), ProgramDwords(p.program) * 4);
        static uint64_t next_id = 1;
        p.program.id = next_id++;
        p.program.address = address;
        p.checked_frame = frame;
        return &(programs[key] = std::move(p)).program;
    }

    // Shaders compiled recently, for diagnosing GPU hangs (a hang right after a scene change is
    // nearly always a new shader).
    struct RecentShader { uint64_t frame, program_id; bool branches; uint64_t glsl_hash; };
    std::vector<RecentShader> recent_shaders;
    void LogFrameShaders() {
        std::fprintf(stderr, "[renderer] shaders compiled in the last frames (frame %llu now):\n", (unsigned long long)frame);
        for (const auto& s : recent_shaders)
            if (s.frame + 3 >= frame)
                std::fprintf(stderr, "  frame %llu program %llu glsl %llu%s\n", (unsigned long long)s.frame,
                             (unsigned long long)s.program_id, (unsigned long long)s.glsl_hash,
                             s.branches ? " (has branches)" : "");
    }

    // Fast path: (program, key) -> module, so GLSL is only generated for new combinations.
    std::unordered_map<uint64_t, VkShaderModule> modules_by_key;
    uint64_t module_gen_ns = 0, module_rest_ns = 0;
    uint64_t tex_create_ns = 0, tex_detile_ns = 0;  // this frame: texture image + memory creation; detile + copy  // this frame: GLSL generation; cache load / compile / create

    // --- pipelines: what one depends on besides its shaders (no padding: hashed and stored as bytes) ---
    struct PipeDesc {
        uint32_t prim, depth_control, stencil_control, stencil_ref, stencil_ref_bf, target_mask, color_control, mode_cntl;
        uint32_t blend[8];  // CB_BLEND0..7_CONTROL
        uint32_t color_count;
        VkFormat colors[8], depth;
        uint8_t write_mask[8];  // per attachment
    };
    static_assert(sizeof(PipeDesc) == 112, "PipeDesc must have no padding");
    static uint64_t PipeKeyHash(VkShaderModule vs, VkShaderModule ps, const PipeDesc& pd) {
        struct { VkShaderModule vs, ps; PipeDesc pd; } k{vs, ps, pd};
        static_assert(sizeof(k) == 128, "no padding");
        return FastHash(&k, sizeof k);
    }
    VkPipeline BuildPipeline(VkShaderModule vmod, VkShaderModule pmod, const PipeDesc& pd);

    // Pipeline list (<data>/shadercache/pipelines-<build>.bin: the two shaders' cache hashes + PipeDesc per
    // record). Every pipeline the game needs is appended; at start-up worker threads re-create the listed ones
    // in the background (loading their SPIR-V), so an area the game loads later doesn't build ~100 pipelines
    // in one frame. Finished ones are handed to the GPU thread at each flip (MergeWarmPipelines).
    struct PipeRecord {
        uint64_t vs_h, ps_h;
        PipeDesc pd;
    };
    std::string pipeline_list_path;
    std::unordered_set<uint64_t> pipeline_list_known;  // record hashes in the file
    std::mutex warm_mutex;
    std::vector<std::pair<uint64_t, VkShaderModule>> warm_modules;  // done by the workers, not yet merged
    std::vector<std::pair<uint64_t, VkPipeline>> warm_pipelines;
    std::atomic<bool> warm_stop{false};
    std::atomic<int> warm_running{0};
    bool warm_started = false;

    uint64_t ModuleHash(VkShaderModule m) const {
        if (!m) return 0;
        for (const auto& [h, mod] : modules)
            if (mod == m) return h;
        return 0;
    }
    void RecordPipeline(VkShaderModule vmod, VkShaderModule pmod, const PipeDesc& pd) {
        if (pipeline_list_path.empty()) return;
        PipeRecord rec{ModuleHash(vmod), ModuleHash(pmod), pd};
        if (!rec.vs_h || (pmod && !rec.ps_h)) return;
        const uint64_t rh = FastHash(&rec, sizeof rec);
        if (!pipeline_list_known.insert(rh).second) return;
        std::ofstream(pipeline_list_path, std::ios::binary | std::ios::app).write(reinterpret_cast<const char*>(&rec), sizeof rec);
    }
    void StartPipelineWarmup();
    void MergeWarmPipelines() {
        std::lock_guard lock(warm_mutex);
        for (const auto& [h, m] : warm_modules) {
            if (!modules.count(h)) modules[h] = m;
            compiling.erase(h);
        }
        for (const auto& [k, pl] : warm_pipelines) {
            if (!pipelines.count(k)) {
                pipelines[k] = pl;
                if (auto b = building.find(k); b != building.end() && pl) {
                    RecordPipeline(b->second.vmod, b->second.pmod, b->second.pd);
                    pipeline_cache_dirty = true;
                }
            }
            building.erase(k);
        }
        warm_modules.clear();
        warm_pipelines.clear();
    }

    // Shader index (<data>/shadercache/index-<build>.bin, 16-byte records): a stable key (the shader's machine
    // code hash and its translation settings) -> the GLSL hash its SPIR-V is cached under. A later run with the
    // same build loads the SPIR-V without translating the shader to GLSL first, which was most of the cost of
    // the shaders a new area brings in. The build ID is the executable's write time, so a rebuild (a changed
    // translator) starts a new index rather than reuse a stale translation.
    std::unordered_map<uint64_t, uint64_t> shader_index;
    std::string shader_index_path;
    bool shader_index_loaded = false;

    static std::string ShaderCacheDir() {
        return hle::DataRoot().empty() ? std::string("run/shadercache") : hle::DataRoot() + "/shadercache";
    }
    // Names this build of the port in the shader cache files (a new build translates differently): its PE link time
    // and image size, which travel with the file (an installer does not keep its write time exactly).
    static unsigned long long ExeCacheKey() {
#ifndef _WIN32
        // Linux: the executable's modification time and size (kept by the Steam Deck package's tarball).
        struct stat st{};
        if (stat("/proc/self/exe", &st) != 0) return 0;
        return (uint64_t(st.st_mtime) << 32) ^ uint64_t(st.st_size);
#else
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleA(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
        return (uint64_t(nt->FileHeader.TimeDateStamp) << 32) | nt->OptionalHeader.SizeOfImage;
#endif
    }
    void LoadShaderIndex() {
        shader_index_loaded = true;
        char name[64];
        std::snprintf(name, sizeof name, "/index-%016llx.bin", ExeCacheKey());
        shader_index_path = ShaderCacheDir() + name;
        std::ifstream in(shader_index_path, std::ios::binary);
        uint64_t rec[2];
        while (in.read(reinterpret_cast<char*>(rec), sizeof rec)) shader_index[rec[0]] = rec[1];
        std::fprintf(stderr, "[renderer] shader index: %zu entries\n", shader_index.size());
    }
    VkShaderModule LoadCachedSpirv(uint64_t h) {
        std::ifstream in(ShaderCacheDir() + "/" + std::to_string(h) + ".spv", std::ios::binary | std::ios::ate);
        if (!in || in.tellg() <= 0 || in.tellg() % 4 != 0) return VK_NULL_HANDLE;
        std::vector<uint32_t> spv(size_t(in.tellg()) / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(spv.data()), std::streamsize(spv.size() * 4));
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = spv.size() * 4;
        smci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        return vkCreateShaderModule(d, &smci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
    }

    // `defer`: the draw can be skipped while the module compiles in the background (null is returned meanwhile).
    VkShaderModule GetModule(const gcn::ShaderProgram& program, const gcn::ShaderKey& key, bool defer = false) {
        uint64_t kh = FastHash(&program.id, sizeof program.id);
        const uint32_t fixed[3] = {uint32_t(key.stage), key.user_sgprs, key.ps_input_ena};
        kh = FastHash(fixed, sizeof fixed, kh);
        kh = FastHash(key.image_types.data(), key.image_types.size(), kh);
        kh = FastHash(key.mrt_map, sizeof key.mrt_map, kh);
        kh = FastHash(&key.fp16_targets, 1, kh);
        if (key.ps_input_map) kh = FastHash(key.ps_input_cntl, sizeof key.ps_input_cntl, kh);
        uint32_t scaling[2] = {0, key.scaled_images};
        std::memcpy(&scaling[0], &key.scale, 4);
        kh = FastHash(scaling, sizeof scaling, kh);
        if (gcn::TakeShaderDebugChanged()) modules_by_key.clear();  // debug remote changed a shader's output
        if (auto fast = modules_by_key.find(kh); fast != modules_by_key.end()) return fast->second;
        if (auto p = pending_by_key.find(kh); p != pending_by_key.end()) {  // compiling in the background
            if (auto m = modules.find(p->second); m != modules.end()) {
                modules_by_key[kh] = m->second;
                pending_by_key.erase(p);
                return m->second;
            }
            if (defer && compiling.count(p->second)) return VK_NULL_HANDLE;
            pending_by_key.erase(p);
        }

        // The stable key: the same settings, keyed by the machine code instead of this run's program id.
        uint64_t ks = FastHash(&program.content_hash, sizeof program.content_hash);
        ks = FastHash(fixed, sizeof fixed, ks);
        ks = FastHash(key.image_types.data(), key.image_types.size(), ks);
        ks = FastHash(key.mrt_map, sizeof key.mrt_map, ks);
        ks = FastHash(&key.fp16_targets, 1, ks);
        if (key.ps_input_map) ks = FastHash(key.ps_input_cntl, sizeof key.ps_input_cntl, ks);
        ks = FastHash(scaling, sizeof scaling, ks);
        static const bool no_index = std::getenv("LDPORT_SAVE_GLSL") != nullptr;
        const bool use_index = !no_index && gcn::DebugPsSpec().empty();
        if (use_index) {
            if (!shader_index_loaded) LoadShaderIndex();
            if (auto ix = shader_index.find(ks); ix != shader_index.end()) {
                const auto t_load = std::chrono::steady_clock::now();
                VkShaderModule m = VK_NULL_HANDLE;
                if (auto it = modules.find(ix->second); it != modules.end()) {
                    m = it->second;
                } else if (defer && AsyncShaders() && std::filesystem::exists(ShaderCacheDir() + "/" + std::to_string(ix->second) + ".spv")) {
                    pending_by_key[kh] = ix->second;  // loaded by a shader worker
                    if (compiling.insert(ix->second).second) QueueShaderJob({ix->second, 0, {}, key.stage, {}, {}, {}, {}});
                    return VK_NULL_HANDLE;
                } else if ((m = LoadCachedSpirv(ix->second)) != VK_NULL_HANDLE) {
                    modules[ix->second] = m;
                }
                module_rest_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_load).count());
                if (m) return modules_by_key[kh] = m;
            }
        }

        const auto t_gen = std::chrono::steady_clock::now();
        const std::string glsl = gcn::GenerateGlsl(program, key);
        const uint64_t h = Hash(glsl.data(), glsl.size());
        struct AddNs {  // module time by step, for the hitch log
            uint64_t& to;
            std::chrono::steady_clock::time_point t;
            ~AddNs() { to += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); }
        };
        module_gen_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_gen).count());
        AddNs rest{module_rest_ns, std::chrono::steady_clock::now()};
        if (use_index && shader_index.emplace(ks, h).second) {
            std::error_code ec;
            std::filesystem::create_directories(ShaderCacheDir(), ec);
            const uint64_t rec[2] = {ks, h};
            std::ofstream(shader_index_path, std::ios::binary | std::ios::app).write(reinterpret_cast<const char*>(rec), sizeof rec);
        }
        recent_shaders.push_back({frame, program.id, glsl.find("blk_") != std::string::npos, h});
        if (recent_shaders.size() > 64) recent_shaders.erase(recent_shaders.begin());
        if (std::getenv("LDPORT_SAVE_GLSL")) {  // keep every generated shader for inspection
            std::filesystem::create_directories("run/glsl");
            std::ofstream("run/glsl/" + std::to_string(h) + ".glsl") << glsl;
        }
        // Instructions the translator doesn't know yet, once each.
        for (size_t pos = glsl.find("// unhandled "); pos != std::string::npos; pos = glsl.find("// unhandled ", pos + 1)) {
            static std::map<std::string, int> seen;
            const std::string line = glsl.substr(pos + 13, glsl.find('\n', pos) - pos - 13);
            const std::string op = line.substr(0, line.find(' '));
            if (seen[op]++ == 0) std::fprintf(stderr, "[renderer] translator: unhandled %s\n", line.c_str());
        }
        auto it = modules.find(h);
        if (it != modules.end()) return modules_by_key[kh] = it->second;
        // Background compiling: a draw that may wait (a 3D object, see Draw) has a shader worker load or compile the
        // module and is skipped until it is ready, instead of stalling the frame for it.
        if (defer && AsyncShaders()) {
            pending_by_key[kh] = h;
            if (compiling.insert(h).second) QueueShaderJob({h, 0, glsl, key.stage, {}, {}, {}, {}});
            return VK_NULL_HANDLE;
        }

        // SPIR-V disk cache (<data>/shadercache/<glsl hash>.spv): later runs skip the GLSL compile.
        const std::string cache_dir = hle::DataRoot().empty() ? std::string("run/shadercache") : hle::DataRoot() + "/shadercache";
        const std::string cache_path = cache_dir + "/" + std::to_string(h) + ".spv";
        {
            std::ifstream in(cache_path, std::ios::binary | std::ios::ate);
            if (in && in.tellg() > 0 && in.tellg() % 4 == 0) {
                std::vector<uint32_t> spv(size_t(in.tellg()) / 4);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(spv.data()), std::streamsize(spv.size() * 4));
                VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smci.codeSize = spv.size() * 4;
                smci.pCode = spv.data();
                VkShaderModule cached = VK_NULL_HANDLE;
                if (vkCreateShaderModule(d, &smci, nullptr, &cached) == VK_SUCCESS) {
                    modules[h] = cached;
                    modules_by_key[kh] = cached;
                    return cached;
                }
            }
        }
        // No SPIR-V optimization (CompileGlsl): the driver optimizes anyway, and spirv-opt takes seconds-to-minutes on
        // the translator's larger shaders (block-dispatch loops), stalling the game.
        VkShaderModule module = CompileGlsl(compiler, glsl, key.stage, h);
        modules[h] = module;
        modules_by_key[kh] = module;
        return module;
    }

    // GLSL -> SPIR-V -> module, also written to the SPIR-V disk cache. Runs on the GPU thread or a shader worker
    // (each with its own compiler).
    VkShaderModule CompileGlsl(shaderc_compiler_t comp, const std::string& glsl, gcn::Stage stage, uint64_t h) {
        shaderc_compile_options_t opts = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_zero);  // see GetModule
        const shaderc_shader_kind kind = stage == gcn::Stage::Vertex ? shaderc_vertex_shader : shaderc_fragment_shader;
        shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, glsl.data(), glsl.size(), kind, "gcn", "main", opts);
        shaderc_compile_options_release(opts);
        VkShaderModule module = VK_NULL_HANDLE;
        if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = shaderc_result_get_length(r);
            smci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
            vkCreateShaderModule(d, &smci, nullptr, &module);
            std::error_code ec;
            std::filesystem::create_directories(ShaderCacheDir(), ec);
            const std::string tmp = ShaderCacheDir() + "/" + std::to_string(h) + ".spv.tmp";
            std::ofstream(tmp, std::ios::binary).write(shaderc_result_get_bytes(r), std::streamsize(smci.codeSize));
            std::filesystem::rename(tmp, ShaderCacheDir() + "/" + std::to_string(h) + ".spv", ec);
        } else {
            std::error_code ec;
            std::filesystem::create_directories("run/shader_errors", ec);
            const std::string path = "run/shader_errors/" + std::to_string(h) + ".glsl";
            std::ofstream(path) << glsl << "\n/*\n" << shaderc_result_get_error_message(r) << "\n*/\n";
            std::fprintf(stderr, "[renderer] shader compile failed, see %s\n", path.c_str());
        }
        shaderc_result_release(r);
        return module;
    }

    // --- background compiling (Display Settings / LDPORT_SYNC_SHADERS=1 turns it off) ---
    // Jobs: compile a module (glsl set) or build a pipeline (modules + PipeDesc). Results go through
    // warm_modules / warm_pipelines and are merged at each flip (MergeWarmPipelines), like the warm-up's.
    struct ShaderJob {
        uint64_t h;         // module: GLSL hash
        uint64_t pipe_key;  // pipeline: its key (0 for a module job)
        std::string glsl;
        gcn::Stage stage;
        VkShaderModule vmod, pmod;
        PipeDesc pd;
        std::chrono::steady_clock::time_point queued;
    };
    static bool AsyncShaders() {
        static const bool on = std::getenv("LDPORT_SYNC_SHADERS") == nullptr;
        return on;
    }
    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::deque<ShaderJob> jobs;
    bool job_workers_started = false;
    std::unordered_map<uint64_t, uint64_t> pending_by_key;  // module key -> GLSL hash being compiled
    std::unordered_set<uint64_t> compiling;                 // GLSL hashes queued or compiling (GPU thread's view)
    struct PendingPipe { VkShaderModule vmod, pmod; PipeDesc pd; };
    std::unordered_map<uint64_t, PendingPipe> building;     // pipeline keys queued or building
    std::atomic<uint64_t> async_done{0}, async_wait_ns{0}, deferred_draws{0};
    void QueueShaderJob(ShaderJob job) {
        job.queued = std::chrono::steady_clock::now();
        std::lock_guard lock(job_mutex);
        if (!job_workers_started) {
            job_workers_started = true;
            const unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency() / 4));
            for (unsigned w = 0; w < n; ++w) std::thread([this] { ShaderWorker(); }).detach();
            std::fprintf(stderr, "[renderer] background shader compiling on %u threads\n", n);
        }
        // Modules first: a pipeline job only exists once its modules do.
        if (job.pipe_key) jobs.push_back(std::move(job));
        else jobs.push_front(std::move(job));
        job_cv.notify_one();
    }
    void ShaderWorker() {
        shaderc_compiler_t comp = shaderc_compiler_initialize();
        for (;;) {
            ShaderJob job;
            {
                std::unique_lock lock(job_mutex);
                job_cv.wait(lock, [&] { return warm_stop || !jobs.empty(); });
                if (warm_stop) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            if (job.pipe_key) {
                VkPipeline pl = BuildPipeline(job.vmod, job.pmod, job.pd);
                std::lock_guard wl(warm_mutex);
                warm_pipelines.push_back({job.pipe_key, pl});
            } else {
                VkShaderModule m = LoadCachedSpirv(job.h);  // compiled by an earlier run
                if (!m && !job.glsl.empty()) m = CompileGlsl(comp, job.glsl, job.stage, job.h);
                std::lock_guard wl(warm_mutex);
                warm_modules.push_back({job.h, m});
            }
            async_done.fetch_add(1, std::memory_order_relaxed);
            async_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - job.queued).count()),
                                    std::memory_order_relaxed);
        }
        shaderc_compiler_release(comp);
    }

    // --- render targets ---
    // Addresses where a target was grown to the size it is sampled at (see TargetView).
    std::unordered_set<uint64_t> grown_at;

    const Target* GetTarget(uint64_t address, VkFormat format, uint32_t w, uint32_t h, bool depth) {
        auto key = std::make_tuple(address, format, w, h);
        auto it = targets.find(key);
        if (it != targets.end() && !grown_at.count(address)) return &it->second;
        // A draw whose screen scissor is smaller than the surface asks for a smaller size: it still
        // renders into the same surface, so reuse an existing target there that is at least as big
        // (the scissor confines the drawing). Making a new image would lose those draws.
        const Target* best = nullptr;
        for (const auto& [k, tg] : targets)
            if (tg.address == address && tg.format == format && tg.depth == depth && tg.width >= w && tg.height >= h &&
                (!best || uint64_t(tg.width) * tg.height > uint64_t(best->width) * best->height))
                best = &tg;
        if (best) return best;
        EndRendering();  // the new image's layout transition can't happen inside a render pass
        Target t;
        t.address = address;
        t.format = format;
        t.width = w;
        t.height = h;
        t.depth = depth;
        t.pw = Scaled(w);
        t.ph = Scaled(h);
        const VkImageUsageFlags usage =
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        t.image = CreateImage(VK_IMAGE_TYPE_2D, format, t.pw, t.ph, 1, 1, 1, usage, 0, t.memory);
        if (!t.image) return nullptr;
        const bool stencil = format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
        const VkImageAspectFlags aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                                : VK_IMAGE_ASPECT_COLOR_BIT;
        t.view = CreateView(t.image, VK_IMAGE_VIEW_TYPE_2D, format, aspect, 1);
        ToGeneral(t.image, aspect);
        std::fprintf(stderr, "[renderer] new %s target %llx %ux%u format %d (image %ux%u)\n", depth ? "depth" : "color",
                     (unsigned long long)address, w, h, int(format), t.pw, t.ph);
        target_at.clear();  // a new target may be the one sampled at its address
        return &(targets[key] = t);
    }

    const Target* FindTargetByAddress(uint64_t address) {
        const Target* best = nullptr;
        for (const auto& [key, t] : targets)
            if (t.address == address && !t.depth && (!best || t.width * t.height > best->width * best->height)) best = &t;
        return best;
    }

    // Debug: writes every colour target as run/dump/<address>_<w>x<h>_<vkformat>[_display].raw, or with
    // `shot` only the display target, as run/shots/<frame>_<w>x<h>_<vkformat>_display.raw.
    // Experiment (LDPORT_WRITEBACK=1): copy small colour targets back into guest memory after each
    // frame, linearly, in case the CPU reads rendered results (e.g. average luminance for exposure).
    void WritebackSmallTargets() {
        std::vector<const Target*> list;
        if (RenderScale() != 1.0f) return;  // guest memory holds the game's size
        for (const auto& [key, tg] : targets)
            if (!tg.depth && tg.address && uint64_t(tg.width) * tg.height <= 512ull * 512ull) list.push_back(&tg);
        if (list.empty()) return;
        vkQueueWaitIdle(dev->queue());
        for (const Target* tp : list) {
            const Target& tg = *tp;
            const uint32_t bpp = tg.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8
                               : (tg.format == VK_FORMAT_R16_SFLOAT || tg.format == VK_FORMAT_R16_UNORM) ? 2 : 4;
            const VkDeviceSize bytes = VkDeviceSize(tg.width) * tg.height * 16;
            VkBuffer buf;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(d, &bci, nullptr, &buf);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, buf, &req);
            VkDeviceMemory mem = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
            vkBindBufferMemory(d, buf, mem, 0);
            BeginFrame();
            EndRendering();
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {tg.width, tg.height, 1};
            vkCmdCopyImageToBuffer(cmd, tg.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &copy);
            SubmitFrame();
            WaitAll("a target writeback");
            void* src;
            vkMapMemory(d, mem, 0, bytes, 0, &src);
            std::memcpy(reinterpret_cast<void*>(tg.address), src, size_t(tg.width) * tg.height * bpp);
            vkUnmapMemory(d, mem);
            vkDestroyBuffer(d, buf, nullptr);
            vkFreeMemory(d, mem, nullptr);
        }
    }
    // A render scale change (from the Options menu): drop every target so the next frame makes them at
    // the new size. Runs between frames, after this frame was presented.
    void ApplyScaleChange() {
        const float want = g_wanted_scale.exchange(0.0f);
        if (want <= 0.0f || want == RenderScale()) return;
        EndRendering();
        if (recording) SubmitFrame();
        WaitAll("a resolution change");
        vkQueueWaitIdle(dev->queue());
        for (auto& [key, v] : target_views) vkDestroyImageView(d, v, nullptr);
        target_views.clear();
        target_view_cache.clear();
        target_at.clear();
        // Depth-sampling views (motion vectors, FSR 2) are keyed by image handle, and a new image can reuse a
        // destroyed one's handle: a stale view there crashed the driver at the first FSR 2 frame after start-up.
        for (auto& [img, v] : depth_views) vkDestroyImageView(d, v, nullptr);
        depth_views.clear();
        for (auto& [key, t] : targets) {
            vkDestroyImageView(d, t.view, nullptr);
            vkDestroyImage(d, t.image, nullptr);
            vkFreeMemory(d, t.memory, nullptr);
        }
        targets.clear();
        grown_at.clear();
        bound_color = {};
        bound_depth = nullptr;
        g_render_scale = want;
        std::fprintf(stderr, "[renderer] render scale now %.3f\n", want);
    }
    bool flat_trapped = false, dump_next = false, flat_trapped_dumped = false, dump_this = false;
    void DumpTargets(uint64_t display_address, bool shot = false) {
        vkQueueWaitIdle(dev->queue());
        std::filesystem::create_directories(shot ? "run/shots" : "run/dump");
        for (const auto& [key, t] : targets) {
            if (t.depth || (shot && t.address != display_address)) continue;
            const VkDeviceSize bytes = VkDeviceSize(t.pw) * t.ph * 16;
            VkBuffer buf;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(d, &bci, nullptr, &buf);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, buf, &req);
            VkDeviceMemory mem = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
            vkBindBufferMemory(d, buf, mem, 0);
            BeginFrame();
            EndRendering();
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {t.pw, t.ph, 1};
            vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &copy);
            SubmitFrame();
            WaitAll("a target dump");
            void* p;
            vkMapMemory(d, mem, 0, bytes, 0, &p);
            char name[160];
            if (shot)
                std::snprintf(name, sizeof name, "run/shots/%06llu_%ux%u_%d_display.raw", (unsigned long long)frame,
                              t.pw, t.ph, int(t.format));
            else
                std::snprintf(name, sizeof name, "run/dump/%llx_%ux%u_%d%s.raw", (unsigned long long)t.address, t.pw,
                              t.ph, int(t.format), t.address == display_address ? "_display" : "");
            const uint32_t bpp = t.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
            std::ofstream(name, std::ios::binary).write(static_cast<const char*>(p), std::streamsize(VkDeviceSize(t.pw) * t.ph * bpp));
            // LDPORT_FLAT_TRAP=1: a screenshot that is one flat colour arms a full capture and target
            // dump of the next frame (single-frame flashes are hard to hit by frame number).
            static const bool flat_trap = std::getenv("LDPORT_FLAT_TRAP") != nullptr;
            static const uint64_t flat_trap_from = flat_trap ? std::strtoull(std::getenv("LDPORT_FLAT_TRAP"), nullptr, 10) : 0;
            if (shot && flat_trap && frame >= flat_trap_from && !flat_trapped && bpp == 4) {
                const uint32_t* px = static_cast<const uint32_t*>(p);
                const size_t count = size_t(t.pw) * t.ph;
                // Only the picture between the letterbox bars (rows 15%..85%), against the centre pixel.
                const size_t first = size_t(t.ph * 15 / 100) * t.pw, last = size_t(t.ph * 85 / 100) * t.pw;
                const uint32_t centre = px[count / 2 + t.pw / 2] & 0xFFFFFF;
                // "Flash-coloured": the picture averages almost no green but strong red or blue
                // (the magenta/blue flashes; real scenes here always carry some green).
                uint64_t sr = 0, sg = 0, sb = 0, n = 0;
                for (size_t i = first; i < last && i < count; i += 997, ++n) {
                    sb += px[i] & 0xFF;
                    sg += (px[i] >> 8) & 0xFF;
                    sr += (px[i] >> 16) & 0xFF;
                }
                // LDPORT_FLAT_TRAP_GREEN=1 traps bright flat green instead (the pre-title fade glitch).
                static const bool want_green = std::getenv("LDPORT_FLAT_TRAP_GREEN") != nullptr;
                bool flat = n && (want_green ? (sg / n > 150 && sr / n < 60 && sb / n < 60)
                                              : (sg / n < 15 && (sb / n > 150 || sr / n > 150)));
                if (flat) {
                    flat_trapped = true;
                    char cap_name[64];
                    std::snprintf(cap_name, sizeof cap_name, "flash_%llu.txt", (unsigned long long)frame);
                    gpu::Capture::Get().SaveRolling(cap_name);
                    dump_this = true;  // dump this frame's targets once the shot loop is done
                    std::fprintf(stderr, "[renderer] flat frame %llu (colour %06x, %llu draws): capturing the next frame\n",
                                 (unsigned long long)frame, centre, (unsigned long long)frame_draws);
                }
            }
            vkUnmapMemory(d, mem);
            vkDestroyBuffer(d, buf, nullptr);
            vkFreeMemory(d, mem, nullptr);
        }
        if (!shot) std::fprintf(stderr, "[renderer] dumped %zu targets to run/dump\n", targets.size());
    }

    // --- textures ---
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;  // own allocation (null when pooled)
        TexAlloc alloc;                          // pooled allocation
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t mips = 1, layers = 1;
        uint64_t content_hash = 0;
        uint64_t checked_frame = ~0ull;
        std::map<uint32_t, VkImageView> views;  // by dst_sel swizzle
        // The game's button icon sheet (hle/button_icons.h): 0 not checked yet, 1 another texture, 2 the sheet;
        // icon_set is the icon set its image shows.
        int icon_state = 0;
        int icon_set = icons::kPlayStation;
    };
    std::unordered_map<uint64_t, Texture> textures;  // by descriptor hash
    struct Shape {
        gcn::ImageType type = gcn::ImageType::Tex2D;
        uint32_t depth = 1, layers = 1;
    };
    std::unordered_map<uint64_t, VkSampler> samplers;  // by S# hash
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    uint8_t* upload_map = nullptr;
    VkDeviceSize upload_used = 0, upload_end = 0;  // this slot's part
    static constexpr VkDeviceSize kUploadSize = 256ull << 20;

    static VkComponentSwizzle Sel(uint32_t s) {
        static const VkComponentSwizzle k[8] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO,
                                                VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R,   VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        return k[s & 7];
    }

    // Change detection for textures the CPU may rewrite (streaming, colour-grading LUTs): small
    // textures are hashed whole; larger ones at 16 windows spread over *all* their data (every
    // slice and mip), not just the start of level 0 - a 3D LUT's first slice is the black corner
    // of the colour cube and barely changes when the grading does.
    static uint64_t SampleHash(const uint8_t* p, uint64_t size) {
        if (size <= 64 * 1024) return FastHash(p, size);
        constexpr uint64_t kWindow = 256, kWindows = 16;
        uint64_t h = FastHash(p, kWindow);
        for (uint64_t i = 1; i <= kWindows; ++i) {
            const uint64_t at = std::min(size - kWindow, size * i / (kWindows + 1));
            h = FastHash(p + at, kWindow, h);
        }
        return FastHash(p + size - kWindow, kWindow, h);
    }

    VkImageView EnsureTexture(const uint32_t* t) {
        const uint64_t base = ((uint64_t(t[1] & 0xFF) << 32) | t[0]) << 8;
        const uint32_t dfmt = (t[1] >> 20) & 0x3F, nfmt = (t[1] >> 26) & 0xF;
        const uint32_t width = (t[2] & 0x3FFF) + 1, height = ((t[2] >> 14) & 0x3FFF) + 1;
        const uint32_t swizzle = t[3] & 0xFFF, base_level = (t[3] >> 12) & 0xF, last_level = (t[3] >> 16) & 0xF;
        const uint32_t tiling = (t[3] >> 20) & 0x1F, type = (t[3] >> 28) & 0xF;
        const bool pow2_pad = (t[3] >> 25) & 1;
        const uint32_t pitch = ((t[4] >> 13) & 0x3FFF) + 1, depth = (t[4] & 0x1FFF) + 1;
        const uint32_t last_array = (t[5] >> 13) & 0x1FFF;
        const auto itype = gcn::ImageType(type);
        // Fast path: already checked this frame and the view for this swizzle exists.
        uint32_t ident[5] = {t[0], t[1], t[2], t[3] & ~0xFFFu, t[4]};  // everything but the swizzle
        const uint64_t ident_hash = FastHash(ident, sizeof ident);
        if (auto found = textures.find(ident_hash); found != textures.end() && found->second.checked_frame == frame &&
                                                  found->second.image) {
            if (auto v = found->second.views.find(swizzle); v != found->second.views.end()) return v->second;
        }
        if ((itype != gcn::ImageType::Tex2D && itype != gcn::ImageType::Tex3D && itype != gcn::ImageType::Cube &&
             itype != gcn::ImageType::Tex2DArray) ||
            !gpu::IsTileModeSupported(tiling) || base < 0x10000)
            return VK_NULL_HANDLE;
        bool block = false;
        const VkFormat format = TextureFormat(dfmt, nfmt, block);
        if (format == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        const uint32_t bpe = TextureElementBytes(dfmt);
        const uint32_t levels = std::max(1u, last_level + 1);
        auto mips = gpu::LayoutMips(tiling, width, height, pitch, levels, bpe, block, pow2_pad);
        // Each level holds all of its slices back to back (array layers, cube faces, 3D depth slices).
        Shape shape;
        shape.type = itype;
        shape.depth = itype == gcn::ImageType::Tex3D ? depth : 1;
        shape.layers = itype == gcn::ImageType::Cube ? 6 : itype == gcn::ImageType::Tex2DArray ? last_array + 1 : 1;
        uint64_t offset = 0, data_end = 0;
        for (uint32_t l = 0; l < mips.size(); ++l) {
            mips[l].offset = offset;
            const uint32_t slices = itype == gcn::ImageType::Tex3D ? std::max(1u, depth >> l) : shape.layers;
            data_end = offset + mips[l].size * slices;  // end of real data (no trailing alignment)
            offset += (mips[l].size * slices + 255) & ~uint64_t(255);
        }
        const uint8_t* src = reinterpret_cast<const uint8_t*>(base);

        Texture& tex = textures[ident_hash];
        if (tex.checked_frame != frame) {
            tex.checked_frame = frame;
            static const bool always = std::getenv("LDPORT_TEX_ALWAYS") != nullptr;  // experiment
            const uint64_t h = always ? frame * 0x9E3779B97F4A7C15ull : SampleHash(src, data_end);
            const bool new_contents = tex.image && h != tex.content_hash;
            if (new_contents) tex.icon_state = 0;
            // The game's PlayStation button icons: shown as the set for the device in use (the sets are built in
            // the background; until then, and for PlayStation, the game's own sheet goes up as it is).
            if (tex.icon_state == 0 && icons::Ready()) {
                const bool candidate = (format == VK_FORMAT_BC3_UNORM_BLOCK || format == VK_FORMAT_BC3_SRGB_BLOCK) &&
                                       itype == gcn::ImageType::Tex2D && icons::SheetSize(width, height);
                tex.icon_state = candidate && IsIconSheet(src, mips[0], bpe, tiling) ? 2 : 1;
                if (tex.icon_state == 2)
                    std::fprintf(stderr, "[icons] the game's icon sheet is at 0x%llx\n", (unsigned long long)base);
            }
            const bool icons_changed = tex.image && tex.icon_state == 2 && tex.icon_set != icons::Applicable();
            if (new_contents || icons_changed) {  // memory was reused for new contents, or other icons: re-upload
                // The frame still on the GPU may sample the old image: retire it, destroy it later.
                Retired& old = retired[slot];
                for (auto& [s, v] : tex.views) old.views.push_back(v);
                tex.views.clear();
                old.images.push_back({tex.image, tex.memory});
                if (tex.alloc.block != ~0u) old.allocs.push_back(tex.alloc);
                tex.image = VK_NULL_HANDLE;
                tex.memory = VK_NULL_HANDLE;
                tex.alloc = {};
            }
            if (!tex.image) {
                const int set = tex.icon_state == 2 ? icons::Applicable() : int(icons::kPlayStation);
                if (const std::vector<uint8_t>* px = icons::Pixels(set)) {
                    const VkFormat rgba = format == VK_FORMAT_BC3_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                    if (!UploadPixels(tex, px->data(), width, height, rgba)) return VK_NULL_HANDLE;
                } else if (!Upload(tex, src, mips, shape, format, width, height, bpe, block, tiling)) {
                    return VK_NULL_HANDLE;
                }
                tex.icon_set = set;
                tex.content_hash = h;
            }
        }
        if (!tex.image) return VK_NULL_HANDLE;
        auto [it, inserted] = tex.views.try_emplace(swizzle, VK_NULL_HANDLE);
        if (inserted) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = tex.image;
            vci.viewType = itype == gcn::ImageType::Tex3D        ? VK_IMAGE_VIEW_TYPE_3D
                           : itype == gcn::ImageType::Cube       ? VK_IMAGE_VIEW_TYPE_CUBE
                           : itype == gcn::ImageType::Tex2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                 : VK_IMAGE_VIEW_TYPE_2D;
            vci.format = tex.format;
            vci.components = {Sel(swizzle), Sel(swizzle >> 3), Sel(swizzle >> 6), Sel(swizzle >> 9)};
            const uint32_t first = std::min(base_level, tex.mips - 1);
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, first, tex.mips - first, 0, tex.layers};
            vkCreateImageView(d, &vci, nullptr, &it->second);
        }
        return it->second;
    }

    // Runs fn(0..n-1) on the calling thread plus a few persistent workers.
    static void ParallelFor(size_t n, const std::function<void(size_t)>& fn) {
        struct Pool {
            std::mutex m;
            std::condition_variable cv, done_cv;
            const std::function<void(size_t)>* job = nullptr;
            std::atomic<size_t> next{0};
            size_t count = 0, generation = 0;
            int busy = 0;
            std::vector<std::thread> threads;
            explicit Pool(unsigned workers) {
                for (unsigned i = 0; i < workers; ++i)
                    threads.emplace_back([this] {
                        size_t seen = 0;
                        for (;;) {
                            const std::function<void(size_t)>* f;
                            size_t total;
                            {
                                std::unique_lock lock(m);
                                cv.wait(lock, [&] { return generation != seen; });
                                seen = generation;
                                f = job;
                                total = count;
                                ++busy;
                            }
                            for (size_t i; (i = next++) < total;) (*f)(i);
                            std::lock_guard lock(m);
                            if (--busy == 0) done_cv.notify_all();
                        }
                    });
                for (auto& t : threads) t.detach();
            }
        };
        static Pool pool(std::max(1u, std::min(6u, std::thread::hardware_concurrency() / 2)));
        {
            std::lock_guard lock(pool.m);
            pool.job = &fn;
            pool.count = n;
            pool.next = 0;
            ++pool.generation;
        }
        pool.cv.notify_all();
        for (size_t i; (i = pool.next++) < n;) fn(i);
        std::unique_lock lock(pool.m);
        pool.done_cv.wait(lock, [&] { return pool.busy == 0; });
    }

    bool Upload(Texture& tex, const uint8_t* src, const std::vector<gpu::MipLayout>& mips, const Shape& shape,
                VkFormat format, uint32_t width, uint32_t height, uint32_t bpe, bool block, uint32_t tiling) {
        const bool is3d = shape.type == gcn::ImageType::Tex3D;
        auto slices_at = [&](uint32_t level) { return is3d ? std::max(1u, shape.depth >> level) : shape.layers; };
        VkDeviceSize total = 0;
        for (uint32_t l = 0; l < mips.size(); ++l)
            total += ((uint64_t(mips[l].width) * mips[l].height * bpe + 15) & ~VkDeviceSize(15)) * slices_at(l);
        if (upload_used + total > upload_end) return false;  // full for this frame; try again next frame
        EndRendering();
        tex.format = format;
        tex.mips = uint32_t(mips.size());
        tex.layers = shape.layers;
        const auto t_create = std::chrono::steady_clock::now();
        tex.image = CreateTextureImage(is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, format, width, height, shape.depth,
                                       shape.layers, tex.mips,
                                       shape.type == gcn::ImageType::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                                       tex.memory, tex.alloc);
        if (!tex.image) return false;
        const auto t_detile = std::chrono::steady_clock::now();
        tex_create_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_detile - t_create).count());
        struct AddDetile {
            uint64_t& to;
            std::chrono::steady_clock::time_point t;
            ~AddDetile() { to += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); }
        } add_detile{tex_detile_ns, t_detile};
        ToGeneral(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, shape.layers, tex.mips);
        std::vector<VkBufferImageCopy> copies;
        // Detile every level and slice into its own part of the upload buffer, several at once on the larger
        // textures (a new area brings in ~100 textures in one frame).
        struct Job {
            const uint8_t* src;
            uint8_t* dst;
            const gpu::MipLayout* mip;
            uint32_t y0, y1;  // element rows (large levels are split into strips)
        };
        std::vector<Job> jobs;
        uint64_t job_bytes = 0;
        constexpr uint32_t kStripBytes = 256u << 10;
        for (uint32_t level = 0, used = 0; level < mips.size(); ++level) {
            const auto& m = mips[level];
            const uint64_t row_bytes = uint64_t(m.width) * bpe;
            // Strip height: whole micro tiles (8 rows), about kStripBytes of output each.
            const uint32_t strip = std::max(8u, uint32_t(kStripBytes / std::max<uint64_t>(1, row_bytes)) & ~7u);
            for (uint32_t s = 0; s < slices_at(level); ++s) {
                for (uint32_t y = 0; y < m.height; y += strip)
                    jobs.push_back({src + m.offset + s * m.size, upload_map + upload_used + used, &m, y,
                                    std::min(m.height, y + strip)});
                const uint64_t bytes = (uint64_t(m.width) * m.height * bpe + 15) & ~VkDeviceSize(15);
                used += uint32_t(bytes);
                job_bytes += bytes;
            }
        }
        std::atomic<bool> detile_ok{true};
        auto run = [&](const Job& j) {
            if (!gpu::DetileRows(tiling, j.src, j.dst, *j.mip, bpe, j.y0, j.y1)) detile_ok = false;
        };
        if (job_bytes >= (512u << 10) && jobs.size() > 1) {
            ParallelFor(jobs.size(), [&](size_t i) { run(jobs[i]); });
        } else {
            for (const Job& j : jobs) run(j);
        }
        if (!detile_ok) return false;
        for (uint32_t level = 0; level < mips.size(); ++level) {
            const auto& m = mips[level];
            for (uint32_t s = 0; s < slices_at(level); ++s) {
                VkBufferImageCopy c{};
                c.bufferOffset = upload_used;
                c.bufferRowLength = m.width * (block ? 4 : 1);
                c.bufferImageHeight = m.height * (block ? 4 : 1);
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, is3d ? 0 : s, 1};
                c.imageOffset = {0, 0, is3d ? int32_t(s) : 0};
                c.imageExtent = {std::max(1u, width >> level), std::max(1u, height >> level), 1};
                copies.push_back(c);
                upload_used += (uint64_t(m.width) * m.height * bpe + 15) & ~VkDeviceSize(15);
            }
        }
        vkCmdCopyBufferToImage(cmd, upload, tex.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()), copies.data());
        FullBarrier();
        return true;
    }

    // Is this texture's level 0 the game's button icon sheet? Compares its blocks, detiled, with the game's file.
    static bool IsIconSheet(const uint8_t* src, const gpu::MipLayout& m, uint32_t bpe, uint32_t tiling) {
        std::vector<uint8_t> linear(size_t(m.width) * m.height * bpe);
        if (!gpu::DetileRows(tiling, src + m.offset, linear.data(), m, bpe, 0, m.height)) return false;
        return icons::IsGameSheet(linear.data(), linear.size());
    }

    // A one-level 2D texture from host pixels (4 bytes each, top row first): the replacement icon sheets.
    bool UploadPixels(Texture& tex, const uint8_t* pixels, uint32_t width, uint32_t height, VkFormat format) {
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        const VkDeviceSize total = (bytes + 15) & ~VkDeviceSize(15);
        if (upload_used + total > upload_end) return false;  // full for this frame; try again next frame
        EndRendering();
        tex.format = format;
        tex.mips = 1;
        tex.layers = 1;
        tex.image = CreateTextureImage(VK_IMAGE_TYPE_2D, format, width, height, 1, 1, 1, 0, tex.memory, tex.alloc);
        if (!tex.image) return false;
        ToGeneral(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        std::memcpy(upload_map + upload_used, pixels, size_t(bytes));
        VkBufferImageCopy c{};
        c.bufferOffset = upload_used;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, upload, tex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        FullBarrier();
        upload_used += total;
        return true;
    }

    VkSampler GetSampler(const uint32_t* s) {
        const uint64_t h = FastHash(s, 16);
        auto [it, inserted] = samplers.try_emplace(h, VK_NULL_HANDLE);
        if (!inserted) return it->second;
        static const VkSamplerAddressMode kClamp[8] = {
            VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.addressModeU = kClamp[s[0] & 7];
        sci.addressModeV = kClamp[(s[0] >> 3) & 7];
        sci.addressModeW = kClamp[(s[0] >> 6) & 7];
        const uint32_t aniso = (s[0] >> 9) & 7;
        const uint32_t mag = (s[2] >> 20) & 3, min = (s[2] >> 22) & 3, mip = (s[2] >> 26) & 3;
        sci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.anisotropyEnable = aniso > 0 && (min >= 2 || mag >= 2);
        sci.maxAnisotropy = float(1u << aniso);
        // LOD clamps and bias vary per texture; rounded to quarter steps (and the upper clamp left to the
        // image view's own mip count) so the samplers stay few: the driver allows only ~4000.
        auto quarter = [](float v) { return std::round(v * 4.0f) / 4.0f; };
        sci.minLod = quarter(float(s[1] & 0xFFF) / 256.0f);
        sci.maxLod = mip == 0 ? sci.minLod + 0.25f : VK_LOD_CLAMP_NONE;
        sci.mipLodBias = quarter(float(int32_t(s[2] << 18) >> 18) / 256.0f);  // signed 6.8 fixed point
        const uint32_t cmp = (s[0] >> 12) & 7;
        sci.compareEnable = cmp != 0;
        sci.compareOp = CompareOp(cmp);
        sci.borderColor = (s[3] >> 30) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                        : (s[3] >> 30) == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        // Many S#s differ only in bits that don't change the Vulkan sampler; the driver allows ~4000
        // samplers in all (running out broke the overlay and would break new textures), so samplers are
        // shared by their Vulkan state.
        const float state[] = {float(sci.addressModeU), float(sci.addressModeV), float(sci.addressModeW),
                               float(sci.magFilter),    float(sci.minFilter),    float(sci.mipmapMode),
                               float(sci.anisotropyEnable), sci.maxAnisotropy,   sci.minLod,
                               sci.maxLod,              sci.mipLodBias,          float(sci.compareEnable),
                               float(sci.compareOp),    float(sci.borderColor)};
        const uint64_t sh = FastHash(state, sizeof state);
        auto [shared, fresh] = samplers_by_state.try_emplace(sh, VK_NULL_HANDLE);
        if (fresh && vkCreateSampler(d, &sci, nullptr, &shared->second) != VK_SUCCESS) {
            std::fprintf(stderr, "[renderer] sampler creation failed (%zu made)\n", samplers_by_state.size());
            shared->second = sampler;  // the default
        }
        it->second = shared->second;
        return it->second;
    }
    std::unordered_map<uint64_t, VkSampler> samplers_by_state;

    struct ResolvedTextures {
        std::vector<VkImageView> views;
        std::vector<VkSampler> samplers;
        uint32_t target_mask = 0;  // bindings that are render targets
    };
    ResolvedTextures ResolveTextures(const std::vector<gcn::ResolvedImage>& images) {
        ResolvedTextures r;
        r.views.reserve(images.size());
        r.samplers.reserve(images.size());
        for (const auto& img : images) {
            LogTexture(img.tsharp);
            VkImageView view = TargetView(img.tsharp);
            if (view && r.views.size() < 32) r.target_mask |= 1u << r.views.size();
            if (!view) view = EnsureTexture(img.tsharp);
            if (!view) {
                static std::map<uint64_t, int> logged;
                const uint32_t* t = img.tsharp;
                const uint64_t base = ((uint64_t(t[1] & 0xFF) << 32) | t[0]) << 8;
                if (logged[base]++ == 0 && logged.size() < 40)
                    std::fprintf(stderr, "[renderer] placeholder for texture %llx: dfmt %u nfmt %u %ux%u tiling %u type %u\n",
                                 (unsigned long long)base, (t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF, (t[2] & 0x3FFF) + 1,
                                 ((t[2] >> 14) & 0x3FFF) + 1, (t[3] >> 20) & 0x1F, (t[3] >> 28) & 0xF);
            }
            r.views.push_back(view);
            r.samplers.push_back(GetSampler(img.ssharp));
        }
        return r;
    }

    // Survey: logs each distinct T# layout once (format, size, tiling) with LDPORT_TEXLOG=1.
    void LogTexture(const uint32_t* t) {
        static const bool on = std::getenv("LDPORT_TEXLOG") != nullptr;
        if (!on) return;
        static std::map<uint64_t, int> seen;
        const uint64_t base = ((uint64_t(t[1] & 0xFF) << 32) | t[0]) << 8;
        if (seen[base]++) return;
        const uint32_t dfmt = (t[1] >> 20) & 0x3F, nfmt = (t[1] >> 26) & 0xF;
        const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
        const uint32_t tiling = (t[3] >> 20) & 0x1F, type = (t[3] >> 28) & 0xF, last_level = (t[3] >> 16) & 0xF;
        const uint32_t depth = (t[4] & 0x1FFF) + 1, pitch = ((t[4] >> 13) & 0x3FFF) + 1;
        bool is_rt = false;
        for (const auto& [k, tg] : targets) is_rt |= tg.address == base;
        std::fprintf(stderr,
                     "[tex] %llx dfmt %u nfmt %u %ux%u depth %u pitch %u mips %u tiling %u type %u dst_sel %03x words %08x %08x %08x %08x %08x %08x %08x %08x%s\n",
                     (unsigned long long)base, dfmt, nfmt, w, h, depth, pitch, last_level + 1, tiling, type, t[3] & 0xFFF,
                     t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], is_rt ? " (render target)" : "");
    }

    // A view of the render target a T# points at (with the T#'s channel swizzle), or null.
    std::map<std::pair<VkImage, uint32_t>, VkImageView> target_views;
    // The target sampled at each address (the first in `targets` order), found once: this runs for
    // every texture of every draw. Cleared when a target is added.
    std::unordered_map<uint64_t, const Target*> target_at;
    std::unordered_map<uint64_t, VkImageView> target_view_cache;  // (image, swizzle) -> view
    VkImageView TargetView(const uint32_t* t) {
        const uint64_t base = ((uint64_t(t[1] & 0xFF) << 32) | t[0]) << 8;
        auto [at, fresh] = target_at.try_emplace(base, nullptr);
        if (fresh) {
            const bool grown = grown_at.count(base) != 0;
            for (const auto& [key, tg] : targets)
                if (tg.address == base &&
                    (!at->second || (grown && !tg.depth && uint64_t(tg.width) * tg.height >
                                                               uint64_t(at->second->width) * at->second->height))) {
                    at->second = &tg;
                    if (!grown) break;
                }
            // A small target sampled as a bigger texture: the game drew into the corner of a larger surface and
            // reads it with that surface's coordinates (the HUD's character portrait is drawn at 256x256 into a
            // 1024x1024 surface, then sampled as 1024x1024; stretching the 256x256 image over the whole texture
            // showed only the portrait's empty corner). Make a target at the sampled size; draws there then go to it
            // (GetTarget prefers it), into its corner as on the console. Small surfaces only, never the screen.
            const Target* seen = at->second;
            const uint32_t tw = (t[2] & 0x3FFF) + 1, th = ((t[2] >> 14) & 0x3FFF) + 1;
            if (seen && !seen->depth && !grown && seen->width < 1280 && tw <= 2048 && th <= 2048 &&
                tw >= seen->width && th >= seen->height && (tw > seen->width || th > seen->height)) {
                const VkFormat format = seen->format;
                if (const Target* big = GetTarget(base, format, tw, th, false)) {  // clears target_at
                    grown_at.insert(base);
                    target_at[base] = big;
                    std::fprintf(stderr, "[renderer] target %llx sampled as %ux%u: grown from %ux%u\n",
                                 (unsigned long long)base, tw, th, seen->width, seen->height);
                }
            }
        }
        const Target* target = target_at[base];
        if (!target) return VK_NULL_HANDLE;
        if (override_tex_from && target == override_tex_from) {  // the composite's re-draw
            target = override_tex_to;
            ++override_hits;
        }
        static const bool texlog = std::getenv("LDPORT_TEXLOG") != nullptr;
        if (texlog) {
            const uint32_t tw = (t[2] & 0x3FFF) + 1, th = ((t[2] >> 14) & 0x3FFF) + 1;
            static std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>, int> logged;
            if (logged[{base, tw, th, target->width, target->height}]++ == 0)
                std::fprintf(stderr, "[renderer] sample %llx as %ux%u: target is %ux%u%s\n", (unsigned long long)base, tw,
                             th, target->width, target->height,
                             tw == target->width && th == target->height ? "" : "  <-- size mismatch");
        }
        const uint32_t swizzle = t[3] & 0xFFF;
        const uint64_t vkey = reinterpret_cast<uint64_t>(target->image) * 4099 + swizzle;
        if (auto c = target_view_cache.find(vkey); c != target_view_cache.end()) return c->second;
        auto [it, inserted] = target_views.try_emplace({target->image, swizzle}, VK_NULL_HANDLE);
        if (!inserted) return target_view_cache[vkey] = it->second;
        struct Remember {
            std::unordered_map<uint64_t, VkImageView>& cache;
            uint64_t key;
            VkImageView& view;
            ~Remember() { cache[key] = view; }
        } remember{target_view_cache, vkey, it->second};
        static const VkComponentSwizzle kSel[8] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = target->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = target->format;
        if (target->depth) {
            vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};  // sample depth only
        } else {
            vci.components = {kSel[swizzle & 7], kSel[(swizzle >> 3) & 7], kSel[(swizzle >> 6) & 7], kSel[(swizzle >> 9) & 7]};
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCreateImageView(d, &vci, nullptr, &it->second);
        return it->second;
    }

    // Counts a draw the renderer can't do yet; logs the first few of each reason with the target.
    // --- camera tracking (temporal upscaling) ---
    // Scene vertex shaders read a per-frame constant block (found by a buffer descriptor in their user data):
    // [0..15] view-projection (row vectors: clip = pos x M), [16..31] view (last column 0,0,0,1), [32..35] camera
    // position (w 1), [36..37] near / far. The first scene draw of each frame that has one sets this frame's.
    struct Camera {
        bool valid = false;
        float vp[16], view[16], pos[4], near_far[2];
        uint64_t address = 0;
        const Target* depth = nullptr;   // the scene's depth buffer
        float z_offset = 0, z_scale = 1;  // viewport depth mapping (depth = z_ndc * scale + offset)
        bool y_down = true;               // negative viewport y scale (D3D: NDC y up = screen top)
    } cam_cur, cam_prev;

    static bool Readable(const void* p, size_t bytes) {
        // The last committed region seen (most blocks live in the same constant ring): no system call for them.
        static thread_local uintptr_t cached_lo = 0, cached_hi = 0;
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        if (a >= cached_lo && a + bytes <= cached_hi) return true;
#ifndef _WIN32
        uint8_t probe[256];
        for (size_t at = 0; at < bytes; at += sizeof probe)
            if (!host::SafeCopy(probe, static_cast<const uint8_t*>(p) + at, std::min(sizeof probe, bytes - at))) return false;
        return true;
#else
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        cached_lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        cached_hi = cached_lo + mbi.RegionSize;
        return a + bytes <= cached_hi;
#endif
    }

    // Every camera seen this frame and how many draws used it: the scene's camera is the busiest (the HUD's 3D
    // character portrait draws into the same depth buffer with its own camera, often first).
    struct Candidate {
        Camera cam;
        uint32_t draws = 0;
    };
    std::vector<Candidate> cam_candidates;
    std::unordered_set<uint64_t> not_cameras;  // blocks that failed the checks (cleared every frame)

    // --- ultrawide 2D (Display Settings: Ultrawide) ---
    // 2D keeps the game's 16:9 in the middle of the wide screen: the HUD over a 3D frame is drawn squeezed
    // (Draw), and a frame without a 3D camera (menus, the Toy Pad screen, movies) is resampled into the middle
    // with black bars at the flip.
    uint32_t disp_any_draws = 0;  // draws into a display buffer this frame
    // HUD detection (Draw): the scene's colour buffer, the bloom chain seen, the HUD begun (reset each frame).
    uint64_t scene_color = 0;
    bool post_seen = false, hud_started = false;
    VkImage pb_tmp = VK_NULL_HANDLE;
    VkDeviceMemory pb_mem = VK_NULL_HANDLE;
    uint32_t pb_w = 0, pb_h = 0;
    VkFormat pb_format = VK_FORMAT_UNDEFINED;

    // The 16:9 share of the screen width (1 when not ultrawide).
    static float UltrawideSqueeze() {
        static const float s = 1920.0f / float(port::UltrawideWidth(port::BootUltrawide()));
        return s;
    }

    void Pillarbox(const Target& t) {
        if (pb_w != t.pw || pb_h != t.ph || pb_format != t.format || !pb_tmp) {
            if (pb_tmp) vkDestroyImage(d, pb_tmp, nullptr);
            if (pb_mem) vkFreeMemory(d, pb_mem, nullptr);
            pb_tmp = CreateImage(VK_IMAGE_TYPE_2D, t.format, t.pw, t.ph, 1, 1, 1,
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, pb_mem);
            if (!pb_tmp) return;
            ToGeneral(pb_tmp, VK_IMAGE_ASPECT_COLOR_BIT);
            pb_w = t.pw;
            pb_h = t.ph;
            pb_format = t.format;
        }
        BeginFrame();
        EndRendering();
        FullBarrier();
        VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, pb_tmp, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        FullBarrier();
        const int32_t w = int32_t(std::lround(float(t.pw) * UltrawideSqueeze())), x = (int32_t(t.pw) - w) / 2;
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {int32_t(t.pw), int32_t(t.ph), 1};
        b.dstOffsets[0] = {x, 0, 0};
        b.dstOffsets[1] = {x + w, int32_t(t.ph), 1};
        vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, pb_tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        FullBarrier();
        VkImageCopy cp{};
        cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.extent = {t.pw, t.ph, 1};
        vkCmdCopyImage(cmd, pb_tmp, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        FullBarrier();
    }

    // --- pre-HUD picture in display colours (gameplay: the HUD is drawn into the scene buffer) ---
    Target ph_scene, ph_disp;                 // copies: the scene before the HUD; its composite into display colours
    const Target* ph_scene_from = nullptr;    // the scene buffer ph_scene was copied from this frame
    const Target* override_color_from = nullptr;  // while re-drawing the composite: its target and source swapped
    const Target* override_color_to = nullptr;
    const Target* override_tex_from = nullptr;  // sampled instead: ph_scene for the scene buffer
    const Target* override_tex_to = nullptr;
    bool in_dup = false;
    uint32_t override_hits = 0;

    // A spare image shaped like `like` (format and size), recreated when that changes.
    bool EnsureSpare(Target& t, const Target& like) {
        if (t.image && t.format == like.format && t.pw == like.pw && t.ph == like.ph) return true;
        if (t.view) vkDestroyImageView(d, t.view, nullptr);
        if (t.image) vkDestroyImage(d, t.image, nullptr);
        if (t.memory) vkFreeMemory(d, t.memory, nullptr);
        t = Target{};
        t.image = CreateImage(VK_IMAGE_TYPE_2D, like.format, like.pw, like.ph, 1, 1, 1,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              0, t.memory);
        if (!t.image) return false;
        t.view = CreateView(t.image, VK_IMAGE_VIEW_TYPE_2D, like.format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        ToGeneral(t.image, VK_IMAGE_ASPECT_COLOR_BIT);
        t.format = like.format;
        t.width = like.width;
        t.height = like.height;
        t.pw = like.pw;
        t.ph = like.ph;
        return true;
    }

    // At a flip: this frame's camera is the candidate with the most draws.
    void PickCamera() {
        const Candidate* best = nullptr;
        for (const Candidate& c : cam_candidates)
            if (!best || c.draws > best->draws) best = &c;
        if (best) cam_cur = best->cam;
        cam_candidates.clear();
        not_cameras.clear();
    }

    // Returns the candidate this draw's camera is (-1: none).
    int TrackCamera(const uint32_t* user, uint32_t count, const Target* depth_target, float zo, float zs, bool y_down) {
        for (uint32_t i = 0; i + 4 <= count; ++i) {
            const uint32_t* v = user + i;
            if ((v[3] & 0xFFF) != 0xFAC || (v[1] & 0xFFFF) == 0 || (v[1] & 0xFFFF) > 0xFF || !v[0]) continue;
            const uint32_t stride = (v[1] >> 16) & 0x3FFF;
            const uint64_t bytes = stride ? uint64_t(stride) * v[2] : v[2];
            if (bytes < 160) continue;
            const uint64_t base = (uint64_t(v[1] & 0xFFFF) << 32) | v[0];
            for (size_t k = 0; k < cam_candidates.size(); ++k)
                if (cam_candidates[k].cam.address == base && cam_candidates[k].cam.depth == depth_target) {
                    ++cam_candidates[k].draws;
                    return int(k);
                }
            if (not_cameras.count(base)) continue;
            const float* f = reinterpret_cast<const float*>(base);
            if (!Readable(f, 160)) {
                not_cameras.insert(base);
                continue;
            }
            auto near0 = [](float x) { return std::fabs(x) < 1e-4f; };
            auto near1 = [](float x) { return std::fabs(x - 1.0f) < 1e-4f; };
            const float row = f[16] * f[16] + f[17] * f[17] + f[18] * f[18];  // view rotation rows are unit length
            if (!near0(f[19]) || !near0(f[23]) || !near0(f[27]) || !near1(f[31]) || !near1(f[35]) ||
                !(f[36] > 0.0f && f[37] > f[36] * 2.0f) || std::fabs(row - 1.0f) > 0.01f) {
                not_cameras.insert(base);
                continue;
            }
            // Ultrawide (Display Settings): cutscene cameras keep 16:9; widen them horizontally (Hor+) before the GPU reads
            // them: the view-projection's x column and the tan(half FOV) x at [40] (its ratio to [41] is the aspect).
            static const float wide = port::BootUltrawide() ? float(port::UltrawideWidth(port::BootUltrawide())) / 1080.0f : 0.0f;
            if (wide > 16.0f / 9.0f + 0.01f && Readable(f, 168) && f[41] > 0.0f &&
                std::fabs(f[40] / f[41] - 16.0f / 9.0f) < 0.01f) {
                float* w = const_cast<float*>(f);
                const float k = (16.0f / 9.0f) / wide;
                w[0] *= k;
                w[4] *= k;
                w[8] *= k;
                w[12] *= k;
                w[40] /= k;
            }
            Candidate cand;
            Camera& c = cand.cam;
            std::memcpy(c.vp, f, 64);
            std::memcpy(c.view, f + 16, 64);
            std::memcpy(c.pos, f + 32, 16);
            c.near_far[0] = f[36];
            c.near_far[1] = f[37];
            c.address = base;
            c.depth = depth_target;
            c.z_offset = zo;
            c.z_scale = zs;
            c.y_down = y_down;
            c.valid = true;
            cand.draws = 1;
            if (cam_candidates.size() >= 16) return -1;
            cam_candidates.push_back(cand);
            return int(cam_candidates.size()) - 1;
        }
        return -1;
    }

    // --- motion vectors: depth -> position (inverse current view-projection) -> previous view-projection ---
    VkPipeline mv_pipe = VK_NULL_HANDLE, mv_show_pipe = VK_NULL_HANDLE;
    VkPipelineLayout mv_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mv_set_layout = VK_NULL_HANDLE;
    VkSampler mv_sampler = VK_NULL_HANDLE;
    VkImage mv_image = VK_NULL_HANDLE, mv_show = VK_NULL_HANDLE;
    VkDeviceMemory mv_mem = VK_NULL_HANDLE, mv_show_mem = VK_NULL_HANDLE;
    VkImageView mv_view = VK_NULL_HANDLE, mv_show_view = VK_NULL_HANDLE;
    uint32_t mv_w = 0, mv_h = 0;
    std::map<VkImage, VkImageView> depth_views;  // depth-only sampled views of depth targets

    static void Inverse4(const float* m, float* out) {  // general 4x4 inverse (row-major)
        float inv[16];
        inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
        inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
        inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
        inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
        inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
        inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
        double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
        if (det == 0) det = 1;
        for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
    }

    VkPipeline CompileCompute(const char* name, const char* src) {
        shaderc_compile_options_t opts = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compilation_result_t r =
            shaderc_compile_into_spv(compiler, src, std::strlen(src), shaderc_compute_shader, name, "main", opts);
        VkPipeline pipe = VK_NULL_HANDLE;
        if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = shaderc_result_get_length(r);
            ci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(d, &ci, nullptr, &m);
            VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
            pci.layout = mv_layout;
            vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe);
            vkDestroyShaderModule(d, m, nullptr);
        } else {
            std::fprintf(stderr, "[motion] %s: %s\n", name, shaderc_result_get_error_message(r));
        }
        shaderc_result_release(r);
        shaderc_compile_options_release(opts);
        return pipe;
    }

    bool InitMotion() {
        if (mv_layout) return mv_pipe != VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(d, &sci, nullptr, &mv_sampler);
        const VkDescriptorSetLayoutBinding binds[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dl.bindingCount = 2;
        dl.pBindings = binds;
        vkCreateDescriptorSetLayout(d, &dl, nullptr, &mv_set_layout);
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &mv_set_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(d, &pl, nullptr, &mv_layout);
        // Motion: per pixel, where it was last frame (pixels, current -> previous).
        mv_pipe = CompileCompute("motion", R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D motion;
layout(push_constant) uniform P { mat4 m; vec4 map; vec4 size; } pc;  // map: z offset, z scale, y down
void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
    const float d = texelFetch(depth_tex, p, 0).r;
    const vec2 uv = (vec2(p) + 0.5) / pc.size.xy;
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, pc.map.z > 0.5 ? 1.0 - uv.y * 2.0 : uv.y * 2.0 - 1.0);
    const vec4 prev = pc.m * vec4(ndc, (d - pc.map.x) / pc.map.y, 1.0);
    vec2 prev_ndc = prev.xy / prev.w;
    const vec2 prev_uv = vec2(prev_ndc.x * 0.5 + 0.5, pc.map.z > 0.5 ? 0.5 - prev_ndc.y * 0.5 : prev_ndc.y * 0.5 + 0.5);
    imageStore(motion, p, vec4((prev_uv - uv) * pc.size.xy, 0.0, 0.0));
}
)");
        // Debug (LDPORT_SHOW_MOTION=1): the motion as colours (grey = still).
        mv_show_pipe = CompileCompute("motion_show", R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D motion;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D outp;
layout(push_constant) uniform P { mat4 m; vec4 map; vec4 size; } pc;
void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
    const vec2 mv = texelFetch(motion, p, 0).xy;
    imageStore(outp, p, vec4(clamp(0.5 + mv.x * 0.02, 0.0, 1.0), clamp(0.5 + mv.y * 0.02, 0.0, 1.0), 0.5, 1.0));
}
)");
        std::fprintf(stderr, "[motion] %s\n", mv_pipe ? "ready" : "unavailable");
        return mv_pipe != VK_NULL_HANDLE;
    }

    void MakeImage(VkFormat format, uint32_t w, uint32_t h, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        if (view) vkDestroyImageView(d, view, nullptr);
        if (img) vkDestroyImage(d, img, nullptr);
        if (mem) vkFreeMemory(d, mem, nullptr);
        img = CreateImage(VK_IMAGE_TYPE_2D, format, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          0, mem);
        view = CreateView(img, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        ToGeneral(img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    void PushPass(VkPipeline pipe, VkImageView sampled, VkImageView storage, const void* consts, uint32_t w, uint32_t h) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        VkDescriptorImageInfo a{mv_sampler, sampled, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo b{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w2[2]{};
        w2[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &a, nullptr, nullptr};
        w2[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &b, nullptr, nullptr};
        push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mv_layout, 0, 2, w2);
        vkCmdPushConstants(cmd, mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 96, consts);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        FullBarrier();
    }

    // --- TAA (Anti-aliasing: TAA) ---
    // Scene draws (into the main depth buffer) get a sub-pixel viewport jitter; the display buffer is copied
    // before the HUD is drawn on it (just before its second draw: the first is the scene composite); at the end
    // of the frame the resolve blends that with last frame's result (reprojected by the motion vectors, clamped
    // to the 3x3 neighbourhood) and puts back every pixel the HUD changed.
    std::set<uint64_t> display_addrs;  // display buffers seen at flips
    uint32_t display_draws = 0;
    bool prehud_valid = false;
    uint64_t main_depth = 0;  // address of the scene depth buffer (from the camera's draw)
    VkImage prehud = VK_NULL_HANDLE, taa_out = VK_NULL_HANDLE, hist[2] = {};
    VkDeviceMemory prehud_mem = VK_NULL_HANDLE, taa_out_mem = VK_NULL_HANDLE, hist_mem[2] = {};
    VkImageView prehud_view = VK_NULL_HANDLE, taa_out_view = VK_NULL_HANDLE, hist_view[2] = {};
    uint32_t taa_w = 0, taa_h = 0;
    int hist_index = 0;
    bool hist_valid = false;
    VkPipeline taa_pipe = VK_NULL_HANDLE;
    VkPipelineLayout taa_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout taa_set_layout = VK_NULL_HANDLE;
    VkSampler taa_linear = VK_NULL_HANDLE;
    float jitter_x = 0, jitter_y = 0;  // this frame's, in target pixels
    uint32_t jittered_draws = 0, last_jittered_draws = 0;  // scene draws this frame (debug log)

    static bool Fsr2On() { return port::Fsr2Mode(port::Get().scale.load()); }
    static bool TaaOn() { return port::Get().aa.load() == 2 && !Fsr2On(); }
    static bool TemporalOn() { return TaaOn() || Fsr2On(); }
    static float Halton(uint32_t i, uint32_t b) {
        float f = 1, r = 0;
        for (; i; i /= b) { f /= float(b); r += f * float(i % b); }
        return r;
    }

    void EnsureTaaImages(uint32_t w, uint32_t h) {
        if (w == taa_w && h == taa_h && prehud) return;
        MakeImage(VK_FORMAT_R8G8B8A8_UNORM, w, h, prehud, prehud_mem, prehud_view);
        MakeImage(VK_FORMAT_R8G8B8A8_UNORM, w, h, taa_out, taa_out_mem, taa_out_view);
        for (int i = 0; i < 2; ++i) MakeImage(VK_FORMAT_R8G8B8A8_UNORM, w, h, hist[i], hist_mem[i], hist_view[i]);
        taa_w = w;
        taa_h = h;
        hist_valid = false;
    }

    bool InitTaa() {
        if (taa_layout) return taa_pipe != VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(d, &sci, nullptr, &taa_linear);
        VkDescriptorSetLayoutBinding binds[6];
        for (uint32_t i = 0; i < 6; ++i)
            binds[i] = {i, i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dl.bindingCount = 6;
        dl.pBindings = binds;
        vkCreateDescriptorSetLayout(d, &dl, nullptr, &taa_set_layout);
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &taa_set_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(d, &pl, nullptr, &taa_layout);
        const char* src = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D cur;     // this frame, before the HUD
layout(set = 0, binding = 1) uniform sampler2D motion;  // pixels, current -> previous
layout(set = 0, binding = 2) uniform sampler2D hist;    // last frame's result (scene only)
layout(set = 0, binding = 3) uniform sampler2D final_;  // this frame with the HUD
layout(set = 0, binding = 4, rgba8) uniform writeonly image2D hist_out;
layout(set = 0, binding = 5, rgba8) uniform writeonly image2D outp;
layout(push_constant) uniform P { vec4 size; vec4 params; } pc;  // size: w, h, 1/w, 1/h; params: blend, history ok
void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
    const ivec2 hi = ivec2(pc.size.xy) - 1;
    const vec3 c = texelFetch(cur, p, 0).rgb;
    vec3 mn = c, mx = c;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const vec3 s = texelFetch(cur, clamp(p + ivec2(x, y), ivec2(0), hi), 0).rgb;
            mn = min(mn, s);
            mx = max(mx, s);
        }
    const vec2 uv = (vec2(p) + 0.5) * pc.size.zw;
    const vec2 puv = uv + texelFetch(motion, min(p, textureSize(motion, 0) - 1), 0).xy * pc.size.zw;
    vec3 res = c;
    if (pc.params.y > 0.5 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0))))
        res = mix(clamp(textureLod(hist, puv, 0.0).rgb, mn, mx), c, pc.params.x);
    imageStore(hist_out, p, vec4(res, 1.0));
    const vec3 f = texelFetch(final_, p, 0).rgb;
    const bool hud = any(greaterThan(abs(f - c), vec3(0.5 / 255.0)));  // the HUD drew here: keep it as drawn
    imageStore(outp, p, vec4(hud ? f : res, 1.0));
}
)";
        // Reuse CompileCompute (it builds against mv_layout): build with the TAA layout instead.
        VkPipelineLayout saved = mv_layout;
        mv_layout = taa_layout;
        taa_pipe = CompileCompute("taa", src);
        mv_layout = saved;
        std::fprintf(stderr, "[taa] %s\n", taa_pipe ? "ready" : "unavailable");
        return taa_pipe != VK_NULL_HANDLE;
    }

    void TaaPass(uint64_t display_address) {
        const Target* disp = FindTargetByAddress(display_address);
        if (!disp || !cam_cur.valid || !cam_cur.depth || !InitMotion() || !InitTaa()) return;
        if (cam_cur.depth->pw != disp->pw || cam_cur.depth->ph != disp->ph) return;
        BeginFrame();
        EndRendering();
        EnsureTaaImages(disp->pw, disp->ph);
        if (!prehud_valid) {  // no HUD this frame: the final picture is the scene
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {disp->pw, disp->ph, 1};
            vkCmdCopyImage(cmd, disp->image, VK_IMAGE_LAYOUT_GENERAL, prehud, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            FullBarrier();
        }
        MotionPass(display_address, true);
        if (!mv_view || mv_w != disp->pw || mv_h != disp->ph) return;  // no motion vectors yet (first camera frame)
        // A camera cut (the camera jumps) starts the history again.
        const float dx = cam_cur.pos[0] - cam_prev.pos[0], dy = cam_cur.pos[1] - cam_prev.pos[1],
                    dz = cam_cur.pos[2] - cam_prev.pos[2];
        const bool cut = !cam_prev.valid || dx * dx + dy * dy + dz * dz > 4.0f;
        const int src = hist_index, dst = hist_index ^ 1;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taa_pipe);
        VkDescriptorImageInfo infos[6] = {
            {taa_linear, prehud_view, VK_IMAGE_LAYOUT_GENERAL}, {mv_sampler, mv_view, VK_IMAGE_LAYOUT_GENERAL},
            {taa_linear, hist_view[src], VK_IMAGE_LAYOUT_GENERAL}, {taa_linear, disp->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, hist_view[dst], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, taa_out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w6[6]{};
        for (uint32_t i = 0; i < 6; ++i)
            w6[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1,
                     i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[i],
                     nullptr, nullptr};
        push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taa_layout, 0, 6, w6);
        const float consts[8] = {float(taa_w), float(taa_h), 1.0f / float(taa_w), 1.0f / float(taa_h),
                                 0.1f, (hist_valid && !cut) ? 1.0f : 0.0f, 0, 0};
        vkCmdPushConstants(cmd, taa_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, consts);
        vkCmdDispatch(cmd, (taa_w + 7) / 8, (taa_h + 7) / 8, 1);
        FullBarrier();
        VkImageCopy cp{};
        cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.extent = {taa_w, taa_h, 1};
        vkCmdCopyImage(cmd, taa_out, VK_IMAGE_LAYOUT_GENERAL, disp->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        FullBarrier();
        hist_index = dst;
        hist_valid = true;
    }

    // End of a frame: motion vectors for this frame (needs this and last frame's camera). With
    // LDPORT_SHOW_MOTION=1 they replace the picture in the display target.
    void MotionPass(uint64_t display_address, bool for_taa = false) {
        static const bool show = std::getenv("LDPORT_SHOW_MOTION") != nullptr;
        if (!show && !for_taa) return;
        if (!cam_cur.valid || !cam_prev.valid || !cam_cur.depth || !InitMotion()) return;
        const Target& dt = *cam_cur.depth;
        if (dt.pw != mv_w || dt.ph != mv_h) {
            BeginFrame();
            EndRendering();
            MakeImage(VK_FORMAT_R16G16_SFLOAT, dt.pw, dt.ph, mv_image, mv_mem, mv_view);
            MakeImage(VK_FORMAT_R8G8B8A8_UNORM, dt.pw, dt.ph, mv_show, mv_show_mem, mv_show_view);
            mv_w = dt.pw;
            mv_h = dt.ph;
        }
        VkImageView& dv = depth_views[dt.image];
        if (!dv) dv = CreateView(dt.image, VK_IMAGE_VIEW_TYPE_2D, dt.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
        BeginFrame();
        EndRendering();
        struct { float m[16]; float map[4]; float size[4]; } c{};
        float inv[16];
        Inverse4(cam_cur.vp, inv);
        for (int r = 0; r < 4; ++r)  // M = inverse(VP now) x VP previous (row vectors)
            for (int col = 0; col < 4; ++col) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += inv[r * 4 + k] * cam_prev.vp[k * 4 + col];
                c.m[r * 4 + col] = sum;
            }
        c.map[0] = cam_cur.z_offset;
        c.map[1] = cam_cur.z_scale != 0.0f ? cam_cur.z_scale : 1.0f;
        c.map[2] = cam_cur.y_down ? 1.0f : 0.0f;
        c.size[0] = float(mv_w);
        c.size[1] = float(mv_h);
        FullBarrier();
        PushPass(mv_pipe, dv, mv_view, &c, mv_w, mv_h);
        if (show && !for_taa && mv_show_pipe) {
            PushPass(mv_show_pipe, mv_view, mv_show_view, &c, mv_w, mv_h);
            if (const Target* disp = FindTargetByAddress(display_address)) {
                VkImageCopy cp{};
                cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                cp.extent = {std::min(mv_w, disp->pw), std::min(mv_h, disp->ph), 1};
                vkCmdCopyImage(cmd, mv_show, VK_IMAGE_LAYOUT_GENERAL, disp->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
                FullBarrier();
            }
        }
    }

    // --- FSR 2 (Resolution: FSR 2 modes) ---
    // The scene renders at the lower resolution with the TAA jitter; at the end of the frame FSR 2 upscales the
    // pre-HUD picture (with the main depth and the motion vectors) to the size the picture is shown at, and the
    // composite pass puts the HUD back on (bilinear from the render-size frame, where the HUD drew).
    FfxFsr2Context f2_ctx{};
    std::vector<uint8_t> f2_scratch;
    bool f2_live = false, f2_failed = false, f2_inverted = false, f2_ran = false;
    uint32_t f2_ow = 0, f2_oh = 0;  // context's display size
    VkImage f2_color = VK_NULL_HANDLE, f2_out = VK_NULL_HANDLE, f2_final = VK_NULL_HANDLE;
    VkDeviceMemory f2_color_mem = VK_NULL_HANDLE, f2_out_mem = VK_NULL_HANDLE, f2_final_mem = VK_NULL_HANDLE;
    VkImageView f2_color_view = VK_NULL_HANDLE, f2_out_view = VK_NULL_HANDLE, f2_final_view = VK_NULL_HANDLE;
    uint32_t f2_cw = 0, f2_ch = 0, f2_iw = 0, f2_ih = 0;  // colour input size, output image size
    VkPipeline f2_comp_pipe = VK_NULL_HANDLE;
    bool f2_hist_valid = false;

    static void Fsr2Message(FfxFsr2MsgType type, const wchar_t* message) {
        std::fprintf(stderr, "[fsr2] %s: %ls\n", type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
    }

    // The camera's projection (view^-1 x view-projection, row vectors): vertical FOV, and whether depth runs
    // far = 0 .. near = 1 (inverted).
    void CameraProjection(float& fov, bool& inverted) const {
        float iv[16], P[16];
        Inverse4(cam_cur.view, iv);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += iv[r * 4 + k] * cam_cur.vp[k * 4 + c];
                P[r * 4 + c] = sum;
            }
        fov = 2.0f * std::atan(1.0f / std::max(1e-4f, std::fabs(P[5])));
        const float n = cam_cur.near_far[0], f = cam_cur.near_far[1];
        auto depth_at = [&](float z) {
            const float cz = z * P[10] + P[14], cw = z * P[11] + P[15];
            return (cw != 0.0f ? cz / cw : 0.0f) * cam_cur.z_scale + cam_cur.z_offset;
        };
        const float sgn = (-n * P[11] + P[15]) > 0.0f ? -1.0f : 1.0f;  // view space looks down -z or +z
        inverted = depth_at(sgn * n) > depth_at(sgn * f);
        if (g_f2_debug.load() & 32) inverted = !inverted;
        static uint64_t logged_frame = 0;
        if (frame >= logged_frame + 60) {
            logged_frame = frame;
            std::fprintf(stderr, "[fsr2] camera: aspect %.4f fov %.1f deg, near %.3f far %.1f, depth at near %.4f far %.4f (P10 %.4f P11 %.4f P14 %.4f P15 %.4f, zs %.3f zo %.3f)\n",
                         P[0] != 0.0f ? P[5] / P[0] : 0.0f, fov * 57.2958f, n, f, depth_at(sgn * n), depth_at(sgn * f), P[10], P[11], P[14], P[15],
                         cam_cur.z_scale, cam_cur.z_offset);
        }
    }

    bool EnsureFsr2Context(uint32_t ow, uint32_t oh, bool inverted) {
        if (f2_live && ow == f2_ow && oh == f2_oh && inverted == f2_inverted) return true;
        if (f2_failed) return false;
        if (f2_live) {
            vkDeviceWaitIdle(d);
            ffxFsr2ContextDestroy(&f2_ctx);
            f2_live = false;
        }
        VkPhysicalDevice phys = dev->physical();
        f2_scratch.assign(ffxFsr2GetScratchMemorySizeVK(phys), 0);
        FfxFsr2ContextDescription cd{};
        FfxErrorCode e = ffxFsr2GetInterfaceVK(&cd.callbacks, f2_scratch.data(), f2_scratch.size(), phys,
                                               vkGetDeviceProcAddr);
        if (e == FFX_OK) {
            cd.device = ffxGetDeviceVK(d);
            cd.maxRenderSize = {ow, oh};
            cd.displaySize = {ow, oh};
            // The input is the game's finished, display-referred picture: a fixed exposure of 1 (FSR 2's
            // default without an exposure texture). Auto exposure is for HDR input; on the intro's bright,
            // fast-changing nebula it flashed the swirls white. LDPORT_FSR2_AUTO_EXPOSURE=1 turns it back on.
            static const bool auto_exposure = std::getenv("LDPORT_FSR2_AUTO_EXPOSURE") != nullptr;
            cd.flags = (auto_exposure ? FFX_FSR2_ENABLE_AUTO_EXPOSURE : 0u) | (inverted ? FFX_FSR2_ENABLE_DEPTH_INVERTED : 0u);
            if (std::getenv("LDPORT_FSR2_DEBUG")) cd.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
            cd.fpMessage = Fsr2Message;
            e = ffxFsr2ContextCreate(&f2_ctx, &cd);
        }
        if (e != FFX_OK) {
            std::fprintf(stderr, "[fsr2] context creation failed (%d): FSR 2 modes fall back to FSR 1\n", int(e));
            f2_failed = true;
            return false;
        }
        std::fprintf(stderr, "[fsr2] ready: output %ux%u%s\n", ow, oh, inverted ? ", inverted depth" : "");
        f2_live = true;
        f2_ow = ow;
        f2_oh = oh;
        f2_inverted = inverted;
        f2_hist_valid = false;
        return true;
    }

    bool InitFsr2Composite() {
        if (f2_comp_pipe) return true;
        if (!InitTaa()) return false;
        // Bindings as the TAA layout: 0 FSR 2's result (display size, true RGB), 1 this frame with the HUD,
        // 2 the frame before the HUD (both render size, in the display's byte order), 4 the output.
        const char* src = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D up;
layout(set = 0, binding = 1) uniform sampler2D final_;
layout(set = 0, binding = 2) uniform sampler2D prehud;
layout(set = 0, binding = 4, rgba8) uniform writeonly image2D outp;
layout(push_constant) uniform P { vec4 size; vec4 rsize; } pc;  // size: w, h, 1/w, 1/h; rsize: w, h, final is BGRA
vec3 Final(ivec2 p) { const vec3 c = texelFetch(final_, p, 0).rgb; return pc.rsize.z > 0.5 ? c.bgr : c; }
float Hud(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(pc.rsize.xy) - 1);
    return any(greaterThan(abs(Final(p) - texelFetch(prehud, p, 0).rgb), vec3(0.5 / 255.0))) ? 1.0 : 0.0;
}
void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
    const vec2 uv = (vec2(p) + 0.5) * pc.size.zw;
    const vec2 rp = uv * pc.rsize.xy - 0.5;
    const ivec2 i0 = ivec2(floor(rp));
    const vec2 fr = rp - vec2(i0);
    const float w = mix(mix(Hud(i0), Hud(i0 + ivec2(1, 0)), fr.x), mix(Hud(i0 + ivec2(0, 1)), Hud(i0 + ivec2(1, 1)), fr.x), fr.y);
    vec3 c = texelFetch(up, p, 0).bgr;  // into the display's byte order
    if (pc.rsize.w > 1.5) { c = vec3(w); } else if (w > 0.0 && pc.rsize.w < 0.5) {
        vec3 h = textureLod(final_, uv, 0.0).rgb;
        if (pc.rsize.z > 0.5) h = h.bgr;
        c = mix(c, h, w);
    }
    imageStore(outp, p, vec4(c, 1.0));
}
)";
        VkPipelineLayout saved = mv_layout;
        mv_layout = taa_layout;
        f2_comp_pipe = CompileCompute("fsr2_composite", src);
        mv_layout = saved;
        return f2_comp_pipe != VK_NULL_HANDLE;
    }

    void Layout(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }

    // End of a frame in an FSR 2 mode: upscale into f2_final (the presenter shows it). False when FSR 2 can't
    // run this frame (no camera: menus, movies); the frame is then shown as rendered (the presenter uses FSR 1).
    bool dlss_was_used = false;
    uint32_t f2_why[8] = {};  // frames by outcome (logged every 120 frames)
    uint32_t f2_reason = 0;
    bool Fsr2Pass(uint64_t display_address) {
        const bool r = Fsr2PassInner(display_address);
        ++f2_why[r ? 0 : std::min(f2_reason, 7u)];
        if (frame % 120 == 0) {
            std::fprintf(stderr, "[fsr2] last 120 frames: %u upscaled; skipped: %u no output size, %u no camera, %u size, %u init, %u dispatch; %u resets, %u camera block changes\n",
                         f2_why[0], f2_why[1], f2_why[2], f2_why[3], f2_why[4], f2_why[5], f2_why[6], f2_why[7]);
            std::memset(f2_why, 0, sizeof f2_why);
        }
        return r;
    }
    bool Fsr2PassInner(uint64_t display_address) {
        f2_ran = false;
        const uint32_t ow = g_up_w.load(), oh = g_up_h.load();
        const Target* disp = FindTargetByAddress(display_address);
        f2_reason = !ow || !oh ? 1 : (!disp || !cam_cur.valid || !cam_cur.depth) ? 2 : 4;
        if (!ow || !oh || !disp || !cam_cur.valid || !cam_cur.depth || f2_failed) { f2_hist_valid = false; return false; }
        f2_reason = 3;
        const Target& dt = *cam_cur.depth;
        if (dt.pw != disp->pw || dt.ph != disp->ph || ow < disp->pw || oh < disp->ph) {
            static int logged = 0;
            if (logged++ < 3)
                std::fprintf(stderr, "[fsr2] size mismatch: depth %ux%u, display %ux%u, output %ux%u\n", dt.pw, dt.ph,
                             disp->pw, disp->ph, ow, oh);
            f2_hist_valid = false;
            return false;
        }
        f2_reason = 4;
        if (!InitMotion() || !InitFsr2Composite()) return false;
        float fov = 1.0f;
        bool inverted = false;
        CameraProjection(fov, inverted);
        // DLSS modes: NVIDIA DLSS when it runs here (started on first use), else FSR 2.
        const uint32_t mode = port::Get().scale.load();
        bool use_dlss = false;
        if (port::DlssMode(mode)) {
            if (!g_dlss_tried.exchange(true)) dlss::Init(dev->instance(), dev->physical(), d);
            use_dlss = dlss::Available();
        }
        if (!use_dlss && !EnsureFsr2Context(ow, oh, inverted)) return false;
        if (use_dlss != dlss_was_used) f2_hist_valid = false;  // switching upscalers: no history to carry
        dlss_was_used = use_dlss;
        BeginFrame();
        EndRendering();
        EnsureTaaImages(disp->pw, disp->ph);
        if (f2_cw != disp->pw || f2_ch != disp->ph || !f2_color) {
            if (f2_color_view) vkDestroyImageView(d, f2_color_view, nullptr);
            if (f2_color) vkDestroyImage(d, f2_color, nullptr);
            if (f2_color_mem) vkFreeMemory(d, f2_color_mem, nullptr);
            // The display's bytes are B, G, R, A: a BGRA image reads them as true colours.
            f2_color = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, disp->pw, disp->ph, 1, 1, 1,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, f2_color_mem);
            f2_color_view = CreateView(f2_color, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            ToGeneral(f2_color, VK_IMAGE_ASPECT_COLOR_BIT);
            f2_cw = disp->pw;
            f2_ch = disp->ph;
        }
        if (f2_iw != ow || f2_ih != oh || !f2_out) {
            MakeImage(VK_FORMAT_R8G8B8A8_UNORM, ow, oh, f2_out, f2_out_mem, f2_out_view);
            MakeImage(VK_FORMAT_R8G8B8A8_UNORM, ow, oh, f2_final, f2_final_mem, f2_final_view);
            f2_iw = ow;
            f2_ih = oh;
        }
        if (!prehud_valid) {  // no HUD this frame: the final picture is the scene
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {disp->pw, disp->ph, 1};
            vkCmdCopyImage(cmd, disp->image, VK_IMAGE_LAYOUT_GENERAL, prehud, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            FullBarrier();
        }
        {
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {disp->pw, disp->ph, 1};
            vkCmdCopyImage(cmd, prehud, VK_IMAGE_LAYOUT_GENERAL, f2_color, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            FullBarrier();
        }
        MotionPass(display_address, true);
        if (!cam_prev.valid) f2_hist_valid = false;
        // The first frame with a camera has no motion vectors yet (no previous camera): nothing to upscale with.
        if (!mv_image || mv_w != disp->pw || mv_h != disp->ph) {
            f2_reason = 4;
            return false;
        }
        VkImageView& dv = depth_views[dt.image];
        if (!dv) dv = CreateView(dt.image, VK_IMAGE_VIEW_TYPE_2D, dt.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
        const bool has_stencil = dt.format == VK_FORMAT_D32_SFLOAT_S8_UINT || dt.format == VK_FORMAT_D24_UNORM_S8_UINT;
        const VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        {
            static std::set<VkImage> seen;
            if (seen.insert(dt.image).second)
                std::fprintf(stderr, "[fsr2] depth input %llx %ux%u (image %ux%u) format %d, image %p, colour %ux%u\n",
                             (unsigned long long)dt.address, dt.width, dt.height, dt.pw, dt.ph, int(dt.format),
                             static_cast<void*>(dt.image), disp->pw, disp->ph);
        }
        // FSR 2 samples its inputs in SHADER_READ_ONLY_OPTIMAL; everything here lives in GENERAL.
        Layout(f2_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Layout(mv_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Layout(dt.image, da, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const float dx = cam_cur.pos[0] - cam_prev.pos[0], dy = cam_cur.pos[1] - cam_prev.pos[1],
                    dz = cam_cur.pos[2] - cam_prev.pos[2];
        const bool cut = !cam_prev.valid || dx * dx + dy * dy + dz * dz > 4.0f;
        FfxErrorCode e = FFX_OK;
        const int dbg = g_f2_debug.load();
        const float jx = jitter_x * (dbg & 1 ? -1.0f : 1.0f), jy = jitter_y * (dbg & 2 ? -1.0f : 1.0f);
        if (use_dlss) {
            dlss::Frame fr{};
            fr.color = {f2_color, f2_color_view, VK_FORMAT_B8G8R8A8_UNORM, disp->pw, disp->ph, VK_IMAGE_ASPECT_COLOR_BIT};
            fr.depth = {dt.image, dv, dt.format, dt.pw, dt.ph, VK_IMAGE_ASPECT_DEPTH_BIT};
            fr.motion = {mv_image, mv_view, VK_FORMAT_R16G16_SFLOAT, mv_w, mv_h, VK_IMAGE_ASPECT_COLOR_BIT};
            fr.output = {f2_out, f2_out_view, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT};
            fr.render_w = disp->pw;
            fr.render_h = disp->ph;
            fr.jitter_x = jx;
            fr.jitter_y = jy;
            fr.mv_scale_x = fr.mv_scale_y = 1.0f;  // pixels, current -> previous (as for FSR 2)
            fr.reset = cut || !f2_hist_valid;
            fr.depth_inverted = inverted;
            fr.quality = int(mode - 12);
            if (fr.reset) ++f2_why[6];
            if (cam_cur.address != cam_prev.address) ++f2_why[7];
            if (!dlss::Evaluate(cmd, fr)) e = FFX_ERROR_BACKEND_API_ERROR;
        } else {
        FfxFsr2DispatchDescription dd{};
        dd.commandList = ffxGetCommandListVK(cmd);
        dd.color = ffxGetTextureResourceVK(&f2_ctx, f2_color, f2_color_view, disp->pw, disp->ph,
                                           VK_FORMAT_B8G8R8A8_UNORM, L"FSR2_InputColor");
        dd.depth = ffxGetTextureResourceVK(&f2_ctx, dt.image, dv, dt.pw, dt.ph, dt.format, L"FSR2_InputDepth");
        dd.motionVectors = ffxGetTextureResourceVK(&f2_ctx, mv_image, mv_view, mv_w, mv_h, VK_FORMAT_R16G16_SFLOAT,
                                                   L"FSR2_InputMotionVectors");
        dd.output = ffxGetTextureResourceVK(&f2_ctx, f2_out, f2_out_view, ow, oh, VK_FORMAT_R8G8B8A8_UNORM,
                                            L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.jitterOffset = {jx, jy};
        dd.motionVectorScale = {1.0f, 1.0f};  // pixels, current -> previous (FSR 2 divides by the render size)
        dd.renderSize = {disp->pw, disp->ph};
        dd.enableSharpening = !(g_f2_debug.load() & 128);
        dd.sharpness = 0.3f;
        dd.frameTimeDelta = 1000.0f / 60.0f;
        dd.preExposure = 1.0f;
        dd.reset = cut || !f2_hist_valid;
        if (dd.reset) ++f2_why[6];
        if (cam_cur.address != cam_prev.address) ++f2_why[7];
        dd.cameraNear = cam_cur.near_far[0];
        dd.cameraFar = cam_cur.near_far[1];
        dd.cameraFovAngleVertical = fov;
        dd.viewSpaceToMetersFactor = 1.0f;
        e = ffxFsr2ContextDispatch(&f2_ctx, &dd);
        }
        Layout(f2_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        Layout(mv_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        Layout(dt.image, da, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        FullBarrier();
        if (e != FFX_OK) {
            static int logged = 0;
            if (logged++ < 3) std::fprintf(stderr, "[fsr2] dispatch failed (%d)\n", int(e));
            f2_hist_valid = false;
            f2_reason = 5;
            return false;
        }
        // Composite: FSR 2's picture, with the HUD put back where it drew.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, f2_comp_pipe);
        VkDescriptorImageInfo infos[6] = {
            {taa_linear, f2_out_view, VK_IMAGE_LAYOUT_GENERAL}, {taa_linear, disp->view, VK_IMAGE_LAYOUT_GENERAL},
            {mv_sampler, prehud_view, VK_IMAGE_LAYOUT_GENERAL}, {taa_linear, f2_out_view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, f2_final_view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, f2_final_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w6[6]{};
        for (uint32_t i = 0; i < 6; ++i)
            w6[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1,
                     i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[i],
                     nullptr, nullptr};
        push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taa_layout, 0, 6, w6);
        const float consts[8] = {float(ow), float(oh), 1.0f / float(ow), 1.0f / float(oh),
                                 float(disp->pw), float(disp->ph),
                                 disp->format == VK_FORMAT_B8G8R8A8_UNORM || disp->format == VK_FORMAT_B8G8R8A8_SRGB ? 1.0f : 0.0f, float((g_f2_debug.load() >> 3) & 3)};
        vkCmdPushConstants(cmd, taa_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, consts);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
        FullBarrier();
        f2_hist_valid = true;
        f2_ran = true;
        return true;
    }

    void Skip(const char* reason, const Registers& r) {
        ++skipped;
        static std::map<std::string, int> seen;
        if (seen[reason]++ >= 3) return;
        std::fprintf(stderr, "[renderer] skipped draw (%s): prim %u cb0 %llx cb_control %08x target_mask %08x\n", reason,
                     r.uconfig[gpu::reg::kVgtPrimitiveType],
                     (unsigned long long)(uint64_t(r.context[gpu::reg::kCbColor0Base]) << 8),
                     r.context[gpu::reg::kCbColorControl], r.context[gpu::reg::kCbTargetMask]);
    }

    void Init();
    void Draw(const Registers& r, const gpu::DrawInfo& draw);
};

namespace {

VkCompareOp CompareOp(uint32_t f) {
    static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                       VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                       VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return ops[f & 7];
}

VkBlendFactor BlendFactor(uint32_t f) {
    switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 2: return VK_BLEND_FACTOR_SRC_COLOR;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_DST_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp BlendOp(uint32_t f) {
    switch (f) {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    return VK_BLEND_OP_ADD;
}

VkPrimitiveTopology Topology(uint32_t prim) {
    switch (prim) {
        case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // tri lists, and quad lists after index expansion
    }
}

constexpr uint32_t kPrimQuadList = 0x13, kPrimRectList = 0x11;

float AsFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

void Renderer::Impl::Init() {
    d = dev->device();
    push_descriptor = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
    host_pointer_props = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(d, "vkGetMemoryHostPointerPropertiesEXT"));
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &host_props;
    vkGetPhysicalDeviceProperties2(dev->physical(), &props2);
    import_alignment = host_props.minImportedHostPointerAlignment ? host_props.minImportedHostPointerAlignment : 4096;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev->queue_family();
    vkCreateCommandPool(d, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kMaxFrames;
    vkAllocateCommandBuffers(d, &cai, cmds);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        vkCreateFence(d, &fci, nullptr, &fences[i]);
        qpci.queryCount = 2;
        vkCreateQueryPool(d, &qpci, nullptr, &timestamp_pools[i]);
        if (pass_timing) {
            qpci.queryCount = kMaxPasses * 2;
            vkCreateQueryPool(d, &qpci, nullptr, &pass_pools[i]);
        }
    }
    slot = kMaxFrames - 1;  // BeginFrame moves to slot 0 first
    cmd = cmds[slot];
    fence = fences[slot];
    timestamps = timestamp_pools[slot];
    pass_ts = pass_pools[slot];
    LoadPipelineCache();
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(dev->physical(), &p);
        timestamp_period_ns = p.limits.timestampPeriod;
    }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kRingSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vkCreateBuffer(d, &bci, nullptr, &ring);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, ring, &req);
    ring_memory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    vkBindBufferMemory(d, ring, ring_memory, 0);
    vkMapMemory(d, ring_memory, 0, kRingSize, 0, reinterpret_cast<void**>(&ring_map));
    VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = ring;
    ring_address = vkGetBufferDeviceAddress(d, &bai);

    std::vector<VkDescriptorSetLayoutBinding> bindings(kMaxBindings);
    for (uint32_t i = 0; i < kMaxBindings; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dslci.bindingCount = kMaxBindings;
    dslci.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(d, &dslci, nullptr, &set_layout);
    VkPushConstantRange range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(d, &plci, nullptr, &layout);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(d, &sci, nullptr, &sampler);
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(d, &sci, nullptr, &shadow_sampler);

    compiler = shaderc_compiler_initialize();

    // Texture upload staging (detiled on the CPU, reset every frame).
    VkBufferCreateInfo ubci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ubci.size = kUploadSize;
    ubci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(d, &ubci, nullptr, &upload);
    vkGetBufferMemoryRequirements(d, upload, &req);
    upload_memory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    vkBindBufferMemory(d, upload, upload_memory, 0);
    vkMapMemory(d, upload_memory, 0, kUploadSize, 0, reinterpret_cast<void**>(&upload_map));

    // Placeholder textures (mid grey) until the texture cache exists; one per view type.
    BeginFrame();
    const struct { gcn::ImageType t; VkImageType it; VkImageViewType vt; uint32_t layers; VkImageCreateFlags f; } kinds[] = {
        {gcn::ImageType::Tex1D, VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, 0},
        {gcn::ImageType::Tex2D, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0},
        {gcn::ImageType::Tex3D, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
        {gcn::ImageType::Cube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
        {gcn::ImageType::Tex1DArray, VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, 0},
        {gcn::ImageType::Tex2DArray, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 0},
    };
    for (const auto& k : kinds) {
        Dummy dm;
        dm.image = CreateImage(k.it, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, k.layers, 1,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, k.f, dm.memory);
        dm.view = CreateView(dm.image, k.vt, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, k.layers);
        ToGeneral(dm.image, VK_IMAGE_ASPECT_COLOR_BIT, k.layers);
        VkClearColorValue grey{{0.5f, 0.5f, 0.5f, 1.0f}};
        VkImageSubresourceRange range_all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, k.layers};
        vkCmdClearColorImage(cmd, dm.image, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &range_all);
        dummies[k.t] = dm;
    }
    SubmitFrame();
}

// LDPORT_DRAW_PROFILE=1: CPU time per stage of Draw, reported with the frame line.
const bool g_profile = std::getenv("LDPORT_DRAW_PROFILE") != nullptr;
constexpr int kProfStages = 7;
const char* const kProfNames[kProfStages] = {"shaders", "data", "textures", "targets", "modules", "pipeline", "bind+draw"};
uint64_t g_prof_ns[kProfStages] = {};
uint64_t g_frame_prof_ns[kProfStages] = {};  // this frame's, for the hitch log
struct ProfMark {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    void operator()(int stage) {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
        g_frame_prof_ns[stage] += ns;
        if (g_profile) g_prof_ns[stage] += ns;
        t = now;
    }
};

VkPipeline Renderer::Impl::BuildPipeline(VkShaderModule vmod, VkShaderModule pmod, const PipeDesc& pd) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vmod, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pmod, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = Topology(pd.prim);
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // PA_SU_SC_MODE_CNTL: bit 0 cull front, bit 1 cull back, bit 2 FACE (1 = clockwise is front). The viewport
    // keeps the game's (negative) y scale, so screen-space winding matches the hardware's.
    // LDPORT_NO_CULL=1 turns culling off (as before), LDPORT_FLIP_FACE=1 inverts the winding (experiments).
    static const bool no_cull = std::getenv("LDPORT_NO_CULL") != nullptr;
    static const bool flip_face = std::getenv("LDPORT_FLIP_FACE") != nullptr;
    const uint32_t mode_cntl = pd.mode_cntl;
    rs.cullMode = no_cull ? VK_CULL_MODE_NONE
                          : VkCullModeFlags(((mode_cntl & 1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                                            ((mode_cntl & 2) ? VK_CULL_MODE_BACK_BIT : 0));
    rs.frontFace = (((mode_cntl >> 2) & 1) != 0) != flip_face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const bool has_depth = pd.depth != VK_FORMAT_UNDEFINED;
    const bool has_stencil = pd.depth == VK_FORMAT_D32_SFLOAT_S8_UINT || pd.depth == VK_FORMAT_D24_UNORM_S8_UINT;
    if (has_depth) {
        const uint32_t depth_control = pd.depth_control;
        ds.depthTestEnable = (depth_control >> 1) & 1;
        ds.depthWriteEnable = (depth_control >> 2) & 1;
        ds.depthCompareOp = CompareOp(depth_control >> 4);
        // Stencil (light volumes mark their inside, then light only the marked pixels): DB_DEPTH_CONTROL
        // bit 0 enable, bits 8-10 / 20-22 front / back function, bit 7 separate back face; DB_STENCIL_CONTROL
        // fail / zpass / zfail ops (front bits 0-11, back 12-23); DB_STENCILREFMASK(_BF) test value, mask,
        // write mask, op value.
        if ((depth_control & 1) && has_stencil) {
            const bool back_separate = (depth_control >> 7) & 1;
            const uint32_t sc = pd.stencil_control;
            auto op = [](uint32_t o) {
                switch (o & 0xF) {
                    case 1: return VK_STENCIL_OP_ZERO;
                    case 3: case 4: return VK_STENCIL_OP_REPLACE;  // test value / op value (as reference)
                    case 5: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                    case 6: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                    case 7: return VK_STENCIL_OP_INVERT;
                    case 8: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                    case 9: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
                    default: return VK_STENCIL_OP_KEEP;
                }
            };
            auto face = [&](uint32_t func, uint32_t ops, uint32_t refmask) {
                VkStencilOpState f{};
                f.compareOp = CompareOp(func);
                f.failOp = op(ops);
                f.passOp = op(ops >> 4);
                f.depthFailOp = op(ops >> 8);
                f.compareMask = (refmask >> 8) & 0xFF;
                f.writeMask = (refmask >> 16) & 0xFF;
                // REPLACE_OP writes the op value, REPLACE_TEST the test value.
                const bool replace_op = (ops & 0xF) == 4 || ((ops >> 4) & 0xF) == 4 || ((ops >> 8) & 0xF) == 4;
                f.reference = replace_op ? (refmask >> 24) & 0xFF : refmask & 0xFF;
                return f;
            };
            ds.stencilTestEnable = VK_TRUE;
            ds.front = face(depth_control >> 8, sc, pd.stencil_ref);
            ds.back = back_separate ? face(depth_control >> 20, sc >> 12, pd.stencil_ref_bf) : ds.front;
        }
    }
    VkPipelineColorBlendAttachmentState blends[8]{};
    for (uint32_t i = 0; i < pd.color_count && i < 8; ++i) {
        const uint32_t b = pd.blend[i];
        VkPipelineColorBlendAttachmentState& a = blends[i];
        a.colorWriteMask = pd.write_mask[i];
        a.blendEnable = (b >> 30) & 1;
        a.srcColorBlendFactor = BlendFactor(b & 0x1F);
        a.colorBlendOp = BlendOp((b >> 5) & 7);
        a.dstColorBlendFactor = BlendFactor((b >> 8) & 0x1F);
        const bool separate = (b >> 29) & 1;
        a.srcAlphaBlendFactor = separate ? BlendFactor((b >> 16) & 0x1F) : a.srcColorBlendFactor;
        a.alphaBlendOp = separate ? BlendOp((b >> 21) & 7) : a.colorBlendOp;
        a.dstAlphaBlendFactor = separate ? BlendFactor((b >> 24) & 0x1F) : a.dstColorBlendFactor;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = pd.color_count;
    cb.pAttachments = blends;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = pd.color_count;
    rci.pColorAttachmentFormats = pd.colors;
    rci.depthAttachmentFormat = pd.depth;
    rci.stencilAttachmentFormat = has_stencil ? pd.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rci;
    gp.stageCount = pmod ? 2 : 1;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(d, pipeline_cache, 1, &gp, nullptr, &pipeline) != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    return pipeline;
}

void Renderer::Impl::StartPipelineWarmup() {
    warm_started = true;
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines-%016llx.bin", ExeCacheKey());
    pipeline_list_path = ShaderCacheDir() + name;
    std::error_code ec;
    std::filesystem::create_directories(ShaderCacheDir(), ec);
    std::vector<PipeRecord> records;
    {
        std::ifstream in(pipeline_list_path, std::ios::binary);
        PipeRecord rec;
        while (in.read(reinterpret_cast<char*>(&rec), sizeof rec))
            if (pipeline_list_known.insert(FastHash(&rec, sizeof rec)).second) records.push_back(rec);
    }
    if (records.empty() || std::getenv("LDPORT_NO_PIPELINE_WARMUP")) return;
    const unsigned workers = std::max(1u, std::min(6u, std::thread::hardware_concurrency() / 2));
    std::fprintf(stderr, "[renderer] creating %zu listed pipelines on %u worker threads\n", records.size(), workers);
    auto shared = std::make_shared<std::vector<PipeRecord>>(std::move(records));
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto module_mutex = std::make_shared<std::mutex>();
    auto module_map = std::make_shared<std::unordered_map<uint64_t, VkShaderModule>>();
    const auto t0 = std::chrono::steady_clock::now();
    warm_running = int(workers);
    for (unsigned w = 0; w < workers; ++w) {
        std::thread([this, shared, next, module_mutex, module_map, t0] {
            auto get_module = [&](uint64_t h) -> VkShaderModule {
                if (!h) return VK_NULL_HANDLE;
                {
                    std::lock_guard lock(*module_mutex);
                    if (auto it = module_map->find(h); it != module_map->end()) return it->second;
                }
                VkShaderModule m = LoadCachedSpirv(h);
                std::lock_guard lock(*module_mutex);
                auto [it, inserted] = module_map->try_emplace(h, m);
                if (inserted && m) {
                    std::lock_guard wl(warm_mutex);
                    warm_modules.push_back({h, m});
                } else if (!inserted && m) {
                    vkDestroyShaderModule(d, m, nullptr);  // another worker loaded it first
                }
                return it->second;
            };
            for (size_t i; !warm_stop && (i = (*next)++) < shared->size();) {
                const PipeRecord& rec = (*shared)[i];
                VkShaderModule vm = get_module(rec.vs_h), pm = get_module(rec.ps_h);
                if (!vm || (rec.ps_h && !pm)) continue;
                VkPipeline pl = BuildPipeline(vm, pm, rec.pd);
                if (!pl) continue;
                std::lock_guard wl(warm_mutex);
                warm_pipelines.push_back({PipeKeyHash(vm, pm, rec.pd), pl});
            }
            if (--warm_running == 0)
                std::fprintf(stderr, "[renderer] listed pipelines ready (%.0f ms)\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }).detach();
    }
}

void Renderer::Impl::Draw(const Registers& r, const gpu::DrawInfo& draw) {
    ProfMark mark;
    const auto& sh = r.sh;
    const auto& cx = r.context;
    const uint32_t prim = r.uconfig[reg::kVgtPrimitiveType];
    if (prim == kPrimRectList) return Skip("rect list", r);  // TODO: rect lists need a geometry shader

    // Colour output is off unless CB_COLOR_CONTROL.MODE is "normal".
    const uint32_t cb_mode = (cx[reg::kCbColorControl] >> 4) & 7;
    if (cb_mode >= 2) return Skip("cb mode", r);  // fast-clear eliminate / resolve passes: nothing to emulate yet

    // --- shaders ---
    const uint32_t vs_user = gpu::UserSgprCount(sh[reg::kSpiShaderPgmRsrc2Vs]);
    const uint32_t ps_user = gpu::UserSgprCount(sh[reg::kSpiShaderPgmRsrc2Ps]);
    const uint64_t vs_addr = gpu::ShaderAddress(sh[reg::kSpiShaderPgmLoVs], sh[reg::kSpiShaderPgmLoVs + 1]);
    const uint64_t ps_addr = gpu::ShaderAddress(sh[reg::kSpiShaderPgmLoPs], sh[reg::kSpiShaderPgmLoPs + 1]);
    const uint64_t fetch = vs_user >= 2 ? (uint64_t(sh[reg::kSpiShaderUserDataVs0 + 1]) << 32) | sh[reg::kSpiShaderUserDataVs0] : 0;
    const gcn::ShaderProgram* vs = GetProgram(gcn::Stage::Vertex, vs_addr, fetch, vs_user);
    if (!vs) return Skip("no vertex shader", r);
    const gcn::ShaderProgram* ps = cb_mode == 1 || ps_addr ? GetProgram(gcn::Stage::Pixel, ps_addr, 0, ps_user) : nullptr;

    BeginFrame();
    mark(0);

    // --- per-draw data blocks (CPU scalar pre-pass) ---
    VkDeviceAddress vs_data = 0, ps_data = 0;
    VkDeviceSize unused;
    thread_local std::vector<gcn::ResolvedImage> vs_images, ps_images;  // reused: no per-draw allocation
    auto to_device = [](uint64_t guest, uint64_t bytes) { return GetRenderer().BufferDevice(guest, bytes); };
    uint32_t* vblock = reinterpret_cast<uint32_t*>(RingAlloc(vs->data_dwords * 4, vs_data, unused));
    if (!vblock) return Skip("ring full", r);
    gcn::ResolveSites(*vs, &sh[reg::kSpiShaderUserDataVs0], vblock, vs_images, to_device);
    static std::unordered_set<uint64_t> surveyed;  // survey: vertex formats in use (once per program)
    if (surveyed.insert(vs->id).second) for (const auto& site : vs->sites) {
        if (site.kind != gcn::Site::Buffer) continue;
        const uint32_t* b = vblock + site.data_offset;
        static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> seen;
        if (seen[{b[4], b[5], b[6]}]++ == 0)
            std::fprintf(stderr, "[renderer] vertex format dfmt %u nfmt %u dst_sel %03x stride %u (vs %llx)\n", b[4], b[5],
                         b[6], b[2], (unsigned long long)vs_addr);
    }
    if (ps) {
        uint32_t* pblock = reinterpret_cast<uint32_t*>(RingAlloc(ps->data_dwords * 4, ps_data, unused));
        if (!pblock) return Skip("ring full", r);
        gcn::ResolveSites(*ps, &sh[reg::kSpiShaderUserDataPs0], pblock, ps_images, to_device);
        // LDPORT_CAPTURE_PS=<address>: record that pixel shader's resolved data block and textures.
        static const char* capture_env = std::getenv("LDPORT_CAPTURE_PS");
        static const bool capture_all = capture_env && std::string(capture_env) == "all";
        static const uint64_t capture_ps = capture_env && !capture_all ? std::strtoull(capture_env, nullptr, 16) : 0;
        if (((capture_all && !draw.indexed && draw.index_count <= 6) || (capture_ps && ps_addr == capture_ps)) && gpu::Capture::Get().active()) {
            std::string s = "  ps data:";
            char buf[32];
            for (uint32_t i = 0; i < ps->data_dwords; ++i) {
                std::snprintf(buf, sizeof buf, " %08x", pblock[i]);
                s += buf;
            }
            for (const auto& img : ps_images) {
                const uint64_t base = ((uint64_t(img.tsharp[1] & 0xFF) << 32) | img.tsharp[0]) << 8;
                std::snprintf(buf, sizeof buf, " tex %llx", (unsigned long long)base);
                s += buf;
            }
            gpu::Capture::Get().Note(s.c_str());
        }
        if (gpu::Capture::Get().active()) {  // every draw: the pixel shader's textures
            for (const auto& img : ps_images) {
                const uint32_t* t = img.tsharp;
                char buf[200];
                std::snprintf(buf, sizeof buf, "  ps tex %llx dfmt %u nfmt %u %ux%u mips %u-%u tiling %u type %u sel %03x",
                              (unsigned long long)(((uint64_t(t[1] & 0xFF) << 32) | t[0]) << 8), (t[1] >> 20) & 0x3F,
                              (t[1] >> 26) & 0xF, (t[2] & 0x3FFF) + 1, ((t[2] >> 14) & 0x3FFF) + 1, (t[3] >> 12) & 0xF,
                              (t[3] >> 16) & 0xF, (t[3] >> 20) & 0x1F, (t[3] >> 28) & 0xF, t[3] & 0xFFF);
                gpu::Capture::Get().Note(buf);
            }
        }
    }
    mark(1);
    // Texture uploads are copies, which can't be recorded inside a render pass: do them now.
    const ResolvedTextures vs_tex = ResolveTextures(vs_images);
    const ResolvedTextures ps_tex = ResolveTextures(ps_images);

    mark(2);
    // --- render targets ---
    const uint32_t screen_br = cx[kPaScScreenScissorTl + 1];
    const uint32_t screen_w = screen_br & 0xFFFF, screen_h = screen_br >> 16;
    std::array<const Target*, 8> colors{};
    uint8_t mrt_map[8][4];
    uint8_t fp16_mask = 0;
    VkFormat color_formats[8] = {};
    uint32_t color_count = 0;
    VkExtent2D extent{0, 0};
    const uint32_t target_mask = cx[reg::kCbTargetMask];
    for (uint32_t rt = 0; rt < 8; ++rt) {
        for (int c = 0; c < 4; ++c) mrt_map[rt][c] = uint8_t(c);
        const uint32_t b = reg::kCbColor0Base + rt * reg::kCbColorStride;
        const uint32_t info = cx[b + reg::kCbColorInfo];
        if (cb_mode == 0 || !cx[b] || !((target_mask >> (rt * 4)) & 0xF)) continue;
        const uint32_t fmt = (info >> 2) & 0x1F, number = (info >> 8) & 7, swap = (info >> 11) & 3;
        const VkFormat vkf = ColorFormat(fmt, number);
        if (vkf == VK_FORMAT_UNDEFINED) continue;
        const uint32_t pitch = ((cx[b + reg::kCbColorPitch] & 0x7FF) + 1) * 8;
        const uint32_t slice = ((cx[b + reg::kCbColorSlice] & 0x3FFFFF) + 1) * 64;
        uint32_t w = pitch, h = slice / pitch;
        if (screen_w && screen_w < w) w = screen_w;
        if (screen_h && screen_h < h) h = screen_h;
        colors[rt] = GetTarget(uint64_t(cx[b]) << 8, vkf, w, h, false);
        if (!colors[rt]) continue;
        if (override_color_from && colors[rt] == override_color_from) colors[rt] = override_color_to;
        ColorSwap(swap, ColorComponents(fmt), mrt_map[rt]);
        if (vkf == VK_FORMAT_R16G16B16A16_SFLOAT || vkf == VK_FORMAT_R16G16_SFLOAT || vkf == VK_FORMAT_R16_SFLOAT)
            fp16_mask |= uint8_t(1u << rt);
        color_formats[rt] = vkf;
        color_count = rt + 1;
        // Render area: the attachment's real size (smallest across attachments).
        const VkExtent2D sz{colors[rt]->pw, colors[rt]->ph};
        extent = extent.width == 0 ? sz : VkExtent2D{std::min(extent.width, sz.width), std::min(extent.height, sz.height)};
    }
    const Target* depth = nullptr;
    int draw_cam = -1;  // the camera candidate this draw uses (TrackCamera), -1 none
    const uint32_t z_info = cx[reg::kDbZInfo];
    const bool stencil = (cx[kDbStencilInfo] & 1) != 0;
    if ((z_info & 3) && cx[reg::kDbZReadBase]) {
        const VkFormat df = DepthFormat(z_info & 3, stencil);
        const uint32_t size = cx[reg::kDbDepthSize];
        uint32_t w = ((size & 0x7FF) + 1) * 8, h = (((size >> 11) & 0x7FF) + 1) * 8;
        if (screen_w && screen_w < w) w = screen_w;
        if (screen_h && screen_h < h) h = screen_h;
        if (df != VK_FORMAT_UNDEFINED) depth = GetTarget(uint64_t(cx[reg::kDbZReadBase]) << 8, df, w, h, true);
        // The main camera draws into the screen-sized depth buffer (shadow maps and the Toy Pad screen's little
        // 3D scenes use others or come first with their own cameras).
        if (depth && depth->width >= 1280 && depth->height >= 720) {
            const uint32_t vte = cx[kPaClVteCntl];
            const float zs_ = (vte >> 4) & 1 ? AsFloat(cx[reg::kPaClVportXscale + 4]) : 1.0f;
            const float zo_ = (vte >> 5) & 1 ? AsFloat(cx[reg::kPaClVportXscale + 5]) : 0.0f;
            const float ys_ = (vte >> 2) & 1 ? AsFloat(cx[reg::kPaClVportXscale + 2]) : 1.0f;
            draw_cam = TrackCamera(&sh[reg::kSpiShaderUserDataVs0], vs_user, depth, zo_, zs_, ys_ < 0.0f);
        }
        if (depth && extent.width == 0) extent = {depth->pw, depth->ph};
        else if (depth) extent = {std::min(extent.width, depth->pw), std::min(extent.height, depth->ph)};
    }
    if (extent.width == 0) return Skip("no render target", r);

    // --- where the HUD starts ---
    // In gameplay the HUD is drawn into the scene's colour buffer after post-processing, then the finished
    // frame is copied to the display buffer. The frame goes: scene draws (depth-tested, into the main depth),
    // the bloom chain (smaller targets), full-screen post passes (no vertex buffers) back into the scene buffer,
    // then the HUD: the first draw into the scene buffer after the bloom chain that reads vertex buffers and
    // ignores depth. (Cutscenes and menus draw their 2D into the display buffer instead; see display_draws.)
    const uint32_t dbc = cx[reg::kDbDepthControl];
    const bool depth_always = !(dbc & 2) || ((dbc >> 4) & 7) == 7;
    bool hud_draw = false;
    if (colors[0] && !in_dup) {
        const bool full = colors[0]->width >= 1280 && colors[0]->height >= 720;
        const bool to_display = display_addrs.count(colors[0]->address) != 0;
        if (full && depth && !depth_always && draw_cam >= 0 && !hud_started) scene_color = colors[0]->address;
        else if (scene_color && !full) post_seen = true;
        // The HUD starts at the first draw into a full-size target (the post passes ping-pong between two) after the
        // bloom chain that is either 3D seen by a camera other than the scene's (the HUD's models: the character
        // portrait; compared by matrix: the lighting passes read their own copy of the scene's camera) or 2D
        // sprites (alpha-blended, depth ignored; not the full-screen triangles of the post passes).
        int leader = -1;
        for (size_t k = 0; k < cam_candidates.size(); ++k)
            if (leader < 0 || cam_candidates[k].draws > cam_candidates[size_t(leader)].draws) leader = int(k);
        auto same_camera = [&](int a, int b) {
            if (a < 0 || b < 0) return false;
            const float* p = cam_candidates[size_t(a)].cam.vp;
            const float* q = cam_candidates[size_t(b)].cam.vp;
            for (int k = 0; k < 16; ++k)
                if (std::fabs(p[k] - q[k]) > 1e-3f * std::max(1.0f, std::fabs(q[k]))) return false;
            return true;
        };
        const bool blended = (cx[reg::kCbBlend0Control] >> 30) & 1;
        const bool fullscreen_tri = !draw.indexed && draw.index_count == 3;
        const bool hud_like = draw_cam >= 0 ? !same_camera(draw_cam, leader) : (depth_always && blended && !fullscreen_tri);
        static const bool detect = std::getenv("LDPORT_HUD_DETECT") != nullptr;  // in progress: opt-in
        if (detect && !hud_started && post_seen && full && !to_display && hud_like) {
            scene_color = colors[0]->address;  // the HUD's target from here on
            static const bool log_start = std::getenv("LDPORT_LOG_HUD") != nullptr;
            if (log_start && frame % 300 == 0)
                std::fprintf(stderr, "[hud] frame %llu: HUD starts (camera %d of %zu, leader %d, n %u%s, blend %08x)\n",
                             (unsigned long long)frame, draw_cam, cam_candidates.size(), leader, draw.index_count,
                             draw.indexed ? "i" : "a", cx[reg::kCbBlend0Control]);
            hud_started = true;
            if (TemporalOn() && !prehud_valid) {  // the scene before the HUD (its own format: it may be HDR)
                EndRendering();
                if (EnsureSpare(ph_scene, *colors[0])) {
                    VkImageCopy cp{};
                    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    cp.extent = {colors[0]->pw, colors[0]->ph, 1};
                    vkCmdCopyImage(cmd, colors[0]->image, VK_IMAGE_LAYOUT_GENERAL, ph_scene.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
                    FullBarrier();
                    ph_scene_from = colors[0];
                }
            }
        }
        hud_draw = hud_started && colors[0]->address == scene_color;
    }

    // The game's composite into the display buffer (it grades the colours): draw it once more first, reading the
    // pre-HUD scene and writing a spare display-format image. That is this frame's pre-HUD picture in display
    // colours, for TAA / FSR 2 and their HUD mask.
    if (TemporalOn() && ph_scene_from && !in_dup && !prehud_valid && colors[0] && display_addrs.count(colors[0]->address)) {
        EndRendering();
        if (EnsureSpare(ph_disp, *colors[0])) {
            override_color_from = colors[0];
            override_color_to = &ph_disp;
            override_tex_from = ph_scene_from;
            override_tex_to = &ph_scene;
            in_dup = true;
            override_hits = 0;
            Draw(r, draw);
            in_dup = false;
            static int logged = 0;
            if (logged++ < 3)
                std::fprintf(stderr, "[hud] composite re-drawn from the pre-HUD scene (%u texture swaps; scene %llx %ux%u fmt %d, display fmt %d)\n",
                             override_hits, (unsigned long long)ph_scene_from->address, ph_scene_from->width,
                             ph_scene_from->height, int(ph_scene_from->format), int(colors[0]->format));
            override_color_from = override_color_to = nullptr;
            override_tex_from = override_tex_to = nullptr;
            EndRendering();
            FullBarrier();
            EnsureTaaImages(colors[0]->pw, colors[0]->ph);
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {colors[0]->pw, colors[0]->ph, 1};
            vkCmdCopyImage(cmd, ph_disp.image, VK_IMAGE_LAYOUT_GENERAL, prehud, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            FullBarrier();
            prehud_valid = true;
        }
        ph_scene_from = nullptr;
    }

    // TAA: copy the display buffer before the HUD goes on it (just before its second draw this frame).
    if (TemporalOn() && colors[0] && display_addrs.count(colors[0]->address)) {
        if (display_draws == 1 && !prehud_valid) {
            EndRendering();
            EnsureTaaImages(colors[0]->pw, colors[0]->ph);
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {colors[0]->pw, colors[0]->ph, 1};
            vkCmdCopyImage(cmd, colors[0]->image, VK_IMAGE_LAYOUT_GENERAL, prehud, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            FullBarrier();
            prehud_valid = true;
        }
        ++display_draws;
    }

    // --- dynamic rendering: restart when attachments change ---
    if (!rendering || colors != bound_color || depth != bound_depth) {
        EndRendering();
        VkRenderingAttachmentInfo cas[8]{};
        for (uint32_t i = 0; i < color_count; ++i) {
            cas[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            cas[i].imageView = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
            cas[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            cas[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            cas[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            da.imageView = depth->view;
            da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = color_count;
        ri.pColorAttachments = cas;
        const bool has_stencil = depth && (depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT || depth->format == VK_FORMAT_D24_UNORM_S8_UINT);
        ri.pDepthAttachment = depth ? &da : nullptr;
        ri.pStencilAttachment = has_stencil ? &da : nullptr;
        if (pass_timing && pass_count < kMaxPasses) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pass_ts, pass_count * 2);
            passes.push_back({colors[0] ? colors[0]->address : depth ? depth->address : 0, 0});
            ++pass_count;
        }
        vkCmdBeginRendering(cmd, &ri);
        rendering = true;
        bound_color = colors;
        bound_depth = depth;
        bound_extent = extent;
    }

    // --- viewport and scissor ---
    // PA_CL_VTE_CNTL can switch the viewport scale/offset off per component; then the vertex
    // shader's output is already in window coordinates (2D overlays, fades, full-screen passes).
    VkViewport vp;
    const uint32_t vte = cx[kPaClVteCntl];
    {
        static std::map<uint32_t, int> seen_vte;
        if (seen_vte[vte]++ == 0) std::fprintf(stderr, "[renderer] PA_CL_VTE_CNTL value %08x in use\n", vte);
    }
    auto vport = [&](uint32_t i, uint32_t enable_bit, float fallback) {
        return (vte >> enable_bit) & 1 ? AsFloat(cx[reg::kPaClVportXscale + i]) : fallback;
    };
    const float xs = vport(0, 0, 1.0f), xo = vport(1, 1, 0.0f);
    const float ys = vport(2, 2, 1.0f), yo = vport(3, 3, 0.0f);
    const float zs = vport(4, 4, 1.0f), zo = vport(5, 5, 0.0f);
    vp.x = xo - xs;
    vp.y = yo - ys;
    vp.width = 2.0f * xs;
    vp.height = 2.0f * ys;
    vp.minDepth = zo;
    vp.maxDepth = zo + zs;
    const float S = float(RenderScale());
    vp.x *= S;
    vp.y *= S;
    vp.width *= S;
    vp.height *= S;
    if (vp.width == 0 || vp.height == 0) vp = {0, 0, float(extent.width), float(extent.height), 0, 1};
    // TAA / FSR 2: sub-pixel jitter for scene draws: depth-tested ones (not full-screen post passes, which would
    // shift the picture again, nor HUD sprites, which ignore depth).
    if (TemporalOn() && depth && depth->address == main_depth && !hud_draw && !depth_always) {
        vp.x += jitter_x;
        ++jittered_draws;
        vp.y += jitter_y;
    }
    // Ultrawide: draws into the display buffer after its first one (the scene composite) are the HUD, laid out
    // for 16:9 across the whole width; squeeze them back into the centred 16:9 area.
    bool hud_squeeze = false;
    if (UltrawideSqueeze() < 1.0f && colors[0] && display_addrs.count(colors[0]->address)) {
        hud_squeeze = disp_any_draws >= 1 && !cam_candidates.empty() && vs->has_fetch;  // not full-screen passes
        ++disp_any_draws;
    }
    static const bool log_hud = std::getenv("LDPORT_LOG_HUD") != nullptr;
    if (log_hud && frame % 300 == 0 && colors[0]) {
        int uses_cam = 0;
        for (uint32_t i = 0; i + 4 <= vs_user; ++i) {
            const uint32_t* v = &sh[reg::kSpiShaderUserDataVs0 + i];
            const uint64_t b = (uint64_t(v[1] & 0xFFFF) << 32) | v[0];
            for (const Candidate& c : cam_candidates)
                if (c.cam.address == b) uses_cam = 1;
        }
        std::fprintf(stderr, "[hud] frame %llu draw: target %llx %ux%u display %d squeeze %d vte %08x vp %.1f+%.1f depth %llx n %u%s dbc %08x cam %d vs %08x ps %08x fetch %d blend %08x\n",
                     (unsigned long long)frame, (unsigned long long)colors[0]->address, colors[0]->width,
                     colors[0]->height, int(display_addrs.count(colors[0]->address)), int(hud_squeeze), vte, vp.x,
                     vp.width, (unsigned long long)(depth ? depth->address : 0), draw.index_count,
                     draw.indexed ? "i" : "a", cx[reg::kDbDepthControl], uses_cam, sh[reg::kSpiShaderPgmLoVs], sh[reg::kSpiShaderPgmLoPs], int(vs->has_fetch), cx[reg::kCbBlend0Control]);
    }
    if (UltrawideSqueeze() < 1.0f && hud_draw) hud_squeeze = true;
    if (hud_squeeze) {
        const float W = float(colors[0]->pw), s = UltrawideSqueeze();
        vp.x = W * (1.0f - s) * 0.5f + vp.x * s;
        vp.width *= s;
    }
    vkCmdSetViewport(cmd, 0, 1, &vp);
    // The hardware scissor is the intersection of the screen, window, generic and (when enabled in
    // PA_SC_MODE_CNTL_0) viewport-0 scissors. Each is a TL/BR register pair (x in 0..14, y in 16..30);
    // a BR of 0 is treated as "unset".
    constexpr uint32_t kPaScWindowOffset = 0x080, kPaScWindowScissorTl = 0x081, kPaScVportScissor0Tl = 0x094,
                       kPaScModeCntl0 = 0x292;
    int32_t x0 = 0, y0 = 0, x1 = int32_t(extent.width), y1 = int32_t(extent.height);
    auto clip = [&](uint32_t tl, uint32_t br) {
        if (br == 0) return;
        const double k = RenderScale();
        auto sc = [&](uint32_t v) { return int32_t(std::lround(double(v) * k)); };
        x0 = std::max(x0, sc(tl & 0x7FFF));
        y0 = std::max(y0, sc((tl >> 16) & 0x7FFF));
        x1 = std::min(x1, sc(br & 0x7FFF));
        y1 = std::min(y1, sc((br >> 16) & 0x7FFF));
    };
    clip(cx[kPaScScreenScissorTl], cx[kPaScScreenScissorTl + 1]);
    clip(cx[kPaScWindowScissorTl], cx[kPaScWindowScissorTl + 1]);
    clip(cx[kPaScGenericScissorTl], cx[kPaScGenericScissorTl + 1]);
    if (cx[kPaScModeCntl0] & 2) clip(cx[kPaScVportScissor0Tl], cx[kPaScVportScissor0Tl + 1]);
    if (const uint32_t wo = cx[kPaScWindowOffset]) {
        static std::map<uint32_t, int> seen_wo;
        if (seen_wo[wo]++ == 0) std::fprintf(stderr, "[renderer] PA_SC_WINDOW_OFFSET %08x in use (not applied)\n", wo);
    }
    if (hud_squeeze) {  // ultrawide: the HUD keeps 16:9, centred
        const float W = float(colors[0]->pw), s = UltrawideSqueeze(), off = W * (1.0f - s) * 0.5f;
        x0 = int32_t(std::floor(off + float(x0) * s));
        x1 = int32_t(std::ceil(off + float(x1) * s));
    }
    VkRect2D sc{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Depth "clear" draws: DB_RENDER_CONTROL asks the hardware to write the clear value.
    const uint32_t db_render = cx[kDbRenderControl];
    if (depth && (db_render & 3)) {
        VkClearAttachment ca{};
        ca.aspectMask = ((db_render & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((db_render & 2) && stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil = {AsFloat(cx[kDbDepthClear]), cx[kDbStencilClear] & 0xFF};
        VkClearRect cr{sc, 0, 1};
        if (ca.aspectMask) vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        if (color_count == 0) return;
    }

    mark(3);
    // --- shader modules (keyed by what the draw's descriptors say) ---
    gcn::ShaderKey vkey{gcn::Stage::Vertex, vs_user, 0};
    vkey.scale = RenderScale();
    vkey.scaled_images = RenderScale() != 1.0f ? vs_tex.target_mask : 0;
    for (const auto& img : vs_images) vkey.image_types.push_back(gcn::ImageType((img.tsharp[3] >> 28) & 0xF));
    for (auto& t : vkey.image_types)
        if (int(t) < 8 || int(t) > 13) t = gcn::ImageType::Tex2D;
    // Background compiling covers 3D objects only (depth-tested, not a full-screen pass, not the HUD): skipping one
    // for a frame or two while its shaders compile is barely visible; skipping a post pass or the HUD would flash.
    const bool can_defer = AsyncShaders() && depth && !depth_always && !hud_draw &&
                           !(!draw.indexed && draw.index_count <= 6) && !gpu::Capture::Get().active();
    VkShaderModule vmod = GetModule(*vs, vkey, can_defer);
    VkShaderModule pmod = VK_NULL_HANDLE;
    gcn::ShaderKey pkey{gcn::Stage::Pixel, ps_user, cx[reg::kSpiPsInputEna]};
    if (ps) {
        for (const auto& img : ps_images) pkey.image_types.push_back(gcn::ImageType((img.tsharp[3] >> 28) & 0xF));
        for (auto& t : pkey.image_types)
            if (int(t) < 8 || int(t) > 13) t = gcn::ImageType::Tex2D;
        std::memcpy(pkey.mrt_map, mrt_map, sizeof mrt_map);
        pkey.fp16_targets = fp16_mask;
        constexpr uint32_t kSpiPsInputCntl0 = 0x191;
        pkey.ps_input_map = true;
        pkey.scale = RenderScale();
        pkey.scaled_images = RenderScale() != 1.0f ? ps_tex.target_mask : 0;
        for (uint32_t n = 0; n < 32; ++n) pkey.ps_input_cntl[n] = cx[kSpiPsInputCntl0 + n] & 0x7FF;
        pmod = GetModule(*ps, pkey, can_defer);
    }
    if (can_defer && (!vmod || (ps && !pmod))) {  // compiling in the background (or failed): draw it once ready
        deferred_draws.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!vmod || (ps && !pmod)) return Skip("shader compile", r);

    mark(4);
    // --- pipeline ---
    const uint32_t depth_control = cx[reg::kDbDepthControl];
    PipeDesc pd{};
    pd.prim = prim;
    pd.depth_control = depth ? depth_control : 0;
    pd.stencil_control = depth ? cx[kDbStencilControl] : 0;
    pd.stencil_ref = depth && (depth_control & 1) ? cx[kDbStencilRefMask] : 0;
    pd.stencil_ref_bf = depth && (depth_control & 1) ? cx[kDbStencilRefMaskBf] : 0;
    pd.target_mask = target_mask;
    pd.color_control = cx[reg::kCbColorControl];
    pd.mode_cntl = cx[reg::kPaSuScModeCntl];
    std::memcpy(pd.blend, &cx[reg::kCbBlend0Control], sizeof pd.blend);
    pd.color_count = color_count;
    std::memcpy(pd.colors, color_formats, sizeof color_formats);
    pd.depth = depth ? depth->format : VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < color_count; ++i)
        pd.write_mask[i] = colors[i] && ps && (ps->outputs_mask & (1u << i)) ? uint8_t((target_mask >> (i * 4)) & 0xF) : 0;
    const uint64_t pkey_hash = PipeKeyHash(vmod, pmod, pd);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (auto it = pipelines.find(pkey_hash); it != pipelines.end()) {
        pipeline = it->second;
    } else if (can_defer) {  // built in the background; the draw waits for it
        if (building.try_emplace(pkey_hash, PendingPipe{vmod, pmod, pd}).second)
            QueueShaderJob({0, pkey_hash, {}, gcn::Stage::Vertex, vmod, pmod, pd, {}});
        deferred_draws.fetch_add(1, std::memory_order_relaxed);
        return;
    } else {
        const auto t_pipe = std::chrono::steady_clock::now();
        pipeline = BuildPipeline(vmod, pmod, pd);
        pipeline_cache_dirty = true;
        const double pipe_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_pipe).count();
        if (pipe_ms > 100)
            std::fprintf(stderr, "[renderer] slow pipeline: %.0f ms (vs %llx ps %llx)\n", pipe_ms,
                         (unsigned long long)vs_addr, (unsigned long long)ps_addr);
        pipelines[pkey_hash] = pipeline;
        if (pipeline) RecordPipeline(vmod, pmod, pd);
    }
    if (!pipeline) return Skip("pipeline", r);

    mark(5);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    float blend_constants[4];
    for (int i = 0; i < 4; ++i) blend_constants[i] = AsFloat(cx[kCbBlendRed + i]);
    vkCmdSetBlendConstants(cmd, blend_constants);
    const VkDeviceAddress push[2] = {vs_data, ps_data};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof push, push);

    // Textures (resolved before rendering began): the image view and sampler per site, or the
    // placeholder of the right view type for anything not supported yet.
    VkDescriptorImageInfo infos[kMaxBindings];
    VkWriteDescriptorSet writes[kMaxBindings];
    uint32_t nwrites = 0;
    auto bind_images = [&](const gcn::ShaderProgram& prog, const gcn::ShaderKey& k, const ResolvedTextures& tex,
                           uint32_t base) {
        for (const auto& site : prog.sites) {
            if (site.kind != gcn::Site::Image || base + site.binding >= kMaxBindings) continue;
            const gcn::ImageType t = site.binding < k.image_types.size() ? k.image_types[site.binding] : gcn::ImageType::Tex2D;
            VkImageView view = site.binding < tex.views.size() && tex.views[site.binding] ? tex.views[site.binding]
                                                                                          : dummies[t].view;
            VkSampler samp = site.binding < tex.samplers.size() && tex.samplers[site.binding] ? tex.samplers[site.binding]
                                                                                              : sampler;
            if (site.depth_compare) samp = shadow_sampler;
            infos[nwrites] = {samp, view, VK_IMAGE_LAYOUT_GENERAL};
            writes[nwrites] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[nwrites].dstBinding = base + site.binding;
            writes[nwrites].descriptorCount = 1;
            writes[nwrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[nwrites].pImageInfo = &infos[nwrites];
            ++nwrites;
        }
    };
    bind_images(*vs, vkey, vs_tex, 0);
    if (ps) bind_images(*ps, pkey, ps_tex, gcn::kPixelBindingBase);
    if (nwrites) push_descriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, nwrites, writes);

    // --- the draw ---
    const uint32_t count = draw.index_count;
    // VGT_INDX_OFFSET is added to every index (a base vertex for sub-meshes in shared buffers).
    const int32_t index_offset = int32_t(cx[kVgtIndxOffset]);
    {
        static int logged = 0;
        if (index_offset && logged++ < 5)
            std::fprintf(stderr, "[renderer] VGT_INDX_OFFSET %d on a %s draw\n", index_offset, draw.indexed ? "indexed" : "auto");
    }
    if (prim == kPrimQuadList) {
        // Quads to triangle-list indices, through the guest index buffer if there is one.
        const uint32_t quads = count / 4;
        VkDeviceAddress unused_addr;
        VkDeviceSize offset;
        auto* idx = reinterpret_cast<uint32_t*>(RingAlloc(VkDeviceSize(quads) * 6 * 4, unused_addr, offset));
        if (!idx) return Skip("ring full", r);
        auto source = [&](uint32_t i) -> uint32_t {
            if (!draw.indexed) return i;
            return draw.index_type ? reinterpret_cast<const uint32_t*>(draw.index_address)[i]
                                   : reinterpret_cast<const uint16_t*>(draw.index_address)[i];
        };
        static const uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t q = 0; q < quads; ++q)
            for (int k = 0; k < 6; ++k) idx[q * 6 + k] = source(q * 4 + kOrder[k]);
        vkCmdBindIndexBuffer(cmd, ring, offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, quads * 6, draw.instances, 0, index_offset, 0);
    } else if (draw.indexed) {
        Import* im = ImportFor(draw.index_address);
        if (!im) return Skip("index memory not imported", r);
        vkCmdBindIndexBuffer(cmd, im->buffer, draw.index_address - im->base,
                             draw.index_type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, count, draw.instances, 0, index_offset, 0);
    } else {
        vkCmdDraw(cmd, count, draw.instances, uint32_t(index_offset), 0);
    }
    ++draws;
    if (pass_timing && !passes.empty()) ++passes.back().draws;
    mark(6);
}

// --- Renderer ---

bool Renderer::Init(Device& device) {
    impl_ = new Impl;
    impl_->dev = &device;
    if (!device.has_external_memory_host()) {
        std::fprintf(stderr, "[renderer] VK_EXT_external_memory_host is required\n");
        return false;
    }
    impl_->Init();
    ready_ = true;
    return true;
}

void Renderer::Draw(const gpu::Registers& regs, const gpu::DrawInfo& draw) {
    if (!ready_) return;
    const auto t0 = std::chrono::steady_clock::now();
    impl_->Draw(regs, draw);
    impl_->record_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

bool Renderer::Flip(uint64_t display_address, uint32_t width, uint32_t height) {
    if (!ready_) return false;
    if (impl_->flash_count) impl_->ProbeRecord(display_address, impl_->frame + 1);
    if (!impl_->warm_started) impl_->StartPipelineWarmup();
    impl_->MergeWarmPipelines();
    if (impl_->frame % 600 == 0 && impl_->deferred_draws.load(std::memory_order_relaxed)) {
        const uint64_t done = impl_->async_done.exchange(0), wait = impl_->async_wait_ns.exchange(0);
        std::fprintf(stderr, "[shaders] last 600 frames: %llu draws waited for background compiles; %llu jobs done, "
                     "avg %.0f ms each from queue to ready\n",
                     (unsigned long long)impl_->deferred_draws.exchange(0), (unsigned long long)done,
                     done ? wait / 1e6 / double(done) : 0.0);
    }
    impl_->PickCamera();
    if (Impl::UltrawideSqueeze() < 1.0f && !impl_->cam_cur.valid && impl_->disp_any_draws)
        if (const Target* t = impl_->FindTargetByAddress(display_address)) impl_->Pillarbox(*t);
    impl_->disp_any_draws = 0;
    impl_->scene_color = 0;
    impl_->post_seen = impl_->hud_started = false;
    impl_->ph_scene_from = nullptr;
    impl_->f2_ran = false;
    if (Impl::Fsr2On() && !(g_f2_debug.load() & 64)) impl_->Fsr2Pass(display_address);
    else if (Impl::TaaOn()) impl_->TaaPass(display_address);
    else impl_->MotionPass(display_address);
    impl_->SubmitFrame();
    impl_->display_addrs.insert(display_address);
    impl_->display_draws = 0;
    impl_->prehud_valid = false;
    // Next frame's jitter goes on this scene depth buffer, only while TAA / FSR 2 resolves the frames: a frame
    // they can't take (no camera: menus, the Toy Pad screen) would reach the screen still jittered.
    const bool resolved = Impl::Fsr2On() ? impl_->f2_ran : Impl::TaaOn();
    impl_->main_depth = impl_->cam_cur.valid && impl_->cam_cur.depth && resolved ? impl_->cam_cur.depth->address : 0;
    if (!Impl::TaaOn()) impl_->hist_valid = false;
    if (Impl::Fsr2On() && g_up_w.load()) {  // next frame's jitter: FSR 2's sequence for this upscale
        const Target* t = impl_->FindTargetByAddress(display_address);
        const int32_t rw = t ? int32_t(t->pw) : int32_t(Scaled(width));
        const int32_t phases = ffxFsr2GetJitterPhaseCount(rw, int32_t(g_up_w.load()));
        ffxFsr2GetJitterOffset(&impl_->jitter_x, &impl_->jitter_y, int32_t(impl_->frame % uint64_t(std::max(1, phases))),
                               phases);
    } else {  // TAA: Halton 2,3; 8 phases, in target pixels
        const uint32_t i = uint32_t(impl_->frame % 8) + 1;
        impl_->jitter_x = (Impl::Halton(i, 2) - 0.5f);
        impl_->jitter_y = (Impl::Halton(i, 3) - 0.5f);
    }
    if (g_f2_debug.load() & 4) impl_->jitter_x = impl_->jitter_y = 0.0f;
    impl_->SavePipelineCache(false);
    static const bool writeback = std::getenv("LDPORT_WRITEBACK") != nullptr;
    if (writeback) impl_->WritebackSmallTargets();
    {
        const auto now = std::chrono::steady_clock::now();
        if (impl_->last_flip.time_since_epoch().count()) {
            const double ms = std::chrono::duration<double, std::milli>(now - impl_->last_flip).count();
            impl_->worst_flip_ms = std::max(impl_->worst_flip_ms, ms);
            // Hitch log: a frame over 25 ms, with where this frame's CPU time went (draw recording by stage,
            // new textures / pipelines) next to the frame interval.
            static size_t last_textures = 0, last_pipelines = 0;
            static int hitches_logged = 0;
            const size_t new_tex = impl_->textures.size() - std::min(last_textures, impl_->textures.size());
            const size_t new_pipes = impl_->pipelines.size() - std::min(last_pipelines, impl_->pipelines.size());
            if (ms > 25.0 && hitches_logged < 400) {
                ++hitches_logged;
                double total = 0;
                for (int i = 0; i < kProfStages; ++i) total += g_frame_prof_ns[i] / 1e6;
                std::fprintf(stderr, "[hitch] frame %llu: %.1f ms since the last flip; recording %.1f ms (", (unsigned long long)impl_->frame, ms, total);
                for (int i = 0; i < kProfStages; ++i)
                    std::fprintf(stderr, "%s%s %.1f", i ? ", " : "", kProfNames[i], g_frame_prof_ns[i] / 1e6);
                std::fprintf(stderr, "); %zu new textures, %zu new pipelines; modules: GLSL %.1f ms, load/compile %.1f ms; textures: create %.1f ms, detile %.1f ms\n",
                             new_tex, new_pipes, impl_->module_gen_ns / 1e6, impl_->module_rest_ns / 1e6,
                             impl_->tex_create_ns / 1e6, impl_->tex_detile_ns / 1e6);
            }
            last_textures = impl_->textures.size();
            last_pipelines = impl_->pipelines.size();
            for (uint64_t& x : g_frame_prof_ns) x = 0;
            impl_->module_gen_ns = impl_->module_rest_ns = 0;
            impl_->tex_create_ns = impl_->tex_detile_ns = 0;
            if (ms > 20.0) ++impl_->late_flips;
        }
        impl_->last_flip = now;
    }
    ++impl_->frame;
    // Debug (LDPORT_WATCH_CAMERA=1): write-protect the page of this frame's camera block, so the game's next write
    // to it (when the ring comes round) is caught and its code address logged. Re-armed every 30 frames, 12 times.
#ifdef _WIN32
    if (impl_->cam_cur.valid && std::getenv("LDPORT_WATCH_CAMERA") && impl_->frame % 30 == 0 && g_watch_arms < 12) {
        g_watch_page = impl_->cam_cur.address & ~uint64_t(0xFFF);
        g_watch_block = impl_->cam_cur.address;
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(g_watch_page.load()), 0x1000, PAGE_READONLY, &old)) {
            g_watch_old = old;
            ++g_watch_arms;
            static bool installed = false;
            if (!installed) {
                installed = true;
                AddVectoredExceptionHandler(1, WatchHandler);
            }
        }
    }
#endif
    // Camera: this frame's becomes the previous one (motion vectors need both).
    if (impl_->cam_cur.valid) {
        static const bool log_camera = std::getenv("LDPORT_LOG_CAMERA") != nullptr;
        static const bool every = log_camera && std::getenv("LDPORT_LOG_CAMERA")[0] == '2';
        bool stale = false;
        if (Impl::Readable(reinterpret_cast<const void*>(impl_->cam_cur.address), 160))
            stale = std::memcmp(reinterpret_cast<const void*>(impl_->cam_cur.address), impl_->cam_cur.vp, 64) != 0;
        if (log_camera && impl_->frame % 60 == 0) {
            float fov = 0, iv[16], P[16];
            Impl::Inverse4(impl_->cam_cur.view, iv);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) sum += iv[r * 4 + k] * impl_->cam_cur.vp[k * 4 + c];
                    P[r * 4 + c] = sum;
                }
            fov = 2.0f * std::atan(1.0f / std::max(1e-4f, std::fabs(P[5])));
            std::fprintf(stderr, "[camera] aspect %.4f vfov %.1f deg, pos %.2f %.2f %.2f\n",
                         P[0] != 0.0f ? std::fabs(P[5] / P[0]) : 0.0f, fov * 57.2958f, impl_->cam_cur.pos[0],
                         impl_->cam_cur.pos[1], impl_->cam_cur.pos[2]);
        }
        if (log_camera && every)
            std::fprintf(stderr, "[camera] %u jittered draws, %u draws; ", impl_->jittered_draws,
                         uint32_t(impl_->draws - impl_->draws_at_flip)),
            std::fprintf(stderr, "[camera] %s frame %llu: pos %.4f %.4f %.4f near %.2f far %.1f vp row0 %.5f %.5f %.5f %.5f row3 %.5f %.5f %.5f %.5f (block %llx)\n",
                         stale ? "CHANGED-SINCE-DRAW" : "same", (unsigned long long)impl_->frame, impl_->cam_cur.pos[0], impl_->cam_cur.pos[1],
                         impl_->cam_cur.pos[2], impl_->cam_cur.near_far[0], impl_->cam_cur.near_far[1],
                         impl_->cam_cur.vp[0], impl_->cam_cur.vp[1], impl_->cam_cur.vp[2], impl_->cam_cur.vp[3],
                         impl_->cam_cur.vp[12], impl_->cam_cur.vp[13], impl_->cam_cur.vp[14], impl_->cam_cur.vp[15],
                         (unsigned long long)impl_->cam_cur.address);
        impl_->cam_prev = impl_->cam_cur;
    }
    impl_->last_jittered_draws = impl_->jittered_draws;
    impl_->jittered_draws = 0;
    impl_->cam_cur.valid = false;
    impl_->frame_draws = impl_->draws - impl_->draws_at_flip;
    impl_->draws_at_flip = impl_->draws;
    if (impl_->frame % 120 == 1) {
        static uint64_t last_draws = 0;
        std::fprintf(stderr,
                     "[renderer] frame %llu: %llu draws/frame, GPU %.1f ms/frame, CPU wait %.1f ms/frame, record %.1f ms/frame, "
                     "worst flip %.1f ms, %u late, %llu skipped, %zu pipelines, %zu targets\n",
                     (unsigned long long)impl_->frame, (unsigned long long)((impl_->draws - last_draws) / 120),
                     impl_->gpu_frame_us / 120.0 / 1000.0, impl_->gpu_wait_us / 120.0 / 1000.0,
                     impl_->record_us / 120.0 / 1000.0, impl_->worst_flip_ms, impl_->late_flips,
                     (unsigned long long)impl_->skipped, impl_->pipelines.size(), impl_->targets.size());
        impl_->record_us = 0;
        if (g_profile) {
            std::fprintf(stderr, "[profile] ms/frame:");
            for (int i = 0; i < kProfStages; ++i) {
                std::fprintf(stderr, " %s %.2f", kProfNames[i], g_prof_ns[i] / 120.0 / 1e6);
                g_prof_ns[i] = 0;
            }
            std::fprintf(stderr, "\n");
        }
        impl_->worst_flip_ms = 0;
        impl_->late_flips = 0;
        last_draws = impl_->draws;
        impl_->gpu_wait_us = 0;
        impl_->gpu_frame_us = 0;
        if (impl_->pass_timing && !impl_->pass_totals.empty()) {
            std::vector<std::pair<uint64_t, Impl::PassTotal>> v(impl_->pass_totals.begin(), impl_->pass_totals.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
            double total = 0;
            for (const auto& e : v) total += e.second.ms;
            std::fprintf(stderr, "[passes] GPU ms/frame by target (total %.2f):", total / 120);
            for (size_t i = 0; i < v.size() && i < 12; ++i)
                std::fprintf(stderr, " %llx=%.2fms/%.0fpass/%.0fdraws", (unsigned long long)v[i].first, v[i].second.ms / 120,
                             v[i].second.passes / 120.0, v[i].second.draws / 120.0);
            std::fprintf(stderr, "\n");
            impl_->pass_totals.clear();
        }
    }
    static const int64_t dump_frame = std::getenv("LDPORT_DUMP") ? std::atoll(std::getenv("LDPORT_DUMP")) : -1;
    if (int64_t(impl_->frame) == dump_frame) impl_->DumpTargets(display_address);

    // LDPORT_SHOTS=<n>: a screenshot of the displayed frame every n frames, into run/shots.
    static const int64_t shots = std::getenv("LDPORT_SHOTS") ? std::atoll(std::getenv("LDPORT_SHOTS")) : 0;
    if (shots > 0 && impl_->frame % shots == 0) impl_->DumpTargets(display_address, true);
    if (capture_state_ == 2) {  // the captured frame has ended: dump what it drew
        std::error_code ec;
        std::filesystem::remove_all("run/dump", ec);
        impl_->DumpTargets(display_address);
        capture_state_ = 0;
        capture_done_ = impl_->frame;
    } else if (capture_state_ == 1) {
        gpu::Capture::Get().ArmNextFrame();
        capture_state_ = 2;
    }
    if (shot_wanted_.exchange(false)) {
        impl_->DumpTargets(display_address, true);
        shot_done_ = impl_->frame;
    }
    if (impl_->dump_this) {
        impl_->dump_this = false;
        impl_->DumpTargets(display_address);
        // LDPORT_MEMDUMP=<hex addr>:<hex len>[,...]: raw guest memory at the trap, into run/dump/mem_<addr>.bin.
        if (const char* md = std::getenv("LDPORT_MEMDUMP")) {
            for (const char* q = md; *q;) {
                char* e = nullptr;
                const uint64_t a = std::strtoull(q, &e, 16);
                const uint64_t len = (e && *e == ':') ? std::strtoull(e + 1, &e, 16) : 0;
                if (a && len) {
                    char name[96];
                    std::snprintf(name, sizeof name, "run/dump/mem_%llx.bin", (unsigned long long)a);
                    std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char*>(a), std::streamsize(len));
                }
                q = (e && *e == ',') ? e + 1 : (e ? e : q + std::strlen(q));
                if (!e) break;
            }
        }
        std::error_code ec;  // keep the flash frame's dump apart; the next frame dumps to run/dump
        std::filesystem::remove_all("run/dump_flash", ec);
        std::filesystem::rename("run/dump", "run/dump_flash", ec);
        impl_->dump_next = true;
    } else if (impl_->dump_next && !impl_->flat_trapped_dumped) {
        impl_->flat_trapped_dumped = true;
        impl_->DumpTargets(display_address);
    }
    const Target* t = impl_->FindTargetByAddress(display_address);
    if (!t) {
        impl_->ApplyScaleChange();
        return false;
    }
    if (impl_->f2_ran) GetPresenter().PresentImage(impl_->f2_final, impl_->f2_iw, impl_->f2_ih);
    else GetPresenter().PresentImage(t->image, std::min(Scaled(width), t->pw), std::min(Scaled(height), t->ph));
    impl_->ApplyScaleChange();
    return true;
}

void Renderer::Sync() {
    if (!ready_ || !impl_->recording) return;
    impl_->SubmitFrame();
    impl_->WaitAll("a label sync");
}

uint64_t Renderer::ToDevice(uint64_t guest) {
    if (!ready_ || !guest) return 0;
    Import* im = impl_->ImportFor(guest);
    return im ? im->address + (guest - im->base) : 0;
}

namespace {
bool CopyGuest(void* dst, uint64_t src, uint64_t bytes) {
#ifndef _WIN32
    return host::SafeCopy(dst, reinterpret_cast<const void*>(src), size_t(bytes));
#else
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size_t(bytes));
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
#endif
}
}  // namespace

uint64_t Renderer::BufferDevice(uint64_t guest, uint64_t bytes) {
    static const bool snapshot = std::getenv("LDPORT_SNAPSHOT") != nullptr;
    if (snapshot && ready_ && guest && bytes && bytes <= 16 * 1024) {
        VkDeviceAddress address;
        VkDeviceSize offset;
        if (uint8_t* p = impl_->RingAlloc(bytes, address, offset); p && CopyGuest(p, guest, bytes)) return address;
    }
    return ToDevice(guest);
}

void Renderer::ReleaseGuestMemory(uint64_t base) {
    if (!ready_) return;
    impl_->EndRendering();
    if (impl_->recording) impl_->SubmitFrame();
    impl_->WaitAll("a memory unmap");
    vkQueueWaitIdle(impl_->dev->queue());
    for (Import*& c : impl_->recent_imports) c = nullptr;
    for (auto it = impl_->imports.begin(); it != impl_->imports.end();) {
        if (it->second.base == base) {
            if (it->second.buffer) vkDestroyBuffer(impl_->d, it->second.buffer, nullptr);
            if (it->second.memory) vkFreeMemory(impl_->d, it->second.memory, nullptr);
            it = impl_->imports.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t Renderer::Screenshot(uint32_t timeout_ms) {
    shot_done_ = 0;
    shot_wanted_ = true;
    for (uint32_t waited = 0; waited < timeout_ms && !shot_done_; waited += 10)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return shot_done_;
}

void Renderer::ClearTargetsForReplay() {
    if (!ready_) return;
    impl_->BeginFrame();
    impl_->EndRendering();
    const VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}};
    for (const auto& [key, t] : impl_->targets) {
        if (t.depth) continue;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(impl_->cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &range);
    }
    impl_->FullBarrier();
}

uint64_t Renderer::CaptureFrame(uint32_t timeout_ms) {
    capture_done_ = 0;
    capture_state_ = 1;
    for (uint32_t waited = 0; waited < timeout_ms && !capture_done_; waited += 10)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return capture_done_;
}

float CurrentRenderScale() { return RenderScale(); }
void SetFsr2Debug(int bits) { g_f2_debug = bits; }
bool DlssTried() { return g_dlss_tried.load(); }
void SetUpscaleOutput(uint32_t width, uint32_t height) {
    g_up_w = width;
    g_up_h = height;
}
void SetRenderScale(float scale) {
    if (scale >= 0.25f && scale <= 4.0f) g_wanted_scale = scale;
}

Renderer& GetRenderer() {
    static Renderer r;
    return r;
}

}  // namespace vkr
