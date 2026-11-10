#pragma once
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>

#include "gpu/capture.h"
#include "gpu/registers.h"

namespace vkr {

class Device;

// Turns the command processor's draws into Vulkan rendering. All calls come from the game's GPU
// submission thread; one frame is recorded into one command buffer and submitted at the flip.
class Renderer {
public:
    bool Init(Device& device);
    bool ready() const { return ready_; }

    void Draw(const gpu::Registers& regs, const gpu::DrawInfo& draw);
    // Ends the frame. Returns true if `display_address` was a render target and was presented.
    bool Flip(uint64_t display_address, uint32_t width, uint32_t height);
    // Submits recorded work and waits until the GPU has finished it.
    void Sync();

    // Guest address to a GPU device address (0 if the memory is not registered guest memory).
    uint64_t ToDevice(uint64_t guest);
    // Device address for a shader buffer: with LDPORT_SNAPSHOT=1 small read-only buffers are
    // copied into the per-draw ring so later CPU writes can't change what this draw reads.
    uint64_t BufferDevice(uint64_t guest, uint64_t bytes);
    // Debug remote: saves the next displayed frame to run/shots (like LDPORT_SHOTS); returns its
    // frame number once written, or 0 after `timeout_ms`.
    uint64_t Screenshot(uint32_t timeout_ms);
    // Debug remote: captures the next frame's draws (run/capture) and dumps all its colour targets
    // (run/dump); returns that frame number once written, or 0 after `timeout_ms`.
    uint64_t CaptureFrame(uint32_t timeout_ms);
    // Debug frame freeze: fills every colour target with magenta before a replayed frame, so draws
    // skipped in it leave holes. GPU thread only.
    void ClearTargetsForReplay();
    // Waits for all GPU work and destroys the Vulkan import of the guest range at `base` (the game is
    // unmapping it). GPU thread only.
    void ReleaseGuestMemory(uint64_t base);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    std::atomic<bool> shot_wanted_{false};
    std::atomic<uint64_t> shot_done_{0};
    std::atomic<int> capture_state_{0};  // 1 requested, 2 capturing this frame
    std::atomic<uint64_t> capture_done_{0};
};

Renderer& GetRenderer();

// Render resolution scale (targets are scale x the game's 1080p). A change takes effect at the next
// flip: every render target is recreated at the new size.
float CurrentRenderScale();
void SetRenderScale(float scale);
// FSR 2 resolution modes: the size the picture is shown at (the presenter sets it; 0 x 0 when FSR 2 can't
// be used, e.g. Zoom). The renderer upscales into an image of that size and presents it.
void SetUpscaleOutput(uint32_t width, uint32_t height);
void SetFsr2Debug(int bits);  // debug remote: jitter sign experiments
bool DlssTried();             // a DLSS mode has been used this run (dlss::Available() then says if it runs)

}  // namespace vkr
