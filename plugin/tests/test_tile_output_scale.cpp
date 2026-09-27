// test_tile_output_scale.cpp - Regression test for TileOptions::output_scale
// (plugin/src/core/tile.h/.cpp), added while fixing the THIRD real-Premiere-
// hardware render failure. Field logs showed Premiere handing HandleRender
// an input world (4892x8192) already far larger than the layer's nominal
// full-resolution size (3840x2160) -- itself a symptom of the (now
// retracted) PF_OutFlag_I_EXPAND_BUFFER request -- and this plugin's model
// then applying its OWN native 4x upscale on top of THAT, producing a
// 19568x32768 (641,204,224px) intermediate that blew through every size
// safety margin in size_limits.h and crashed the render with
// PF_Err_INTERNAL_STRUCT_DAMAGED (512).
//
// The fix (see tile.h/tile.cpp) lets a caller request a same-resolution (or
// otherwise smaller-than-native) result via TileOptions::output_scale,
// which downsizes each tile's model-native-scale result immediately after
// inference -- before compositing -- so a full-frame buffer at the model's
// native scale is never allocated, no matter how large the input is.
//
// This test exercises that fix directly against upscale_core (Adobe-
// independent, buildable/runnable on this Linux dev environment):
//   1. Using the EXACT field-reported input world dimensions (4892x8192)
//      and scale=4, confirm that upscale_tiled() with the OLD default
//      behavior (output_scale left unset, i.e. == scale) throws
//      SizeLimitError immediately (641,204,224px > kMaxOutputPixels) --
//      demonstrating the failure mode this fix targets, BEFORE any tile
//      compute even starts (the dummy upscale_fn below aborts the test if
//      it's ever actually called in this case, proving the throw happens
//      at the up-front size check).
//   2. The same input/scale with output_scale=1 (this fix's actual usage
//      from AIUpscale.cpp HandleRender) does NOT throw, actually runs
//      tiled compute, and produces a same-resolution (4892x8192) result --
//      i.e. the exact scenario that used to crash now succeeds.
//   3. Smaller-scale correctness/equivalence check: a uniform-color image
//      processed via (a) the single-tile fast path (tile_size >= image
//      size) and (b) genuine multi-tile compute (small tile_size, so
//      multiple tiles + blending are actually exercised) with the same
//      output_scale=1 request produce IDENTICAL output -- both must
//      reconstruct the same uniform color exactly, so this also serves as
//      a "tiled sequential processing matches single-shot processing"
//      check for the output_scale downsize path.
//   4. Non-square / remainder-tile sizes (e.g. width/height not evenly
//      divisible by the tile stride) don't crash and produce the expected
//      output size, using a deterministic (non-uniform) gradient image
//      compared between two different worker-thread counts to also
//      reconfirm the serial-vs-parallel determinism guarantee still holds
//      through the new downsize-per-tile code path.
#include "tile.h"
#include "size_limits.h"

#include <cassert>
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

// Deterministic stand-in "model": nearest-neighbor replicate each pixel
// `scale` x `scale` times. Cheap (no real inference) but exercises the same
// tile-in/tile-out size contract OnnxUpscaler::infer() does.
upscale::TileUpscaleFn make_nearest_upscale_fn(int scale) {
    return [scale](const upscale::ImageRGBA8& in, upscale::ImageRGBA8& out) {
        out.resize(in.width * scale, in.height * scale);
        for (int y = 0; y < out.height; ++y) {
            const int sy = y / scale;
            for (int x = 0; x < out.width; ++x) {
                const int sx = x / scale;
                const size_t src = (static_cast<size_t>(sy) * in.width + sx) * 4;
                const size_t dst = (static_cast<size_t>(y) * out.width + x) * 4;
                for (int c = 0; c < 4; ++c) out.pixels[dst + c] = in.pixels[src + c];
            }
        }
    };
}

} // namespace

int main() {
    // --- 1 & 2: the exact field-reported dimensions -----------------------
    // Building a real 4892x8192 ImageRGBA8 input just for its size (~160MB)
    // is itself acceptable on CI, but we never want the (would-be-huge)
    // native-scale intermediate this test is about to provoke -- see the
    // aborting dummy callback below for case 1.
    {
        upscale::ImageRGBA8 field_input;
        field_input.resize(4892, 8192);

        upscale::TileOptions opts;
        opts.tile_size = 256;
        opts.overlap = 16;
        opts.scale = 4;
        opts.num_workers = 0;
        // output_scale left at its default (<=0 => falls back to `scale`):
        // this reproduces the OLD (pre-fix) behavior exactly.

        bool aborting_fn_called = false;
        upscale::TileUpscaleFn aborting_fn = [&](const upscale::ImageRGBA8&, upscale::ImageRGBA8&) {
            aborting_fn_called = true;
        };

        bool threw = false;
        std::string what;
        try {
            upscale::ImageRGBA8 out;
            upscale::upscale_tiled(field_input, out, opts, aborting_fn);
        } catch (const upscale::SizeLimitError& ex) {
            threw = true;
            what = ex.what();
        }
        check(threw, "field-size default output_scale: expected SizeLimitError (641,204,224px explosion)");
        check(!aborting_fn_called,
              "field-size default output_scale: size check must fire BEFORE any tile compute (upscale_fn "
              "must never be called)");
        if (threw) {
            std::fprintf(stderr, "(expected) SizeLimitError: %s\n", what.c_str());
        }
    }
    {
        upscale::ImageRGBA8 field_input;
        field_input.resize(4892, 8192);

        upscale::TileOptions opts;
        opts.tile_size = 256;
        opts.overlap = 16;
        opts.scale = 4;
        opts.output_scale = 1; // the fix: same-resolution request
        opts.num_workers = 0;

        upscale::ImageRGBA8 out;
        bool threw = false;
        try {
            upscale::upscale_tiled(field_input, out, opts, make_nearest_upscale_fn(4));
        } catch (const std::exception& ex) {
            threw = true;
            std::fprintf(stderr, "UNEXPECTED exception with output_scale=1: %s\n", ex.what());
        }
        check(!threw, "field-size output_scale=1: must not throw (this is the actual fix)");
        check(out.width == 4892 && out.height == 8192,
              "field-size output_scale=1: output size must equal input size (same-resolution)");
    }

    // --- 3: fast-path vs genuine multi-tile equivalence (uniform image) ---
    {
        upscale::ImageRGBA8 in = make_solid(64, 64, 200, 100, 50, 255);

        upscale::TileOptions fast_opts;
        fast_opts.tile_size = 128; // >= image size -> single-tile fast path
        fast_opts.overlap = 16;
        fast_opts.scale = 4;
        fast_opts.output_scale = 1;
        fast_opts.num_workers = 0;

        upscale::TileOptions tiled_opts = fast_opts;
        tiled_opts.tile_size = 16; // forces many tiles + blending

        upscale::ImageRGBA8 out_fast, out_tiled;
        upscale::upscale_tiled(in, out_fast, fast_opts, make_nearest_upscale_fn(4));
        upscale::upscale_tiled(in, out_tiled, tiled_opts, make_nearest_upscale_fn(4));

        check(out_fast.width == 64 && out_fast.height == 64, "uniform fast-path: size");
        check(out_tiled.width == 64 && out_tiled.height == 64, "uniform tiled: size");
        check(out_fast.pixels == in.pixels, "uniform fast-path: exact color reconstruction");
        check(out_tiled.pixels == out_fast.pixels,
              "uniform tiled (many tiles + blending) matches single-shot fast-path exactly");
    }

    // --- 4: non-square / remainder sizes, serial vs parallel determinism --
    {
        upscale::ImageRGBA8 in = make_gradient(613, 97); // deliberately not a clean multiple of any tile stride

        upscale::TileOptions opts;
        opts.tile_size = 48;
        opts.overlap = 8;
        opts.scale = 4;
        opts.output_scale = 1;

        upscale::ImageRGBA8 out_serial, out_parallel;
        upscale::TileOptions serial_opts = opts;
        serial_opts.num_workers = 1;
        upscale::TileOptions parallel_opts = opts;
        parallel_opts.num_workers = 4;

        bool threw = false;
        try {
            upscale::upscale_tiled(in, out_serial, serial_opts, make_nearest_upscale_fn(4));
            upscale::upscale_tiled(in, out_parallel, parallel_opts, make_nearest_upscale_fn(4));
        } catch (const std::exception& ex) {
            threw = true;
            std::fprintf(stderr, "UNEXPECTED exception on non-square remainder size: %s\n", ex.what());
        }
        check(!threw, "non-square remainder size: must not crash/throw");
        check(out_serial.width == 613 && out_serial.height == 97, "non-square: output size (serial)");
        check(out_parallel.width == 613 && out_parallel.height == 97, "non-square: output size (parallel)");
        check(out_serial.pixels == out_parallel.pixels,
              "non-square remainder size: serial and parallel results are bit-identical (output_scale path)");
    }

    std::printf("test_tile_output_scale: %d checks run, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
