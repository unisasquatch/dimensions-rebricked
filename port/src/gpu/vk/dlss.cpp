// NVIDIA DLSS through NGX (see dlss.h). The renderer gives it the same inputs as FSR 2: the game's finished
// picture before the HUD, its depth, camera motion vectors (render pixels, current -> previous) and the jitter.
#include "gpu/vk/dlss.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace vkr::dlss {

#ifdef _WIN32
namespace {

bool g_tried = false, g_ok = false;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
uint32_t g_rw = 0, g_rh = 0, g_ow = 0, g_oh = 0;
int g_quality = -1;
bool g_inverted = false;
VkDevice g_device = VK_NULL_HANDLE;

// A project ID for a custom engine (NVSDK_NGX_ENGINE_TYPE_CUSTOM): any stable GUID.
constexpr const char* kProjectId = "6f1c2b8e-4d7a-4c55-9a3e-2b7d0f9e1c42";

bool Supported(const std::vector<VkExtensionProperties>& have, const char* name) {
    for (const auto& e : have)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

NVSDK_NGX_PerfQuality_Value Quality(int q) {
    switch (q) {
        case 1: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case 2: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case 3: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case 4: return NVSDK_NGX_PerfQuality_Value_DLAA;
        default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

NVSDK_NGX_Resource_VK Resource(const Image& i, bool rw) {
    VkImageSubresourceRange range{i.aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.image, range, i.format, i.width, i.height, rw);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

}  // namespace

void AddInstanceExtensions(std::vector<const char*>& exts) {
    unsigned int ni = 0, nd = 0;
    const char** inst = nullptr;
    const char** devx = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &inst, &nd, &devx))) return;
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> have(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, have.data());
    for (unsigned int i = 0; i < ni; ++i) {
        bool listed = false;
        for (const char* e : exts) listed = listed || std::strcmp(e, inst[i]) == 0;
        if (!listed && Supported(have, inst[i])) exts.push_back(inst[i]);
    }
}

void AddDeviceExtensions(VkPhysicalDevice phys, std::vector<const char*>& exts) {
    unsigned int ni = 0, nd = 0;
    const char** inst = nullptr;
    const char** devx = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &inst, &nd, &devx))) return;
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> have(count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, have.data());
    for (unsigned int i = 0; i < nd; ++i) {
        bool listed = false;
        for (const char* e : exts) listed = listed || std::strcmp(e, devx[i]) == 0;
        if (!listed && Supported(have, devx[i])) exts.push_back(devx[i]);
    }
}

bool Init(VkInstance instance, VkPhysicalDevice phys, VkDevice device) {
    if (g_tried) return g_ok;
    g_tried = true;
    g_device = device;
    const std::wstring dir = ExeDir();
    const wchar_t* paths[] = {dir.c_str()};  // nvngx_dlss.dll ships next to ldport.exe
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(),
                                                              instance, phys, device, vkGetInstanceProcAddr,
                                                              vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        std::fprintf(stderr, "[dlss] not available: NGX init failed (%#x); DLSS modes use FSR 2\n", unsigned(r));
        return false;
    }
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_params);
    int available = 0, needs_driver = 0;
    if (NVSDK_NGX_SUCCEED(r)) {
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    }
    if (!available) {
        std::fprintf(stderr, "[dlss] not available on this GPU%s; DLSS modes use FSR 2\n",
                     needs_driver ? " (the driver needs updating)" : "");
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    std::fprintf(stderr, "[dlss] ready\n");
    g_ok = true;
    return true;
}

bool Available() { return g_ok; }

bool Evaluate(VkCommandBuffer cmd, const Frame& f) {
    if (!g_ok) return false;
    if (!g_feature || f.render_w != g_rw || f.render_h != g_rh || f.output.width != g_ow ||
        f.output.height != g_oh || f.quality != g_quality || f.depth_inverted != g_inverted) {
        if (g_feature) {
            vkDeviceWaitIdle(g_device);
            NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
            g_feature = nullptr;
        }
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = f.render_w;
        cp.Feature.InHeight = f.render_h;
        cp.Feature.InTargetWidth = f.output.width;
        cp.Feature.InTargetHeight = f.output.height;
        cp.Feature.InPerfQualityValue = Quality(f.quality);
        // Motion vectors at the render size, not jittered (camera-only); the input is the finished SDR picture.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  (f.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
        const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &g_feature, g_params, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            static int logged = 0;
            if (logged++ < 3) std::fprintf(stderr, "[dlss] feature creation failed (%#x)\n", unsigned(r));
            g_feature = nullptr;
            return false;
        }
        g_rw = f.render_w;
        g_rh = f.render_h;
        g_ow = f.output.width;
        g_oh = f.output.height;
        g_quality = f.quality;
        g_inverted = f.depth_inverted;
        std::fprintf(stderr, "[dlss] %ux%u -> %ux%u, quality %d%s\n", g_rw, g_rh, g_ow, g_oh, g_quality,
                     g_inverted ? ", inverted depth" : "");
    }
    NVSDK_NGX_Resource_VK color = Resource(f.color, false), depth = Resource(f.depth, false),
                          motion = Resource(f.motion, false), out = Resource(f.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color;
    ep.Feature.pInOutput = &out;
    ep.pInDepth = &depth;
    ep.pInMotionVectors = &motion;
    ep.InJitterOffsetX = f.jitter_x;
    ep.InJitterOffsetY = f.jitter_y;
    ep.InRenderSubrectDimensions = {f.render_w, f.render_h};
    ep.InReset = f.reset ? 1 : 0;
    ep.InMVScaleX = f.mv_scale_x;
    ep.InMVScaleY = f.mv_scale_y;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = 1000.0f / 60.0f;
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        static int logged = 0;
        if (logged++ < 3) std::fprintf(stderr, "[dlss] evaluate failed (%#x)\n", unsigned(r));
        return false;
    }
    return true;
}

void Shutdown(VkDevice device) {
    if (!g_ok) return;
    if (g_feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
    g_feature = nullptr;
    if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params);
    g_params = nullptr;
    NVSDK_NGX_VULKAN_Shutdown1(device);
    g_ok = false;
}

#else  // Linux / Steam Deck: no DLSS

void AddInstanceExtensions(std::vector<const char*>&) {}
void AddDeviceExtensions(VkPhysicalDevice, std::vector<const char*>&) {}
bool Init(VkInstance, VkPhysicalDevice, VkDevice) { return false; }
bool Available() { return false; }
bool Evaluate(VkCommandBuffer, const Frame&) { return false; }
void Shutdown(VkDevice) {}

#endif

}  // namespace vkr::dlss
