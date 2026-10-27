#include <cstdio>

#include <catch_amalgamated.hpp>

#include "rm/profile.h"
#include "test_support.h"

using namespace rm;
namespace fs = std::filesystem;

namespace {

Profile SampleProfile() {
    Profile p;
    REQUIRE(p.credits.Credit("bank:run_0001", 120) == TxResult::Applied);
    REQUIRE(p.credits.Debit("unlock:character:gamma", 100) == TxResult::Applied);
    REQUIRE(p.credits.Credit("bank:run_0002", 75) == TxResult::Applied);
    p.unlocked_characters = {"gamma"};
    p.unlocked_perks = {"jackpot"};
    p.unlocked_weapons = {"rifle"};
    p.cosmetics = {"hat"};
    p.stats = {3, 1, 1, 3, 77};
    p.stats.credits_earned = 195;
    p.stats.no_damage_arenas = 2;
    p.stats.s_rank_arenas = 1;
    p.stats.runs_completed_as = {{"alpha", 1}};
    p.next_run_number = 4;
    p.active_run = ActiveRunCheckpoint{"run_0003", "alpha", 0xFFFFFFFFFFFFFFFFull, 1, 9, 31};
    return p;
}

void CheckEqual(const Profile& a, const Profile& b) {
    CHECK(a.schema_version == b.schema_version);
    CHECK(a.credits.balance() == b.credits.balance());
    CHECK(a.credits.transactions() == b.credits.transactions());
    CHECK(a.unlocked_characters == b.unlocked_characters);
    CHECK(a.unlocked_perks == b.unlocked_perks);
    CHECK(a.unlocked_weapons == b.unlocked_weapons);
    CHECK(a.cosmetics == b.cosmetics);
    CHECK(a.emotes == b.emotes);
    CHECK(a.stats.runs_started == b.stats.runs_started);
    CHECK(a.stats.runs_completed == b.stats.runs_completed);
    CHECK(a.stats.runs_failed == b.stats.runs_failed);
    CHECK(a.stats.best_arena == b.stats.best_arena);
    CHECK(a.stats.total_kills == b.stats.total_kills);
    CHECK(a.stats.credits_earned == b.stats.credits_earned);
    CHECK(a.stats.no_damage_arenas == b.stats.no_damage_arenas);
    CHECK(a.stats.s_rank_arenas == b.stats.s_rank_arenas);
    CHECK(a.stats.runs_completed_as == b.stats.runs_completed_as);
    CHECK(a.next_run_number == b.next_run_number);
    REQUIRE(a.active_run.has_value() == b.active_run.has_value());
    if (a.active_run) {
        CHECK(a.active_run->run_id == b.active_run->run_id);
        CHECK(a.active_run->seed == b.active_run->seed);
        CHECK(a.active_run->credits_pending == b.active_run->credits_pending);
        CHECK(a.active_run->kills == b.active_run->kills);
        CHECK(a.active_run->arenas_cleared == b.active_run->arenas_cleared);
    }
}

std::size_t ErrorEvents(const MemorySink& sink) { return sink.Count(EventType::Error); }

}  // namespace

TEST_CASE("profile serialisation round-trips with checksum", "[persistence]") {
    const Profile p = SampleProfile();
    const std::string text = SerializeProfile(p);
    CHECK(text.find("\"checksum\": \"fnv1a64:") != std::string::npos);
    Result<ParsedProfile> parsed = ParseProfile(text);
    REQUIRE(parsed.ok());
    CHECK(parsed->migrated_from == -1);
    CheckEqual(p, parsed->profile);
    CHECK(parsed->profile.BankedRunIds() == std::vector<std::string>{"run_0001", "run_0002"});
    CHECK(SerializeProfile(parsed->profile) == text);  // canonical
}

TEST_CASE("profile store saves atomically and keeps a backup", "[persistence]") {
    test::TempDir dir("store");
    MemorySink sink;
    ProfileStore store(dir.path() / "nested" / "profile.json", &sink);
    ProfileLoadResult fresh = store.Load();
    CHECK(fresh.source == ProfileSource::Default);
    CHECK(fresh.errors.empty());

    Profile p = SampleProfile();
    REQUIRE(store.Save(p).ok());
    CHECK(fs::exists(store.main_path()));
    CHECK_FALSE(fs::exists(store.temp_path()));
    CHECK_FALSE(fs::exists(store.backup_path()));  // nothing to rotate on the first save
    p.stats.total_kills = 100;
    REQUIRE(store.Save(p).ok());
    REQUIRE(fs::exists(store.backup_path()));
    CHECK(ParseProfile(test::ReadText(store.backup_path()))->profile.stats.total_kills == 77);

    ProfileLoadResult loaded = store.Load();
    CHECK(loaded.source == ProfileSource::Main);
    CheckEqual(p, loaded.profile);
    CHECK(sink.Count(EventType::ProfileSaved) == 2);
}

TEST_CASE("corrupt profiles are preserved and recovered from backup or default", "[persistence]") {
    test::TempDir dir("recover");
    const fs::path main = dir.path() / "profile.json";
    MemorySink sink;
    ProfileStore store(main, &sink);
    Profile good = SampleProfile();
    REQUIRE(store.Save(good).ok());
    good.stats.total_kills = 88;
    REQUIRE(store.Save(good).ok());  // main: 88, bak: 77

    SECTION("truncated JSON -> .bak") {
        const std::string text = test::ReadText(main);
        test::WriteText(main, text.substr(0, text.size() / 2));
        ProfileLoadResult r = store.Load();
        CHECK(r.source == ProfileSource::Backup);
        CHECK(r.profile.stats.total_kills == 77);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].find("invalid JSON") != std::string::npos);
        REQUIRE(r.preserved.size() == 1);
        CHECK(r.preserved[0].filename() == "profile.json.corrupt-1");
        CHECK(fs::exists(dir.path() / "profile.json.corrupt-1"));
        CHECK_FALSE(fs::exists(main));
        CHECK(ErrorEvents(sink) == 1);
        // Saving again restores main without clobbering the good backup.
        REQUIRE(store.Save(r.profile).ok());
        CHECK(store.Load().source == ProfileSource::Main);
    }
    SECTION("bad checksum -> .bak") {
        std::string text = test::ReadText(main);
        const auto pos = text.find("\"total_kills\": 88");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 17, "\"total_kills\": 99");
        test::WriteText(main, text);
        ProfileLoadResult r = store.Load();
        CHECK(r.source == ProfileSource::Backup);
        CHECK(r.errors[0].find("checksum mismatch") != std::string::npos);
    }
    SECTION("wrong schema -> .bak") {
        test::WriteText(main, R"({"schema_version": 99, "credits": 5})");
        ProfileLoadResult r = store.Load();
        CHECK(r.source == ProfileSource::Backup);
        CHECK(r.errors[0].find("unsupported schema_version 99") != std::string::npos);
    }
    SECTION("valid JSON with invalid fields -> .bak") {
        Profile tampered = good;
        nlohmann::json j = nlohmann::json::parse(SerializeProfile(tampered));
        j["credits"] = -5;
        j.erase("checksum");
        const std::string body = j.dump();
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(Fnv1a64(body)));
        j["checksum"] = std::string("fnv1a64:") + hex;
        test::WriteText(main, j.dump(2));
        ProfileLoadResult r = store.Load();
        CHECK(r.source == ProfileSource::Backup);
        CHECK(r.errors[0].find("'credits' must be >= 0") != std::string::npos);
    }
    SECTION("main and backup corrupt -> default, both preserved") {
        test::WriteText(main, "{\"schema_version\": 1");
        test::WriteText(store.backup_path(), "not json at all");
        ProfileLoadResult r = store.Load();
        CHECK(r.source == ProfileSource::Default);
        CHECK(r.profile.credits.balance() == 0);
        CHECK(r.errors.size() == 2);
        CHECK(fs::exists(dir.path() / "profile.json.corrupt-1"));
        CHECK(fs::exists(dir.path() / "profile.json.corrupt-2"));
        CHECK(ErrorEvents(sink) == 2);
    }
    SECTION("a corrupt main never overwrites a good backup") {
        test::WriteText(main, "garbage");
        Profile next;
        REQUIRE(store.Save(next).ok());
        CHECK(ParseProfile(test::ReadText(store.backup_path()))->profile.stats.total_kills == 77);
    }
}

TEST_CASE("schema 0 profiles migrate to schema 1", "[persistence]") {
    test::TempDir dir("migrate");
    const fs::path main = dir.path() / "profile.json";
    test::WriteText(main, R"({
      "schema_version": 0,
      "credits": 250,
      "unlocks": ["character:gamma", "perk:jackpot", "weapon:rifle"],
      "runs_started": 4,
      "runs_completed": 2,
      "banked_runs": ["run_0001", "run_0002"],
      "next_run": 5
    })");
    ProfileStore store(main);
    ProfileLoadResult r = store.Load();
    REQUIRE(r.errors.empty());
    CHECK(r.source == ProfileSource::Main);
    CHECK(r.migrated_from == 0);
    const Profile& p = r.profile;
    CHECK(p.schema_version == kProfileSchemaVersion);
    CHECK(p.credits.balance() == 250);
    CHECK(p.unlocked_characters == StringSet{"gamma"});
    CHECK(p.unlocked_perks == StringSet{"jackpot"});
    CHECK(p.unlocked_weapons == StringSet{"rifle"});
    CHECK(p.stats.runs_started == 4);
    CHECK(p.stats.runs_completed == 2);
    CHECK(p.next_run_number == 5);
    CHECK(p.IsRunBanked("run_0002"));
    CHECK(BankRunCredits(r.profile, "run_0001", 10) == TxResult::AlreadyApplied);
    // Saved back as schema 1 with a checksum.
    REQUIRE(store.Save(p).ok());
    const std::string saved = test::ReadText(main);
    CHECK(saved.find("\"schema_version\": 1") != std::string::npos);
    CHECK(store.Load().migrated_from == -1);

    CHECK_FALSE(ParseProfile(R"({"schema_version": 0, "credits": 1, "unlocks": ["hat"]})").ok());
}

TEST_CASE("EndRun banks exactly once across save and reload", "[persistence]") {
    test::TempDir dir("endrun");
    const fs::path main = dir.path() / "profile.json";
    std::string run_id;
    std::int64_t banked = 0;
    {
        ProfileStore store(main);
        test::Harness h(test::LoadFixture(), store.Load().profile, Capabilities::All(), &store);
        REQUIRE(h.rc.StartRun("alpha", 99).ok());
        run_id = h.rc.state().run_id;
        for (int arena = 0; arena < 3; ++arena) {
            test::ClearArena(h.rc, arena * 100.0);
            REQUIRE(h.rc.ContinueToRewards().ok());
            REQUIRE(h.rc.ChooseReward(0).ok());
            if (arena < 2) {
                REQUIRE(h.rc.LeaveShop().ok());
                REQUIRE(h.rc.ChooseRoute(0).ok());
            }
        }
        Result<RunSummary> first = h.rc.EndRun();
        REQUIRE(first.ok());
        banked = first->credits_banked;
        CHECK(banked > 0);
        Result<RunSummary> second = h.rc.EndRun();
        REQUIRE(second.ok());
        CHECK(second->already_banked);
        CHECK(second->credits_banked == 0);
        CHECK(h.rc.profile().credits.balance() == banked);
    }
    // "Restart": new store and controller from disk.
    ProfileStore store(main);
    ProfileLoadResult loaded = store.Load();
    REQUIRE(loaded.source == ProfileSource::Main);
    CHECK(loaded.profile.credits.balance() == banked);
    CHECK(loaded.profile.IsRunBanked(run_id));
    CHECK_FALSE(loaded.profile.active_run.has_value());
    Profile reloaded = loaded.profile;
    CHECK(BankRunCredits(reloaded, run_id, banked) == TxResult::AlreadyApplied);
    CHECK(reloaded.credits.balance() == banked);

    test::Harness h2(test::LoadFixture(), loaded.profile, Capabilities::All(), &store);
    CHECK_FALSE(h2.rc.interrupted_run().has_value());
    CHECK(h2.rc.EndRun().code() == ErrorCode::NoActiveRun);
    REQUIRE(h2.rc.StartRun("alpha", 1).ok());
    CHECK(h2.rc.state().run_id == "run_0002");
    CHECK(h2.rc.profile().credits.balance() == banked);
}

TEST_CASE("a crash-interrupted run is banked once at the death rate", "[persistence]") {
    test::TempDir dir("interrupted");
    const fs::path main = dir.path() / "profile.json";
    std::int64_t pending = 0;
    {
        ProfileStore store(main);
        test::Harness h(test::LoadFixture(), store.Load().profile, Capabilities::All(), &store);
        REQUIRE(h.rc.StartRun("alpha", 5).ok());
        test::ClearArena(h.rc, 0.0);  // checkpoint saved after the arena
        pending = h.rc.state().credits_pending;
        REQUIRE(pending > 0);
        // Process "crashes": controller destroyed without EndRun.
    }
    ProfileStore store(main);
    ProfileLoadResult loaded = store.Load();
    REQUIRE(loaded.profile.active_run.has_value());
    CHECK(loaded.profile.active_run->credits_pending == pending);

    test::Harness h(test::LoadFixture(), loaded.profile, Capabilities::All(), &store);
    REQUIRE(h.rc.interrupted_run().has_value());
    Result<RunSummary> settled = h.rc.RecoverInterruptedRun();
    REQUIRE(settled.ok());
    CHECK(settled->credits_banked == pending / 2);
    CHECK(settled->end_reason == "interrupted");
    CHECK(h.rc.RecoverInterruptedRun().code() == ErrorCode::NoActiveRun);
    CHECK(h.rc.profile().stats.runs_failed == 1);

    // Even a stale copy of the checkpoint cannot bank the same run twice.
    Profile stale = h.rc.profile();
    stale.active_run = loaded.profile.active_run;
    test::Harness again(test::LoadFixture(), stale);
    Result<RunSummary> repeat = again.rc.RecoverInterruptedRun();
    REQUIRE(repeat.ok());
    CHECK(repeat->already_banked);
    CHECK(repeat->credits_banked == 0);
    CHECK(again.rc.profile().credits.balance() == pending / 2);

    // StartRun settles automatically when the adapter did not.
    test::Harness auto_settle(test::LoadFixture(), loaded.profile);
    REQUIRE(auto_settle.rc.StartRun("alpha", 6).ok());
    CHECK(auto_settle.rc.profile().credits.balance() == pending / 2);
    CHECK(auto_settle.rc.state().run_id == "run_0002");
}
