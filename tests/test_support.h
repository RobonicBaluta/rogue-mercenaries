#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "rm/content.h"
#include "rm/run_controller.h"

namespace rm::test {

/// Small, stable content set used by unit tests (independent of configs/ balance changes).
ContentSources FixtureSources();

/// Loads sources and fails the current test with the validation errors if any.
std::shared_ptr<const Content> Load(const ContentSources& sources);
std::shared_ptr<const Content> LoadFixture();

/// Edits one fixture file as JSON (e.g. drop perks, change prices) and returns new sources.
ContentSources EditFixture(const std::function<void(nlohmann::json& perks, nlohmann::json& weapons,
                                                    nlohmann::json& characters, nlohmann::json& routes,
                                                    nlohmann::json& economy)>& edit);

/// Repository configs/ directory.
std::filesystem::path ConfigDir();

/// Unique empty directory under the system temp dir, removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadText(const std::filesystem::path& p);
void WriteText(const std::filesystem::path& p, const std::string& text);

/// Controller over the fixture with every capability, in-memory profile.
struct Harness {
    explicit Harness(std::shared_ptr<const Content> content = LoadFixture(), Profile profile = {},
                     Capabilities caps = Capabilities::All(), ProfileStore* store = nullptr);
    MemorySink sink;
    RunController rc;
};

/// Kills `n` enemies by the player at time `t` (+0.1 s each); returns the last outcome.
KillOutcome KillN(RunController& rc, int n, double t, const char* weapon = nullptr);

/// Plays the current arena to completion (budget kills) and returns CompleteArena's result.
ArenaResult ClearArena(RunController& rc, double start_time);

}  // namespace rm::test
