// concurrency.h - Process-wide inference concurrency gate.
//
// Real-Premiere-hardware freeze (4K render on a MacBook Pro M4 Max, 36GB):
// field logs showed dozens of render threads (Premiere parallelizes 4K
// render across its own thread pool, one thread per in-flight frame)
// simultaneously logging "Auto-selected tile size 512px" -- i.e. each of
// those ~14 host render threads was ALSO spinning up tile.cpp's own
// internal worker pool (auto = hardware_concurrency), producing on the
// order of host_threads * tile_workers (~14 x 14 = ~200) threads all
// fighting over the same CPU/ANE at once, saturating every core and
// freezing the machine.
//
// This gate is the "traffic control" fix for that: it caps how many
// upscale() calls (across ALL OnnxUpscaler instances / all threads in this
// process) are allowed to actually be doing inference work at the same
// time, regardless of how many host render threads call in concurrently.
// Combined with forcing TileOptions::num_workers=1 at the plugin layer
// (see AIUpscale.cpp HandleRender -- the host already parallelizes across
// frames, so the plugin must not ALSO parallelize within a frame) and
// sharing one OnnxUpscaler/Ort::Session per model across sequence data
// instances (see OnnxUpscaler::get_shared() in onnx_upscaler.h), the total
// number of concurrently-active inference threads is bounded by
// max_concurrency() instead of growing with however many render threads
// the host happens to spin up.
//
// Default is 2 (a conservative "leave most cores for the rest of Premiere
// and the OS" choice for the M4 Max target machine), overridable via the
// AIUPSCALE_MAX_CONCURRENCY environment variable (clamped to
// [1, hardware_concurrency]) -- see plugin/README.md "安定運用ガイド" for
// the user-facing guidance (set it to 1 on a machine that still struggles).
#pragma once

#include <condition_variable>
#include <mutex>

namespace upscale {

class ConcurrencyGate {
public:
    // Process-wide singleton. The configured limit is resolved once, on
    // first use (from AIUPSCALE_MAX_CONCURRENCY or the hardware-derived
    // default -- see concurrency.cpp), and logged at that point.
    static ConcurrencyGate& instance();

    // RAII guard: blocks in the constructor until a slot is available
    // (i.e. fewer than max_concurrency() callers are currently inside a
    // guarded section), and always releases the slot in the destructor,
    // including when the guarded section throws.
    //
    // IMPORTANT (deadlock avoidance): never acquire a second Guard on the
    // same gate from a thread that is already holding one, and never call
    // anything that itself acquires this gate from inside a
    // gate-guarded section (e.g. tile worker threads spawned from inside
    // OnnxUpscaler::upscale() must NOT themselves try to acquire this
    // gate -- only the single outer upscale() call does). Both invariants
    // hold today: OnnxUpscaler::upscale() acquires exactly one Guard for
    // its entire duration, and neither tile.cpp's worker threads nor
    // OnnxUpscaler::infer() ever touch this gate themselves.
    class Guard {
    public:
        explicit Guard(ConcurrencyGate& gate);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        ConcurrencyGate& gate_;
    };

    int max_concurrency() const { return max_concurrency_; }

    // Test-only introspection: current number of callers inside a guarded
    // section right now, and the highest that count has ever reached
    // since the last reset_peak_for_testing() call (or process start).
    // Used by tests to verify the gate actually caps concurrency at
    // max_concurrency() rather than merely serializing without a real
    // limit (or, conversely, not limiting at all).
    int current_count_for_testing();
    int peak_count_for_testing();
    void reset_peak_for_testing();

private:
    ConcurrencyGate();
    void acquire();
    void release();

    const int max_concurrency_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int in_use_ = 0;
    int peak_ = 0;
};

} // namespace upscale
