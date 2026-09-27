// detail_upscaler.cpp - see detail_upscaler.h for the algorithm overview and
// the clean-room/no-Adobe-code-reused disclaimer.
//
// Pipeline implemented below:
//
//   Stage 1 (lanczos_resize): separable Lanczos-3 base resize. A classic
//   windowed-sinc filter (sinc(x) * sinc(x/a), |x| < a, a=3) applied first
//   along rows then along columns -- the standard two-pass separable-filter
//   construction used by essentially every general-purpose image resizer,
//   because a 2D windowed-sinc kernel factors exactly into the product of
//   two 1D kernels. Skipped when the requested output size already equals
//   the input size (see detail_preserving_upscale()).
//
//   Stage 2 (everything else in detail_preserving_upscale): edge-adaptive,
//   halo-clamped unsharp mask, operating on the RGB channels of the Stage-1
//   result ("base"):
//     a. A small separable Gaussian-ish blur (5-tap {1,4,6,4,1}/16
//        "binomial" approximation of a Gaussian, sigma ~= 1) of `base`.
//     b. detail = base - blur (the classic unsharp-mask high-frequency
//        layer -- whatever the blur removed).
//     c. edge_weight = normalized Sobel gradient magnitude of `base`'s
//        luminance, in [0, 1]. This scales how much of `detail` gets added
//        back per pixel: ~0 in flat regions (so uniform/noisy areas are not
//        amplified), ~1 at strong edges.
//     d. sharpened = base + gain * edge_weight * detail, where `gain` is
//        linearly controlled by detail_amount (0 => gain 0, i.e. Stage 2 is
//        a no-op and the result is exactly the Stage-1 base).
//     e. Halo suppression: `sharpened` is clamped, per channel, to the
//        [min, max] range of `base`'s own 3x3 neighborhood around that
//        pixel. This is what keeps hard edges from ringing/overshooting --
//        the defining visual difference between a "detail-preserving"
//        sharpen and a naive unsharp mask (which haloes badly on strong
//        edges once pushed past a mild strength).
//   Alpha passes through Stage 2 completely untouched (only Stage 1 --
//   ordinary Lanczos resampling -- ever touches alpha, same treatment as
//   RGB there).
#include "detail_upscaler.h"

#include "size_limits.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace upscale {

namespace {

// ---------------------------------------------------------------------------
// Stage 1: separable Lanczos-3 resample.
// ---------------------------------------------------------------------------

constexpr float kPi = 3.14159265358979323846f;
constexpr float kLanczosA = 3.0f; // Lanczos-3: a well-known sweet spot between
                                   // sharpness and ringing for image resampling.

inline float sinc(float x) {
    if (std::fabs(x) < 1e-6f) return 1.0f;
    const float px = kPi * x;
    return std::sin(px) / px;
}

inline float lanczos_weight(float x, float a) {
    if (x <= -a || x >= a) return 0.0f;
    return sinc(x) * sinc(x / a);
}

inline int clamp_index(int idx, int size) {
    return std::clamp(idx, 0, size - 1);
}

// Precomputed per-output-sample filter taps for a separable Lanczos-3 pass
// along one axis (shared structure/logic for both the horizontal and
// vertical passes).
struct AxisFilter {
    std::vector<int> first_src;               // first source index contributing to output i
    std::vector<std::vector<float>> weights;   // weights[i][k] applies to src = first_src[i] + k
};

AxisFilter build_axis_filter(int in_size, int out_size) {
    AxisFilter f;
    f.first_src.resize(out_size);
    f.weights.resize(out_size);

    // input pixels per output pixel; > 1 means this axis is being
    // downsampled (not the common case for an "upscale" effect, but
    // handled correctly -- e.g. a non-uniform aspect-ratio request could
    // shrink one axis while growing the other).
    const float scale = static_cast<float>(in_size) / static_cast<float>(out_size);
    // When downsampling, widen the filter support proportionally (standard
    // practice for band-limiting -- otherwise a shrink would alias). When
    // magnifying (the common case here), keep the canonical Lanczos-3
    // radius.
    const float filter_scale = std::max(scale, 1.0f);
    const float support = kLanczosA * filter_scale;

    for (int i = 0; i < out_size; ++i) {
        // Pixel-center mapping from output index to input space (same
        // convention as resize_rgba_bilinear() in tile.cpp).
        const float center = (i + 0.5f) * scale - 0.5f;
        const int lo = static_cast<int>(std::floor(center - support)) + 1;
        const int hi = static_cast<int>(std::floor(center + support));

        std::vector<float> w;
        w.reserve(static_cast<size_t>(std::max(0, hi - lo + 1)));
        float sum = 0.0f;
        for (int s = lo; s <= hi; ++s) {
            const float x = (static_cast<float>(s) - center) / filter_scale;
            const float wt = lanczos_weight(x, kLanczosA);
            w.push_back(wt);
            sum += wt;
        }
        if (sum != 0.0f) {
            for (float& wt : w) wt /= sum;
        } else if (!w.empty()) {
            // Degenerate case (shouldn't normally happen for any sane
            // in_size/out_size pair): fall back to a single unit-weight tap
            // at the nearest sample rather than dividing by zero.
            std::fill(w.begin(), w.end(), 0.0f);
            w[w.size() / 2] = 1.0f;
        }
        f.first_src[i] = lo;
        f.weights[i] = std::move(w);
    }
    return f;
}

// Separable Lanczos-3 resample of all 4 RGBA channels (alpha resampled the
// same way as RGB -- consistent with how resize_rgba_bilinear() elsewhere
// in this codebase treats alpha).
void lanczos_resize(const ImageRGBA8& in, ImageRGBA8& out, int out_w, int out_h) {
    out.resize(out_w, out_h);

    const AxisFilter fx = build_axis_filter(in.width, out_w);
    const AxisFilter fy = build_axis_filter(in.height, out_h);

    // Horizontal pass first: in.width x in.height -> out_w x in.height,
    // kept in float to avoid compounding 8-bit rounding error across both
    // passes.
    std::vector<float> horiz(static_cast<size_t>(out_w) * in.height * 4);
    for (int y = 0; y < in.height; ++y) {
        const uint8_t* src_row = &in.pixels[static_cast<size_t>(y) * in.width * 4];
        for (int x = 0; x < out_w; ++x) {
            const auto& w = fx.weights[x];
            const int first = fx.first_src[x];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < w.size(); ++k) {
                const int sx = clamp_index(first + static_cast<int>(k), in.width);
                const uint8_t* p = src_row + sx * 4;
                const float wk = w[k];
                acc[0] += wk * p[0];
                acc[1] += wk * p[1];
                acc[2] += wk * p[2];
                acc[3] += wk * p[3];
            }
            float* dst = &horiz[(static_cast<size_t>(y) * out_w + x) * 4];
            dst[0] = acc[0]; dst[1] = acc[1]; dst[2] = acc[2]; dst[3] = acc[3];
        }
    }

    // Vertical pass: out_w x in.height -> out_w x out_h, writing directly
    // into `out` with the final round-to-8-bit + clamp.
    for (int y = 0; y < out_h; ++y) {
        const auto& w = fy.weights[y];
        const int first = fy.first_src[y];
        uint8_t* dst_row = &out.pixels[static_cast<size_t>(y) * out_w * 4];
        for (int x = 0; x < out_w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < w.size(); ++k) {
                const int sy = clamp_index(first + static_cast<int>(k), in.height);
                const float* p = &horiz[(static_cast<size_t>(sy) * out_w + x) * 4];
                const float wk = w[k];
                acc[0] += wk * p[0];
                acc[1] += wk * p[1];
                acc[2] += wk * p[2];
                acc[3] += wk * p[3];
            }
            uint8_t* dst = dst_row + x * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<uint8_t>(std::lround(std::clamp(acc[c], 0.0f, 255.0f)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 2: edge-adaptive, halo-clamped unsharp mask.
// ---------------------------------------------------------------------------

// 5-tap {1,4,6,4,1}/16 separable "binomial" approximation of a Gaussian
// blur (sigma ~= 1), applied to a packed RGB (3-channel) float plane with
// clamp-to-edge addressing at the borders. Chosen over an exact sampled
// Gaussian kernel purely for speed -- this codebase's whole reason for
// existing is to be much faster than the neural engine it replaces, and a
// small binomial kernel is a standard, well-documented low-pass stand-in
// (it's literally the base row of Pascal's triangle, used e.g. as the
// default kernel in Gaussian/Laplacian image pyramids) that's plenty close
// for an unsharp mask's "what did we blur away" reference.
void gaussian_blur_rgb(const std::vector<float>& base_rgb, int w, int h, std::vector<float>& out_blur) {
    static constexpr float kKernel[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};

    std::vector<float> tmp(static_cast<size_t>(w) * h * 3);
    out_blur.assign(static_cast<size_t>(w) * h * 3, 0.0f);

    // Horizontal pass.
    for (int y = 0; y < h; ++y) {
        const float* src_row = &base_rgb[static_cast<size_t>(y) * w * 3];
        float* dst_row = &tmp[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = -2; k <= 2; ++k) {
                const int sx = clamp_index(x + k, w);
                const float* p = src_row + sx * 3;
                const float kw = kKernel[k + 2];
                acc[0] += kw * p[0];
                acc[1] += kw * p[1];
                acc[2] += kw * p[2];
            }
            float* dst = dst_row + x * 3;
            dst[0] = acc[0]; dst[1] = acc[1]; dst[2] = acc[2];
        }
    }
    // Vertical pass.
    for (int y = 0; y < h; ++y) {
        float* dst_row = &out_blur[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = -2; k <= 2; ++k) {
                const int sy = clamp_index(y + k, h);
                const float* p = &tmp[(static_cast<size_t>(sy) * w + x) * 3];
                const float kw = kKernel[k + 2];
                acc[0] += kw * p[0];
                acc[1] += kw * p[1];
                acc[2] += kw * p[2];
            }
            float* dst = dst_row + x * 3;
            dst[0] = acc[0]; dst[1] = acc[1]; dst[2] = acc[2];
        }
    }
}

// Separable 3x3 min/max filter over `base`'s RGB channels, used to build
// the halo-suppression clamp range. A 2D min (or max) filter with a square
// structuring element is separable into two 1D min (max) passes -- a
// standard result from morphological image processing (erosion/dilation
// with a square structuring element) -- which turns what would otherwise
// be an O(9)-neighbor-reads-per-pixel scan into two O(3)-tap passes,
// mirroring the same separable-filter idea the Lanczos/Gaussian stages
// above already use for their own (weighted-sum, not min/max) passes.
void local_min_max_rgb(const ImageRGBA8& base, std::vector<uint8_t>& out_min, std::vector<uint8_t>& out_max) {
    const int w = base.width;
    const int h = base.height;
    const size_t num_pixels = static_cast<size_t>(w) * h;

    // Vertical pass: 3-tap min/max along y (clamp-to-edge), RGB only.
    std::vector<uint8_t> vmin(num_pixels * 3), vmax(num_pixels * 3);
    for (int y = 0; y < h; ++y) {
        const int y0 = clamp_index(y - 1, h);
        const int y1 = clamp_index(y + 1, h);
        const uint8_t* r0 = &base.pixels[static_cast<size_t>(y0) * w * 4];
        const uint8_t* r1 = &base.pixels[static_cast<size_t>(y) * w * 4];
        const uint8_t* r2 = &base.pixels[static_cast<size_t>(y1) * w * 4];
        uint8_t* dmin = &vmin[static_cast<size_t>(y) * w * 3];
        uint8_t* dmax = &vmax[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const uint8_t a = r0[x * 4 + c];
                const uint8_t b = r1[x * 4 + c];
                const uint8_t d = r2[x * 4 + c];
                dmin[x * 3 + c] = std::min(a, std::min(b, d));
                dmax[x * 3 + c] = std::max(a, std::max(b, d));
            }
        }
    }

    // Horizontal pass: 3-tap min/max along x over the vertical pass's
    // result -- the combination of both passes yields the full 3x3
    // neighborhood min/max, exactly as if it had been computed directly.
    out_min.assign(num_pixels * 3, 0);
    out_max.assign(num_pixels * 3, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow_min = &vmin[static_cast<size_t>(y) * w * 3];
        const uint8_t* srow_max = &vmax[static_cast<size_t>(y) * w * 3];
        uint8_t* dmin = &out_min[static_cast<size_t>(y) * w * 3];
        uint8_t* dmax = &out_max[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x) {
            const int x0 = clamp_index(x - 1, w);
            const int x1 = clamp_index(x + 1, w);
            for (int c = 0; c < 3; ++c) {
                dmin[x * 3 + c] = std::min(srow_min[x0 * 3 + c], std::min(srow_min[x * 3 + c], srow_min[x1 * 3 + c]));
                dmax[x * 3 + c] = std::max(srow_max[x0 * 3 + c], std::max(srow_max[x * 3 + c], srow_max[x1 * 3 + c]));
            }
        }
    }
}

} // namespace

void detail_preserving_upscale(const ImageRGBA8& in,
                                ImageRGBA8& out,
                                int out_w,
                                int out_h,
                                float detail_amount) {
    // Validate at the entry point, before any allocation -- same convention
    // as OnnxUpscaler::upscale()/upscale_tiled() (see size_limits.h).
    validate_input_dims(in.width, in.height, 4);
    safe_buffer_bytes(out_w, out_h, 4, 1);

    const float amount = std::clamp(detail_amount, 0.0f, 100.0f);

    // --- Stage 1: base resize (or identity copy for same-size requests) ---
    ImageRGBA8 base;
    if (out_w == in.width && out_h == in.height) {
        base = in; // detail-regen mode: no resize needed (see header comment)
    } else {
        lanczos_resize(in, base, out_w, out_h);
    }

    if (amount <= 0.0f) {
        // detail_amount == 0: pure Lanczos (or identity) result, no Stage 2.
        out = std::move(base);
        return;
    }

    const int w = base.width;
    const int h = base.height;
    const size_t num_pixels = static_cast<size_t>(w) * h;

    // Unpack RGB (not alpha) into a float plane for the blur/gradient/
    // unsharp math below.
    std::vector<float> base_rgb(num_pixels * 3);
    for (size_t i = 0; i < num_pixels; ++i) {
        base_rgb[i * 3 + 0] = static_cast<float>(base.pixels[i * 4 + 0]);
        base_rgb[i * 3 + 1] = static_cast<float>(base.pixels[i * 4 + 1]);
        base_rgb[i * 3 + 2] = static_cast<float>(base.pixels[i * 4 + 2]);
    }

    std::vector<float> blur_rgb;
    gaussian_blur_rgb(base_rgb, w, h, blur_rgb);

    // Luminance plane (computed once so the Sobel pass below doesn't
    // redundantly recompute each neighbor's luminance multiple times).
    std::vector<float> luminance(num_pixels);
    for (size_t i = 0; i < num_pixels; ++i) {
        luminance[i] = 0.299f * base_rgb[i * 3 + 0] + 0.587f * base_rgb[i * 3 + 1] + 0.114f * base_rgb[i * 3 + 2];
    }

    // Sobel gradient magnitude on luminance, normalized to an edge weight
    // in [0, 1] per pixel.
    std::vector<float> edge_weight(num_pixels);
    // Normalization divisor: the theoretical max |Sobel gradient| is 4*255
    // = 1020 (a pure black/white step), but dividing by that would mean
    // only near-maximal contrast edges ever reach full sharpening strength.
    // kEdgeNorm is tuned much smaller so ordinary photographic/graphic
    // edges (not just hard black/white steps) already reach (or nearly
    // reach) full edge_weight, while flat/near-flat regions (gradient close
    // to 0) still correctly get weight close to 0.
    constexpr float kEdgeNorm = 48.0f;
    for (int y = 0; y < h; ++y) {
        const int ym = clamp_index(y - 1, h);
        const int yp = clamp_index(y + 1, h);
        for (int x = 0; x < w; ++x) {
            const int xm = clamp_index(x - 1, w);
            const int xp = clamp_index(x + 1, w);
            const float tl = luminance[static_cast<size_t>(ym) * w + xm];
            const float tc = luminance[static_cast<size_t>(ym) * w + x];
            const float tr = luminance[static_cast<size_t>(ym) * w + xp];
            const float ml = luminance[static_cast<size_t>(y) * w + xm];
            const float mr = luminance[static_cast<size_t>(y) * w + xp];
            const float bl = luminance[static_cast<size_t>(yp) * w + xm];
            const float bc = luminance[static_cast<size_t>(yp) * w + x];
            const float br = luminance[static_cast<size_t>(yp) * w + xp];
            const float gx = (tr + 2 * mr + br) - (tl + 2 * ml + bl);
            const float gy = (bl + 2 * bc + br) - (tl + 2 * tc + tr);
            const float mag = std::sqrt(gx * gx + gy * gy);
            edge_weight[static_cast<size_t>(y) * w + x] = std::clamp(mag / kEdgeNorm, 0.0f, 1.0f);
        }
    }

    // Gain: 0 at detail_amount=0 (handled by the early return above), up to
    // kGainAt100 at detail_amount=100, linear in between. detail_amount=50
    // ("standard") lands at exactly half of kGainAt100.
    constexpr float kGainAt100 = 2.0f;
    const float gain = (amount / 100.0f) * kGainAt100;

    // Halo-suppression clamp range for every pixel, computed once up front
    // via the separable min/max filter above (see local_min_max_rgb()) --
    // equivalent to, but far cheaper than, gathering each pixel's own 3x3
    // neighborhood min/max inline in the loop below.
    std::vector<uint8_t> local_min, local_max;
    local_min_max_rgb(base, local_min, local_max);

    out.resize(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * w + x;
            const float ew = edge_weight[idx];

            const uint8_t* base_px = &base.pixels[idx * 4];
            const uint8_t* lo = &local_min[idx * 3];
            const uint8_t* hi = &local_max[idx * 3];
            uint8_t* out_px = &out.pixels[idx * 4];
            for (int c = 0; c < 3; ++c) {
                const float base_v = base_rgb[idx * 3 + c];
                const float blur_v = blur_rgb[idx * 3 + c];
                const float detail = base_v - blur_v;
                float sharpened = base_v + gain * ew * detail;
                // Halo suppression (see detail_upscaler.h): clamp to the
                // local neighborhood's own value range before the final
                // 8-bit clamp, so an edge can be sharpened but never pushed
                // beyond what already exists nearby.
                sharpened = std::clamp(sharpened, static_cast<float>(lo[c]), static_cast<float>(hi[c]));
                out_px[c] = static_cast<uint8_t>(std::lround(std::clamp(sharpened, 0.0f, 255.0f)));
            }
            out_px[3] = base_px[3]; // alpha untouched by Stage 2
        }
    }
}

} // namespace upscale
