// logger.h - Minimal structured logger for the AI Upscale core.
//
// Single-header, dependency-free logger used by both the CLI and the AE/
// Premiere plugin layer. Design goals:
//   - Structured lines: "<timestamp> [<LEVEL>] [tid=<id>] <message>"
//   - Thread-safe (internal mutex; safe to call from tile worker threads).
//   - Rotates the log file when it exceeds ~5 MB (renames to ".old",
//     overwriting any previous .old, then starts a fresh file) so a long
//     render session never grows the log unboundedly.
//   - Picks a sensible default log path per platform:
//       macOS:            ~/Library/Logs/AIUpscale/AIUpscale.log
//       other (CLI/dev):  $TMPDIR/AIUpscale/AIUpscale.log, falling back to
//                          ./AIUpscale.log if TMPDIR is unset.
//
// Privacy/security note: only log operationally-relevant data (file
// paths of models/inputs the user explicitly supplied, image dimensions,
// execution provider names, tile/thread configuration, exception text).
// Do NOT log environment variables, usernames beyond what's already part
// of a user-supplied path, credentials, or any pixel/image content. Full
// filesystem paths are acceptable (they're necessary for diagnosing
// "wrong model file" type issues) but nothing beyond that should be
// written to the log.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace upscale {

enum class LogLevel { kInfo = 0, kWarn = 1, kError = 2 };

inline const char* log_level_name(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::kInfo: return "INFO";
        case LogLevel::kWarn: return "WARN";
        case LogLevel::kError: return "ERROR";
    }
    return "?";
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // Allows callers (CLI, tests) to redirect logging to a specific path;
    // otherwise the platform default (see file header) is used lazily on
    // first log() call.
    void set_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        path_resolved_ = true;
    }

    std::string path() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_path_locked();
        return path_;
    }

    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_path_locked();
        rotate_if_needed_locked();

        FILE* f = std::fopen(path_.c_str(), "a");
        if (!f) {
            // Last-resort fallback: nowhere sensible to write, so don't
            // crash the render over a logging failure -- just drop it.
            return;
        }

        std::ostringstream tid;
        tid << std::this_thread::get_id();

        char ts[32];
        std::time_t t = std::time(nullptr);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

        std::fprintf(f, "%s [%s] [tid=%s] %s\n", ts, log_level_name(level), tid.str().c_str(), message.c_str());
        std::fclose(f);
    }

private:
    Logger() = default;

    void ensure_path_locked() {
        if (path_resolved_) return;
        path_ = default_path();
        path_resolved_ = true;
    }

    static std::string default_path() {
#if defined(__APPLE__)
        const char* home = std::getenv("HOME");
        if (home && *home) {
            std::string dir = std::string(home) + "/Library/Logs/AIUpscale";
            make_dir_recursive(dir);
            return dir + "/AIUpscale.log";
        }
        return "AIUpscale.log";
#elif defined(_WIN32)
        const char* tmp = std::getenv("TEMP");
        if (!tmp || !*tmp) tmp = std::getenv("TMP");
        if (tmp && *tmp) {
            std::string dir = std::string(tmp) + "\\AIUpscale";
            make_dir_recursive(dir);
            return dir + "\\AIUpscale.log";
        }
        return "AIUpscale.log";
#else
        const char* tmp = std::getenv("TMPDIR");
        if (tmp && *tmp) {
            std::string dir = std::string(tmp);
            if (!dir.empty() && dir.back() == '/') dir.pop_back();
            dir += "/AIUpscale";
            make_dir_recursive(dir);
            return dir + "/AIUpscale.log";
        }
        make_dir_recursive("/tmp/AIUpscale");
        return "/tmp/AIUpscale/AIUpscale.log";
#endif
    }

    static void make_dir_recursive(const std::string& dir) {
#if defined(_WIN32)
        // Best-effort single-level mkdir; parent (TEMP) is assumed to exist.
        _mkdir(dir.c_str());
#else
        // Best-effort single-level mkdir; parent (HOME or TMPDIR) is
        // assumed to exist. mode 0755, ignore EEXIST.
        ::mkdir(dir.c_str(), 0755);
#endif
    }

    void rotate_if_needed_locked() {
        constexpr long kMaxBytes = 5L * 1024 * 1024; // 5 MB
#if defined(_WIN32)
        struct _stat st{};
        if (_stat(path_.c_str(), &st) != 0) return;
        if (st.st_size < kMaxBytes) return;
        std::string old_path = path_ + ".old";
        std::remove(old_path.c_str());
        std::rename(path_.c_str(), old_path.c_str());
#else
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0) return; // file doesn't exist yet: nothing to rotate
        if (st.st_size < kMaxBytes) return;
        std::string old_path = path_ + ".old";
        std::remove(old_path.c_str()); // overwrite any previous .old
        std::rename(path_.c_str(), old_path.c_str());
#endif
    }

    std::mutex mutex_;
    std::string path_;
    bool path_resolved_ = false;
};

inline void log_info(const std::string& msg) { Logger::instance().log(LogLevel::kInfo, msg); }
inline void log_warn(const std::string& msg) { Logger::instance().log(LogLevel::kWarn, msg); }
inline void log_error(const std::string& msg) { Logger::instance().log(LogLevel::kError, msg); }

} // namespace upscale
