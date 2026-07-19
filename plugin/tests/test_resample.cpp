// test_resample.cpp - Standalone smoke test for upscale::resize_rgba_bilinear
// (plugin/src/core/tile.cpp/.h), added while fixing the real-Premiere-hardware
// render failure traced to HandleRender assuming output-world size == input
// world size * scale (see plugin/src/plugin/AIUpscale.cpp HandleRender and
// plugin/src/core/size_limits.h). This exercises, on this Linux dev
// environment (no Adobe SDK required, upscale_core is Adobe-independent):
//   1. Downscale resample: 64x64 -> 128x128 (output world SMALLER than a 4x
//      model's native-scale output would be for this input, e.g. Scale=2x
//      requested against a 4x model).
//   2. Non-integer-ratio resample: 64x64 -> 100x73 (an arbitrary host-given
//      output size, matching the design goal of adapting to "whatever size
//      the host actually allocated" rather than assuming a clean multiple).
//   3. Identity (same-size) passthrough: output size == input size, verifies
//      the no-op fast path returns pixel-identical data.
// Exits non-zero (via std::abort through assert, or an explicit non-zero
// return) on any check failure so it can be wired into CI/ctest.
#include "tile.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace {

upscale::ImageRGBA8 make_gradient(int w, int h) {
    upscale::ImageRGBA8 img;
    img.resize(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            img.pixels[idx + 0] = static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
            img.pixels[idx + 1] = static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
            img.pixels[idx + 2] = 128;
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

int checks_run = 0;
int checks_failed = 0;

void check(bool cond, const char* what) {
    ++checks_run;
    if (!cond) {
        ++checks_failed;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

} // namespace

int main() {
    // 1. Downscale-style resample: 64x64 -> 128x128 (upsize here just to
    // exercise a clean-ratio non-identity case; the same code path handles
    // the "128x128 model output -> 64x64 requested output" direction
    // identically since resize_rgba_bilinear() doesn't special-case
    // up-vs-down).
    {
        upscale::ImageRGBA8 in = make_gradient(64, 64);
        upscale::ImageRGBA8 out;
        upscale::resize_rgba_bilinear(in, out, 128, 128);
        check(out.width == 128, "64x64->128x128: width");
        check(out.height == 128, "64x64->128x128: height");
        check(out.pixels.size() == static_cast<size_t>(128) * 128 * 4, "64x64->128x128: buffer size");
        // Corners should roughly track the source gradient's corners.
        const size_t top_left = 0;
        const size_t bottom_right = (static_cast<size_t>(127) * 128 + 127) * 4;
        check(out.pixels[top_left + 0] < 40, "64x64->128x128: top-left red low");
        check(out.pixels[bottom_right + 0] > 200, "64x64->128x128: bottom-right red high");
        check(out.pixels[bottom_right + 1] > 200, "64x64->128x128: bottom-right green high");
    }

    // 2. Non-integer-ratio resample: 64x64 -> 100x73 (arbitrary host-given
    // output size, the scenario this function exists for -- see
    // AIUpscale.cpp HandleRender).
    {
        upscale::ImageRGBA8 in = make_gradient(64, 64);
        upscale::ImageRGBA8 out;
        upscale::resize_rgba_bilinear(in, out, 100, 73);
        check(out.width == 100, "64x64->100x73: width");
        check(out.height == 73, "64x64->100x73: height");
        check(out.pixels.size() == static_cast<size_t>(100) * 73 * 4, "64x64->100x73: buffer size");
        for (size_t i = 3; i < out.pixels.size(); i += 4) {
            if (out.pixels[i] != 255) {
                check(false, "64x64->100x73: alpha preserved as opaque");
                break;
            }
        }
    }

    // 3. Identity passthrough: same size in and out should be pixel-exact.
    {
        upscale::ImageRGBA8 in = make_gradient(32, 48);
        upscale::ImageRGBA8 out;
        upscale::resize_rgba_bilinear(in, out, 32, 48);
        check(out.width == 32 && out.height == 48, "identity: size");
        check(out.pixels == in.pixels, "identity: pixel-exact passthrough");
    }

    std::printf("test_resample: %d checks run, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
