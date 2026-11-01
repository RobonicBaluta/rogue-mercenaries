#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

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
    // Keep earlier sessions' logs next to the new one for crash triage: the last one as
    // adapter.prev.log (as before) and the ten before it with their start time in the name.
    if (std::filesystem::exists(file, ec)) {
        auto previous = file;
        previous.replace_extension(".prev.log");
        if (std::filesystem::exists(previous, ec)) {
            const auto stamp = std::filesystem::last_write_time(previous, ec);
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
            auto archived = previous.parent_path() / (L"adapter-" + std::to_wstring(secs) + L".log");
            std::filesystem::rename(previous, archived, ec);
        }
        std::filesystem::rename(file, previous, ec);
        std::vector<std::filesystem::path> archives;
        for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
            const auto name = entry.path().filename().wstring();
            if (name.rfind(L"adapter-", 0) == 0 && entry.path().extension() == L".log") archives.push_back(entry.path());
        }
        std::sort(archives.begin(), archives.end());
        while (archives.size() > 10) {
            std::filesystem::remove(archives.front(), ec);
            archives.erase(archives.begin());
        }
    }
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
