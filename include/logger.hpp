#pragma once

#include <print>
#include <format>
#include <source_location>
#include <chrono>
#include <ctime>
#include <mutex>

namespace Kaelum {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5,
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    template<typename... Args>
    void log(LogLevel lvl, std::format_string<Args...> fmt, Args&&... args,
             std::source_location loc = std::source_location::current()) {
        if (lvl < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&time_t));

        const char* level_str = "";
        const char* color_start = "";
        const char* color_end = "\033[0m";

        switch (lvl) {
            case LogLevel::Trace: level_str = "TRC"; color_start = "\033[90m"; break;
            case LogLevel::Debug: level_str = "DBG"; color_start = "\033[36m"; break;
            case LogLevel::Info:  level_str = "INF"; color_start = "\033[32m"; break;
            case LogLevel::Warn:  level_str = "WRN"; color_start = "\033[33m"; break;
            case LogLevel::Error: level_str = "ERR"; color_start = "\033[31m"; break;
            default: break;
        }

        std::print(stderr, "{}{} {:03d} [{}:{}] ", color_start, time_buf, ms.count(),
                   loc.file_name(), loc.line());
        std::print(stderr, fmt, std::forward<Args>(args)...);
        std::print(stderr, "{}\n", color_end);
    }

    template<typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current()) {
        log(LogLevel::Trace, fmt, std::forward<Args>(args)..., loc);
    }

    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current()) {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)..., loc);
    }

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args,
              std::source_location loc = std::source_location::current()) {
        log(LogLevel::Info, fmt, std::forward<Args>(args)..., loc);
    }

    template<typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args,
              std::source_location loc = std::source_location::current()) {
        log(LogLevel::Warn, fmt, std::forward<Args>(args)..., loc);
    }

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current()) {
        log(LogLevel::Error, fmt, std::forward<Args>(args)..., loc);
    }

private:
    Logger() : level_(LogLevel::Trace) {}
    LogLevel level_;
    std::mutex mutex_;
};

#define KAELUM_TRACE(...) Kaelum::Logger::instance().trace(__VA_ARGS__)
#define KAELUM_DEBUG(...) Kaelum::Logger::instance().debug(__VA_ARGS__)
#define KAELUM_INFO(...)  Kaelum::Logger::instance().info(__VA_ARGS__)
#define KAELUM_WARN(...)  Kaelum::Logger::instance().warn(__VA_ARGS__)
#define KAELUM_ERROR(...) Kaelum::Logger::instance().error(__VA_ARGS__)

} // namespace Kaelum
