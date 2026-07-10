#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <cstdio>
#include <string>
#include <format>
#include <source_location>

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
    void log(LogLevel lvl, std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&time_t));

        const char* color_start = "";
        const char* color_end = "\033[0m";

        switch (lvl) {
            case LogLevel::Trace: color_start = "\033[90m"; break;
            case LogLevel::Debug: color_start = "\033[36m"; break;
            case LogLevel::Info:  color_start = "\033[32m"; break;
            case LogLevel::Warn:  color_start = "\033[33m"; break;
            case LogLevel::Error: color_start = "\033[31m"; break;
            default: break;
        }

        std::string msg = std::format(fmt, std::forward<Args>(args)...);
        std::fprintf(stderr, "%s%s %03ld [%s:%d] %s%s\n", 
                     color_start, time_buf, ms.count(),
                     loc.file_name(), loc.line(),
                     msg.c_str(), color_end);
    }

    template<typename... Args>
    void trace(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Trace, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Debug, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Info, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, loc, fmt, std::forward<Args>(args)...);
    }

private:
    Logger() : level_(LogLevel::Trace) {}
    LogLevel level_;
    std::mutex mutex_;
};

#define KAELUM_TRACE(fmt, ...) Kaelum::Logger::instance().trace(std::source_location::current(), fmt, ##__VA_ARGS__)
#define KAELUM_DEBUG(fmt, ...) Kaelum::Logger::instance().debug(std::source_location::current(), fmt, ##__VA_ARGS__)
#define KAELUM_INFO(fmt, ...)  Kaelum::Logger::instance().info(std::source_location::current(), fmt, ##__VA_ARGS__)
#define KAELUM_WARN(fmt, ...)  Kaelum::Logger::instance().warn(std::source_location::current(), fmt, ##__VA_ARGS__)
#define KAELUM_ERROR(fmt, ...) Kaelum::Logger::instance().error(std::source_location::current(), fmt, ##__VA_ARGS__)

} // namespace Kaelum