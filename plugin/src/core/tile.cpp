#include "tile.h"

#include "size_limits.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>

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

// One tile job: input-space rectangle plus the (eventually populated)
// upscaled tile. Kept in a flat vector, in the same fixed (ty, tx) scan
// order the original single-threaded implementation used, so the later
// blend pass is 100% order-independent of how the parallel compute phase
// actually interleaved.
struct TileJob {
    int tx = 0, ty = 0, tw = 0, th = 0;
    ImageRGBA8 tile_out;
};

int resolve_num_workers(int requested, size_t num_jobs) {
    if (num_jobs <= 1) return 1;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int workers = requested > 0 ? requested : static_cast<int>(hw);
    workers = std::max(1, workers);
    workers = static_cast<int>(std::min<size_t>(static_cast<size_t>(workers), num_jobs));
    return workers;
}

} // namespace

void upscale_tiled(const ImageRGBA8& input,
                    ImageRGBA8& output,
                    const TileOptions& opts,
                    const TileUpscaleFn& upscale_fn) {
    const int scale = std::max(1, opts.scale);
    // output_scale defaults to `scale` (original behavior: output ==
    // input*scale, the model's native size). When explicitly set smaller
    // than `scale`, the blend/accumulation buffer -- and every per-tile
    // intermediate -- is sized off output_scale instead, so a full-frame
    // buffer at the model's native scale is never allocated. See
    // TileOptions::output_scale's doc comment in tile.h.
    const int output_scale = opts.output_scale > 0 ? opts.output_scale : scale;
    const bool needs_downsize = (output_scale != scale);

    const int64_t out_w64 = static_cast<int64_t>(input.width) * output_scale;
    const int64_t out_h64 = static_cast<int64_t>(input.height) * output_scale;
    // Validate the full output allocation up front (overflow/size-limit
    // check) before touching any memory -- see limits.h. This is now sized
    // off output_scale (the buffer we actually keep for the whole frame),
    // not the model's native scale.
    safe_buffer_bytes(out_w64, out_h64, 4, 1);
    const int out_w = static_cast<int>(out_w64);
    const int out_h = static_cast<int>(out_h64);
    output.resize(out_w, out_h);

    // Fast path: image fits in a single tile, skip blending/threading
    // entirely.
    if (input.width <= opts.tile_size && input.height <= opts.tile_size) {
        if (!needs_downsize) {
            upscale_fn(input, output);
            return;
        }
        // Still only a single native-scale buffer alive at once (this
        // function's `native_out` local, freed on return) -- fine even for
        // a large `input`, since the whole point of output_scale is to
        // avoid a FULL-FRAME native-scale buffer, and here input is by
        // definition <= one tile.
        ImageRGBA8 native_out;
        upscale_fn(input, native_out);
        resize_rgba_bilinear(native_out, output, out_w, out_h);
        return;
    }

    const int tile = std::max(32, opts.tile_size);
    const int overlap = std::clamp(opts.overlap, 0, tile / 2 - 1);
    const int stride = tile - overlap; // step between tile origins, in input space

    // Build the tile job list in the same order the original serial
    // implementation iterated (ty outer, tx inner). This ordering is what
    // makes the final blend step deterministic/order-independent of
    // however many worker threads computed the tiles.
    std::vector<TileJob> jobs;
    for (int ty = 0; ty < input.height; ty += stride) {
        for (int tx = 0; tx < input.width; tx += stride) {
            TileJob job;
            job.tx = tx;
            job.ty = ty;
            job.tw = std::min(tile, input.width - tx);
            job.th = std::min(tile, input.height - ty);
            jobs.push_back(std::move(job));
        }
    }

    const int num_workers = resolve_num_workers(opts.num_workers, jobs.size());
    {
        // Throttled the same way as AIUpscale.cpp's
        // log_render_diag_if_changed() / OnnxUpscaler's tile-size log: only
        // log when the (job count, tile, overlap, workers) combination
        // actually changes. Field logs from real Premiere Pro hardware
        // showed dozens of concurrent render threads logging this line
        // every single frame -- once tiling settles into a steady state
        // (the common case), that's pure log-flood noise.
        static std::mutex log_cache_mutex;
        static size_t last_jobs = static_cast<size_t>(-1);
        static int last_tile = -1, last_overlap = -1, last_workers = -1;

        std::lock_guard<std::mutex> lock(log_cache_mutex);
        if (jobs.size() != last_jobs || tile != last_tile || overlap != last_overlap ||
            num_workers != last_workers) {
            last_jobs = jobs.size();
            last_tile = tile;
            last_overlap = overlap;
            last_workers = num_workers;

            std::ostringstream msg;
            msg << "upscale_tiled: " << jobs.size() << " tile(s), tile=" << tile
                << " overlap=" << overlap << " workers=" << num_workers;
            log_info(msg.str());
        }
    }

    // --- Parallel compute phase -------------------------------------------------
    // Each worker pulls the next unclaimed job index and runs upscale_fn on
    // it. Tiles are independent (each reads only its own slice of `input`
    // and writes only its own TileJob::tile_out), so this is safe as long
    // as upscale_fn itself is safe to call concurrently (see tile.h /
    // onnx_upscaler.cpp for how OnnxUpscaler::infer() satisfies this).
    //
    // Cooperative error handling: if any worker's upscale_fn throws, we
    // flip `stop_flag` so other workers stop picking up new jobs (already
    // in-flight work on other threads still finishes, since aborting
    // mid-inference isn't safe/meaningful), record the first exception,
    // join everyone, then rethrow on the calling thread. No partial output
    // is ever composited in this case.
    std::atomic<size_t> next_job{0};
    std::atomic<bool> stop_flag{false};
    std::mutex error_mutex;
    std::exception_ptr first_error;

    auto worker_fn = [&]() {
        for (;;) {
            if (stop_flag.load(std::memory_order_relaxed)) return;
            const size_t idx = next_job.fetch_add(1, std::memory_order_relaxed);
            if (idx >= jobs.size()) return;

            TileJob& job = jobs[idx];
            try {
                ImageRGBA8 tile_in;
                tile_in.resize(job.tw, job.th);
                for (int y = 0; y < job.th; ++y) {
                    const uint8_t* src_row =
                        &input.pixels[(static_cast<size_t>(job.ty + y) * input.width + job.tx) * 4];
                    uint8_t* dst_row = &tile_in.pixels[static_cast<size_t>(y) * job.tw * 4];
                    std::copy(src_row, src_row + static_cast<size_t>(job.tw) * 4, dst_row);
                }
                if (!needs_downsize) {
                    upscale_fn(tile_in, job.tile_out);
                } else {
                    // Bound the per-tile native-scale intermediate too (see
                    // limits.h) -- tile_size is normally chosen small enough
                    // that tile*scale is nowhere near the limits, but this
                    // guards against a caller-supplied oversized tile_size.
                    // `tile_native` is scoped to this iteration only: it is
                    // freed as soon as it's resampled down below, so peak
                    // memory across the whole tiled run never includes more
                    // than one tile's worth of native-scale data at a time,
                    // regardless of how large `input` (the full frame) is.
                    safe_buffer_bytes(static_cast<int64_t>(job.tw) * scale,
                                       static_cast<int64_t>(job.th) * scale, 4, 1);
                    ImageRGBA8 tile_native;
                    upscale_fn(tile_in, tile_native);
                    resize_rgba_bilinear(tile_native, job.tile_out,
                                          job.tw * output_scale, job.th * output_scale);
                }
            } catch (...) {
                stop_flag.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!first_error) {
                    first_error = std::current_exception();
                }
                return;
            }
        }
    };

    if (num_workers <= 1) {
        worker_fn();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_workers));
        for (int i = 0; i < num_workers; ++i) {
            threads.emplace_back(worker_fn);
        }
        for (auto& t : threads) t.join();
    }

    if (first_error) {
        log_error("upscale_tiled: aborting after worker exception (see rethrown error for details)");
        std::rethrow_exception(first_error);
    }

    // --- Serial blend phase -------------------------------------------------
    // Composited in the same fixed job order every time, independent of
    // num_workers -- this is what guarantees bit-identical output between
    // serial (num_workers=1) and parallel runs.
    std::vector<float> accum(static_cast<size_t>(out_w) * out_h * 4, 0.0f);
    std::vector<float> weight(static_cast<size_t>(out_w) * out_h, 0.0f);

    for (const TileJob& job : jobs) {
        const int tow = job.tile_out.width;
        const int toh = job.tile_out.height;
        const int out_ox = job.tx * output_scale;
        const int out_oy = job.ty * output_scale;

        for (int y = 0; y < toh; ++y) {
            const int oy = out_oy + y;
            if (oy >= out_h) break;

            // Converts a position within job.tile_out (which is in
            // output_scale space) back to an input-space distance from the
            // tile's edge, for feathering purposes -- hence dividing by
            // output_scale here, not `scale` (the model's native scale):
            // job.tile_out's own pixel grid is output_scale-space
            // regardless of what scale the model internally used to
            // produce it.
            const int dist_top = job.ty > 0 ? y / output_scale : overlap;
            const int dist_bottom = (job.ty + job.th) < input.height ? (job.th - 1 - y / output_scale) : overlap;
            const float fy = std::min(feather_weight(dist_top, overlap),
                                       feather_weight(dist_bottom, overlap));

            for (int x = 0; x < tow; ++x) {
                const int ox = out_ox + x;
                if (ox >= out_w) break;

                const int dist_left = job.tx > 0 ? x / output_scale : overlap;
                const int dist_right = (job.tx + job.tw) < input.width ? (job.tw - 1 - x / output_scale) : overlap;
                const float fx = std::min(feather_weight(dist_left, overlap),
                                           feather_weight(dist_right, overlap));

                const float w = std::max(fx * fy, 1e-4f);

                const size_t out_idx = (static_cast<size_t>(oy) * out_w + ox);
                const uint8_t* px = &job.tile_out.pixels[(static_cast<size_t>(y) * tow + x) * 4];
                for (int c = 0; c < 4; ++c) {
                    accum[out_idx * 4 + c] += px[c] * w;
                }
                weight[out_idx] += w;
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

void resize_rgba_bilinear(const ImageRGBA8& in, ImageRGBA8& out, int out_w, int out_h) {
    if (out_w <= 0 || out_h <= 0) {
        out.resize(std::max(0, out_w), std::max(0, out_h));
        return;
    }
    if (in.width <= 0 || in.height <= 0) {
        out.resize(out_w, out_h); // zero-filled by ImageRGBA8::resize()
        return;
    }
    if (in.width == out_w && in.height == out_h) {
        out = in; // identity: exact size match, no resample needed
        return;
    }

    out.resize(out_w, out_h);
    const float sx = static_cast<float>(in.width) / out_w;
    const float sy = static_cast<float>(in.height) / out_h;
    for (int y = 0; y < out_h; ++y) {
        float fy = (y + 0.5f) * sy - 0.5f;
        fy = std::clamp(fy, 0.0f, static_cast<float>(in.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, in.height - 1);
        const float wy = fy - y0;
        for (int x = 0; x < out_w; ++x) {
            float fx = (x + 0.5f) * sx - 0.5f;
            fx = std::clamp(fx, 0.0f, static_cast<float>(in.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, in.width - 1);
            const float wx = fx - x0;

            const size_t i00 = (static_cast<size_t>(y0) * in.width + x0) * 4;
            const size_t i10 = (static_cast<size_t>(y0) * in.width + x1) * 4;
            const size_t i01 = (static_cast<size_t>(y1) * in.width + x0) * 4;
            const size_t i11 = (static_cast<size_t>(y1) * in.width + x1) * 4;
            const size_t dst = (static_cast<size_t>(y) * out_w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = in.pixels[i00 + c] * (1 - wx) + in.pixels[i10 + c] * wx;
                const float bot = in.pixels[i01 + c] * (1 - wx) + in.pixels[i11 + c] * wx;
                const float v = top * (1 - wy) + bot * wy;
                out.pixels[dst + c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
        }
    }
}

int choose_tile_size(const TileSizingParams& params) {
    static const int kCandidates[] = {768, 512, 384, 256, 192, 128, 64};
    const int preferred_default = params.accelerated ? 512 : 256;

    const size_t budget = params.memory_budget_bytes != 0
        ? params.memory_budget_bytes
        : (params.accelerated ? (static_cast<size_t>(8) * 1024 * 1024 * 1024)   // 8 GiB: CoreML on 36GB M4 Max
                               : (static_cast<size_t>(2) * 1024 * 1024 * 1024)); // 2 GiB: conservative CPU default

    // Heuristic peak-memory-per-tile estimate: output tile pixels *
    // channels * sizeof(float) * a generous multiplier standing in for a
    // typical CNN super-resolution model's intermediate activation maps
    // (not an exact accounting -- onnxruntime's real footprint depends on
    // the specific model graph -- but conservative enough to avoid OOM on
    // the target hardware while still preferring large tiles when there's
    // plenty of headroom).
    constexpr int kActivationMemoryMultiplier = 8;
    constexpr int kBytesPerFloat = 4;
    constexpr int kChannels = 4;

    const int scale = std::max(1, params.scale);

    for (int t : kCandidates) {
        if (t > preferred_default) continue; // never exceed the recommended default even if budget allows more
        const int64_t out_edge = static_cast<int64_t>(t) * scale;
        const int64_t per_tile_bytes = out_edge * out_edge * kChannels * kBytesPerFloat * kActivationMemoryMultiplier;
        if (per_tile_bytes > 0 && static_cast<uint64_t>(per_tile_bytes) <= static_cast<uint64_t>(budget)) {
            return t;
        }
    }
    return 64; // smallest safety floor
}

} // namespace upscale
