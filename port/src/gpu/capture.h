#pragma once
#include <cstdint>
#include <string>

#include "gpu/registers.h"

namespace gpu {

struct DrawInfo {
    bool indexed;
    uint32_t index_count;
    uint64_t index_address;  // 0 for auto-index draws
    uint32_t index_type;     // 0 = 16-bit, 1 = 32-bit
    uint32_t instances;
};

// Frame capture for scoping the renderer: with LDPORT_CAPTURE=<frame>, every draw and dispatch
// in that frame is written to run/capture/frame_<n>.txt, and each distinct shader's code to
// run/capture/shaders/<address>.bin.
class Capture {
public:
    static Capture& Get();
    bool active() const { return active_; }
    void OnFrameStart(uint64_t frame);
    // Capture the next frame that starts (used by the flat-frame trap).
    void ArmNextFrame() { if (!active_) target_frame_ = -2; }
    // Rolling mode (LDPORT_FLAT_TRAP): every frame is written to run/capture/rolling.txt, replaced
    // at each frame start; SaveRolling keeps a copy of the frame in progress.
    void SaveRolling(const char* name);
    void Note(const char* line);
    void OnDraw(const Registers& r, const DrawInfo& d);
    void OnDispatch(const Registers& r, uint32_t x, uint32_t y, uint32_t z);

private:
    Capture();
    void DumpShader(uint64_t address, const char* stage);

    int64_t target_frame_ = -1;
    bool active_ = false;
    bool rolling_ = false;
    std::string save_next_;  // after a trap: also keep the following frame
    void* file_ = nullptr;
    uint32_t draws_ = 0;
};

}  // namespace gpu
