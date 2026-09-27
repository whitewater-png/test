#include "concurrency.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>

#include "logger.h"

namespace upscale {

namespace {

int compute_default_max_concurrency(unsigned hw) {
    // Conservative default: 2 concurrent inference slots. On the M4 Max
    // target machine (14 cores) this leaves the bulk of the machine free
    // for Premiere's own render/UI threads and the rest of the OS, which
    // is the whole point -- see concurrency.h's file header for the
    // freeze this is fixing. Never exceed hardware_concurrency (relevant
    // on a small CI/dev VM).
    return static_cast<int>(std::min<unsigned>(hw, 2u));
}

int resolve_max_concurrency() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4; // hardware_concurrency() is allowed to return 0 when undetectable

    const char* env = std::getenv("AIUPSCALE_MAX_CONCURRENCY");
    if (env && *env) {
        char* end = nullptr;
        const long val = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && val >= 1) {
            const int clamped = static_cast<int>(std::min<long>(val, static_cast<long>(hw)));
            log_info("ConcurrencyGate: AIUPSCALE_MAX_CONCURRENCY='" + std::string(env) +
                      "' -> using max_concurrency=" + std::to_string(clamped) +
                      " (hardware_concurrency=" + std::to_string(hw) + ")");
            return clamped;
        }
        log_warn("ConcurrencyGate: ignoring invalid AIUPSCALE_MAX_CONCURRENCY value '" + std::string(env) +
                  "' (must be an integer >= 1); using default instead.");
    }

    const int def = compute_default_max_concurrency(hw);
    log_info("ConcurrencyGate: using default max_concurrency=" + std::to_string(def) +
              " (hardware_concurrency=" + std::to_string(hw) +
              "; set AIUPSCALE_MAX_CONCURRENCY=N to override, e.g. 1 on a machine that still struggles)");
    return def;
}

} // namespace

ConcurrencyGate& ConcurrencyGate::instance() {
    static ConcurrencyGate gate;
    return gate;
}

ConcurrencyGate::ConcurrencyGate() : max_concurrency_(std::max(1, resolve_max_concurrency())) {}

void ConcurrencyGate::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return in_use_ < max_concurrency_; });
    ++in_use_;
    peak_ = std::max(peak_, in_use_);
}

void ConcurrencyGate::release() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --in_use_;
    }
    cv_.notify_one();
}

int ConcurrencyGate::current_count_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_use_;
}

int ConcurrencyGate::peak_count_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_;
}

void ConcurrencyGate::reset_peak_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    peak_ = in_use_;
}

ConcurrencyGate::Guard::Guard(ConcurrencyGate& gate) : gate_(gate) {
    gate_.acquire();
}

ConcurrencyGate::Guard::~Guard() {
    gate_.release();
}

} // namespace upscale
