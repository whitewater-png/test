// test_detail_upscaler.cpp - Unit test for the classical edge-preserving
// upscaler (plugin/src/core/detail_upscaler.h/.cpp), added when this
// clean-room, non-neural algorithm replaced Real-ESRGAN/ONNX as the AI
// Upscale effect's DEFAULT engine (the neural engine remains available as
// an opt-in "Engine" choice, but was too slow for realtime/render use in
// Premiere -- several seconds per 4K frame on real hardware). Exercises, on
// this Linux dev environment (no Adobe SDK / GPU required, upscale_core is
// Adobe-independent):
//   1. Size correctness: 64x64 -> 256x256 produces a 256x256 output.
//   2. detail_amount=0 vs detail_amount=100 differ on an image with real
//      edges (proves the unsharp-mask stage actually does something when
//      enabled, and that 0 truly disables it).
//   3. Halo suppression: with detail_amount=100 (this implementation's
//      maximum strength) on a same-size (detail-regen) request -- where the
//      "base" image Stage 2 clamps against is exactly the input, byte for
//      byte -- every output pixel must fall within its own 3x3
//      neighborhood's [min, max] range in the INPUT image. This is a direct
//      regression test of the halo-suppression clamp, not a tautology: it
//      would fail immediately if that clamp were ever removed or broken.
//   4. Same-size input/output (the plugin's actual detail-regen usage
//      pattern) works and does not resize.
//   5. A flat/solid-color image does not gain noise from sharpening at
//      detail_amount=100 -- since a flat image has zero gradient
//      everywhere, edge_weight is 0 everywhere, so output must be
//      byte-identical to input.
//   6. Informational speed measurement: 1920x1080 -> 3840x2160 timed once
//      and logged to stdout (not asserted -- CPU speed varies across CI/dev
//      machines -- but printed so it can be cited as a reference figure,
//      per plugin/README.md).
#include "detail_upscaler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

int checks_run = 0;
int checks_failed = 0;

void check(bool cond, const char* what) {
    ++checks_run;
    if (!cond) {
        ++checks_failed;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

upscale::ImageRGBA8 make_solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    upscale::ImageRGBA8 img;
    img.resize(w, h);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = a;
    }
    return img;
}

// A pattern with real edges (a coarse checkerboard plus a diagonal
// gradient) so the Sobel-based edge weight is non-zero in plenty of
// places -- flat images would make detail_amount=0 vs 100 indistinguishable
// since edge_weight would be ~0 everywhere.
upscale::ImageRGBA8 make_checker_gradient(int w, int h) {
    upscale::ImageRGBA8 img;
    img.resize(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
            const uint8_t base = checker ? 220 : 30;
            img.pixels[idx + 0] = base;
            img.pixels[idx + 1] = static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
            img.pixels[idx + 2] = static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

} // namespace

int main() {
    // --- 1: base resize size correctness -----------------------------------
    {
        upscale::ImageRGBA8 in = make_checker_gradient(64, 64);
        upscale::ImageRGBA8 out;
        upscale::detail_preserving_upscale(in, out, 256, 256, 50.0f);
        check(out.width == 256, "64x64->256x256: width");
        check(out.height == 256, "64x64->256x256: height");
        check(out.pixels.size() == static_cast<size_t>(256) * 256 * 4, "64x64->256x256: buffer size");
    }

    // --- 2: detail=0 vs detail=100 differ on an image with real edges -----
    {
        upscale::ImageRGBA8 in = make_checker_gradient(64, 64);
        upscale::ImageRGBA8 out0, out100;
        upscale::detail_preserving_upscale(in, out0, 256, 256, 0.0f);
        upscale::detail_preserving_upscale(in, out100, 256, 256, 100.0f);
        check(out0.width == 256 && out0.height == 256, "detail=0: size");
        check(out100.width == 256 && out100.height == 256, "detail=100: size");
        check(out0.pixels != out100.pixels, "detail=0 vs detail=100: outputs differ on an edgy image");
    }

    // --- 3: halo suppression (same-size request, exact base = input) ------
    {
        upscale::ImageRGBA8 in = make_checker_gradient(48, 48);
        upscale::ImageRGBA8 out;
        upscale::detail_preserving_upscale(in, out, 48, 48, 100.0f);
        check(out.width == 48 && out.height == 48, "halo test: same-size output");

        bool halo_violation = false;
        for (int y = 0; y < 48 && !halo_violation; ++y) {
            for (int x = 0; x < 48 && !halo_violation; ++x) {
                for (int c = 0; c < 3; ++c) {
                    uint8_t lo = 255, hi = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = std::min(std::max(x + dx, 0), 47);
                            int ny = std::min(std::max(y + dy, 0), 47);
                            const uint8_t v = in.pixels[(static_cast<size_t>(ny) * 48 + nx) * 4 + c];
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                    }
                    const uint8_t out_v = out.pixels[(static_cast<size_t>(y) * 48 + x) * 4 + c];
                    if (out_v < lo || out_v > hi) {
                        halo_violation = true;
                        std::fprintf(stderr,
                                     "halo violation at (%d,%d) channel %d: out=%d not in [%d,%d]\n",
                                     x, y, c, out_v, lo, hi);
                        break;
                    }
                }
            }
        }
        check(!halo_violation, "detail=100 same-size: every output pixel stays within its local 3x3 input min/max");
    }

    // --- 4: same-size input/output (detail-regen mode) skips the resize ---
    {
        upscale::ImageRGBA8 in = make_checker_gradient(80, 60);
        upscale::ImageRGBA8 out;
        upscale::detail_preserving_upscale(in, out, 80, 60, 0.0f);
        check(out.width == 80 && out.height == 60, "detail-regen (detail=0): size unchanged");
        check(out.pixels == in.pixels, "detail-regen (detail=0): pure identity, byte-exact");
    }

    // --- 5: flat/solid image gains no noise from sharpening ----------------
    {
        upscale::ImageRGBA8 in = make_solid(40, 40, 128, 90, 200, 255);
        upscale::ImageRGBA8 out;
        upscale::detail_preserving_upscale(in, out, 40, 40, 100.0f);
        check(out.width == 40 && out.height == 40, "flat image: size unchanged");
        check(out.pixels == in.pixels, "flat image: detail=100 does not introduce noise (byte-exact passthrough)");
    }

    // --- 6: informational speed measurement (not asserted) -----------------
    {
        upscale::ImageRGBA8 in = make_checker_gradient(1920, 1080);
        upscale::ImageRGBA8 out;
        const auto t0 = std::chrono::steady_clock::now();
        upscale::detail_preserving_upscale(in, out, 3840, 2160, 50.0f);
        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
        check(out.width == 3840 && out.height == 2160, "1920x1080->3840x2160: size");
        std::printf("test_detail_upscaler: 1920x1080->3840x2160 detail_amount=50 elapsed: %.3fs (reference figure, "
                    "not asserted -- see plugin/README.md)\n",
                    elapsed_sec);
    }

    std::printf("test_detail_upscaler: %d checks run, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
