#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/common.h"
#include "rm/economy.h"
#include "rm/status.h"
#include "rm/version.h"

namespace rm {

class IEventSink;

struct ProfileStats {
    std::int64_t runs_started = 0;
    std::int64_t runs_completed = 0;
    std::int64_t runs_failed = 0;
    std::int64_t best_arena = 0;  // most arenas cleared in one run
    std::int64_t total_kills = 0;
    // Lifetime counters behind character challenges (schema 1, optional fields).
    std::int64_t credits_earned = 0;    // credits banked by runs (not spent)
    std::int64_t no_damage_arenas = 0;  // arenas cleared without taking damage
    std::int64_t s_rank_arenas = 0;     // arenas rated S
    StringMap<std::int64_t> runs_completed_as;  // completed runs per character id
};

/// Snapshot of the run in progress, saved at StartRun and after each arena so earned credits
/// survive a crash. RunController settles it (banked at the death rate) before the next run.
struct ActiveRunCheckpoint {
    std::string run_id;
    std::string character_id;
    std::uint64_t seed = 0;
    int arenas_cleared = 0;
    std::int64_t kills = 0;
    std::int64_t credits_pending = 0;
};

/// Persistent meta-progression; the only state that survives a run. Mod-owned file, never
/// the game's save.
struct Profile {
    int schema_version = kProfileSchemaVersion;
    /// Credits balance plus the applied-transaction log. Banked runs are recorded as
    /// "bank:<run_id>" (this is the banked-run-id ledger), unlocks as "unlock:<kind>:<id>".
    Ledger credits;
    StringSet unlocked_characters;
    StringSet unlocked_perks;
    StringSet unlocked_weapons;
    StringSet cosmetics;  // reserved
    StringSet emotes;     // reserved
    ProfileStats stats;
    std::int64_t next_run_number = 1;
    std::optional<ActiveRunCheckpoint> active_run;

    [[nodiscard]] bool IsRunBanked(std::string_view run_id) const;
    [[nodiscard]] std::vector<std::string> BankedRunIds() const;
};

std::string BankTxId(std::string_view run_id);

/// Credits a run's earnings exactly once per run id (AlreadyApplied on repeats).
TxResult BankRunCredits(Profile& profile, std::string_view run_id, std::int64_t amount);

/// JSON document with a "checksum" field (FNV-1a 64 of the canonical body).
std::string SerializeProfile(const Profile& profile);

struct ParsedProfile {
    Profile profile;
    int migrated_from = -1;  // original schema version when a migration ran
};

/// Parses and validates (JSON, checksum, schema, ranges); migrates schema 0 -> 1.
Result<ParsedProfile> ParseProfile(std::string_view json_text);

enum class ProfileSource { Main, Backup, Default };
std::string_view ProfileSourceName(ProfileSource s);

struct ProfileLoadResult {
    Profile profile;
    ProfileSource source = ProfileSource::Default;
    int migrated_from = -1;
    std::vector<std::string> errors;                   // one per rejected file
    std::vector<std::filesystem::path> preserved;      // where rejected files were moved
};

/// Atomic, recoverable profile file.
///  Save: write <main>.tmp, flush to disk, copy the current main to <main>.bak if it still
///        validates, then rename tmp over main (replace-existing).
///  Load: main -> .bak -> fresh default. A file failing parse/checksum/schema validation is
///        moved to <main>.corrupt-<n> (first free n) and reported via the sink as an error.
class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path main_path, IEventSink* sink = nullptr);

    ProfileLoadResult Load();
    Status Save(const Profile& profile);

    [[nodiscard]] const std::filesystem::path& main_path() const { return main_; }
    [[nodiscard]] std::filesystem::path backup_path() const;
    [[nodiscard]] std::filesystem::path temp_path() const;
    void set_sink(IEventSink* sink) { sink_ = sink; }

private:
    std::filesystem::path main_;
    IEventSink* sink_ = nullptr;
};

}  // namespace rm
