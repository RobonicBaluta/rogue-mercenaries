#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "rm/events.h"

namespace rm {

/// Unix epoch milliseconds. Injectable so tests produce stable timestamps.
using WallClockMs = std::function<std::int64_t()>;
std::int64_t SystemClockMs();

/// "2026-10-27T18:07:01.123Z"
std::string FormatIsoUtcMs(std::int64_t unix_ms);

enum class FlushPolicy {
    EveryEvent,  // flush after each line (safest; default)
    EveryN,      // flush every flush_every_n lines and always on errors
    Manual,      // only on Flush()/rotation/destruction (errors still flush)
};

struct JsonlOptions {
    std::filesystem::path directory;
    std::string base_name = "rm_events";  // current file: <base>.jsonl, rotated: <base>.<i>.jsonl
    EventLevel max_level = EventLevel::Info;  // events more verbose than this are dropped
    std::uint64_t max_file_bytes = 4u * 1024u * 1024u;
    int keep_files = 5;  // rotated files kept besides the current one
    FlushPolicy flush = FlushPolicy::EveryEvent;
    int flush_every_n = 20;
    std::string mod_version;  // empty = kModVersion
    WallClockMs clock;        // empty = SystemClockMs
};

/// Writes one JSON object per line:
///   {"ts","event","level","run_id","arena","mod_version", ...event fields}
/// Event fields that collide with those keys are written as "f_<key>". Thread-safe.
class JsonlSink final : public IEventSink {
public:
    explicit JsonlSink(JsonlOptions options);
    ~JsonlSink() override;
    JsonlSink(const JsonlSink&) = delete;
    JsonlSink& operator=(const JsonlSink&) = delete;

    void OnEvent(const Event& event) override;
    void Flush();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::filesystem::path current_path() const;
    [[nodiscard]] std::filesystem::path rotated_path(int index) const;
    [[nodiscard]] std::uint64_t lines_written() const { return lines_; }

    static std::string FormatLine(const Event& event, std::int64_t unix_ms,
                                  std::string_view mod_version);

private:
    void OpenLocked();
    void RotateLocked();
    void CloseLocked();

    JsonlOptions options_;
    mutable std::mutex mutex_;
    std::FILE* file_ = nullptr;
    std::uint64_t file_bytes_ = 0;
    std::uint64_t lines_ = 0;
    int unflushed_ = 0;
};

}  // namespace rm
