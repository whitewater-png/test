// upscale_cli.cpp - Test/utility CLI for the Adobe-independent core.
//
// This binary exists so the core inference + tiling code can be built and
// exercised on any platform (including this Linux/CPU-only dev
// environment, where the Adobe SDK plugin cannot be compiled) without
// needing Premiere Pro or After Effects installed.
//
// Usage:
//   upscale_cli model.onnx input.png output.png [--tile 256] [--overlap 16]
//               [--scale 4] [--jobs N] [--engine ai|detail] [--detail N]
//
// --engine selects the processing engine, mirroring the Premiere/AE
// plugin's "Engine" popup (see plugin/src/plugin/AIUpscale.h/.cpp):
//   ai      (default, for backward compatibility with existing scripts/
//            docs that assume the model.onnx argument is always used) --
//            the original Real-ESRGAN/ONNX Runtime engine.
//   detail  the classical, non-neural Detail Preserve engine (see
//           core/detail_upscaler.h). No model is loaded or required in
//           this mode -- the model.onnx positional argument is still
//           expected syntactically (for simplicity, argument parsing below
//           doesn't special-case argument COUNT on --engine detail) but is
//           never opened/read; pass any placeholder, e.g. "-".
// --detail N (0..100, default 50) sets the Detail Preserve engine's
// unsharp-mask strength; ignored when --engine ai is used.
//
// Error handling: any failure (bad args, missing/corrupt model, missing/
// corrupt image, size-limit violation, inference error) is reported on
// stderr AND logged via upscale::log_error() (see core/logger.h for the
// log file location), and the process exits non-zero. No partial output
// file is ever written on failure.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

// Cap stb_image's own internal decode limits to something sane before
// including it, as defense-in-depth alongside the post-decode dimension
// check below (STBI_MAX_DIMENSIONS bounds width/height stb_image itself
// will accept while parsing the file).
#define STBI_MAX_DIMENSIONS 16384

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "size_limits.h"
#include "logger.h"
#include "onnx_upscaler.h"
#include "detail_upscaler.h"

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s model.onnx input.png output.png [--tile N] [--overlap N] [--scale N] [--jobs N]\n"
        "               [--engine ai|detail] [--detail N]\n"
        "  --tile N     tile edge length in pixels (default: auto-selected, see README)\n"
        "  --overlap N  tile overlap in pixels (default 16)\n"
        "  --scale N    requested output scale factor\n"
        "               (default: model native scale for --engine ai; 4 for --engine detail)\n"
        "  --jobs N     number of worker threads for tile processing (--engine ai only;\n"
        "               --engine detail is single-threaded, no tiling)\n"
        "               (default: 0 = auto, i.e. hardware_concurrency; 1 = serial)\n"
        "  --engine {ai,detail}  processing engine (default: ai, for backward compatibility)\n"
        "               ai     = Real-ESRGAN via ONNX Runtime (needs model.onnx)\n"
        "               detail = classical, non-neural Detail Preserve engine (fast; no\n"
        "                        model needed -- model.onnx argument is accepted but\n"
        "                        ignored, pass any placeholder such as \"-\")\n"
        "  --detail N   Detail Preserve engine strength, 0..100 (default 50; ignored for\n"
        "               --engine ai)\n",
        argv0);
}

int fail(const std::string& context, const std::string& detail) {
    const std::string msg = context + ": " + detail;
    std::fprintf(stderr, "%s\n", msg.c_str());
    upscale::log_error(std::string("upscale_cli: ") + msg);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string model_path = argv[1];
    const std::string input_path = argv[2];
    const std::string output_path = argv[3];

    int tile_size = 0;    // 0 => auto-selected by OnnxUpscaler::upscale()
    int overlap = 16;
    int requested_scale = 0; // 0 => use model's native scale (--engine ai) / see below (--engine detail)
    int jobs = 0;             // 0 => auto (hardware_concurrency); --engine ai only
    std::string engine = "ai"; // "ai" (default, backward-compatible) or "detail"
    double detail_amount = 50.0; // --engine detail only, 0..100

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_int = [&](int& out_val) -> bool {
            if (i + 1 >= argc) return false;
            out_val = std::atoi(argv[++i]);
            return true;
        };
        auto next_double = [&](double& out_val) -> bool {
            if (i + 1 >= argc) return false;
            out_val = std::atof(argv[++i]);
            return true;
        };
        auto next_string = [&](std::string& out_val) -> bool {
            if (i + 1 >= argc) return false;
            out_val = argv[++i];
            return true;
        };
        if (arg == "--tile") {
            if (!next_int(tile_size)) { print_usage(argv[0]); return 1; }
        } else if (arg == "--overlap") {
            if (!next_int(overlap)) { print_usage(argv[0]); return 1; }
        } else if (arg == "--scale") {
            if (!next_int(requested_scale)) { print_usage(argv[0]); return 1; }
        } else if (arg == "--jobs") {
            if (!next_int(jobs)) { print_usage(argv[0]); return 1; }
            if (jobs < 0) { std::fprintf(stderr, "--jobs must be >= 0\n"); return 1; }
        } else if (arg == "--engine") {
            if (!next_string(engine)) { print_usage(argv[0]); return 1; }
            if (engine != "ai" && engine != "detail") {
                std::fprintf(stderr, "--engine must be 'ai' or 'detail' (got '%s')\n", engine.c_str());
                return 1;
            }
        } else if (arg == "--detail") {
            if (!next_double(detail_amount)) { print_usage(argv[0]); return 1; }
            if (detail_amount < 0.0 || detail_amount > 100.0) {
                std::fprintf(stderr, "--detail must be in [0, 100] (got %g)\n", detail_amount);
                return 1;
            }
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    upscale::log_info("upscale_cli: starting, engine=" + engine + " model='" + model_path + "' input='" +
                       input_path + "' output='" + output_path + "' tile=" + std::to_string(tile_size) +
                       " overlap=" + std::to_string(overlap) + " scale=" + std::to_string(requested_scale) +
                       " jobs=" + std::to_string(jobs) + " detail=" + std::to_string(detail_amount));

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(input_path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        return fail("Failed to load input image '" + input_path + "'", stbi_failure_reason());
    }

    // Post-decode validation: reject implausible/oversized decoded images
    // even though stb_image itself already applies STBI_MAX_DIMENSIONS --
    // belt-and-suspenders against a crafted/corrupt PNG header.
    try {
        upscale::validate_input_dims(w, h, 4);
    } catch (const upscale::SizeLimitError& ex) {
        stbi_image_free(data);
        return fail("Input image rejected", ex.what());
    }

    upscale::ImageRGBA8 in;
    in.resize(w, h);
    std::memcpy(in.pixels.data(), data, static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);

    std::printf("Input: %s (%dx%d)\n", input_path.c_str(), w, h);
    std::printf("Engine: %s\n", engine.c_str());

    upscale::ImageRGBA8 out;
    double elapsed_sec = 0.0;

    if (engine == "detail") {
        // Detail Preserve (classical, non-neural) engine -- no model is
        // loaded at all; model_path is accepted purely for argv-shape
        // compatibility (see print_usage()/file header) but never opened.
        // Default requested_scale for this engine is 4 (matching
        // upscale_video.sh's own default and the historical --scale
        // default for the AI engine's model-native-scale behavior);
        // --scale 1 requests same-resolution "detail regeneration" (the
        // mode the Premiere plugin's Detail Preserve engine always uses).
        const int effective_scale = (requested_scale > 0) ? requested_scale : 4;
        const int out_w = w * effective_scale;
        const int out_h = h * effective_scale;
        std::printf("Detail amount: %.1f\n", detail_amount);

        const auto t0 = std::chrono::steady_clock::now();
        try {
            upscale::detail_preserving_upscale(in, out, out_w, out_h, static_cast<float>(detail_amount));
        } catch (const upscale::SizeLimitError& ex) {
            return fail("Size limit error", ex.what());
        } catch (const std::exception& ex) {
            return fail("Unexpected error during upscaling", ex.what());
        }
        const auto t1 = std::chrono::steady_clock::now();
        elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    } else {
        upscale::OnnxUpscaler upscaler;
        try {
            upscaler.load(model_path);
        } catch (const upscale::OnnxUpscalerError& ex) {
            return fail("Model load error", ex.what());
        }
        std::printf("Model: %s (execution provider: %s%s)\n", model_path.c_str(),
                    upscaler.active_provider().c_str(),
                    upscaler.fell_back_to_cpu() ? ", fell back from requested accelerated provider" : "");

        upscale::TileOptions opts;
        opts.tile_size = tile_size; // 0 => auto
        opts.overlap = overlap;
        opts.num_workers = jobs; // 0 => auto

        const auto t0 = std::chrono::steady_clock::now();
        try {
            upscaler.upscale(in, out, requested_scale, opts);
        } catch (const upscale::OnnxUpscalerError& ex) {
            return fail("Inference error", ex.what());
        } catch (const upscale::SizeLimitError& ex) {
            return fail("Size limit error", ex.what());
        } catch (const std::exception& ex) {
            return fail("Unexpected error during upscaling", ex.what());
        }
        const auto t1 = std::chrono::steady_clock::now();
        elapsed_sec = std::chrono::duration<double>(t1 - t0).count();

        std::printf("Model native scale: %dx\n", upscaler.native_scale());
    }

    std::printf("Output: %s (%dx%d)\n", output_path.c_str(), out.width, out.height);
    std::printf("Elapsed: %.3fs (jobs=%d)\n", elapsed_sec, jobs);

    if (!stbi_write_png(output_path.c_str(), out.width, out.height, 4, out.pixels.data(), out.width * 4)) {
        return fail("Failed to write output image", output_path);
    }

    upscale::log_info("upscale_cli: done, output=" + std::to_string(out.width) + "x" + std::to_string(out.height) +
                       " elapsed_sec=" + std::to_string(elapsed_sec));
    std::printf("Done.\n");
    return 0;
}
