#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include <catch_amalgamated.hpp>

#include "rm/diagnostics.h"
#include "rm/telemetry.h"
#include "rm/version.h"
#include "rm/zip.h"
#include "test_support.h"

using namespace rm;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::vector<std::string> Lines(const fs::path& p) {
    std::vector<std::string> out;
    std::istringstream in(test::ReadText(p));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

/// Output directory for artefacts checked by the Python reader (CTest sets RM_TEST_OUTPUT_DIR).
std::optional<fs::path> OutputDir() {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "RM_TEST_OUTPUT_DIR") != 0 || value == nullptr) return std::nullopt;
    fs::path p(value);
    std::free(value);
#else
    const char* value = std::getenv("RM_TEST_OUTPUT_DIR");
    if (value == nullptr) return std::nullopt;
    fs::path p(value);
#endif
    fs::create_directories(p);
    return p;
}

std::uint32_t Read32(const std::string& s, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24);
}

std::uint32_t Read16(const std::string& s, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8);
}

/// Minimal independent reader: walks the central directory and validates each local entry.
std::map<std::string, std::string> ReadZip(const std::string& zip) {
    std::map<std::string, std::string> files;
    REQUIRE(zip.size() >= 22);
    const std::size_t eocd = zip.size() - 22;
    REQUIRE(Read32(zip, eocd) == 0x06054B50u);
    const std::uint32_t count = Read16(zip, eocd + 10);
    const std::uint32_t cd_size = Read32(zip, eocd + 12);
    std::size_t cd = Read32(zip, eocd + 16);
    REQUIRE(cd + cd_size == eocd);
    for (std::uint32_t i = 0; i < count; ++i) {
        REQUIRE(Read32(zip, cd) == 0x02014B50u);
        CHECK(Read16(zip, cd + 10) == 0);  // stored
        const std::uint32_t crc = Read32(zip, cd + 16);
        const std::uint32_t size = Read32(zip, cd + 20);
        CHECK(Read32(zip, cd + 24) == size);
        const std::uint32_t name_len = Read16(zip, cd + 28);
        const std::uint32_t extra_len = Read16(zip, cd + 30);
        const std::uint32_t comment_len = Read16(zip, cd + 32);
        const std::uint32_t local = Read32(zip, cd + 42);
        const std::string name = zip.substr(cd + 46, name_len);
        REQUIRE(Read32(zip, local) == 0x04034B50u);
        CHECK(Read32(zip, local + 14) == crc);
        CHECK(Read16(zip, local + 26) == name_len);
        CHECK(zip.substr(local + 30, name_len) == name);
        const std::size_t data_at = local + 30 + name_len + Read16(zip, local + 28);
        const std::string data = zip.substr(data_at, size);
        CHECK(Crc32(data) == crc);
        files[name] = data;
        cd += 46 + name_len + extra_len + comment_len;
    }
    return files;
}

}  // namespace

TEST_CASE("ISO-8601 UTC timestamps with milliseconds", "[telemetry]") {
    CHECK(FormatIsoUtcMs(0) == "1970-01-01T00:00:00.000Z");
    CHECK(FormatIsoUtcMs(951782400000) == "2000-02-29T00:00:00.000Z");
    CHECK(FormatIsoUtcMs(1793037421123) == "2026-10-26T17:57:01.123Z");
    CHECK(FormatIsoUtcMs(4102444799999) == "2099-12-31T23:59:59.999Z");
    CHECK(FormatIsoUtcMs(-1) == "1969-12-31T23:59:59.999Z");
}

TEST_CASE("JSONL lines have the documented shape", "[telemetry]") {
    test::TempDir dir("jsonl");
    std::int64_t clock = 1793037421123;
    JsonlOptions opts;
    opts.directory = dir.path();
    opts.clock = [&clock] { return clock++; };
    opts.max_level = EventLevel::Info;
    {
        JsonlSink sink(opts);
        REQUIRE(sink.is_open());
        Event e;
        e.type = EventType::EnemyKilled;
        e.run_id = "run_0007";
        e.arena = 2;
        e.Add("by_player", true).Add("remaining", 3).Add("ratio", 0.5).Add("weapon_id", "m92f").Add("event", "clash");
        sink.OnEvent(e);
        Event debug;
        debug.type = EventType::PerkTriggered;
        debug.level = EventLevel::Debug;  // filtered at Info
        sink.OnEvent(debug);
        Event err;
        err.type = EventType::Error;
        err.level = EventLevel::Error;
        err.Add("message", std::string("bad \"quote\"\n"));
        sink.OnEvent(err);
        CHECK(sink.lines_written() == 2);
    }
    const std::vector<std::string> lines = Lines(dir.path() / "rm_events.jsonl");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].rfind(R"({"ts":"2026-10-26T17:57:01.123Z","event":"enemy_killed","level":"info","run_id":"run_0007","arena":2,"mod_version":")", 0) == 0);
    const json first = json::parse(lines[0]);
    CHECK(first["mod_version"] == kModVersion);
    CHECK(first["by_player"] == true);
    CHECK(first["remaining"] == 3);
    CHECK(first["ratio"] == 0.5);
    CHECK(first["weapon_id"] == "m92f");
    CHECK(first["event"] == "enemy_killed");
    CHECK(first["f_event"] == "clash");  // reserved key collision
    const json second = json::parse(lines[1]);
    CHECK(second["level"] == "error");
    CHECK(second["ts"] == "2026-10-26T17:57:01.124Z");
    CHECK(second["message"] == "bad \"quote\"\n");
    CHECK(second["run_id"] == "");
    CHECK(second["arena"] == 0);
}

TEST_CASE("JSONL rotation keeps a bounded number of files", "[telemetry]") {
    test::TempDir dir("rotate");
    JsonlOptions opts;
    opts.directory = dir.path();
    opts.base_name = "rot";
    opts.max_file_bytes = 600;
    opts.keep_files = 2;
    opts.flush = FlushPolicy::EveryN;
    opts.flush_every_n = 3;
    opts.clock = [] { return std::int64_t{0}; };
    {
        JsonlSink sink(opts);
        for (int i = 0; i < 40; ++i) {
            Event e;
            e.type = EventType::CurrencyChanged;
            e.Add("i", i);
            sink.OnEvent(e);
        }
        CHECK(sink.lines_written() == 40);
    }
    CHECK(fs::exists(dir.path() / "rot.jsonl"));
    CHECK(fs::exists(dir.path() / "rot.1.jsonl"));
    CHECK(fs::exists(dir.path() / "rot.2.jsonl"));
    CHECK_FALSE(fs::exists(dir.path() / "rot.3.jsonl"));
    for (const char* f : {"rot.jsonl", "rot.1.jsonl", "rot.2.jsonl"}) {
        CHECK(fs::file_size(dir.path() / f) <= 600);
        for (const std::string& line : Lines(dir.path() / f)) CHECK_NOTHROW(json::parse(line));
    }
    const std::vector<std::string> newest = Lines(dir.path() / "rot.jsonl");
    REQUIRE_FALSE(newest.empty());
    CHECK(json::parse(newest.back())["i"] == 39);
}

TEST_CASE("controller telemetry through JsonlSink with damage sampling", "[telemetry]") {
    test::TempDir dir("controller_jsonl");
    JsonlOptions opts;
    opts.directory = dir.path();
    opts.max_level = EventLevel::Debug;
    opts.clock = [] { return std::int64_t{1793037421000}; };
    {
        JsonlSink sink(opts);
        RunControllerOptions ro;
        ro.capabilities = Capabilities::All();
        ro.sink = &sink;
        ro.damage_sample_every = 10;
        RunController rc(test::LoadFixture(), Profile{}, ro);
        REQUIRE(rc.StartRun("alpha", 1).ok());
        REQUIRE(rc.BeginArena(0.0).ok());
        for (int i = 0; i < 100; ++i) {
            HitContext hit;
            hit.now = 1.0 + i;
            rc.ComputeOutgoingMultiplier(hit);
        }
        test::KillN(rc, 5, 2.0);
        REQUIRE(rc.CompleteArena(10.0).ok());
        CHECK(rc.ChooseReward(0).code == ErrorCode::WrongPhase);  // error event
    }
    std::map<std::string, int> counts;
    for (const std::string& line : Lines(dir.path() / "rm_events.jsonl")) {
        const json j = json::parse(line);
        counts[j["event"].get<std::string>()]++;
        if (j["event"] == "arena_started") {
            CHECK(j["run_id"] == "run_0001");
            CHECK(j["arena"] == 1);
        }
    }
    CHECK(counts["run_started"] == 1);
    CHECK(counts["arena_started"] == 1);
    CHECK(counts["enemy_killed"] == 5);
    CHECK(counts["damage_modified"] == 10);  // 100 hits sampled every 10th
    CHECK(counts["arena_completed"] == 1);
    CHECK(counts["currency_changed"] >= 6);
    CHECK(counts["error"] == 1);
}

TEST_CASE("stored ZIP archives are valid", "[zip]") {
    CHECK(Crc32("123456789") == 0xCBF43926u);
    CHECK(Crc32("") == 0u);
    std::string binary(300, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>(i * 7);
    const std::vector<ZipEntry> files = {
        {"summary.txt", "hello zip\n"},
        {"logs/rm_events.jsonl", "{\"a\":1}\n{\"b\":2}\n"},
        {"empty.txt", ""},
        {"bin/data.bin", binary},
    };
    test::TempDir dir("zip");
    const fs::path out = OutputDir().value_or(dir.path()) / "stored.zip";
    REQUIRE(WriteStoredZip(out, files).ok());
    const auto read = ReadZip(test::ReadText(out));
    REQUIRE(read.size() == files.size());
    for (const ZipEntry& f : files) CHECK(read.at(f.name) == f.data);
    CHECK(WriteStoredZip(dir.path() / "bad.zip", {{"../escape.txt", "x"}}).code == ErrorCode::InvalidArgument);
    CHECK(WriteStoredZip(dir.path() / "bad.zip", {{"/abs.txt", "x"}}).code == ErrorCode::InvalidArgument);
}

TEST_CASE("diagnostic export packs only allowed files plus a summary", "[zip]") {
    test::TempDir dir("diag");
    const fs::path logs = dir.path() / "logs";
    const fs::path configs = dir.path() / "configs";
    fs::create_directories(logs / "nested");
    fs::create_directories(configs);
    {
        JsonlOptions opts;
        opts.directory = logs;
        opts.clock = [] { return std::int64_t{1793037421000}; };
        JsonlSink sink(opts);
        for (int i = 0; i < 30; ++i) {
            Event e;
            e.type = EventType::Error;
            e.level = EventLevel::Error;
            e.Add("operation", "test").Add("n", i);
            sink.OnEvent(e);
        }
        Event info;
        info.type = EventType::RunStarted;
        sink.OnEvent(info);
    }
    test::WriteText(logs / "adapter.log", "adapter started\n");
    test::WriteText(logs / "re5dx9.exe", "MZ binary");
    test::WriteText(logs / "proxy.dll", "MZ binary");
    test::WriteText(logs / "nested" / "deep.jsonl", "{}\n");
    test::WriteText(configs / "perks.json", "{}");
    test::WriteText(configs / "notes.bin", "x");
    test::WriteText(dir.path() / "crash.txt", "crash info");
    test::WriteText(dir.path() / "savegame.sav", "player data");

    const fs::path out = OutputDir().value_or(dir.path()) / "diagnostics.zip";
    const DiagnosticsReport report =
        ExportDiagnostics(out, logs, configs, {dir.path() / "crash.txt", dir.path() / "savegame.sav"},
                          "adapter: re5 test build\nexe_sha256: 751726F0", DiagnosticsOptions{5});
    REQUIRE(report.status.ok());
    const auto files = ReadZip(test::ReadText(out));
    CHECK(files.count("summary.txt") == 1);
    CHECK(files.count("logs/rm_events.jsonl") == 1);
    CHECK(files.count("logs/adapter.log") == 1);
    CHECK(files.count("configs/perks.json") == 1);
    CHECK(files.count("extra/crash.txt") == 1);
    CHECK(files.size() == 5);  // no exe/dll/sav/bin, no nested directories
    CHECK(report.skipped.size() == 1);  // savegame.sav (explicit extra with a forbidden type)
    const std::string& summary = files.at("summary.txt");
    CHECK(summary.find(std::string("mod_version: ") + kModVersion) != std::string::npos);
    CHECK(summary.find("adapter: re5 test build") != std::string::npos);
    CHECK(summary.find("last 5 of 30 error events") != std::string::npos);
    CHECK(summary.find("\"n\":29") != std::string::npos);
    CHECK(summary.find("\"n\":24") == std::string::npos);
}
