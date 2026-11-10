// libSceVideoOut: the display. Buffers the game registers are shown in an SDL window on flip.
// Presented through Vulkan. Until the GPU renderer exists, flipping shows the buffer's raw memory.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "hle/clock.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "hle/videoout.h"
#include "hle/port_settings.h"
#include "ui/overlay.h"
#include <string>
#include "gpu/vk/device.h"
#include "gpu/vk/renderer.h"

namespace {

constexpr int kVideoOutHandle = 1;
constexpr uint64_t kEventFlip = 0x6;
constexpr int kMaxBuffers = 16;

struct BufferAttribute {
    int32_t pixel_format, tiling_mode, aspect_ratio;
    uint32_t width, height, pitch_in_pixels, option, reserved0;
    uint64_t reserved1;
};

struct Display {
    std::mutex mutex;
    void* buffers[kMaxBuffers] = {};
    BufferAttribute attr{};
    SDL_Window* window = nullptr;
    bool vulkan_ready = false;
    std::atomic<uint64_t> flips{0};
};
Display g_display;

// Main thread: windowed, borderless (fullscreen at the desktop) or exclusive fullscreen at the display's
// desktop mode. The renderer follows the new window size like any resize.
void ApplyDisplayMode(uint32_t mode) {
    SDL_Window* w = g_display.window;
    if (!w) return;
    if (mode == 0) {
        SDL_SetWindowFullscreen(w, false);
    } else {
        const SDL_DisplayMode* desktop = mode == 2 ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w)) : nullptr;
        SDL_SetWindowFullscreenMode(w, desktop);  // null = borderless
        SDL_SetWindowFullscreen(w, true);
    }
    std::fprintf(stderr, "[videoout] display mode: %s\n", port::DisplayModeName(mode));
}

// SDL wants windows and Vulkan surfaces created on the main thread.
void CreateWindowOnMain(void*) {
    // Port settings (Options menu): saved resolution, aspect ratio and fullscreen.
    port::Load();
    // The saved display mode (Options menu; F11 / Alt+Enter toggle windowed / borderless) is applied once
    // the window exists; LDPORT_FULLSCREEN=1 asks for borderless.
    if (std::getenv("LDPORT_FULLSCREEN") && !port::Get().display.load())
        port::Get().display = uint32_t(port::DisplayMode::Borderless);
    g_display.window = SDL_CreateWindow("LEGO Dimensions", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    ApplyDisplayMode(port::Get().display.load());
    // Render resolution (decided after the window: SDL starts its video system there). The game draws
    // at 1080p; on a 2160p (4K) display render at twice that unless LDPORT_SCALE says otherwise
    // (LDPORT_SCALE=1 for native 1080p).
    // The tallest connected display counts (the primary one can be a virtual display, e.g. Parsec's);
    // desktop modes can be in scaled units under Windows display scaling, so compare real pixels.
    {
        float tallest = 0;
        int count = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
            for (int i = 0; i < count; ++i)
                if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i])) {
                    const float px = mode->h * (mode->pixel_density > 0 ? mode->pixel_density : 1.0f);
                    std::fprintf(stderr, "[video] display %d: %dx%d, density %.2f\n", i, mode->w, mode->h,
                                 mode->pixel_density);
                    tallest = std::max(tallest, px);
                }
            SDL_free(ids);
        }
        port::SetAutoScale(tallest >= 2160 ? 2.0f : 1.0f);  // what Resolution: Automatic means here
    }
    // Options > Display Settings: a chosen Resolution with the upscaler off sets the render scale (1440p = 1.333 x
    // the game's 1080p); Automatic follows the display. Upscalers set it themselves each frame (the presenter).
    if (!std::getenv("LDPORT_SCALE")) {
        const std::string s = std::to_string(port::NativeScale(port::Get().resolution.load()));
        _putenv_s("LDPORT_SCALE", s.c_str());
    }
    std::fprintf(stderr, "[video] render scale %s\n", std::getenv("LDPORT_SCALE") ? std::getenv("LDPORT_SCALE") : "1");
    g_display.vulkan_ready = g_display.window && vkr::GetDevice().Init(g_display.window) &&
                             vkr::GetPresenter().Init(vkr::GetDevice(), g_display.window);
    if (g_display.vulkan_ready && !std::getenv("LDPORT_NO_RENDERER")) vkr::GetRenderer().Init(vkr::GetDevice());
    if (g_display.vulkan_ready) ui::Init(g_display.window);  // the in-game Toy Pad picker
}

SYSV int Open(int, int, int, const void*) {
    SDL_RunOnMainThread(CreateWindowOnMain, nullptr, true);
    return kVideoOutHandle;
}
SYSV int SetBufferAttribute(BufferAttribute* a, uint32_t format, uint32_t tiling, uint32_t aspect, uint32_t width,
                            uint32_t height, uint32_t pitch) {
    *a = {int32_t(format), int32_t(tiling), int32_t(aspect), width, height, pitch, 0, 0, 0};
    return 0;
}
SYSV int RegisterBuffers(int, int start, void* const* addresses, int count, const BufferAttribute* attr) {
    std::lock_guard lock(g_display.mutex);
    for (int i = 0; i < count && start + i < kMaxBuffers; ++i) g_display.buffers[start + i] = addresses[i];
    g_display.attr = *attr;
    std::fprintf(stderr, "[videoout] %d buffers %ux%u format %#x tiling %d at", count, attr->width, attr->height,
                 attr->pixel_format, attr->tiling_mode);
    for (int i = 0; i < count; ++i) std::fprintf(stderr, " %p", addresses[i]);
    std::fprintf(stderr, "\n");
    return 0;  // buffer group index
}
SYSV int AddFlipEvent(hle::EventQueue* q, int, void* udata) {
    q->Add(kEventFlip, hle::kFilterVideoOut, udata);
    return 0;
}
SYSV int Ok() { return 0; }

}  // namespace

namespace hle {

void VideoOutFlip(int buffer_index, int64_t flip_arg) {
    const void* pixels = nullptr;
    BufferAttribute attr;
    {
        std::lock_guard lock(g_display.mutex);
        if (buffer_index >= 0 && buffer_index < kMaxBuffers) pixels = g_display.buffers[buffer_index];
        attr = g_display.attr;
    }
    // The renderer presents its render target for this display buffer; if the game never drew to it
    // (or the renderer is off), show the buffer's memory as before.
    if (hle::FastForwarding()) {  // nothing drawn or shown; the game clock steps one frame
        const uint64_t flips = ++g_display.flips;
        ClockOnFlip(flips);
        TriggerAll(kEventFlip, kFilterVideoOut, flip_arg);
        return;
    }
    const bool presented = vkr::GetRenderer().Flip(reinterpret_cast<uint64_t>(pixels), attr.width, attr.height);
    if (!presented && g_display.vulkan_ready && pixels && attr.width)
        vkr::GetPresenter().PresentPixels(pixels, attr.width, attr.height, attr.pitch_in_pixels);
    ClockOnFlip(++g_display.flips);
    TriggerAll(kEventFlip, kFilterVideoOut, flip_arg);
}

uint64_t VideoOutFlipCount() { return g_display.flips; }

void VideoOutRepresent(int buffer_index) {
    const void* pixels = nullptr;
    BufferAttribute attr;
    {
        std::lock_guard lock(g_display.mutex);
        if (buffer_index >= 0 && buffer_index < kMaxBuffers) pixels = g_display.buffers[buffer_index];
        attr = g_display.attr;
    }
    vkr::GetRenderer().Flip(reinterpret_cast<uint64_t>(pixels), attr.width, attr.height);
}

void VideoOutMainThreadTick() {
    uint32_t mode = 0;
    if (g_display.window && port::TakeDisplayModeRequest(mode)) ApplyDisplayMode(mode);
}

void VideoOutSetTitle(const char* title) {
    if (g_display.window) SDL_SetWindowTitle(g_display.window, title);
}

}  // namespace hle

#define VIDEO(name, fn) HLE_EXPORT("libSceVideoOut", name, reinterpret_cast<const void*>(&fn))
VIDEO("sceVideoOutOpen", Open);
VIDEO("sceVideoOutSetBufferAttribute", SetBufferAttribute);
VIDEO("sceVideoOutRegisterBuffers", RegisterBuffers);
VIDEO("sceVideoOutAddFlipEvent", AddFlipEvent);
VIDEO("sceVideoOutSetFlipRate", Ok);
VIDEO("sceVideoOutSetWindowModeMargins", Ok);
VIDEO("sceVideoOutAdjustColor_", Ok);
VIDEO("sceVideoOutColorSettingsSetGamma_", Ok);
