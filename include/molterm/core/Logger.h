#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace molterm {

enum class LogLevel { Debug, Info, Warn, Error };

// Structured file logger — writes to ~/.molterm/molterm.log
// Thread-safe, singleton pattern. No-op if file cannot be opened.
class Logger {
public:
    static Logger& instance();

    void open();   // open/create log file (call once at startup)
    void close();  // flush and close

    void setLevel(LogLevel lvl) { level_ = lvl; }
    LogLevel level() const { return level_; }

    void log(LogLevel lvl, const char* fmt, ...);

    // Convenience wrappers
    void debug(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    void vlog(LogLevel lvl, const char* fmt, va_list args);

    FILE* file_ = nullptr;
    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
};

// Macros for convenience (compile to nothing in release if desired)
#define MLOG_DEBUG(...) ::molterm::Logger::instance().debug(__VA_ARGS__)
#define MLOG_INFO(...)  ::molterm::Logger::instance().info(__VA_ARGS__)
#define MLOG_WARN(...)  ::molterm::Logger::instance().warn(__VA_ARGS__)
#define MLOG_ERROR(...) ::molterm::Logger::instance().error(__VA_ARGS__)

} // namespace molterm
