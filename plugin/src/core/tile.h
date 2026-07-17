// tile.h - Tiled inference helper for large images.
//
// Splits an image into overlapping tiles, runs a caller-supplied upscale
// function on each tile (optionally across a worker thread pool), and
// blends the results back together using a simple linear feather in the
// overlap region. This bounds peak memory / VRAM usage regardless of the
// input frame size, which matters both for the CPU path exercised in this
// repo and for GPU/ANE execution providers where memory is shared or
// otherwise constrained.
//
// Parallelism note: tile *compute* (the upscale_fn callback -- pre/post
// processing plus, depending on the execution provider, inference itself)
// runs across up to `opts.num_workers` threads. The blend/composite step
// that writes each tile's result into the shared output accumulation
// buffer always runs single-threaded, in the same fixed (ty, tx) order
// regardless of worker count or completion order. This makes
// upscale_tiled()'s output bit-identical whether num_workers is 1 or N --
// only the wall-clock time changes -- which is required for the
// serial-vs-parallel correctness check described in plugin/README.md.
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
//
// Thread-safety requirement: this callback MUST be safe to call
// concurrently from up to `opts.num_workers` threads when parallel tiling
// is enabled (see TileOptions::num_workers). OnnxUpscaler::infer()
// satisfies this (see onnx_upscaler.cpp for how it serializes the actual
// Ort::Session::Run() call for non-CPU execution providers while still
// parallelizing pre/post-processing).
using TileUpscaleFn = std::function<void(const ImageRGBA8& tile_in, ImageRGBA8& tile_out)>;

struct TileOptions {
    int tile_size = 256;   // edge length of each (square) input tile, in pixels
    int overlap = 16;      // overlap between adjacent tiles, in pixels
    int scale = 4;         // output/input scale factor applied by upscale_fn

    // Number of worker threads used to compute tiles concurrently.
    //   0 => auto: std::thread::hardware_concurrency(), capped to the
    //        number of tiles in the image (never spin up more threads
    //        than there is work).
    //   1 => fully serial (also the automatic behavior when there's only
    //        one tile, or on the fast path where the whole image fits in
    //        a single tile).
    //  >1 => use exactly this many workers (still capped to tile count).
    int num_workers = 0;
};

// Runs `upscale_fn` over tiles of `input` and composites the results into
// `output` (which is resized to input.width*scale x input.height*scale).
// If the image is smaller than a single tile, this degenerates to a single
// full-image call with no blending overhead.
//
// Throws whatever upscale_fn throws (e.g. upscale::OnnxUpscalerError) --
// if any worker's call to upscale_fn throws, all other in-flight/pending
// tile jobs are stopped cooperatively (via an atomic flag) and the first
// exception encountered is rethrown from this function on the calling
// thread. Partial/garbage output is never returned to the caller.
void upscale_tiled(const ImageRGBA8& input,
                    ImageRGBA8& output,
                    const TileOptions& opts,
                    const TileUpscaleFn& upscale_fn);

// ---------------------------------------------------------------------------
// Tile-size auto-selection.
// ---------------------------------------------------------------------------

// Inputs to choose_tile_size(): the target output geometry and whether the
// active execution provider gets meaningfully more memory/compute headroom
// (CoreML/CUDA/DirectML) vs. plain CPU.
struct TileSizingParams {
    int64_t input_width = 0;
    int64_t input_height = 0;
    int scale = 4;                       // model's native scale factor
    bool accelerated = false;            // true for CoreML/CUDA/DirectML, false for CPU
    size_t memory_budget_bytes = 0;      // 0 => auto (see choose_tile_size())
};

// Picks a default tile edge length (pixels, input space) sized for the
// target machine's memory budget. This is a heuristic estimate, not an
// exact accounting of onnxruntime's internal activation memory -- it
// multiplies the *output* tile's pixel footprint by a generous constant
// factor to approximate peak intermediate-activation memory for a typical
// CNN-based super-resolution model, and picks the largest candidate tile
// size that stays under budget.
//
// Defaults (when memory_budget_bytes == 0): 8 GiB budget when
// `accelerated` is true (CoreML on a 36 GB unified-memory M4 Max has
// ample headroom for larger tiles -> fewer seams, less overhead), 2 GiB
// when running on plain CPU (conservative, since this repo's CPU dev path
// may run on much smaller machines too). These translate to a preferred
// default of 512px tiles when accelerated, 256px on CPU, stepping down
// automatically if the estimated memory would exceed budget.
int choose_tile_size(const TileSizingParams& params);

} // namespace upscale
