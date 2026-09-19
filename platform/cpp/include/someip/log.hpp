// Graded stderr logger (P1, mirrors vsomeip logging levels).
//
// Levels: debug < info < warn < error. A process-wide default level is taken
// from SOMEIP_LOG_LEVEL (debug|info|warn|error, default info) and can be
// overridden per logger. Writes go to stderr so stdout stays machine-readable.
#ifndef SOMEIP_LOG_HPP
#define SOMEIP_LOG_HPP

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace someip {
namespace log {

enum class Level { Debug = 10, Info = 20, Warn = 30, Error = 40 };

inline Level level_from_name(const std::string &name) {
    if (name == "debug") return Level::Debug;
    if (name == "warn") return Level::Warn;
    if (name == "error") return Level::Error;
    return Level::Info;
}

inline Level default_level() {
    const char *env = std::getenv("SOMEIP_LOG_LEVEL");
    return env ? level_from_name(env) : Level::Info;
}

class Logger {
public:
    explicit Logger(std::string component, Level level = default_level())
        : component_(std::move(component)), level_(level) {}

    void set_level(Level level) { level_ = level; }
    Level level() const { return level_; }

    void log(Level level, const char *fmt, ...) {
        if (level < level_) {
            return;
        }
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        const char *name = "info";
        switch (level) {
            case Level::Debug: name = "debug"; break;
            case Level::Info: name = "info"; break;
            case Level::Warn: name = "warn"; break;
            case Level::Error: name = "error"; break;
        }
        const time_t now = std::time(nullptr);
        struct tm tmv {};
        localtime_r(&now, &tmv);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        std::lock_guard<std::mutex> lk(mu_);
        std::fprintf(stderr, "[%s][%s][%s] %s\n", name, ts,
                     component_.c_str(), buf);
        std::fflush(stderr);
    }

    void debug(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        logv(Level::Debug, fmt, ap);
        va_end(ap);
    }
    void info(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        logv(Level::Info, fmt, ap);
        va_end(ap);
    }
    void warn(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        logv(Level::Warn, fmt, ap);
        va_end(ap);
    }
    void error(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        logv(Level::Error, fmt, ap);
        va_end(ap);
    }

private:
    void logv(Level level, const char *fmt, va_list ap) {
        if (level < level_) {
            return;
        }
        char buf[1024];
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        log(level, "%s", buf);
    }
    std::string component_;
    Level level_;
    static std::mutex mu_;
};

inline std::mutex Logger::mu_;

}  // namespace log
}  // namespace someip

#endif  // SOMEIP_LOG_HPP