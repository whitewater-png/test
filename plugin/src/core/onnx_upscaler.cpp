#include "onnx_upscaler.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

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
#ifdef UPSCALE_WITH_COREML
        try {
            options.AppendExecutionProvider("CoreML", {});
            chosen = "CoreMLExecutionProvider";
            return true;
        } catch (const Ort::Exception&) {
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
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        active_provider_name_ = append_providers(options, provider_preference);

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
        throw OnnxUpscalerError(std::string("Failed to load ONNX model '") + model_path + "': " + ex.what());
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

// Box-filter downsample of a full RGBA8 image to an arbitrary smaller
// size. Used to go from the model's native scale (e.g. 4x) down to a
// smaller requested scale (e.g. 2x) when no matching model is available.
// NOTE: this is a plain box/area filter, not a sharper Lanczos-style
// filter -- acceptable quality for stepping down an already-upscaled
// image, and keeps this core dependency-free (no separate resampling
// library).
void box_downsample(const ImageRGBA8& in, ImageRGBA8& out, int out_w, int out_h) {
    out.resize(out_w, out_h);
    const float sx = static_cast<float>(in.width) / out_w;
    const float sy = static_cast<float>(in.height) / out_h;
    for (int y = 0; y < out_h; ++y) {
        const int y0 = static_cast<int>(y * sy);
        const int y1 = std::max(y0 + 1, static_cast<int>((y + 1) * sy));
        for (int x = 0; x < out_w; ++x) {
            const int x0 = static_cast<int>(x * sx);
            const int x1 = std::max(x0 + 1, static_cast<int>((x + 1) * sx));

            std::array<float, 4> acc{0, 0, 0, 0};
            int count = 0;
            for (int yy = y0; yy < std::min(y1, in.height); ++yy) {
                for (int xx = x0; xx < std::min(x1, in.width); ++xx) {
                    const size_t src = (static_cast<size_t>(yy) * in.width + xx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += in.pixels[src + c];
                    ++count;
                }
            }
            const size_t dst = (static_cast<size_t>(y) * out_w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                out.pixels[dst + c] = count > 0
                    ? static_cast<uint8_t>(std::lround(acc[c] / count))
                    : 0;
            }
        }
    }
}

} // namespace

void OnnxUpscaler::infer(const ImageRGBA8& in, ImageRGBA8& out) {
    if (!is_loaded()) {
        throw OnnxUpscalerError("infer() called before load()");
    }
    if (in.width <= 0 || in.height <= 0) {
        throw OnnxUpscalerError("infer() called with an empty image");
    }

    try {
        std::vector<float> input_data = to_nchw_float(in);
        std::array<int64_t, 4> input_shape{1, 3, in.height, in.width};

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            impl_->mem_info, input_data.data(), input_data.size(),
            input_shape.data(), input_shape.size());

        const char* input_names[] = {impl_->input_name.c_str()};
        const char* output_names[] = {impl_->output_name.c_str()};

        auto output_tensors = impl_->session->Run(
            Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);

        Ort::Value& output_tensor = output_tensors.front();
        auto out_info = output_tensor.GetTensorTypeAndShapeInfo();
        auto out_shape = out_info.GetShape();
        if (out_shape.size() != 4 || out_shape[1] != 3) {
            throw OnnxUpscalerError("Unexpected model output shape (expected [1,3,H,W])");
        }
        const int out_h = static_cast<int>(out_shape[2]);
        const int out_w = static_cast<int>(out_shape[3]);

        std::vector<uint8_t> alpha = resize_alpha_bilinear(in, out_w, out_h);
        from_nchw_float(output_tensor.GetTensorData<float>(), out_w, out_h, alpha, out);
    } catch (const Ort::Exception& ex) {
        throw OnnxUpscalerError(std::string("ONNX inference failed: ") + ex.what());
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
    if (native_scale_ == 0) {
        probe_native_scale();
    }

    TileOptions opts = tile_opts;
    opts.scale = native_scale_;

    ImageRGBA8 native_out;
    upscale_tiled(in, native_out, opts, [this](const ImageRGBA8& tile_in, ImageRGBA8& tile_out) {
        infer(tile_in, tile_out);
    });

    if (requested_scale <= 0 || requested_scale == native_scale_) {
        out = std::move(native_out);
        return;
    }

    if (requested_scale > native_scale_) {
        // Caller asked for more than the model natively provides. We do
        // not attempt a second inference pass (chaining SR passes tends
        // to amplify artifacts); return the native-scale result as-is.
        // Plugin-layer callers should pick a model whose native scale
        // matches the requested scale where possible.
        out = std::move(native_out);
        return;
    }

    // requested_scale < native_scale_: downsample the native-scale result
    // down to the requested size (see box_downsample() note re: filter
    // quality tradeoff).
    const int target_w = in.width * requested_scale;
    const int target_h = in.height * requested_scale;
    box_downsample(native_out, out, target_w, target_h);
}

} // namespace upscale
