#include "gpu/vk/fsr.h"

#include "gpu/vk/device.h"

#include <shaderc/shaderc.h>
#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// AMD's constant setup, compiled for the CPU.
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace vkr {
namespace {

// The FidelityFX headers next to the executable (fsr1/), or in the source tree when run from the build folder.
std::string ReadHeader(const char* name) {
    const char* base = SDL_GetBasePath();
    for (const std::string dir : {std::string(base ? base : "") + "fsr1/", std::string("port/third_party/fsr1/")}) {
        std::ifstream in(dir + name, std::ios::binary);
        if (in) {
            std::ostringstream s;
            s << in.rdbuf();
            return s.str();
        }
    }
    return {};
}

// Both passes: 64 threads, each filtering a 2x2 group of 8x8 tiles (a 16x16 block per workgroup).
constexpr const char* kEasu = R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform Con { uvec4 c0, c1, c2, c3; } con;
AF4 FsrEasuRF(AF2 p) { return textureGather(src, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(src, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(src, p, 2); }
#define FSR_EASU_F 1
FSR1_HEADER
void Filter(AU2 pos) {
    AF3 c;
    FsrEasuF(c, pos, con.c0, con.c1, con.c2, con.c3);
    // The source is a B8G8R8A8 image sampled as RGB: store BGR so the bytes match the swapchain.
    imageStore(dst, ASU2(pos), AF4(c.b, c.g, c.r, 1.0));
}
)";

constexpr const char* kRcas = R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform Con { uvec4 c0, c1, c2, c3; } con;
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(src, p, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
FSR1_HEADER
void Filter(AU2 pos) {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, con.c0);
    imageStore(dst, ASU2(pos), AF4(c, 1.0));
}
)";

constexpr const char* kMain = R"(
layout(local_size_x = 64) in;
void main() {
    AU2 gxy = ARmp8x8(gl_LocalInvocationID.x) + AU2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    Filter(gxy);
    gxy.x += 8u;
    Filter(gxy);
    gxy.y += 8u;
    Filter(gxy);
    gxy.x -= 8u;
    Filter(gxy);
}
)";

void ToLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
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

VkShaderModule Fsr1::Compile(const char* name, const char* body) {
    const std::string a = ReadHeader("ffx_a.h"), fsr = ReadHeader("ffx_fsr1.h");
    if (a.empty() || fsr.empty()) {
        std::fprintf(stderr, "[fsr] FidelityFX headers not found (fsr1/ffx_a.h, ffx_fsr1.h): FSR 1 is off\n");
        return VK_NULL_HANDLE;
    }
    std::string src = std::string("#version 450\n#define A_GPU 1\n#define A_GLSL 1\n") + a + body + kMain;
    const size_t at = src.find("FSR1_HEADER");
    src.replace(at, 11, fsr);
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_performance);
    shaderc_compilation_result_t r =
        shaderc_compile_into_spv(compiler, src.data(), src.size(), shaderc_compute_shader, name, "main", opts);
    VkShaderModule module = VK_NULL_HANDLE;
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = shaderc_result_get_length(r);
        ci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
        vkCreateShaderModule(dev_->device(), &ci, nullptr, &module);
    } else {
        std::fprintf(stderr, "[fsr] %s: %s\n", name, shaderc_result_get_error_message(r));
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(opts);
    shaderc_compiler_release(compiler);
    return module;
}

bool Fsr1::Init(Device& device) {
    dev_ = &device;
    VkDevice d = device.device();
    push_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(d, &sci, nullptr, &sampler_);
    const VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 2;
    dl.pBindings = binds;
    vkCreateDescriptorSetLayout(d, &dl, nullptr, &set_layout_);
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(d, &pl, nullptr, &layout_);
    for (auto [body, name, pipe] : {std::tuple{kEasu, "fsr1_easu", &easu_}, std::tuple{kRcas, "fsr1_rcas", &rcas_}}) {
        VkShaderModule m = Compile(name, body);
        if (!m) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
        ci.layout = layout_;
        vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, pipe);
        vkDestroyShaderModule(d, m, nullptr);
    }
    ready_ = easu_ && rcas_ && push_;
    std::fprintf(stderr, "[fsr] FSR 1 %s\n", ready_ ? "ready" : "unavailable");
    return ready_;
}

bool Fsr1::EnsureImages(uint32_t w, uint32_t h) {
    if (mid_ && w == w_ && h == h_) return true;
    VkDevice d = dev_->device();
    vkDeviceWaitIdle(d);
    for (auto [img, mem, view] : {std::tuple{&mid_, &mid_mem_, &mid_view_}, std::tuple{&out_, &out_mem_, &out_view_}}) {
        if (*view) vkDestroyImageView(d, *view, nullptr);
        if (*img) vkDestroyImage(d, *img, nullptr);
        if (*mem) vkFreeMemory(d, *mem, nullptr);
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(d, &ici, nullptr, img) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, *img, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(d, &mai, nullptr, mem) != VK_SUCCESS) return false;
        vkBindImageMemory(d, *img, *mem, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = *img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(d, &vci, nullptr, view);
    }
    w_ = w;
    h_ = h;
    return true;
}

void Fsr1::Record(VkCommandBuffer cmd, VkImageView source_view, uint32_t src_w, uint32_t src_h, uint32_t in_w,
                  uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_stops) {
    if (!ready_ || !EnsureImages(out_w, out_h)) return;
    AU1 con[16] = {};
    FsrEasuCon(&con[0], &con[4], &con[8], &con[12], AF1(in_w), AF1(in_h), AF1(src_w), AF1(src_h), AF1(out_w), AF1(out_h));
    auto pass = [&](VkPipeline pipe, VkImageView in, VkImageView out, const AU1* constants) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        VkDescriptorImageInfo src_info{sampler_, in, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst_info{VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_info, nullptr, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst_info, nullptr, nullptr};
        push_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 2, w);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, constants);
        vkCmdDispatch(cmd, (out_w + 15) / 16, (out_h + 15) / 16, 1);
    };
    ToLayout(cmd, mid_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    pass(easu_, source_view, mid_view_, con);
    ToLayout(cmd, mid_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);  // EASU writes before RCAS reads
    AU1 rcas[16] = {};
    FsrRcasCon(rcas, sharpness_stops);
    ToLayout(cmd, out_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    pass(rcas_, mid_view_, out_view_, rcas);
    ToLayout(cmd, out_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

// --- FXAA ---
namespace {
constexpr const char* kFxaa = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform Con { vec2 rcp; uvec2 size; } con;
float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 At(vec2 uv) { return textureLod(src, uv, 0.0).rgb; }
void main() {
    const uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= con.size.x || p.y >= con.size.y) return;
    const vec2 uv = (vec2(p) + 0.5) * con.rcp;
    const vec3 m = At(uv);
    const float lnw = Luma(At(uv + vec2(-1.0, -1.0) * con.rcp)), lne = Luma(At(uv + vec2(1.0, -1.0) * con.rcp));
    const float lsw = Luma(At(uv + vec2(-1.0, 1.0) * con.rcp)), lse = Luma(At(uv + vec2(1.0, 1.0) * con.rcp));
    const float lm = Luma(m);
    const float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    const float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (lmax - lmin < max(0.0312, lmax * 0.125)) {  // not an edge: keep the pixel
        imageStore(dst, ivec2(p), vec4(m, 1.0));
        return;
    }
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    const float reduce = max((lnw + lne + lsw + lse) * (0.25 * 0.125), 1.0 / 128.0);
    const float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp_min, vec2(-8.0), vec2(8.0)) * con.rcp;
    const vec3 a = 0.5 * (At(uv + dir * (1.0 / 3.0 - 0.5)) + At(uv + dir * (2.0 / 3.0 - 0.5)));
    const vec3 b = a * 0.5 + 0.25 * (At(uv - dir * 0.5) + At(uv + dir * 0.5));
    const float lb = Luma(b);
    imageStore(dst, ivec2(p), vec4((lb < lmin || lb > lmax) ? a : b, 1.0));
}
)";
}  // namespace

bool Fxaa::Init(Device& device) {
    dev_ = &device;
    VkDevice d = device.device();
    push_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(d, &sci, nullptr, &sampler_);
    const VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 2;
    dl.pBindings = binds;
    vkCreateDescriptorSetLayout(d, &dl, nullptr, &set_layout_);
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(d, &pl, nullptr, &layout_);
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(compiler, kFxaa, std::strlen(kFxaa), shaderc_compute_shader,
                                                              "fxaa", "main", opts);
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        VkShaderModule m = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = shaderc_result_get_length(r);
        ci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
        vkCreateShaderModule(d, &ci, nullptr, &m);
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
        pci.layout = layout_;
        vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe_);
        vkDestroyShaderModule(d, m, nullptr);
    } else {
        std::fprintf(stderr, "[fxaa] %s\n", shaderc_result_get_error_message(r));
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(opts);
    shaderc_compiler_release(compiler);
    ready_ = pipe_ && push_;
    std::fprintf(stderr, "[fxaa] %s\n", ready_ ? "ready" : "unavailable");
    return ready_;
}

void Fxaa::Record(VkCommandBuffer cmd, VkImageView source_view, uint32_t src_w, uint32_t src_h, uint32_t w, uint32_t h) {
    if (!ready_) return;
    VkDevice d = dev_->device();
    if (!out_ || src_w != w_ || src_h != h_) {
        vkDeviceWaitIdle(d);
        if (out_view_) vkDestroyImageView(d, out_view_, nullptr);
        if (out_) vkDestroyImage(d, out_, nullptr);
        if (out_mem_) vkFreeMemory(d, out_mem_, nullptr);
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {src_w, src_h, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCreateImage(d, &ici, nullptr, &out_);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, out_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = dev_->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(d, &mai, nullptr, &out_mem_);
        vkBindImageMemory(d, out_, out_mem_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = out_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(d, &vci, nullptr, &out_view_);
        w_ = src_w;
        h_ = src_h;
    }
    ToLayout(cmd, out_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    VkDescriptorImageInfo src_info{sampler_, source_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dst_info{VK_NULL_HANDLE, out_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_info, nullptr, nullptr};
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst_info, nullptr, nullptr};
    push_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 2, wr);
    struct { float rx, ry; uint32_t w, h; } con{1.0f / float(src_w), 1.0f / float(src_h), w, h};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &con);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    ToLayout(cmd, out_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);  // writes before the next pass reads
}

}  // namespace vkr
