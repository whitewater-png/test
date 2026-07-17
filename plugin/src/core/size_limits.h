// limits.h - Safety limits and overflow-checked size arithmetic.
//
// All image size math in this codebase (width * height * channels *
// bytes_per_channel) is carried out with these helpers so that:
//   1. Integer overflow during multiplication is caught BEFORE it happens
//      (checked division-based guards, not __int128, to stay portable
//      across the MSVC/Clang/GCC toolchains this plugin targets).
//   2. Absurd/malicious inputs (e.g. a corrupt PiPL-fed size, a crafted
//      PNG header) are rejected with a clear error instead of causing a
//      huge or failing allocation deep inside tile.cpp/onnx_upscaler.cpp.
//
// The limits below are sized so that legitimate FHD/QHD/UHD workloads
// (including UHD 4x = 15360x8640 RGBA, ~531 MiB) pass comfortably, while
// still bounding worst-case memory use on the target 36 GB unified-memory
// M4 Max machine (and any smaller CPU-only dev machine, like this one).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace upscale {

// Per-side pixel limits.
constexpr int64_t kMaxInputDim = 8192;    // input width/height, per side
constexpr int64_t kMaxOutputDim = 32768;  // output width/height, per side (safety valve; UHD 4x = 15360 fits easily)

// Total output pixel count limit. UHD (3840x2160) at 4x = 15360x8640 =
// 132,710,400 pixels, so this is set comfortably above that.
constexpr int64_t kMaxOutputPixels = 200'000'000LL;

// Single-buffer byte size limit (applies to any one contiguous allocation
// such as an output RGBA8 image or an NCHW float tensor). UHD 4x RGBA8 is
// ~531 MiB; float tensors are larger (4x the byte width), so this is set
// well above both.
constexpr size_t kMaxSingleBufferBytes = static_cast<size_t>(4) * 1024 * 1024 * 1024; // 4 GiB

class SizeLimitError : public std::runtime_error {
public:
    explicit SizeLimitError(const std::string& msg) : std::runtime_error(msg) {}
};

// Overflow-checked multiplication of two non-negative int64_t values.
// Throws SizeLimitError instead of silently wrapping.
inline int64_t checked_mul_i64(int64_t a, int64_t b, const char* what) {
    if (a < 0 || b < 0) {
        throw SizeLimitError(std::string("negative size in ") + what);
    }
    if (a != 0 && b > (std::numeric_limits<int64_t>::max)() / a) {
        throw SizeLimitError(std::string("integer overflow computing ") + what);
    }
    return a * b;
}

// Validates raw input image dimensions (before any allocation). Called at
// every boundary where external data enters the pipeline: CLI PNG load,
// AE PF_EffectWorld conversion, etc.
inline void validate_input_dims(int64_t w, int64_t h, int64_t channels = 4) {
    if (w <= 0 || h <= 0) {
        throw SizeLimitError("invalid image dimensions: width/height must be positive (got " +
                              std::to_string(w) + "x" + std::to_string(h) + ")");
    }
    if (w > kMaxInputDim || h > kMaxInputDim) {
        throw SizeLimitError("input image exceeds maximum supported dimension of " +
                              std::to_string(kMaxInputDim) + "px per side (got " +
                              std::to_string(w) + "x" + std::to_string(h) + ")");
    }
    if (channels <= 0 || channels > 4) {
        throw SizeLimitError("unsupported pixel channel count: " + std::to_string(channels));
    }
}

// Computes width * height * channels * bytes_per_channel with overflow
// checks at every multiplication step, then validates the result against
// kMaxOutputPixels / kMaxSingleBufferBytes. Throws SizeLimitError on any
// violation; otherwise returns the byte count. Used before every
// significant allocation (output images, NCHW tensors, blend buffers).
inline size_t safe_buffer_bytes(int64_t w, int64_t h, int64_t channels, int64_t bytes_per_channel = 1) {
    if (w <= 0 || h <= 0 || channels <= 0 || bytes_per_channel <= 0) {
        throw SizeLimitError("invalid (non-positive) dimensions for buffer size calculation");
    }
    if (w > kMaxOutputDim || h > kMaxOutputDim) {
        throw SizeLimitError("output image exceeds maximum supported dimension of " +
                              std::to_string(kMaxOutputDim) + "px per side (got " +
                              std::to_string(w) + "x" + std::to_string(h) + ")");
    }
    const int64_t pixels = checked_mul_i64(w, h, "pixel count (width*height)");
    if (pixels > kMaxOutputPixels) {
        throw SizeLimitError("output pixel count " + std::to_string(pixels) +
                              " exceeds safety limit of " + std::to_string(kMaxOutputPixels));
    }
    const int64_t channel_bytes = checked_mul_i64(pixels, channels, "pixel*channels");
    const int64_t total_bytes = checked_mul_i64(channel_bytes, bytes_per_channel, "channel*bytes_per_channel");
    if (static_cast<uint64_t>(total_bytes) > static_cast<uint64_t>(kMaxSingleBufferBytes)) {
        throw SizeLimitError("output buffer size " + std::to_string(total_bytes) +
                              " bytes exceeds safety limit of " +
                              std::to_string(kMaxSingleBufferBytes / (1024 * 1024)) + " MiB");
    }
    return static_cast<size_t>(total_bytes);
}

} // namespace upscale
