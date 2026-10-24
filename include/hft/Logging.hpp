#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace hft::logging {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

[[nodiscard]] inline Level level_from_string(std::string_view value,
                                             Level fallback = Level::Info) {
    if (value == "trace") return Level::Trace;
    if (value == "debug") return Level::Debug;
    if (value == "info") return Level::Info;
    if (value == "warn" || value == "warning") return Level::Warn;
    if (value == "error") return Level::Error;
    if (value == "off") return Level::Off;
    return fallback;
}

[[nodiscard]] inline std::string_view level_name(Level level) {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
    }
    return "info";
}

inline std::string escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    return result;
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(Level level) noexcept { level_ = level; }
    [[nodiscard]] Level level() const noexcept { return level_; }

    void set_stream(std::ostream& stream) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        stream_ = &stream;
    }

    void log(Level level, std::string_view component, std::string_view message) {
        if (level < level_ || level_ == Level::Off) {
            return;
        }
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const std::int64_t timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        const std::lock_guard<std::mutex> lock(mutex_);
        *stream_ << "{\"ts_ns\":" << timestamp_ns
                 << ",\"level\":\"" << level_name(level)
                 << "\",\"component\":\"" << escape(component)
                 << "\",\"message\":\"" << escape(message) << "\"}\n";
        stream_->flush();
    }

private:
    Logger() = default;
    Level level_{Level::Info};
    std::ostream* stream_{&std::cerr};
    std::mutex mutex_;
};

inline void log(Level level, std::string_view component, std::string_view message) {
    Logger::instance().log(level, component, message);
}

inline void trace(std::string_view component, std::string_view message) {
    log(Level::Trace, component, message);
}
inline void debug(std::string_view component, std::string_view message) {
    log(Level::Debug, component, message);
}
inline void info(std::string_view component, std::string_view message) {
    log(Level::Info, component, message);
}
inline void warn(std::string_view component, std::string_view message) {
    log(Level::Warn, component, message);
}
inline void error(std::string_view component, std::string_view message) {
    log(Level::Error, component, message);
}

} // namespace hft::logging
