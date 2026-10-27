#include <chrono>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <system_error>
#include <variant>

#include <nlohmann/json.hpp>

#include "rm/telemetry.h"
#include "rm/version.h"

namespace rm {
namespace {

namespace fs = std::filesystem;

std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

bool IsReserved(std::string_view key) {
    return key == "ts" || key == "event" || key == "level" || key == "run_id" || key == "arena" ||
           key == "mod_version";
}

std::FILE* OpenAppend(const fs::path& path) {
    std::FILE* f = nullptr;
#if defined(_WIN32)
    // _wfopen_s opens files unshared; readers (diagnostic export, log
    // viewers) must be able to open the live log, so allow shared reads.
    f = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
#else
    f = std::fopen(path.c_str(), "ab");
#endif
    return f;
}

}  // namespace

std::int64_t SystemClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string FormatIsoUtcMs(std::int64_t unix_ms) {
    std::int64_t days = FloorDiv(unix_ms, 86400000);
    std::int64_t rem = unix_ms - days * 86400000;
    const int hour = static_cast<int>(rem / 3600000);
    rem %= 3600000;
    const int minute = static_cast<int>(rem / 60000);
    rem %= 60000;
    const int second = static_cast<int>(rem / 1000);
    const int milli = static_cast<int>(rem % 1000);

    // Civil-from-days (H. Hinnant), proleptic Gregorian.
    days += 719468;
    const std::int64_t era = FloorDiv(days, 146097);
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t year = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (month <= 2) ++year;

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month,
                  day, hour, minute, second, milli);
    return buf;
}

std::string JsonlSink::FormatLine(const Event& event, std::int64_t unix_ms, std::string_view mod_version) {
    nlohmann::ordered_json j;
    j["ts"] = FormatIsoUtcMs(unix_ms);
    j["event"] = std::string(EventTypeName(event.type));
    j["level"] = std::string(EventLevelName(event.level));
    j["run_id"] = event.run_id;
    j["arena"] = event.arena;
    j["mod_version"] = std::string(mod_version);
    for (const EventField& f : event.fields) {
        const std::string key = IsReserved(f.key) ? "f_" + f.key : f.key;
        std::visit([&j, &key](const auto& v) { j[key] = v; }, f.value);
    }
    return j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

JsonlSink::JsonlSink(JsonlOptions options) : options_(std::move(options)) {
    if (options_.mod_version.empty()) options_.mod_version = kModVersion;
    if (!options_.clock) options_.clock = SystemClockMs;
    std::lock_guard<std::mutex> lock(mutex_);
    OpenLocked();
}

JsonlSink::~JsonlSink() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
}

fs::path JsonlSink::current_path() const { return options_.directory / (options_.base_name + ".jsonl"); }

fs::path JsonlSink::rotated_path(int index) const {
    return options_.directory / (options_.base_name + "." + std::to_string(index) + ".jsonl");
}

bool JsonlSink::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

void JsonlSink::OpenLocked() {
    std::error_code ec;
    if (!options_.directory.empty()) fs::create_directories(options_.directory, ec);
    const fs::path path = current_path();
    file_ = OpenAppend(path);
    file_bytes_ = 0;
    if (file_ != nullptr) {
        const auto size = fs::file_size(path, ec);
        if (!ec) file_bytes_ = size;
    }
}

void JsonlSink::CloseLocked() {
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
    unflushed_ = 0;
}

void JsonlSink::RotateLocked() {
    CloseLocked();
    std::error_code ec;
    if (options_.keep_files <= 0) {
        fs::remove(current_path(), ec);
    } else {
        fs::remove(rotated_path(options_.keep_files), ec);
        for (int i = options_.keep_files - 1; i >= 1; --i) {
            const fs::path from = rotated_path(i);
            if (fs::exists(from, ec)) fs::rename(from, rotated_path(i + 1), ec);
        }
        fs::rename(current_path(), rotated_path(1), ec);
    }
    OpenLocked();
}

void JsonlSink::OnEvent(const Event& event) {
    if (static_cast<int>(event.level) > static_cast<int>(options_.max_level)) return;
    const std::string line = FormatLine(event, options_.clock(), options_.mod_version) + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) OpenLocked();
    if (file_ == nullptr) return;
    if (file_bytes_ > 0 && file_bytes_ + line.size() > options_.max_file_bytes) {
        RotateLocked();
        if (file_ == nullptr) return;
    }
    std::fwrite(line.data(), 1, line.size(), file_);
    file_bytes_ += line.size();
    ++lines_;
    ++unflushed_;
    const bool is_error = event.level == EventLevel::Error;
    const bool flush = options_.flush == FlushPolicy::EveryEvent || is_error ||
                       (options_.flush == FlushPolicy::EveryN && unflushed_ >= options_.flush_every_n);
    if (flush) {
        std::fflush(file_);
        unflushed_ = 0;
    }
}

void JsonlSink::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) std::fflush(file_);
    unflushed_ = 0;
}

}  // namespace rm
