#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace rmre5 {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Info;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Info: return "INFO ";
        default: return "DEBUG";
    }
}

}  // namespace

void LogOpen(const std::filesystem::path& file, LogLevel level) {
    std::lock_guard lock(g_mutex);
    g_level = level;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Keep the previous session's log next to the new one for crash triage.
    auto previous = file;
    previous.replace_extension(".prev.log");
    std::filesystem::rename(file, previous, ec);
    g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
}

void LogSetLevel(LogLevel level) {
    std::lock_guard lock(g_mutex);
    g_level = level;
}

void LogClose() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void Log(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) > static_cast<int>(g_level)) {
        return;
    }
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::lock_guard lock(g_mutex);
    if (!g_file) {
        return;
    }
    std::fprintf(g_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [t%05lu] %s\n", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, LevelName(level), GetCurrentThreadId(),
                 message);
    std::fflush(g_file);
}

}  // namespace rmre5
