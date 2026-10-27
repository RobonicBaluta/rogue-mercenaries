#pragma once

// Adapter diagnostic log (plain text, one line per call). Gameplay telemetry
// goes through the core JSONL sink; this file is for loader/hook/engine facts.

#include <filesystem>

namespace rmre5 {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void LogOpen(const std::filesystem::path& file, LogLevel level);
void LogSetLevel(LogLevel level);
void LogClose();
void Log(LogLevel level, const char* fmt, ...);

#define RM_LOGE(...) ::rmre5::Log(::rmre5::LogLevel::Error, __VA_ARGS__)
#define RM_LOGW(...) ::rmre5::Log(::rmre5::LogLevel::Warn, __VA_ARGS__)
#define RM_LOGI(...) ::rmre5::Log(::rmre5::LogLevel::Info, __VA_ARGS__)
#define RM_LOGD(...) ::rmre5::Log(::rmre5::LogLevel::Debug, __VA_ARGS__)

}  // namespace rmre5
