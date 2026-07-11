#include "log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>

namespace ae {

namespace {
std::vector<LogEntry> g_entries;
uint64_t g_version = 0;
double g_qpcToSec = 0.0;
LONGLONG g_start = 0;

double nowSeconds() {
    if (g_qpcToSec == 0.0) {
        LARGE_INTEGER f, s;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&s);
        g_qpcToSec = 1.0 / (double)f.QuadPart;
        g_start = s.QuadPart;
    }
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - g_start) * g_qpcToSec;
}
} // namespace

void logf(LogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LogEntry e;
    e.level = level;
    e.time = nowSeconds();
    e.text = buf;
    // Keep the ring bounded; drop the oldest quarter when full.
    if (g_entries.size() >= 4000)
        g_entries.erase(g_entries.begin(), g_entries.begin() + 1000);
    g_entries.push_back(std::move(e));
    ++g_version;

    std::fprintf(level == LogLevel::Info ? stdout : stderr, "%s%s\n",
                 level == LogLevel::Error ? "[error] " : level == LogLevel::Warn ? "[warn] " : "",
                 buf);
}

const std::vector<LogEntry>& logEntries() { return g_entries; }
uint64_t logVersion() { return g_version; }
void clearLog() { g_entries.clear(); ++g_version; }

} // namespace ae
