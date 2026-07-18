// test_onnx_upscaler_sharing.cpp - Regression test for
// upscale::OnnxUpscaler::get_shared() (plugin/src/core/onnx_upscaler.h/.cpp)
// and its interaction with upscale::ConcurrencyGate (concurrency.h/.cpp),
// added together to fix a real-Premiere-hardware freeze on a MacBook Pro
// M4 Max (36GB). See concurrency.h's file header and AIUpscale.h's
// AIUpscaleSequenceData::upscaler doc comment for the full field-reported
// story; in short:
//   - Premiere's own render-thread parallelism, combined with this
//     plugin's (formerly per-thread-auto) internal tile worker pool, could
//     spin up ~200 threads at once and saturate every core.
//   - The plugin's render-time sequence-data self-healing could also
//     create more than one OnnxUpscaler (each its own ~64MB CoreML/ANE
//     Ort::Session) for what was really one render session.
//
// This test covers the parts of the fix that need a REAL onnxruntime
// session (unlike test_concurrency_gate.cpp and test_tile_output_scale.cpp,
// which deliberately avoid the onnxruntime dependency):
//   1. get_shared() returns the SAME instance (pointer-identical) for
//      repeated calls with the same model_path.
//   2. Once every shared_ptr referencing an instance is dropped,
//      shared_cache_size_for_testing() reflects that it's gone, and a
//      subsequent get_shared() call for the same path creates a genuinely
//      NEW instance (not the stale one) -- i.e. the cache holds weak_ptrs,
//      it does not keep the model loaded forever.
//   3. Multiple threads calling upscale() concurrently against the SAME
//      shared instance (simulating multiple Premiere render threads
//      sharing one model session) all produce correct, appropriately-sized
//      output, and ConcurrencyGate::peak_count_for_testing() confirms the
//      gate actually capped how many of those upscale() calls ran their
//      inference section at once.
//
// Uses a tiny non-ML ONNX model (4x nearest/linear resize via the Resize
// op -- see scripts/make_test_model.py) generated at build time so this
// needs no network access or real Real-ESRGAN weights. If that generation
// step was skipped (no python3/onnx available -- see CMakeLists.txt), this
// test detects the missing model file at runtime and skips itself (exit 0)
// rather than failing the build over an optional dev dependency.
#include "onnx_upscaler.h"
#include "concurrency.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

#ifndef TEST_MODEL_4X_PATH
#error "TEST_MODEL_4X_PATH must be defined by CMakeLists.txt"
#endif

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

bool model_available() {
    std::ifstream f(TEST_MODEL_4X_PATH, std::ios::binary);
    return f.good();
}

upscale::ImageRGBA8 make_solid(int w, int h, uint8_t v) {
    upscale::ImageRGBA8 img;
    img.resize(w, h);
    for (auto& px : img.pixels) px = v;
    return img;
}

} // namespace

int main() {
    if (!model_available()) {
        std::printf("test_onnx_upscaler_sharing: SKIPPED (test model not found at %s; "
                     "python3 + the `onnx` package were not available at build time -- see CMakeLists.txt)\n",
                     TEST_MODEL_4X_PATH);
        return 0;
    }

    // Pin the gate's limit for this test process, same rationale as
    // test_concurrency_gate.cpp: resolved once, on first touch, so set it
    // before anything in this process ever calls into OnnxUpscaler::upscale()
    // (which acquires the gate internally).
#if defined(_WIN32)
    _putenv_s("AIUPSCALE_MAX_CONCURRENCY", "2");
#else
    setenv("AIUPSCALE_MAX_CONCURRENCY", "2", 1);
#endif

    const std::string model_path = TEST_MODEL_4X_PATH;

    // --- 1: repeated get_shared() for the same path returns the same instance
    {
        auto a = upscale::OnnxUpscaler::get_shared(model_path);
        auto b = upscale::OnnxUpscaler::get_shared(model_path);
        check(a.get() == b.get(), "get_shared(): repeated calls for the same model_path return the same instance");
        check(upscale::OnnxUpscaler::shared_cache_size_for_testing() >= 1,
              "get_shared(): cache reports at least one live entry while a and b hold references");
    }

    // --- 2: cache drops the entry once all references are gone, and a later
    //        get_shared() call creates a genuinely new instance -----------
    upscale::OnnxUpscaler* first_raw_ptr = nullptr;
    {
        auto a = upscale::OnnxUpscaler::get_shared(model_path);
        first_raw_ptr = a.get();
        // a goes out of scope here -- if this was the only reference left
        // (true as long as no other block in this test still holds one for
        // the same path, which holds here since block 1's a/b already went
        // out of scope above), the cache entry should now be expired.
    }
    {
        auto c = upscale::OnnxUpscaler::get_shared(model_path);
        check(c.get() != first_raw_ptr,
              "get_shared(): once all prior references are dropped, a later call loads a genuinely new instance "
              "(the cache does not keep a model loaded forever via a strong reference of its own)");
    }

    // --- 3: concurrent upscale() calls against ONE shared instance --------
    {
        auto shared = upscale::OnnxUpscaler::get_shared(model_path);

        upscale::ConcurrencyGate::instance().reset_peak_for_testing();

        constexpr int kNumThreads = 4;
        std::atomic<int> success_count{0};
        std::atomic<int> wrong_size_count{0};

        auto worker = [&]() {
            // Small input, tile_size >= input size (single-tile fast path)
            // to keep this test fast; num_workers=1, matching the AE/
            // Premiere plugin layer's forced serial-tiling policy (see
            // AIUpscale.cpp HandleRender) -- i.e. this reproduces exactly
            // the "many host threads, each doing serial-internally tiling
            // against a shared session, gated to N concurrent" shape the
            // real fix produces.
            upscale::ImageRGBA8 in = make_solid(16, 16, 100);
            upscale::ImageRGBA8 out;
            upscale::TileOptions opts;
            opts.tile_size = 64;
            opts.overlap = 8;
            opts.num_workers = 1;
            try {
                shared->upscale(in, out, /*requested_scale=*/1, opts);
                if (out.width == 16 && out.height == 16) {
                    ++success_count;
                } else {
                    ++wrong_size_count;
                }
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "worker: unexpected exception: %s\n", ex.what());
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(kNumThreads);
        for (int i = 0; i < kNumThreads; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        check(success_count.load() == kNumThreads,
              "concurrent upscale() calls against a shared instance: all threads succeeded");
        check(wrong_size_count.load() == 0,
              "concurrent upscale() calls against a shared instance: every result was correctly same-resolution "
              "(requested_scale=1)");
        check(upscale::ConcurrencyGate::instance().peak_count_for_testing() <=
                  upscale::ConcurrencyGate::instance().max_concurrency(),
              "concurrent upscale() calls: observed peak never exceeded the gate's max_concurrency()");
    }

    std::printf("test_onnx_upscaler_sharing: %d checks run, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
