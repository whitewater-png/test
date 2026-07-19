// onnx_upscaler.h - Adobe-independent super-resolution engine.
//
// Wraps an ONNX Runtime session running a single-image super-resolution
// model (e.g. Real-ESRGAN exported to ONNX). This class has no dependency
// on Adobe SDKs, PNG libraries, or anything platform-specific beyond the
// ONNX Runtime C++ API, so it can be unit-tested/CLI-tested on any
// platform (including this Linux/CPU-only environment) and then linked
// unchanged into the After Effects/Premiere Pro plugin once the Adobe SDK
// is available.
#pragma once

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "tile.h"

// Forward declarations to avoid forcing onnxruntime_cxx_api.h on every
// includer of this header.
namespace Ort {
struct Env;
struct Session;
struct MemoryInfo;
}

namespace upscale {

// Execution provider preference, tried in this order until one succeeds.
// Only kCPU is exercised in this repository's Linux CI/dev environment;
// the others compile only when the corresponding onnxruntime EP was built
// in and are skipped at runtime if unavailable.
enum class ExecutionProvider {
    kAuto = 0, // try CUDA -> DirectML -> CoreML -> CPU, in that order
    kCUDA,
    kDirectML,
    kCoreML,
    kCPU,
};

class OnnxUpscalerError : public std::runtime_error {
public:
    explicit OnnxUpscalerError(const std::string& msg) : std::runtime_error(msg) {}
};

class OnnxUpscaler {
public:
    OnnxUpscaler();
    ~OnnxUpscaler();

    OnnxUpscaler(const OnnxUpscaler&) = delete;
    OnnxUpscaler& operator=(const OnnxUpscaler&) = delete;

    // Loads the ONNX model at `model_path` and creates an inference
    // session using the requested execution provider (falling back to CPU
    // if the preferred provider is unavailable at runtime). Throws
    // OnnxUpscalerError on failure.
    void load(const std::string& model_path,
              ExecutionProvider provider_preference = ExecutionProvider::kAuto);

    // Returns a shared OnnxUpscaler (and thus a shared Ort::Session) for
    // `model_path`, loading and caching a new one if none is currently
    // live for that exact path. Process-wide cache keyed on model_path.
    //
    // This exists to fix a real-Premiere-hardware freeze: the AE/Premiere
    // plugin layer's per-sequence-data self-healing (see AIUpscale.cpp
    // ensure_sequence_data()) can end up creating more than one
    // AIUpscaleSequenceData for what is really the same render session
    // (e.g. once per render thread that ever saw a null sequence_data).
    // Each sequence data used to own its own OnnxUpscaler, which meant
    // its own ~64MB CoreML/ANE session plus working memory -- multiple
    // sessions all fighting over the ANE/GPU/CPU at once, on top of the
    // thread-count problem ConcurrencyGate (concurrency.h) addresses.
    // get_shared() instead hands every caller asking for the same
    // model_path a shared_ptr to the SAME OnnxUpscaler/Ort::Session;
    // Ort::Session::Run() is documented thread-safe, so multiple threads
    // running inference against the shared session concurrently (still
    // capped by ConcurrencyGate) is safe. The underlying instance is
    // reference-counted: it is destroyed (and its session freed) once the
    // last shared_ptr referencing it goes out of scope -- no explicit
    // process-exit cleanup is required.
    //
    // Throws OnnxUpscalerError if the (new) load fails; on failure nothing
    // is cached for `model_path`, so a later retry can succeed once the
    // underlying problem (e.g. a temporarily locked file) is resolved.
    static std::shared_ptr<OnnxUpscaler> get_shared(
        const std::string& model_path,
        ExecutionProvider provider_preference = ExecutionProvider::kAuto);

    // Test-only: number of distinct model_path entries currently backed by
    // a live (not-yet-destroyed) shared instance. Used to verify get_shared()
    // actually dedupes concurrent/repeated requests for the same path
    // instead of creating a new session each time.
    static size_t shared_cache_size_for_testing();

    // Runs a single forward pass on `in` (no tiling) and writes the result
    // to `out`. The output size is determined by the model's native scale
    // factor (see native_scale()), NOT by any externally requested scale.
    // Throws OnnxUpscalerError on failure (e.g. session not loaded, or a
    // shape/runtime error from onnxruntime).
    void infer(const ImageRGBA8& in, ImageRGBA8& out);

    // Runs tiled inference (see tile.h), downsizing each tile immediately
    // after inference to `requested_scale` when that's smaller than the
    // model's native scale (via TileOptions::output_scale -- see tile.h),
    // so a full-frame buffer at the model's native scale is never
    // materialized. This is the primary entry point most callers (CLI, AE
    // plugin) should use.
    //
    // `requested_scale` is the scale the caller wants:
    //   - <= 0: use the model's native scale as-is (no downsizing).
    //   - 1: same-resolution "detail regeneration" -- the model's
    //     native-scale detail is generated per-tile and immediately
    //     resampled back down to the tile's original size before
    //     compositing (this is what the AE/Premiere plugin uses now --
    //     see AIUpscale.cpp HandleRender -- since growing a layer's actual
    //     output buffer via PF_OutFlag_I_EXPAND_BUFFER proved unreliable
    //     on real Premiere Pro hosts).
    //   - between 1 and native scale: downsized per-tile the same way.
    //   - >= native scale: the native-scale result is used as-is; we do
    //     not attempt a second inference pass (chaining SR passes tends to
    //     amplify artifacts). Plugin-layer callers should pick a model
    //     whose native scale matches the requested scale where possible.
    // The per-tile downsize step uses resize_rgba_bilinear() (see tile.h)
    // -- NOT a high quality Lanczos filter, but adequate for stepping down
    // an already-upscaled tile.
    // If tile_opts.tile_size <= 0, a tile size is auto-selected via
    // choose_tile_size() based on the active execution provider and
    // input/output geometry (see tile.h). Otherwise tile_opts.tile_size is
    // used as given, EXCEPT: if the initial attempt fails specifically
    // with a memory-allocation-shaped failure (std::bad_alloc, or an
    // OnnxUpscalerError wrapping an onnxruntime OOM), upscale() halves the
    // tile size once and retries a single time before giving up -- see
    // the "自動リトライ" note in plugin/README.md's stable-operation
    // section.
    void upscale(const ImageRGBA8& in,
                 ImageRGBA8& out,
                 int requested_scale,
                 const TileOptions& tile_opts);

    // Native scale factor inferred from the model on first inference (a
    // 1x1 probe run). Returns 0 if the model hasn't been probed yet.
    int native_scale() const { return native_scale_; }

    bool is_loaded() const { return session_ != nullptr; }

    // Name of the execution provider actually in effect after load()
    // resolved fallbacks (e.g. "CPUExecutionProvider").
    const std::string& active_provider() const { return active_provider_name_; }

    // True when load() was asked for an accelerated provider (CoreML/
    // CUDA/DirectML) but it was unavailable/failed to initialize and
    // silently fell back to CPU. Callers (AE plugin layer, CLI) should
    // surface this to the user/log -- a silent CPU fallback on what the
    // user believes is a GPU/ANE-accelerated run is a common source of
    // "why is this so slow" confusion.
    bool fell_back_to_cpu() const { return fell_back_to_cpu_; }

private:
    void probe_native_scale();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Only used to avoid pulling onnxruntime headers into this header;
    // actual Ort::Session lives in Impl.
    void* session_ = nullptr;

    int native_scale_ = 0;
    std::string active_provider_name_;
    bool fell_back_to_cpu_ = false;

    // Guards the actual Ort::Session::Run() call when the active provider
    // is not CPU. Design rationale (see tile.h / tile.cpp for the
    // parallel tiling side of this):
    //   - Ort::Session::Run() on the SAME session IS documented as
    //     thread-safe by onnxruntime, so for the CPU execution provider we
    //     deliberately do NOT serialize here -- tiles run their Run() call
    //     concurrently across worker threads, sharing this one session.
    //     To avoid oversubscribing CPU cores (parallel tiles each also
    //     spinning up intra-op threads), load() sets intra_op_num_threads
    //     to 1 whenever tile-level parallelism is in play; see load().
    //   - For CoreML (and, if ever enabled, CUDA/DirectML) we still only
    //     ever attach one session, but the task's design explicitly calls
    //     for serializing the inference call itself on those providers
    //     (ANE/GPU contention, and to keep behavior conservative pending
    //     real hardware validation) while still parallelizing the
    //     surrounding pre/post-processing (NCHW packing, alpha resize,
    //     clamping) across tile worker threads. infer_mutex_ implements
    //     that serialization.
    std::mutex infer_mutex_;
    bool serialize_inference_ = false;

    // Guards the native_scale_ probe (see probe_native_scale()) so that
    // when a shared OnnxUpscaler (see get_shared() above) is called
    // concurrently by multiple threads before it has ever been probed,
    // only one thread actually runs the probe inference and writes
    // native_scale_ -- the rest wait and then observe the already-probed
    // value, instead of racing to write the same (non-atomic) int from
    // multiple threads.
    std::mutex probe_mutex_;

    // Throttles the "Auto-selected tile size" log line (see upscale()) to
    // only fire when the selected size actually changes, the same pattern
    // AIUpscale.cpp's log_render_diag_if_changed() uses. Field logs from a
    // real Premiere Pro render showed dozens of concurrent render threads
    // each logging this line every frame -- pure log-flood noise once the
    // steady-state tile size stops changing, and a (very) minor
    // contributor to the same-named freeze investigation.
    std::mutex tile_log_mutex_;
    int last_logged_tile_size_ = -1;
};

} // namespace upscale
