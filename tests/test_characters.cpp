#include <algorithm>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

// Fixture plus two challenge-locked characters.
ContentSources ChallengeSources() {
    return test::EditFixture([](json&, json&, json& characters, json&, json&) {
        characters["characters"].push_back({{"id", "epsilon"},
                                            {"name", "Epsilon"},
                                            {"starting_weapons", json::array({"pistol"})},
                                            {"unlock_kind", "runs_completed_as"},
                                            {"unlock_character", "alpha"},
                                            {"unlock_count", 1},
                                            {"unlock_text", "Complete a run as Alpha"}});
        characters["characters"].push_back({{"id", "zeta"},
                                            {"name", "Zeta"},
                                            {"starting_weapons", json::array({"pistol"})},
                                            {"unlock_kind", "total_kills"},
                                            {"unlock_count", 5},
                                            {"unlock_text", "Kill 5 enemies"}});
    });
}

const CharacterAvailability* Find(const std::vector<CharacterAvailability>& list, std::string_view id) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const CharacterAvailability& c) { return c.id == id; });
    return it == list.end() ? nullptr : &*it;
}

// Plays every arena of a run without taking damage and banks it.
RunSummary CompleteRun(RunController& rc, std::string_view character) {
    REQUIRE(rc.StartRun(character, 7).ok());
    double t = 0.0;
    while (rc.phase() != Phase::RunComplete) {
        test::ClearArena(rc, t);
        REQUIRE(rc.ContinueToRewards().ok());
        if (rc.phase() == Phase::RewardSelection) REQUIRE(rc.ChooseReward(0).ok());
        if (rc.phase() == Phase::Shop) {
            REQUIRE(rc.LeaveShop().ok());
            REQUIRE(rc.ChooseRoute(0).ok());
        }
        t += 100.0;
    }
    const Result<RunSummary> summary = rc.EndRun();
    REQUIRE(summary.ok());
    return *summary;
}

std::vector<std::string> LoadErrors(const std::function<void(json&)>& edit_characters) {
    return Content::LoadFromStrings(test::EditFixture([&](json&, json&, json& characters, json&, json&) {
               edit_characters(characters);
           }))
        .errors;
}

bool HasError(const std::vector<std::string>& errors, std::string_view needle) {
    for (const std::string& e : errors) UNSCOPED_INFO("got: " << e);
    return std::any_of(errors.begin(), errors.end(),
                       [&](const std::string& e) { return e.find(needle) != std::string::npos; });
}

}  // namespace

TEST_CASE("challenge-locked characters report progress and reject StartRun", "[characters]") {
    test::Harness h(test::Load(ChallengeSources()));
    const auto list = h.rc.Characters();
    const CharacterAvailability* epsilon = Find(list, "epsilon");
    const CharacterAvailability* zeta = Find(list, "zeta");
    REQUIRE(epsilon != nullptr);
    REQUIRE(zeta != nullptr);
    CHECK(epsilon->gate == CharacterGate::Locked);
    CHECK(epsilon->unlock_text == "Complete a run as Alpha");
    CHECK(epsilon->challenge.current == 0);
    CHECK(epsilon->challenge.target == 1);
    CHECK(zeta->gate == CharacterGate::Locked);
    CHECK(zeta->challenge.target == 5);
    CHECK_FALSE(h.rc.StartRun("epsilon", 1).ok());
    CHECK(h.sink.Count(EventType::CharacterUnlocked) == 0);
    // Characters without a cost or a challenge stay free.
    REQUIRE(Find(list, "alpha") != nullptr);
    CHECK(Find(list, "alpha")->gate == CharacterGate::Selectable);
}

TEST_CASE("challenges already met by the loaded profile unlock on construction", "[characters]") {
    Profile profile;
    profile.stats.total_kills = 9;
    test::Harness h(test::Load(ChallengeSources()), profile);
    CHECK(h.sink.Count(EventType::CharacterUnlocked) == 1);
    CHECK(h.rc.profile().unlocked_characters.count("zeta") == 1);
    const auto list = h.rc.Characters();
    REQUIRE(Find(list, "zeta") != nullptr);
    CHECK(Find(list, "zeta")->gate == CharacterGate::Selectable);
    CHECK(Find(list, "zeta")->challenge.current == 5);  // clamped to the target
    CHECK(Find(list, "epsilon")->gate == CharacterGate::Locked);
    CHECK(h.rc.StartRun("zeta", 1).ok());
}

TEST_CASE("a completed run updates lifetime stats and unlocks the themed character", "[characters]") {
    test::Harness h(test::Load(ChallengeSources()));
    const RunSummary summary = CompleteRun(h.rc, "alpha");
    REQUIRE(summary.completed);
    const ProfileStats& stats = h.rc.profile().stats;
    CHECK(stats.runs_completed == 1);
    CHECK(stats.runs_completed_as.at("alpha") == 1);
    CHECK(stats.credits_earned == summary.credits_banked);
    CHECK(stats.no_damage_arenas == summary.arenas_cleared);
    CHECK(stats.total_kills == summary.total_kills);
    CHECK(h.rc.profile().unlocked_characters.count("epsilon") == 1);
    CHECK(h.sink.Count(EventType::CharacterUnlocked) >= 1);
    // The summary names what the run unlocked, for its results screen.
    CHECK(std::count(summary.unlocked.begin(), summary.unlocked.end(), "epsilon") == 1);
    CHECK(summary.unlocked.size() == static_cast<std::size_t>(h.sink.Count(EventType::CharacterUnlocked)));
    const auto list = h.rc.Characters();
    REQUIRE(Find(list, "epsilon") != nullptr);
    CHECK(Find(list, "epsilon")->gate == CharacterGate::Selectable);
    REQUIRE(Find(list, "zeta") != nullptr);
    CHECK(Find(list, "zeta")->challenge.current == std::min<std::int64_t>(stats.total_kills, 5));

    // Reloading the same profile reports nothing new.
    test::Harness again(test::Load(ChallengeSources()), h.rc.profile());
    CHECK(again.sink.Count(EventType::CharacterUnlocked) == 0);
    CHECK(again.rc.profile().unlocked_characters.count("epsilon") == 1);
}

TEST_CASE("lifetime stats survive a profile round trip", "[characters]") {
    Profile p;
    p.stats.credits_earned = 321;
    p.stats.no_damage_arenas = 4;
    p.stats.s_rank_arenas = 2;
    p.stats.runs_completed_as["alpha"] = 3;
    const Result<ParsedProfile> parsed = ParseProfile(SerializeProfile(p));
    REQUIRE(parsed.ok());
    CHECK(parsed->profile.stats.credits_earned == 321);
    CHECK(parsed->profile.stats.no_damage_arenas == 4);
    CHECK(parsed->profile.stats.s_rank_arenas == 2);
    CHECK(parsed->profile.stats.runs_completed_as == p.stats.runs_completed_as);
}

TEST_CASE("challenge definitions are validated", "[characters]") {
    CHECK(HasError(LoadErrors([](json& c) {
                       c["characters"][0]["unlock_kind"] = "bogus";
                       c["characters"][0]["unlock_count"] = 1;
                   }),
                   "unknown unlock_kind"));
    CHECK(HasError(LoadErrors([](json& c) {
                       c["characters"][0]["unlock_kind"] = "total_kills";
                   }),
                   "unlock_count must be >= 1"));
    CHECK(HasError(LoadErrors([](json& c) {
                       c["characters"][0]["unlock_kind"] = "runs_completed_as";
                       c["characters"][0]["unlock_count"] = 1;
                   }),
                   "needs unlock_character"));
    CHECK(HasError(LoadErrors([](json& c) {
                       c["characters"][0]["unlock_kind"] = "runs_completed_as";
                       c["characters"][0]["unlock_count"] = 1;
                       c["characters"][0]["unlock_character"] = "nobody";
                   }),
                   "is not a character"));
}
