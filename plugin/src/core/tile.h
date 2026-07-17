// tile.h - Tiled inference helper for large images.
//
// Splits an image into overlapping tiles, runs a caller-supplied upscale
// function on each tile, and blends the results back together using a
// simple linear feather in the overlap region. This bounds peak memory /
// VRAM usage regardless of the input frame size, which matters both for
// the CPU path exercised in this repo and for GPU execution providers
// where VRAM is limited.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace upscale {

// Simple 8-bit RGBA interleaved image, tightly packed (stride == width*4).
struct ImageRGBA8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // size == width*height*4

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    }
};

// Callback signature: given a tile (small ImageRGBA8), produce the
// upscaled tile at a fixed integer scale factor. The callback owns the
// decision of what "scale" means (it is informed by the model), but for
// tiling purposes the caller must know the scale ahead of time so output
// tile geometry can be computed.
using TileUpscaleFn = std::function<void(const ImageRGBA8& tile_in, ImageRGBA8& tile_out)>;

struct TileOptions {
    int tile_size = 256;   // edge length of each (square) input tile, in pixels
    int overlap = 16;      // overlap between adjacent tiles, in pixels
    int scale = 4;         // output/input scale factor applied by upscale_fn
};

// Runs `upscale_fn` over tiles of `input` and composites the results into
// `output` (which is resized to input.width*scale x input.height*scale).
// If the image is smaller than a single tile, this degenerates to a single
// full-image call with no blending overhead.
void upscale_tiled(const ImageRGBA8& input,
                    ImageRGBA8& output,
                    const TileOptions& opts,
                    const TileUpscaleFn& upscale_fn);

} // namespace upscale
