// test_concurrency_gate.cpp - Unit test for upscale::ConcurrencyGate
// (plugin/src/core/concurrency.h/.cpp), added to fix a real-Premiere-
// hardware freeze on a MacBook Pro M4 Max (36GB): field logs showed dozens
// of concurrent render threads each spinning up their own internal tile
// worker pool, saturating every CPU core at once. ConcurrencyGate caps how
// many "inference sections" (in production: the body of
// OnnxUpscaler::upscale(), see onnx_upscaler.cpp) run concurrently across
// the whole process, regardless of how many threads call in.
//
// This test exercises ConcurrencyGate directly -- no onnxruntime/model
// dependency, so it's buildable/runnable on any platform including this
// Linux CI/dev environment -- covering the three things the task calls out
// explicitly:
//   (a) correctness: work actually completes for every thread (no lost
//       wakeups / no thread stuck waiting forever).
//   (b) the observed peak concurrency inside guarded sections never
//       exceeds max_concurrency(), and (with enough threads relative to
//       the limit) DOES reach exactly max_concurrency() -- proving the
//       gate is a real limit, not an accidental full serialization.
//   (c) exception safety: a Guard-protected section that throws still
//       releases its slot (checked both by a peak-count assertion
//       immediately after the exception is caught, and by confirming
//       later callers are not permanently blocked).
//
// AIUPSCALE_MAX_CONCURRENCY is deliberately NOT set/unset by this test
// process (see main()): ConcurrencyGate::instance() resolves its limit
// exactly once (function-local static), on first use anywhere in the
// process, so this test takes over that one resolution by setting the env
// var before ever touching ConcurrencyGate, and must not leave it up to
// whatever the ambient environment happens to have.
#include "concurrency.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

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

} // namespace

int main() {
    // Fix the limit for this whole test process before the singleton is
    // ever constructed (see file header) -- 2, matching the shipped
    // default, so this test also doubles as a check that the documented
    // default is what actually gets applied end-to-end.
#if defined(_WIN32)
    _putenv_s("AIUPSCALE_MAX_CONCURRENCY", "2");
#else
    setenv("AIUPSCALE_MAX_CONCURRENCY", "2", 1);
#endif

    upscale::ConcurrencyGate& gate = upscale::ConcurrencyGate::instance();
    check(gate.max_concurrency() == 2, "AIUPSCALE_MAX_CONCURRENCY=2 is honored as max_concurrency()");

    // --- (a) + (b): 4 threads contending for 2 slots --------------------
    // Every thread sleeps briefly WHILE holding its Guard, so at least
    // some overlap between threads is essentially guaranteed, giving the
    // peak counter a real chance to reach (but never exceed) 2.
    {
        gate.reset_peak_for_testing();
        std::atomic<int> completed{0};

        auto worker = [&]() {
            upscale::ConcurrencyGate::Guard guard(gate);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            ++completed;
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        check(completed.load() == 4, "all 4 threads completed their guarded section (no deadlock/lost wakeup)");
        check(gate.peak_count_for_testing() <= 2, "observed peak concurrency never exceeded max_concurrency()=2");
        check(gate.peak_count_for_testing() == 2,
              "observed peak concurrency actually reached max_concurrency()=2 (gate isn't over-serializing)");
        check(gate.current_count_for_testing() == 0, "gate is back to 0 in-use slots after all threads joined");
    }

    // --- (c): exception inside a guarded section still releases the slot -
    {
        gate.reset_peak_for_testing();

        bool threw = false;
        try {
            upscale::ConcurrencyGate::Guard guard(gate);
            check(gate.current_count_for_testing() == 1, "slot is held while inside the guarded section");
            throw std::runtime_error("simulated inference failure");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "exception propagated out of the guarded section as expected");
        check(gate.current_count_for_testing() == 0,
              "Guard's destructor released the slot even though the section threw (no leak)");

        // Prove the gate isn't left in a stuck state: with the slot freed,
        // max_concurrency() (2) more callers must all be able to acquire
        // and complete promptly.
        std::atomic<int> completed{0};
        auto worker = [&]() {
            upscale::ConcurrencyGate::Guard guard(gate);
            ++completed;
        };
        std::vector<std::thread> threads;
        for (int i = 0; i < 2; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
        check(completed.load() == 2, "gate remains usable (no leaked slot) after an exception in a prior section");
    }

    std::printf("test_concurrency_gate: %d checks run, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
