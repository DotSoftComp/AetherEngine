// Aether Engine — engine-wide log: printf-style, mirrored to the console and
// kept in a ring buffer that the editor's Output Log panel renders.
#pragma once
#include <string>
#include <vector>

namespace ae {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    LogLevel level = LogLevel::Info;
    double time = 0.0; // seconds since launch
    std::string text;
};

void logf(LogLevel level, const char* fmt, ...);
const std::vector<LogEntry>& logEntries();
uint64_t logVersion(); // bumps on every append (for auto-scroll detection)
void clearLog();

#define AE_LOG(...)   ::ae::logf(::ae::LogLevel::Info, __VA_ARGS__)
#define AE_WARN(...)  ::ae::logf(::ae::LogLevel::Warn, __VA_ARGS__)
#define AE_ERROR(...) ::ae::logf(::ae::LogLevel::Error, __VA_ARGS__)

} // namespace ae
