#include "tile.h"

#include <algorithm>
#include <cmath>

namespace upscale {

namespace {

// Linear feather weight for a pixel at `dist` pixels from the nearest edge
// of the tile's "core" region (dist < 0 means inside the core, i.e. weight
// 1). `feather` is the overlap width used for blending. Returns a value in
// [0, 1].
float feather_weight(int dist_from_edge, int feather) {
    if (feather <= 0) return 1.0f;
    if (dist_from_edge >= feather) return 1.0f;
    if (dist_from_edge <= 0) return 0.0f;
    return static_cast<float>(dist_from_edge) / static_cast<float>(feather);
}

} // namespace

void upscale_tiled(const ImageRGBA8& input,
                    ImageRGBA8& output,
                    const TileOptions& opts,
                    const TileUpscaleFn& upscale_fn) {
    const int scale = std::max(1, opts.scale);
    const int out_w = input.width * scale;
    const int out_h = input.height * scale;
    output.resize(out_w, out_h);

    // Fast path: image fits in a single tile, skip blending entirely.
    if (input.width <= opts.tile_size && input.height <= opts.tile_size) {
        upscale_fn(input, output);
        return;
    }

    const int tile = std::max(32, opts.tile_size);
    const int overlap = std::clamp(opts.overlap, 0, tile / 2 - 1);
    const int stride = tile - overlap; // step between tile origins, in input space

    // Accumulation buffers in output space (float for correct weighted blend).
    std::vector<float> accum(static_cast<size_t>(out_w) * out_h * 4, 0.0f);
    std::vector<float> weight(static_cast<size_t>(out_w) * out_h, 0.0f);

    for (int ty = 0; ty < input.height; ty += stride) {
        for (int tx = 0; tx < input.width; tx += stride) {
            const int tw = std::min(tile, input.width - tx);
            const int th = std::min(tile, input.height - ty);

            ImageRGBA8 tile_in;
            tile_in.resize(tw, th);
            for (int y = 0; y < th; ++y) {
                const uint8_t* src_row = &input.pixels[(static_cast<size_t>(ty + y) * input.width + tx) * 4];
                uint8_t* dst_row = &tile_in.pixels[static_cast<size_t>(y) * tw * 4];
                std::copy(src_row, src_row + static_cast<size_t>(tw) * 4, dst_row);
            }

            ImageRGBA8 tile_out;
            upscale_fn(tile_in, tile_out);

            // tile_out is expected to be tw*scale x th*scale. Guard against
            // callbacks that used a different scale than opts.scale.
            const int tow = tile_out.width;
            const int toh = tile_out.height;
            const int out_ox = tx * scale;
            const int out_oy = ty * scale;

            for (int y = 0; y < toh; ++y) {
                const int oy = out_oy + y;
                if (oy >= out_h) break;

                // Distance (in input pixels) from this row to the nearest
                // tile edge that actually borders a neighboring tile.
                const int dist_top = ty > 0 ? y / scale : overlap; // no top neighbor -> full weight
                const int dist_bottom = (ty + th) < input.height ? (th - 1 - y / scale) : overlap;
                const float fy = std::min(feather_weight(dist_top, overlap),
                                           feather_weight(dist_bottom, overlap));

                for (int x = 0; x < tow; ++x) {
                    const int ox = out_ox + x;
                    if (ox >= out_w) break;

                    const int dist_left = tx > 0 ? x / scale : overlap;
                    const int dist_right = (tx + tw) < input.width ? (tw - 1 - x / scale) : overlap;
                    const float fx = std::min(feather_weight(dist_left, overlap),
                                               feather_weight(dist_right, overlap));

                    // Combined weight; add a small epsilon floor so seams
                    // between tiles never end up with zero total weight.
                    const float w = std::max(fx * fy, 1e-4f);

                    const size_t out_idx = (static_cast<size_t>(oy) * out_w + ox);
                    const uint8_t* px = &tile_out.pixels[(static_cast<size_t>(y) * tow + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        accum[out_idx * 4 + c] += px[c] * w;
                    }
                    weight[out_idx] += w;
                }
            }
        }
    }

    for (int i = 0; i < out_w * out_h; ++i) {
        const float w = weight[i] > 0.0f ? weight[i] : 1.0f;
        for (int c = 0; c < 4; ++c) {
            float v = accum[static_cast<size_t>(i) * 4 + c] / w;
            v = std::clamp(v, 0.0f, 255.0f);
            output.pixels[static_cast<size_t>(i) * 4 + c] = static_cast<uint8_t>(std::lround(v));
        }
    }
}

} // namespace upscale
