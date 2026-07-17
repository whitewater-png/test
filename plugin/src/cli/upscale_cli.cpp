// upscale_cli.cpp - Test/utility CLI for the Adobe-independent core.
//
// This binary exists so the core inference + tiling code can be built and
// exercised on any platform (including this Linux/CPU-only dev
// environment, where the Adobe SDK plugin cannot be compiled) without
// needing Premiere Pro or After Effects installed.
//
// Usage:
//   upscale_cli model.onnx input.png output.png [--tile 256] [--overlap 16] [--scale 4]
#include <cstdio>
#include <cstring>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "onnx_upscaler.h"

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s model.onnx input.png output.png [--tile N] [--overlap N] [--scale N]\n"
        "  --tile N     tile edge length in pixels (default 256)\n"
        "  --overlap N  tile overlap in pixels (default 16)\n"
        "  --scale N    requested output scale factor (default: model native scale)\n",
        argv0);
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

    int tile_size = 256;
    int overlap = 16;
    int requested_scale = 0; // 0 => use model's native scale

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_int = [&](int& out_val) -> bool {
            if (i + 1 >= argc) return false;
            out_val = std::atoi(argv[++i]);
            return true;
        };
        if (arg == "--tile") {
            if (!next_int(tile_size)) { print_usage(argv[0]); return 1; }
        } else if (arg == "--overlap") {
            if (!next_int(overlap)) { print_usage(argv[0]); return 1; }
        } else if (arg == "--scale") {
            if (!next_int(requested_scale)) { print_usage(argv[0]); return 1; }
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(input_path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        std::fprintf(stderr, "Failed to load input image '%s': %s\n", input_path.c_str(), stbi_failure_reason());
        return 1;
    }

    upscale::ImageRGBA8 in;
    in.resize(w, h);
    std::memcpy(in.pixels.data(), data, static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);

    std::printf("Input: %s (%dx%d)\n", input_path.c_str(), w, h);

    upscale::OnnxUpscaler upscaler;
    try {
        upscaler.load(model_path);
    } catch (const upscale::OnnxUpscalerError& ex) {
        std::fprintf(stderr, "Model load error: %s\n", ex.what());
        return 1;
    }
    std::printf("Model: %s (execution provider: %s)\n", model_path.c_str(), upscaler.active_provider().c_str());

    upscale::TileOptions opts;
    opts.tile_size = tile_size;
    opts.overlap = overlap;

    upscale::ImageRGBA8 out;
    try {
        upscaler.upscale(in, out, requested_scale, opts);
    } catch (const upscale::OnnxUpscalerError& ex) {
        std::fprintf(stderr, "Inference error: %s\n", ex.what());
        return 1;
    }

    std::printf("Model native scale: %dx\n", upscaler.native_scale());
    std::printf("Output: %s (%dx%d)\n", output_path.c_str(), out.width, out.height);

    if (!stbi_write_png(output_path.c_str(), out.width, out.height, 4, out.pixels.data(), out.width * 4)) {
        std::fprintf(stderr, "Failed to write output image '%s'\n", output_path.c_str());
        return 1;
    }

    std::printf("Done.\n");
    return 0;
}
