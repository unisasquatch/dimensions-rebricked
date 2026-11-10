// libSceGnmDriver: command buffer submission, flips, compute queues, GPU events, and the
// helper functions that write shader-setup PM4 packets into the game's command buffers.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu/command_processor.h"
#include "gpu/vk/renderer.h"
#include "host/guest_memory.h"
#include "gpu/pm4.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "host/compat.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "hle/videoout.h"

namespace {

std::mutex g_submit_mutex;

// --- the GPU thread ---
// The PS4's GPU runs submissions asynchronously: submit returns at once, and the game waits on labels,
// EOP events and flips. Recording thousands of draws on the game's own render thread made every frame
// wait for it, so submissions (and compute doorbells, to keep their order) go through a queue to one
// GPU thread. The game may run at most kMaxQueuedFlips frames ahead. LDPORT_GPU_THREAD=0: synchronous.
class GpuThread {
public:
    static constexpr int kMaxQueuedFlips = 2;
    static bool Enabled() {
        static const bool on = !(std::getenv("LDPORT_GPU_THREAD") && std::atoi(std::getenv("LDPORT_GPU_THREAD")) == 0);
        return on;
    }
    void Push(std::function<void()> job, bool flip) {
        std::unique_lock lock(mutex_);
        if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
        if (flip) {
            done_.wait(lock, [this] { return queued_flips_ < kMaxQueuedFlips; });
            ++queued_flips_;
        }
        jobs_.push_back({std::move(job), flip});
        ready_.notify_one();
    }

private:
    struct Job { std::function<void()> run; bool flip; };
    void Run() {
        SetThreadDescription(GetCurrentThread(), L"GPU");
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job.run();
            if (job.flip) {
                std::lock_guard lock(mutex_);
                --queued_flips_;
                done_.notify_all();
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_, done_;
    std::deque<Job> jobs_;
    int queued_flips_ = 0;
    std::thread thread_;
};
GpuThread g_gpu_thread;

void OnGpuAndWait(std::function<void()> job) {
    if (!GpuThread::Enabled()) {
        std::lock_guard lock(g_submit_mutex);
        job();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    g_gpu_thread.Push([&] {
        {
            std::lock_guard lock(g_submit_mutex);
            job();
        }
        std::lock_guard lock(m);
        done = true;
        cv.notify_all();
    }, false);
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return done; });
}

// Unmapping guest memory: finish GPU work that may read it, then drop its Vulkan import.
void ReleaseRangeOnGpu(uint64_t base) {
    OnGpuAndWait([base] { vkr::GetRenderer().ReleaseGuestMemory(base); });
}
const bool g_release_hook_set = [] {
    host::SetGuestRangeReleaseHook(&ReleaseRangeOnGpu);
    return true;
}();

// Blocks until `job` has run on the GPU thread after everything queued before it.
void OnGpuAndWait(std::function<void()> job);

// Runs `job` on the GPU thread, or here when it is off.
void OnGpu(std::function<void()> job, bool flip = false) {
    if (GpuThread::Enabled()) g_gpu_thread.Push(std::move(job), flip);
    else job();
}

// --- writing packets into guest command buffers ---
class Emitter {
public:
    Emitter(uint32_t* cmd, uint32_t capacity) : cmd_(cmd), cap_(capacity) {}

    void ShRegs(uint32_t reg, std::initializer_list<uint32_t> values, bool compute = false) {
        Packet(pm4::kSetShReg, reg, values, compute);
    }
    void ContextReg(uint32_t reg, uint32_t value) { Packet(pm4::kSetContextReg, reg, {value}, false); }
    void TaggedNop(uint32_t tag, uint32_t value) {
        if (!Fits(3)) return;
        cmd_[n_++] = pm4::Type3(pm4::kNop, 2);
        cmd_[n_++] = tag;
        cmd_[n_++] = value;
    }
    // Pads the rest of the reservation with one NOP so the game's fixed-size advance stays valid.
    uint32_t FillRest() {
        const uint32_t left = cap_ - n_;
        if (left == 1) cmd_[n_] = 0x80000000;  // type-2 filler
        else if (left >= 2) cmd_[n_] = pm4::Type3(pm4::kNop, left - 1);
        n_ = cap_;
        return n_;
    }

private:
    bool Fits(uint32_t dwords) {
        if (n_ + dwords <= cap_) return true;
        if (!overflowed_) std::fprintf(stderr, "[gnm] packet does not fit in %u dwords\n", cap_);
        overflowed_ = true;
        return false;
    }
    void Packet(uint32_t op, uint32_t reg, std::initializer_list<uint32_t> values, bool compute) {
        if (!Fits(2 + uint32_t(values.size()))) return;
        cmd_[n_++] = pm4::Type3(op, 1 + uint32_t(values.size()), compute);
        cmd_[n_++] = reg;
        for (uint32_t v : values) cmd_[n_++] = v;
    }

    uint32_t* cmd_;
    uint32_t cap_, n_ = 0;
    bool overflowed_ = false;
};

// Fetch-shader modifiers replace the VGPR/SGPR counts in PGM_RSRC1.
uint32_t ApplyModifier(uint32_t rsrc1, uint32_t modifier) {
    return modifier ? (rsrc1 & 0xFCFFFC00u) | modifier : rsrc1;
}

struct VsRegs { uint32_t lo, hi, rsrc1, rsrc2, vs_out_config, pos_format, cl_vs_out_cntl; };
struct PsRegs {
    uint32_t lo, hi, rsrc1, rsrc2, z_format, col_format, input_ena, input_addr, in_control, baryc_cntl,
        db_shader_control, cb_shader_mask;
};
struct CsRegs { uint32_t lo, hi, rsrc1, rsrc2, num_x, num_y, num_z; };
struct StageRegs { uint32_t lo, hi, rsrc1, rsrc2; };  // leading fields of the Es/Gs/Ls/Hs structs

SYSV int SetVsShader(uint32_t* cmd, uint32_t n, const VsRegs* r, uint32_t modifier) {
    Emitter e(cmd, n);
    e.ShRegs(pm4::kShPgmLoVs, {r->lo, r->hi, ApplyModifier(r->rsrc1, modifier), r->rsrc2});
    e.ContextReg(pm4::kSpiVsOutConfig, r->vs_out_config);
    e.ContextReg(pm4::kSpiShaderPosFormat, r->pos_format);
    e.ContextReg(pm4::kPaClVsOutCntl, r->cl_vs_out_cntl);
    e.FillRest();
    return 0;
}
SYSV int SetPsShader(uint32_t* cmd, uint32_t n, const PsRegs* r) {
    Emitter e(cmd, n);
    if (r) {
        e.ShRegs(pm4::kShPgmLoPs, {r->lo, r->hi, r->rsrc1, r->rsrc2});
        e.ContextReg(pm4::kSpiShaderZFormat, r->z_format);
        e.ContextReg(pm4::kSpiShaderColFormat, r->col_format);
        e.ContextReg(pm4::kSpiPsInputEna, r->input_ena);
        e.ContextReg(pm4::kSpiPsInputAddr, r->input_addr);
        e.ContextReg(pm4::kSpiPsInControl, r->in_control);
        e.ContextReg(pm4::kSpiBarycCntl, r->baryc_cntl);
        e.ContextReg(pm4::kDbShaderControl, r->db_shader_control);
        e.ContextReg(pm4::kCbShaderMask, r->cb_shader_mask);
    } else {
        e.ContextReg(pm4::kSpiShaderColFormat, 0);  // no pixel shader: depth-only
    }
    e.FillRest();
    return 0;
}
SYSV int SetCsShader(uint32_t* cmd, uint32_t n, const CsRegs* r, uint32_t modifier) {
    Emitter e(cmd, n);
    e.ShRegs(pm4::kComputePgmLo, {r->lo, r->hi}, true);
    e.ShRegs(pm4::kComputePgmRsrc1, {ApplyModifier(r->rsrc1, modifier), r->rsrc2}, true);
    e.ShRegs(pm4::kComputeNumThreadX, {r->num_x, r->num_y, r->num_z}, true);
    e.FillRest();
    return 0;
}
// Geometry and tessellation stages: program address and resources only for now.
// TODO: their extra context registers (GS ring/outputs, tessellation config) when a draw needs them.
template <uint32_t kBase>
SYSV int SetStageShader(uint32_t* cmd, uint32_t n, const StageRegs* r, uint32_t modifier) {
    Emitter e(cmd, n);
    e.ShRegs(kBase, {r->lo, r->hi, ApplyModifier(r->rsrc1, modifier), r->rsrc2});
    e.FillRest();
    return 0;
}
SYSV int SetEmbeddedVsShader(uint32_t* cmd, uint32_t n, uint32_t id, uint32_t) {
    Emitter e(cmd, n);
    e.TaggedNop(pm4::kTagEmbeddedVs, id);
    e.FillRest();
    return 0;
}
SYSV int SetEmbeddedPsShader(uint32_t* cmd, uint32_t n, uint32_t id) {
    Emitter e(cmd, n);
    e.TaggedNop(pm4::kTagEmbeddedPs, id);
    e.FillRest();
    return 0;
}
// Fills the reserved space with a NOP: markers, flip waits (flips complete synchronously here),
// GS ring setup.
SYSV int FillNop(uint32_t* cmd, uint32_t n) {
    Emitter(cmd, n).FillRest();
    return 0;
}
// Returns the dwords written, which the game adds to its command pointer.
SYSV uint32_t InitDefaultHardwareState(uint32_t* cmd, uint32_t n) {
    const uint32_t used = n < 0x100 ? n : 0x100;
    Emitter e(cmd, used);
    e.TaggedNop(pm4::kTagInitState, 0);
    return e.FillRest();
}

// --- submission ---
// LDPORT_LOG_SUBMITS=1: one line per submission (draws it carried, flip buffer, thread).
const bool g_log_submits = std::getenv("LDPORT_LOG_SUBMITS") != nullptr;
// The submitted buffers (the game's arrays may be on its stack, so they are copied).
using Buffers = std::vector<std::pair<const uint32_t*, uint32_t>>;
Buffers CopyBuffers(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes) {
    Buffers b;
    for (uint32_t i = 0; i < count; ++i)
        if (dcb[i]) b.push_back({static_cast<const uint32_t*>(dcb[i]), dcb_bytes[i] / 4});
    return b;
}
// Debug freeze (/debug/freeze): the game's submit-and-flip waits while the GPU thread re-runs the last
// frame's command buffers, so draw skips and captures all apply to one still frame.
std::vector<Buffers> g_frame_buffers, g_last_frame;  // GPU thread
int g_last_flip_buffer = -1;
std::atomic<bool> g_freeze{false};
void ReplayLastFrame() {
    vkr::GetRenderer().ClearTargetsForReplay();
    for (const auto& b : g_last_frame)
        for (const auto& [dwords, count] : b) gpu::Gpu().Execute(dwords, count, false);
    hle::VideoOutRepresent(g_last_flip_buffer);
    gpu::Gpu().EndFrame();
}

void ExecuteBuffers(const Buffers& buffers, int flip_buffer = -1) {
    std::lock_guard lock(g_submit_mutex);
    g_frame_buffers.push_back(buffers);
    const uint64_t before = gpu::Gpu().stats().draws;
    for (const auto& [dwords, count] : buffers) gpu::Gpu().Execute(dwords, count, false);
    if (g_log_submits)
        std::fprintf(stderr, "[submit] flips %llu: %zu dcbs, %llu draws%s%d thread %lu\n",
                     (unsigned long long)hle::VideoOutFlipCount(), buffers.size(),
                     (unsigned long long)(gpu::Gpu().stats().draws - before), flip_buffer >= 0 ? ", flip buffer " : ", no flip ",
                     flip_buffer, (unsigned long)GetCurrentThreadId());
}

SYSV int SubmitCommandBuffers(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes, void* const*,
                              const uint32_t*) {
    OnGpu([b = CopyBuffers(count, dcb, dcb_bytes)] { ExecuteBuffers(b); });
    return 0;
}
SYSV int SubmitAndFlipCommandBuffers(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes, void* const*,
                                     const uint32_t*, uint32_t /*video_handle*/, uint32_t buffer_index,
                                     uint32_t /*flip_mode*/, int64_t flip_arg) {
    while (g_freeze) OnGpuAndWait([] { ReplayLastFrame(); });
    OnGpu([b = CopyBuffers(count, dcb, dcb_bytes), buffer_index, flip_arg] {
        ExecuteBuffers(b, int(buffer_index));
        hle::VideoOutFlip(int(buffer_index), flip_arg);
        gpu::Gpu().EndFrame();
        // Keep the last frame that drew something: some scenes flip again without new draws.
        static uint64_t draws_at_flip = 0;
        const uint64_t draws = gpu::Gpu().stats().draws;
        if (draws != draws_at_flip) {
            g_last_frame = std::move(g_frame_buffers);
            g_last_flip_buffer = int(buffer_index);
        }
        draws_at_flip = draws;
        g_frame_buffers.clear();
        static uint64_t frames = 0;
        if (++frames % 300 == 1) {
            const auto& s = gpu::Gpu().stats();
            std::fprintf(stderr, "[gnm] frame %llu: %llu packets, %llu draws, %llu dispatches, %llu labels\n",
                         (unsigned long long)frames, (unsigned long long)s.packets, (unsigned long long)s.draws,
                         (unsigned long long)s.dispatches, (unsigned long long)s.labels);
        }
    }, true);
    return 0;
}

// --- compute queues: a ring the game writes packets into, then rings the doorbell ---
struct ComputeQueue {
    uint32_t* ring;
    uint32_t size_dw;
    uint32_t* read_ptr;
    uint32_t offset_dw = 0;
};
std::mutex g_queue_mutex;
std::map<uint32_t, ComputeQueue> g_queues;
uint32_t g_next_queue = 1;

SYSV int MapComputeQueue(uint32_t, uint32_t, uint32_t* ring, uint32_t size_dw, uint32_t* read_ptr) {
    std::lock_guard lock(g_queue_mutex);
    g_queues[g_next_queue] = {ring, size_dw, read_ptr};
    return int(g_next_queue++);
}
SYSV int MapComputeQueueWithPriority(uint32_t pipe, uint32_t queue, uint32_t* ring, uint32_t size_dw,
                                     uint32_t* read_ptr, uint32_t) {
    return MapComputeQueue(pipe, queue, ring, size_dw, read_ptr);
}
SYSV void UnmapComputeQueue(uint32_t id) {
    std::lock_guard lock(g_queue_mutex);
    g_queues.erase(id);
}
SYSV void DingDong(uint32_t id, uint32_t next_offset_dw) {
    ComputeQueue q;
    {
        std::lock_guard lock(g_queue_mutex);
        auto it = g_queues.find(id);
        if (it == g_queues.end()) return;
        q = it->second;
        it->second.offset_dw = next_offset_dw;
    }
    OnGpu([q, next_offset_dw] {
        std::lock_guard lock(g_submit_mutex);
        if (next_offset_dw >= q.offset_dw) {
            gpu::Gpu().Execute(q.ring + q.offset_dw, next_offset_dw - q.offset_dw, true);
        } else {  // wrapped
            gpu::Gpu().Execute(q.ring + q.offset_dw, q.size_dw - q.offset_dw, true);
            gpu::Gpu().Execute(q.ring, next_offset_dw, true);
        }
        if (q.read_ptr) *q.read_ptr = next_offset_dw;  // the game reuses the ring up to here
    });
}

// --- GPU events ---
SYSV int AddEqEvent(hle::EventQueue* q, uint32_t id, void* udata) {
    q->Add(id, hle::kFilterGraphicsCore, udata);
    return 0;
}
SYSV int DeleteEqEvent(hle::EventQueue* q, uint32_t id) {
    q->Remove(id, hle::kFilterGraphicsCore);
    return 0;
}
SYSV uint32_t GetEqEventType(const hle::KernelEvent* ev) { return uint32_t(ev->ident); }
SYSV uint64_t GetEqTimeStamp(const hle::KernelEvent* ev) { return uint64_t(ev->data); }

// --- misc ---
SYSV void* TessFactorRingBase() {
    static void* ring = _aligned_malloc(0x20000, 0x10000);
    return ring;
}
SYSV uint32_t Identity(uint32_t v) { return v; }
SYSV int Zero() { return 0; }
SYSV int One() { return 1; }

}  // namespace

#define GNM(name, fn) HLE_EXPORT("libSceGnmDriver", name, reinterpret_cast<const void*>(&fn))
GNM("sceGnmSetVsShader", SetVsShader);
GNM("sceGnmUpdateVsShader", SetVsShader);
GNM("sceGnmSetPsShader", SetPsShader);
GNM("sceGnmUpdatePsShader", SetPsShader);
GNM("sceGnmSetCsShader", SetCsShader);
GNM("sceGnmSetEsShader", SetStageShader<pm4::kShPgmLoEs>);
GNM("sceGnmSetGsShader", SetStageShader<pm4::kShPgmLoGs>);
GNM("sceGnmUpdateGsShader", SetStageShader<pm4::kShPgmLoGs>);
GNM("sceGnmSetHsShader", SetStageShader<pm4::kShPgmLoHs>);
GNM("sceGnmUpdateHsShader", SetStageShader<pm4::kShPgmLoHs>);
GNM("sceGnmSetLsShader", SetStageShader<pm4::kShPgmLoLs>);
GNM("sceGnmSetEmbeddedVsShader", SetEmbeddedVsShader);
GNM("sceGnmSetEmbeddedPsShader", SetEmbeddedPsShader);
GNM("sceGnmSetGsRingSizes", FillNop);
GNM("sceGnmInsertWaitFlipDone", FillNop);
GNM("sceGnmInsertPushMarker", FillNop);
GNM("sceGnmInsertPushColorMarker", FillNop);
GNM("sceGnmInsertPopMarker", FillNop);
GNM("sceGnmInsertSetMarker", FillNop);
GNM("sceGnmInsertSetColorMarker", FillNop);
GNM("sceGnmDrawInitDefaultHardwareState200", InitDefaultHardwareState);
GNM("sceGnmDispatchInitDefaultHardwareState", InitDefaultHardwareState);
GNM("sceGnmSubmitCommandBuffers", SubmitCommandBuffers);
GNM("sceGnmSubmitAndFlipCommandBuffers", SubmitAndFlipCommandBuffers);
GNM("sceGnmSubmitDone", Zero);
GNM("sceGnmAreSubmitsAllowed", One);
GNM("sceGnmFlushGarlic", Zero);
GNM("sceGnmMapComputeQueue", MapComputeQueue);
GNM("sceGnmMapComputeQueueWithPriority", MapComputeQueueWithPriority);
GNM("sceGnmUnmapComputeQueue", UnmapComputeQueue);
GNM("sceGnmDingDong", DingDong);
GNM("sceGnmAddEqEvent", AddEqEvent);
GNM("sceGnmDeleteEqEvent", DeleteEqEvent);
GNM("sceGnmGetEqEventType", GetEqEventType);
GNM("sceGnmGetEqTimeStamp", GetEqTimeStamp);
GNM("sceGnmGetTheTessellationFactorRingBufferBaseAddress", TessFactorRingBase);
GNM("sceGnmLogicalCuIndexToPhysicalCuIndex", Identity);
GNM("sceGnmLogicalCuMaskToPhysicalCuMask", Identity);
GNM("sceGnmIsUserPaEnabled", Zero);
GNM("sceGnmDebugHardwareStatus", Zero);
GNM("sceGnmDriverCaptureInProgress", Zero);
GNM("sceGnmDriverTraceInProgress", Zero);
GNM("sceGnmDriverTriggerCapture", Zero);
GNM("sceGnmValidateCommandBuffers", Zero);
GNM("sceGnmValidateGetVersion", Zero);
GNM("sceGnmValidateOnSubmitEnabled", Zero);
GNM("sceGnmValidateResetState", Zero);
GNM("sceGnmValidationRegisterMemoryCheckCallback", Zero);
GNM("sceGnmSetupMipStatsReport", Zero);
GNM("sceGnmDisableMipStatsReport", Zero);
GNM("sceGnmRequestMipStatsReportAndReset", Zero);
GNM("sceRazorIsLoaded", Zero);
GNM("sceRazorCaptureImmediate", Zero);
GNM("sceRazorCaptureSinceLastFlip", Zero);
GNM("sceRazorCaptureCommandBuffersOnlyImmediate", Zero);
GNM("sceRazorCaptureCommandBuffersOnlySinceLastFlip", Zero);
// Signature unconfirmed; logs its first call so we notice if the game relies on it.
HLE_CONSTANT("libSceGnmDriver", "sceGnmRequestFlipAndSubmitDone", 0);

namespace hle {
void SetFrameFreeze(bool on) { g_freeze = on; }
}  // namespace hle
