#include "onnx_upscaler.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <new>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "size_limits.h"
#include "logger.h"
#include "concurrency.h"

namespace upscale {

// ---------------------------------------------------------------------------
// Impl: holds the actual onnxruntime objects so the public header stays
// free of onnxruntime includes.
// ---------------------------------------------------------------------------
struct OnnxUpscaler::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "upscale_core"};
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::string input_name;
    std::string output_name;
};

OnnxUpscaler::OnnxUpscaler() : impl_(std::make_unique<Impl>()) {}
OnnxUpscaler::~OnnxUpscaler() = default;

namespace {

// Process-wide shared-instance cache backing get_shared() (see
// onnx_upscaler.h for the full rationale). Keyed on model_path; holds only
// weak_ptrs so a model that's no longer referenced by anyone (e.g. after a
// mode switch away from it, once the last sequence data drops its
// shared_ptr) is naturally freed rather than kept alive forever by the
// cache itself. Guarded by a single mutex -- held across the (one-time,
// per model_path) load() call too, which serializes concurrent first-time
// loads of the SAME OR DIFFERENT model_path against each other; this is a
// deliberate simplicity tradeoff since get_shared() is only called from
// mode-switch-time paths (ensure_model_loaded() in AIUpscale.cpp), not
// per-frame, so any contention here is rare and brief relative to a
// render's overall runtime.
std::mutex& shared_cache_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::weak_ptr<OnnxUpscaler>>& shared_cache_map() {
    static std::unordered_map<std::string, std::weak_ptr<OnnxUpscaler>> m;
    return m;
}

} // namespace

std::shared_ptr<OnnxUpscaler> OnnxUpscaler::get_shared(const std::string& model_path,
                                                        ExecutionProvider provider_preference) {
    std::lock_guard<std::mutex> lock(shared_cache_mutex());
    auto& cache = shared_cache_map();

    auto it = cache.find(model_path);
    if (it != cache.end()) {
        if (std::shared_ptr<OnnxUpscaler> existing = it->second.lock()) {
            return existing; // another caller is already keeping this model's session alive
        }
        // Stale entry (last shared_ptr was already dropped) -- fall through
        // and replace it with a freshly loaded instance below.
    }

    auto instance = std::make_shared<OnnxUpscaler>();
    instance->load(model_path, provider_preference); // throws OnnxUpscalerError on failure; nothing cached in that case
    cache[model_path] = instance;
    log_info("OnnxUpscaler::get_shared: created new shared session for '" + model_path +
              "' (live cache entries=" + std::to_string(cache.size()) + ")");
    return instance;
}

size_t OnnxUpscaler::shared_cache_size_for_testing() {
    std::lock_guard<std::mutex> lock(shared_cache_mutex());
    size_t live = 0;
    for (const auto& [path, weak_instance] : shared_cache_map()) {
        if (!weak_instance.expired()) ++live;
    }
    return live;
}

namespace {

// Appends execution providers to `options` in priority order, skipping any
// that are not compiled in (guarded by preprocessor defines set by
// CMakeLists.txt based on which onnxruntime EP libraries were found) or
// not available at runtime (guarded by try/catch, since onnxruntime
// throws Ort::Exception when a provider's runtime deps, e.g. the CUDA
// driver, are missing).
//
// Returns the name of the provider actually appended first (best-effort;
// onnxruntime does not expose a clean "which EP got used" API upfront, so
// active_provider() is really "which provider we attempted first without
// throwing" -- good enough for logging/diagnostics purposes).
std::string append_providers(Ort::SessionOptions& options, ExecutionProvider pref) {
    std::string chosen = "CPUExecutionProvider";

    auto try_cuda = [&]() -> bool {
#ifdef UPSCALE_WITH_CUDA
        try {
            OrtCUDAProviderOptions cuda_opts{};
            options.AppendExecutionProvider_CUDA(cuda_opts);
            chosen = "CUDAExecutionProvider";
            return true;
        } catch (const Ort::Exception&) {
            return false;
        }
#else
        return false;
#endif
    };

    auto try_dml = [&]() -> bool {
#ifdef UPSCALE_WITH_DIRECTML
        try {
            // Requires the DirectML EP provider header; only compiled on
            // Windows builds that located the DirectML EP.
            options.AppendExecutionProvider("DML", {});
            chosen = "DmlExecutionProvider";
            return true;
        } catch (const Ort::Exception&) {
            return false;
        }
#else
        return false;
#endif
    };

    auto try_coreml = [&]() -> bool {
#if defined(UPSCALE_WITH_COREML) && defined(__APPLE__)
        try {
            // CoreML EP options (onnxruntime >= 1.17 generic provider-options
            // map interface). Tuned for the M4 Max target machine:
            //   - ModelFormat=MLProgram: required for full ANE (Apple Neural
            //     Engine) eligibility on recent Apple Silicon; the older
            //     NeuralNetwork format is CPU/GPU-only for many ops.
            //   - MLComputeUnits=ALL: lets CoreML schedule ops across the
            //     ANE, GPU, and CPU as it sees fit -- generally the best
            //     throughput on Apple Silicon, since CoreML's own scheduler
            //     knows the per-op tradeoffs better than a fixed choice.
            //   - RequireStaticInputShapes=0: our tiles vary in size at the
            //     image border (last row/column of tiles), so dynamic
            //     input shapes must remain supported.
            std::unordered_map<std::string, std::string> coreml_opts = {
                {"ModelFormat", "MLProgram"},
                {"MLComputeUnits", "ALL"},
                {"RequireStaticInputShapes", "0"},
                {"EnableOnSubgraphs", "0"},
            };
            options.AppendExecutionProvider("CoreML", coreml_opts);
            chosen = "CoreMLExecutionProvider";
            return true;
        } catch (const Ort::Exception& ex) {
            log_warn(std::string("CoreML execution provider unavailable, will fall back: ") + ex.what());
            return false;
        }
#else
        return false;
#endif
    };

    switch (pref) {
        case ExecutionProvider::kCUDA:
            try_cuda();
            break;
        case ExecutionProvider::kDirectML:
            try_dml();
            break;
        case ExecutionProvider::kCoreML:
            try_coreml();
            break;
        case ExecutionProvider::kCPU:
            // CPU is always implicitly available; nothing to append.
            break;
        case ExecutionProvider::kAuto:
        default:
            // CUDA -> DirectML -> CoreML -> CPU, first success wins.
            if (try_cuda()) break;
            if (try_dml()) break;
            if (try_coreml()) break;
            break;
    }

    // CPU EP is always registered implicitly by onnxruntime as the final
    // fallback, so we don't need to append it explicitly.
    return chosen;
}

} // namespace

void OnnxUpscaler::load(const std::string& model_path, ExecutionProvider provider_preference) {
    // Fail fast with a clear message rather than letting onnxruntime throw
    // an opaque error deep inside session construction -- this also keeps
    // the "model not found" case distinguishable from "model is corrupt"
    // in logs.
    {
        std::ifstream probe(model_path, std::ios::binary);
        if (!probe.good()) {
            const std::string msg = "Model file not found or unreadable: '" + model_path + "'";
            log_error(msg);
            throw OnnxUpscalerError(msg);
        }
    }

    try {
        Ort::SessionOptions options;
        // Intra-op thread count: derived from ConcurrencyGate's configured
        // max_concurrency() rather than hardcoded, so onnxruntime's own
        // per-Run() thread pool cannot oversubscribe cores no matter how
        // many callers are allowed through the gate at once.
        //
        // Background (this is the fix for a real-Premiere-hardware freeze):
        // Premiere parallelizes 4K rendering across many host threads,
        // each of which used to ALSO spin up tile.cpp's own worker pool
        // (auto = hardware_concurrency) -- host_threads * tile_workers
        // could reach ~200 threads all fighting for the same cores/ANE at
        // once, freezing the machine. The plugin layer now forces
        // TileOptions::num_workers=1 (see AIUpscale.cpp HandleRender -- the
        // host already parallelizes across frames, so tile.cpp must not
        // ALSO parallelize within a frame), and ConcurrencyGate
        // (concurrency.h) caps how many upscale() calls run inference
        // concurrently process-wide (default 2, AIUPSCALE_MAX_CONCURRENCY
        // to override). With those two pieces in place, up to
        // max_concurrency() Run() calls can be genuinely concurrent, so
        // letting each one use more than a single intra-op thread no
        // longer risks oversubscription the way a fixed "always 1" or a
        // naive "always hardware_concurrency()" would -- as long as the
        // per-call thread count times max_concurrency() stays within
        // hardware_concurrency(). hardware_concurrency() / max_concurrency()
        // (floored to at least 1) achieves exactly that bound.
        const unsigned hw_threads = std::max(1u, std::thread::hardware_concurrency());
        const int max_conc = std::max(1, ConcurrencyGate::instance().max_concurrency());
        const int intra_op_threads = std::max(1, static_cast<int>(hw_threads) / max_conc);
        options.SetIntraOpNumThreads(intra_op_threads);
        log_info("OnnxUpscaler::load: intra_op_num_threads=" + std::to_string(intra_op_threads) +
                  " (hardware_concurrency=" + std::to_string(hw_threads) +
                  ", concurrency_gate.max_concurrency=" + std::to_string(max_conc) + ")");
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Only an EXPLICIT accelerated-provider request (kCoreML/kCUDA/
        // kDirectML) that ends up on CPU counts as a "fallback" worth
        // warning about. kAuto trying accelerated providers and settling
        // on CPU (e.g. this repo's Linux CPU-only dev environment) is
        // expected, ordinary behavior, not a failure -- warning about it
        // every run would be noise.
        const bool explicitly_wanted_acceleration =
            provider_preference == ExecutionProvider::kCoreML ||
            provider_preference == ExecutionProvider::kCUDA ||
            provider_preference == ExecutionProvider::kDirectML;
        active_provider_name_ = append_providers(options, provider_preference);
        serialize_inference_ = (active_provider_name_ != "CPUExecutionProvider");
        fell_back_to_cpu_ = explicitly_wanted_acceleration && (active_provider_name_ == "CPUExecutionProvider");

        if (fell_back_to_cpu_) {
            log_warn("Requested an accelerated execution provider but none initialized successfully; "
                     "falling back to CPUExecutionProvider. Performance will be significantly lower.");
        }
        log_info("Loading model '" + model_path + "' with execution provider: " + active_provider_name_);

#ifdef _WIN32
        std::wstring wpath(model_path.begin(), model_path.end());
        impl_->session = std::make_unique<Ort::Session>(impl_->env, wpath.c_str(), options);
#else
        impl_->session = std::make_unique<Ort::Session>(impl_->env, model_path.c_str(), options);
#endif

        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name_alloc = impl_->session->GetInputNameAllocated(0, allocator);
        auto output_name_alloc = impl_->session->GetOutputNameAllocated(0, allocator);
        impl_->input_name = input_name_alloc.get();
        impl_->output_name = output_name_alloc.get();

        session_ = impl_->session.get();
        native_scale_ = 0; // re-probe on first inference of the new model
    } catch (const Ort::Exception& ex) {
        const std::string msg = std::string("Failed to load ONNX model '") + model_path + "': " + ex.what();
        log_error(msg);

        // If the failure happened while trying an accelerated provider,
        // attempt one automatic CPU fallback load rather than failing the
        // whole operation outright (per the "モデルロード失敗・CoreML初期化
        // 失敗時はCPUへフォールバック" requirement).
        if (provider_preference != ExecutionProvider::kCPU) {
            log_warn("Retrying model load on CPUExecutionProvider after accelerated-provider load failure.");
            try {
                load(model_path, ExecutionProvider::kCPU);
                fell_back_to_cpu_ = true;
                return;
            } catch (const OnnxUpscalerError& retry_ex) {
                log_error(std::string("CPU fallback load also failed: ") + retry_ex.what());
                throw;
            }
        }
        throw OnnxUpscalerError(msg);
    }
}

namespace {

// Encodes an RGBA8 tile's RGB channels into an NCHW float32 tensor in
// [0, 1]. Alpha is dropped here (handled separately via bilinear resize,
// see below) since most super-resolution models are trained on RGB-only
// data.
std::vector<float> to_nchw_float(const ImageRGBA8& img) {
    const int w = img.width, h = img.height;
    std::vector<float> out(static_cast<size_t>(3) * w * h);
    const size_t plane = static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t src = (static_cast<size_t>(y) * w + x) * 4;
            const size_t idx = static_cast<size_t>(y) * w + x;
            out[0 * plane + idx] = img.pixels[src + 0] / 255.0f; // R
            out[1 * plane + idx] = img.pixels[src + 1] / 255.0f; // G
            out[2 * plane + idx] = img.pixels[src + 2] / 255.0f; // B
        }
    }
    return out;
}

// Decodes an NCHW float32 [0,1] RGB tensor of shape [1,3,H,W] into an
// ImageRGBA8, using the given alpha-upscaled channel for the A byte.
void from_nchw_float(const float* data, int w, int h, const std::vector<uint8_t>& alpha, ImageRGBA8& out) {
    out.resize(w, h);
    const size_t plane = static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * w + x;
            const size_t dst = idx * 4;
            auto clamp01to255 = [](float v) -> uint8_t {
                v = std::clamp(v, 0.0f, 1.0f);
                return static_cast<uint8_t>(std::lround(v * 255.0f));
            };
            out.pixels[dst + 0] = clamp01to255(data[0 * plane + idx]);
            out.pixels[dst + 1] = clamp01to255(data[1 * plane + idx]);
            out.pixels[dst + 2] = clamp01to255(data[2 * plane + idx]);
            out.pixels[dst + 3] = alpha.empty() ? 255 : alpha[idx];
        }
    }
}

// Simple bilinear resize of the alpha channel (nearest-adjacent-sample
// bilinear, not a high quality filter -- alpha is typically flat/simple in
// video frames so this is a deliberate simplicity/perf tradeoff).
std::vector<uint8_t> resize_alpha_bilinear(const ImageRGBA8& img, int out_w, int out_h) {
    std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h);
    if (img.width <= 0 || img.height <= 0) {
        std::fill(out.begin(), out.end(), 255);
        return out;
    }
    const float sx = static_cast<float>(img.width) / out_w;
    const float sy = static_cast<float>(img.height) / out_h;
    for (int y = 0; y < out_h; ++y) {
        float fy = (y + 0.5f) * sy - 0.5f;
        fy = std::clamp(fy, 0.0f, static_cast<float>(img.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, img.height - 1);
        const float wy = fy - y0;
        for (int x = 0; x < out_w; ++x) {
            float fx = (x + 0.5f) * sx - 0.5f;
            fx = std::clamp(fx, 0.0f, static_cast<float>(img.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, img.width - 1);
            const float wx = fx - x0;

            auto a = [&](int xx, int yy) {
                return static_cast<float>(img.pixels[(static_cast<size_t>(yy) * img.width + xx) * 4 + 3]);
            };
            const float top = a(x0, y0) * (1 - wx) + a(x1, y0) * wx;
            const float bot = a(x0, y1) * (1 - wx) + a(x1, y1) * wx;
            const float v = top * (1 - wy) + bot * wy;
            out[static_cast<size_t>(y) * out_w + x] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
        }
    }
    return out;
}

// NOTE: this file previously had a box_downsample() helper here that
// stepped a full native-scale frame down to a smaller requested scale
// after the fact. It's gone -- OnnxUpscaler::upscale() below now passes
// the requested scale through to upscale_tiled() as TileOptions::
// output_scale, which downsizes each tile immediately after inference
// instead of ever materializing a full-frame native-scale buffer (see
// tile.h/tile.cpp). This was required to fix a real-Premiere-hardware
// render failure where the host handed HandleRender an input world far
// larger than the nominal source resolution, making even a "just downsize
// afterward" whole-frame native buffer too large to allocate.

} // namespace

void OnnxUpscaler::infer(const ImageRGBA8& in, ImageRGBA8& out) {
    if (!is_loaded()) {
        throw OnnxUpscalerError("infer() called before load()");
    }
    // NULL/zero/negative-size + oversized-input rejection (see limits.h).
    // Tiles are already bounded by TileOptions::tile_size, but infer() can
    // also be called directly (e.g. probe_native_scale()), so validate
    // here too rather than trusting callers.
    validate_input_dims(in.width, in.height, 4);

    try {
        // Pre-processing (NCHW packing) is NOT covered by infer_mutex_ --
        // it only touches this call's own local buffers, so it runs fully
        // concurrently across tile worker threads regardless of execution
        // provider.
        std::vector<float> input_data = to_nchw_float(in);
        std::array<int64_t, 4> input_shape{1, 3, in.height, in.width};

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            impl_->mem_info, input_data.data(), input_data.size(),
            input_shape.data(), input_shape.size());

        const char* input_names[] = {impl_->input_name.c_str()};
        const char* output_names[] = {impl_->output_name.c_str()};

        std::vector<Ort::Value> output_tensors;
        {
            // The actual inference call. On the CPU execution provider we
            // deliberately do NOT lock here: onnxruntime documents
            // Ort::Session::Run() as safe to call concurrently on the same
            // session, and tile.cpp relies on that for CPU-path tile
            // parallelism. On non-CPU providers (CoreML, etc.) we
            // serialize this call per the task's design -- pre/post
            // processing above/below stays parallel, only the Run() call
            // itself is exclusive.
            std::unique_lock<std::mutex> lock(infer_mutex_, std::defer_lock);
            if (serialize_inference_) lock.lock();
            output_tensors = impl_->session->Run(
                Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);
        }

        Ort::Value& output_tensor = output_tensors.front();
        auto out_info = output_tensor.GetTensorTypeAndShapeInfo();
        auto out_shape = out_info.GetShape();
        if (out_shape.size() != 4 || out_shape[0] != 1 || out_shape[1] != 3) {
            throw OnnxUpscalerError("Unexpected model output shape (expected [1,3,H,W], got batch=" +
                                     std::to_string(out_shape.empty() ? -1 : out_shape[0]) + ")");
        }
        const int out_h = static_cast<int>(out_shape[2]);
        const int out_w = static_cast<int>(out_shape[3]);
        // Validate the model-reported output size too -- a malicious or
        // corrupt model could claim an enormous output shape.
        validate_input_dims(out_w, out_h, 4);

        std::vector<uint8_t> alpha = resize_alpha_bilinear(in, out_w, out_h);
        from_nchw_float(output_tensor.GetTensorData<float>(), out_w, out_h, alpha, out);
    } catch (const Ort::Exception& ex) {
        const std::string msg = std::string("ONNX inference failed: ") + ex.what();
        log_error(msg);
        throw OnnxUpscalerError(msg);
    } catch (const std::bad_alloc&) {
        const std::string msg = "ONNX inference failed: out of memory";
        log_error(msg);
        throw OnnxUpscalerError(msg);
    }
}

void OnnxUpscaler::probe_native_scale() {
    // Run a tiny 8x8 probe through the model and measure the output size
    // ratio. This works for any model whose scale factor is fixed and
    // independent of input size (true of Real-ESRGAN and similar
    // architectures), avoiding a dependency on ONNX metadata that models
    // may or may not populate consistently.
    ImageRGBA8 probe_in;
    probe_in.resize(8, 8);
    std::fill(probe_in.pixels.begin(), probe_in.pixels.end(), 128);

    ImageRGBA8 probe_out;
    infer(probe_in, probe_out);

    if (probe_out.width % probe_in.width == 0 && probe_out.height % probe_in.height == 0 &&
        probe_out.width / probe_in.width == probe_out.height / probe_in.height) {
        native_scale_ = probe_out.width / probe_in.width;
    } else {
        // Non-integer or non-uniform scale: fall back to rounding the
        // width ratio, which is still useful for downstream downsampling
        // decisions even if imperfect.
        native_scale_ = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(probe_out.width) / probe_in.width)));
    }
}

void OnnxUpscaler::upscale(const ImageRGBA8& in, ImageRGBA8& out, int requested_scale, const TileOptions& tile_opts) {
    if (!is_loaded()) {
        throw OnnxUpscalerError("upscale() called before load()");
    }
    // Reject NULL/empty/oversized input before any allocation (see
    // limits.h; also covers the "0x0 input" abnormal-input test case).
    validate_input_dims(in.width, in.height, 4);

    // ConcurrencyGate: caps how many upscale() calls (across ALL
    // OnnxUpscaler instances/threads in this process) are actually doing
    // inference work at once -- see concurrency.h for the full real-
    // Premiere-hardware freeze this fixes. Acquired for the ENTIRE rest of
    // this call (probing + tiling + inference), released via RAII on every
    // return path including exceptions. Safe against deadlock: neither
    // tile.cpp's worker threads nor infer() ever try to acquire this gate
    // themselves, so there is no re-entrant acquire from within this
    // guarded section.
    ConcurrencyGate::Guard concurrency_guard(ConcurrencyGate::instance());

    if (native_scale_ == 0) {
        // Double-checked locking: probe_mutex_ only serializes the (rare,
        // one-time-per-model) probe itself, not every upscale() call --
        // see probe_mutex_'s doc comment in onnx_upscaler.h for why this
        // matters now that a shared OnnxUpscaler (get_shared()) can be
        // entered concurrently by multiple threads before it's ever been
        // probed.
        std::lock_guard<std::mutex> probe_lock(probe_mutex_);
        if (native_scale_ == 0) {
            probe_native_scale();
        }
    }

    TileOptions opts = tile_opts;
    opts.scale = native_scale_;
    // output_scale: if the caller asked for less than the model's native
    // scale (e.g. a 2x request against a 4x model, or a same-resolution
    // "detail regeneration" request where requested_scale == 1), tell
    // upscale_tiled() to downsize EACH TILE immediately after inference
    // rather than assembling a full-frame native-scale buffer and
    // downsampling it afterward. This bounds peak memory to one tile's
    // native-scale footprint regardless of how large `in` is -- see
    // tile.h's TileOptions::output_scale doc comment. If requested_scale is
    // <= 0 or >= native scale, output_scale falls back to native_scale_
    // (the original no-downsize behavior).
    const bool downsize_requested = (requested_scale > 0 && requested_scale < native_scale_);
    opts.output_scale = downsize_requested ? requested_scale : native_scale_;
    if (opts.tile_size <= 0) {
        // Auto-select a tile size based on the active execution provider
        // and target geometry (see tile.h / choose_tile_size()).
        TileSizingParams sizing;
        sizing.input_width = in.width;
        sizing.input_height = in.height;
        sizing.scale = native_scale_;
        sizing.accelerated = (active_provider_name_ != "CPUExecutionProvider");
        opts.tile_size = choose_tile_size(sizing);
        // Throttled: only log when the auto-selected size actually changes
        // (see tile_log_mutex_/last_logged_tile_size_ doc comment in
        // onnx_upscaler.h) -- field logs showed this line flooding
        // AIUpscale.log from dozens of concurrent render threads every
        // single frame once tiling settled into a steady state.
        std::lock_guard<std::mutex> tile_log_lock(tile_log_mutex_);
        if (opts.tile_size != last_logged_tile_size_) {
            last_logged_tile_size_ = opts.tile_size;
            log_info("Auto-selected tile size " + std::to_string(opts.tile_size) +
                      "px (provider=" + active_provider_name_ + ")");
        }
    }

    // Overall output-size safety check up front (also re-checked inside
    // upscale_tiled(), but doing it here gives a clearer error before any
    // tiling work starts). Sized off output_scale -- the size we actually
    // keep for the whole frame -- not the model's native scale.
    safe_buffer_bytes(static_cast<int64_t>(in.width) * opts.output_scale,
                       static_cast<int64_t>(in.height) * opts.output_scale, 4, 1);

    ImageRGBA8 result;
    auto run_tiled = [&](TileOptions attempt_opts) {
        upscale_tiled(in, result, attempt_opts, [this](const ImageRGBA8& tile_in, ImageRGBA8& tile_out) {
            infer(tile_in, tile_out);
        });
    };

    try {
        run_tiled(opts);
    } catch (const std::bad_alloc&) {
        // Memory-pressure retry policy: halve the tile size exactly once
        // and try again; if that also fails, give up and let the
        // exception propagate (host layer turns this into a user-visible
        // error + log entry).
        const int retry_tile = std::max(32, opts.tile_size / 2);
        log_warn("upscale(): out of memory at tile_size=" + std::to_string(opts.tile_size) +
                  ", retrying once at tile_size=" + std::to_string(retry_tile));
        TileOptions retry_opts = opts;
        retry_opts.tile_size = retry_tile;
        try {
            run_tiled(retry_opts);
        } catch (const std::bad_alloc&) {
            log_error("upscale(): out of memory even at reduced tile_size=" + std::to_string(retry_tile) +
                       "; aborting.");
            throw OnnxUpscalerError("Out of memory during upscaling, even after reducing tile size to " +
                                     std::to_string(retry_tile) + "px. Try a smaller input or lower scale.");
        }
    }

    // upscale_tiled() has already produced `result` at the requested
    // output_scale (native scale if requested_scale was <=0 or larger than
    // native, or downsized in-tile otherwise) -- see comment above. No
    // further whole-frame resample step needed (box_downsample() is now
    // unused for this path but kept below for any direct caller that still
    // wants a one-shot non-tiled box-filter downsample).
    out = std::move(result);
}

} // namespace upscale
