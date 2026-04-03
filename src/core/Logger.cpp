#include "molterm/core/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <cstring>

namespace molterm {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    close();
}

void Logger::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) return;

    // Ensure ~/.molterm/ exists
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::string dir = std::string(home) + "/.molterm";
    std::filesystem::create_directories(dir);

    std::string path = dir + "/molterm.log";
    file_ = std::fopen(path.c_str(), "a");
    if (file_) {
        // Write session separator
        std::time_t now = std::time(nullptr);
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(file_, "\n── MolTerm session started %s ──\n", timeBuf);
        std::fflush(file_);
    }
}

void Logger::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::time_t now = std::time(nullptr);
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(file_, "── MolTerm session ended %s ──\n", timeBuf);
        std::fclose(file_);
        file_ = nullptr;
    }
}

static const char* levelStr(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void Logger::vlog(LogLevel lvl, const char* fmt, va_list args) {
    if (lvl < level_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;

    std::time_t now = std::time(nullptr);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&now));

    std::fprintf(file_, "[%s] %s  ", timeBuf, levelStr(lvl));
    std::vfprintf(file_, fmt, args);
    std::fprintf(file_, "\n");
    std::fflush(file_);
}

void Logger::log(LogLevel lvl, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(lvl, fmt, args);
    va_end(args);
}

void Logger::debug(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vlog(LogLevel::Debug, fmt, args); va_end(args);
}

void Logger::info(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vlog(LogLevel::Info, fmt, args); va_end(args);
}

void Logger::warn(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vlog(LogLevel::Warn, fmt, args); va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vlog(LogLevel::Error, fmt, args); va_end(args);
}

} // namespace molterm
