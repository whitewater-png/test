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

    // Runs a single forward pass on `in` (no tiling) and writes the result
    // to `out`. The output size is determined by the model's native scale
    // factor (see native_scale()), NOT by any externally requested scale.
    // Throws OnnxUpscalerError on failure (e.g. session not loaded, or a
    // shape/runtime error from onnxruntime).
    void infer(const ImageRGBA8& in, ImageRGBA8& out);

    // Runs tiled inference (see tile.h) followed by, if necessary, a
    // box/bilinear resample down to `requested_scale`. This is the
    // primary entry point most callers (CLI, AE plugin) should use.
    //
    // `requested_scale` is the scale the caller wants (2 or 4, typically).
    // If the model's native scale is larger than requested_scale (e.g. a
    // 4x model used for a 2x request), the 4x result is downsampled with a
    // simple box/bilinear filter -- NOT a high quality Lanczos filter, see
    // note in the .cpp -- to reach the requested size. If native scale is
    // smaller than requested, we upscale to native scale and then use the
    // model's own output as the base without further upsampling attempts
    // (a mismatch here generally indicates the wrong model was loaded).
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

private:
    void probe_native_scale();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Only used to avoid pulling onnxruntime headers into this header;
    // actual Ort::Session lives in Impl.
    void* session_ = nullptr;

    int native_scale_ = 0;
    std::string active_provider_name_;
};

} // namespace upscale
