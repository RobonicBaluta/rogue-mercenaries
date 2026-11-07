// The ladder (EconomyConfig::Ladder, the user's progression of 2026-11-07): run lengths that end on
// an easy boss, then a choice of easy or hard, the hard one opening the next length; a finale arena
// after the last length once the listed bosses are beaten; later arenas of a long run harder.
#include <string>

#include <catch_amalgamated.hpp>
#include <nlohmann/json.hpp>

#include "rm/routes.h"
#include "test_support.h"

using namespace rm;

namespace {

constexpr const char* kMaps = R"({"schema_version": 1, "maps": [
  {"id": "a0", "name": "Arena Zero", "stage": 0, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a1", "name": "Arena One", "stage": 1, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a2", "name": "Arena Two", "stage": 2, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a3", "name": "Arena Three", "stage": 103, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "lair", "name": "Lair", "stage": 200, "points": [[0, 0, 0]], "boss": {"class": "uEm40", "name": "Beast", "hp": 5000}},
  {"id": "dome", "name": "Dome", "stage": 201, "points": [[0, 0, 0]], "boss": {"class": "uEm43", "name": "Giant", "hp": 8000}},
  {"id": "core", "name": "Core", "stage": 202, "points": [[0, 0, 0]], "boss": {"class": "uEm81", "name": "Wesker", "hp": 9000}}]})";

// Short: 3 arenas (Beast, then Beast or Giant); long: 4 arenas, opened by the short hard boss, its
// bosses at 1.5x health; the finale (Wesker) after long once both hard bosses are beaten.
ContentSources LadderSources() {
    ContentSources src = test::FixtureSources();
    src.maps = kMaps;
    nlohmann::json eco = nlohmann::json::parse(src.economy);
    eco["ladder"] = {
        {"enabled", true},
        {"lengths",
         {{{"id", "short"}, {"name", "Short"}, {"arenas", 3}, {"easy_boss", 200}, {"hard_boss", 201}},
          {{"id", "long"}, {"name", "Long"}, {"arenas", 4}, {"easy_boss", 200}, {"hard_boss", 201}, {"unlocked_by", "short:hard"},
           {"boss_hp_mult", 1.5}}}},
        {"finale", {{"boss", 202}, {"after", "long"}, {"unlocked_by", {"short:hard", "long:hard"}}, {"boss_hp_mult", 2.0}}},
        {"growth_from_arena", 3},
        {"incoming_per_arena", 0.1},
        {"toughness_per_arena", 0.2},
        {"budget_per_arena", 2}};
    src.economy = eco.dump();
    return src;
}

// Plays a run to its end, choosing the route offer `boss_choice` at the last layer.
std::string PlayToBoss(test::Harness& h, std::size_t boss_choice, double& t) {
    while (true) {
        const int index = h.rc.state().arena_index;
        const int count = h.rc.state().arena_count;
        if (index + 1 >= count) break;
        h.rc.SetMapStage(-1);  // a normal arena (not the last run's boss map)
        test::ClearArena(h.rc, t);
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(0).ok());
        REQUIRE(h.rc.LeaveShop().ok());
        const std::size_t pick = h.rc.state().arena_index + 1 == count ? boss_choice : 0;
        REQUIRE(h.rc.ChooseRoute(pick).ok());
        t += 100.0;
    }
    const int boss = h.rc.PlannedMapKey();
    h.rc.SetMapStage(boss);
    REQUIRE(h.rc.BeginArena(t).ok());
    const auto spawns = h.rc.DirectorTick(t + 1.0, std::array<double, 3>{0, 0, 0});
    REQUIRE(spawns.size() == 1);
    const std::string cls = spawns[0].unit_class;
    h.rc.DirectorSpawned(spawns[0].ticket, "boss");
    KillEvent kill;
    kill.now = t + 2.0;
    kill.by_player = true;
    kill.enemy_id = "boss";
    REQUIRE(h.rc.OnEnemyKilled(kill).counted);
    REQUIRE(h.rc.CompleteArena(t + 30.0).ok());
    t += 100.0;
    return cls + "@" + std::to_string(boss);
}

}  // namespace

TEST_CASE("the ladder: easy boss first, then a choice, the hard one opening the next length", "[ladder]") {
    test::Harness h(test::Load(LadderSources()));
    h.rc.SetArenaPool({0, 1, 2, 103, 200, 201, 202}, 0);
    auto lengths = h.rc.LadderLengths();
    REQUIRE(lengths.size() == 2);
    CHECK(lengths[0].open);
    CHECK_FALSE(lengths[0].hard_open);
    CHECK_FALSE(lengths[1].open);
    CHECK(h.rc.StartRun("alpha", 1, "", "long").code == ErrorCode::Locked);

    // Short, first time: only the easy boss ends it.
    REQUIRE(h.rc.StartRun("alpha", 1, "", "short").ok());
    CHECK(h.rc.state().arena_count == 3);
    CHECK(h.rc.state().run_map.layers.back().size() == 1);
    CHECK(h.rc.state().run_map.layers.back()[0].map_key == 200);
    double t = 0.0;
    CHECK(PlayToBoss(h, 0, t) == "uEm40@200");
    REQUIRE(h.rc.ContinueToRewards().ok());
    const auto first = h.rc.EndRun();
    REQUIRE(first.ok());
    // The results say what opened: the giant as a choice at the end of a short run.
    REQUIRE(first.value().ladder_unlocks.size() == 1);
    CHECK(first.value().ladder_unlocks[0].kind == LadderUnlock::Kind::HardBoss);
    CHECK(first.value().ladder_unlocks[0].length_id == "short");
    CHECK(first.value().ladder_unlocks[0].arenas == 3);
    CHECK(first.value().ladder_unlocks[0].boss == 201);
    CHECK(h.rc.profile().stats.bosses_beaten.count("short:easy") == 1);
    CHECK(h.rc.profile().stats.bosses_beaten.count("boss:uEm40") == 1);
    lengths = h.rc.LadderLengths();
    CHECK(lengths[0].hard_open);
    CHECK_FALSE(lengths[1].open);

    // Short again: easy or hard; the hard one opens long.
    REQUIRE(h.rc.StartRun("alpha", 2, "", "short").ok());
    REQUIRE(h.rc.state().run_map.layers.back().size() == 2);
    CHECK(h.rc.state().run_map.layers.back()[0].map_key == 200);
    CHECK(h.rc.state().run_map.layers.back()[1].map_key == 201);
    CHECK(PlayToBoss(h, 1, t) == "uEm43@201");
    REQUIRE(h.rc.ContinueToRewards().ok());
    const auto second = h.rc.EndRun();
    REQUIRE(second.ok());
    REQUIRE(second.value().ladder_unlocks.size() == 1);
    CHECK(second.value().ladder_unlocks[0].kind == LadderUnlock::Kind::Length);
    CHECK(second.value().ladder_unlocks[0].length_id == "long");
    CHECK(second.value().ladder_unlocks[0].arenas == 4);
    CHECK(second.value().ladder_unlocks[0].boss == 200);
    CHECK(h.rc.profile().stats.bosses_beaten.count("short:hard") == 1);
    lengths = h.rc.LadderLengths();
    CHECK(lengths[1].open);
    CHECK_FALSE(lengths[1].finale_open);

    // Long: four arenas; its boss at 1.5x health; easy only at first.
    REQUIRE(h.rc.StartRun("alpha", 3, "", "long").ok());
    CHECK(h.rc.state().arena_count == 4);
    CHECK(h.rc.state().run_map.layers.back().size() == 1);
}

TEST_CASE("the finale follows the last length once its bosses are beaten", "[ladder]") {
    Profile seasoned;
    seasoned.stats.bosses_beaten = {"short:easy", "short:hard", "long:easy", "long:hard"};
    test::Harness h(test::Load(LadderSources()), seasoned);
    h.rc.SetArenaPool({0, 1, 2, 103, 200, 201, 202}, 0);
    const auto lengths = h.rc.LadderLengths();
    REQUIRE(lengths.size() == 2);
    CHECK(lengths[1].finale_open);
    CHECK(lengths[1].arenas == 5);
    CHECK_FALSE(lengths[0].finale_open);
    REQUIRE(h.rc.StartRun("alpha", 4, "", "long").ok());
    const RunMap& map = h.rc.state().run_map;
    REQUIRE(map.layers.size() == 5);
    CHECK(map.layers[3].size() == 2);  // easy and hard
    REQUIRE(map.layers[4].size() == 1);
    CHECK(map.layers[4][0].map_key == 202);
    for (const RunMapNode& n : map.layers[3]) CHECK(n.next == std::vector<int>{0});
}

TEST_CASE("the last hard boss opens the finale, and the results say so", "[ladder]") {
    Profile climbed;
    climbed.stats.bosses_beaten = {"short:easy", "short:hard", "long:easy"};
    test::Harness h(test::Load(LadderSources()), climbed);
    h.rc.SetArenaPool({0, 1, 2, 103, 200, 201, 202}, 0);
    REQUIRE(h.rc.StartRun("alpha", 6, "", "long").ok());
    CHECK(h.rc.state().arena_count == 4);  // no finale yet
    double t = 0.0;
    CHECK(PlayToBoss(h, 1, t) == "uEm43@201");
    REQUIRE(h.rc.ContinueToRewards().ok());
    const auto summary = h.rc.EndRun();
    REQUIRE(summary.ok());
    REQUIRE(summary.value().ladder_unlocks.size() == 1);
    CHECK(summary.value().ladder_unlocks[0].kind == LadderUnlock::Kind::Finale);
    CHECK(summary.value().ladder_unlocks[0].length_id == "long");
    CHECK(summary.value().ladder_unlocks[0].boss == 202);
    // A boss already beaten opens nothing new.
    REQUIRE(h.rc.StartRun("alpha", 7, "", "short").ok());
    CHECK(PlayToBoss(h, 1, t) == "uEm43@201");
    REQUIRE(h.rc.ContinueToRewards().ok());
    const auto again = h.rc.EndRun();
    REQUIRE(again.ok());
    CHECK(again.value().ladder_unlocks.empty());
}

TEST_CASE("a long run's later arenas are harder, a boss's health by its length", "[ladder]") {
    Profile opened;
    opened.stats.bosses_beaten = {"short:easy", "short:hard"};
    test::Harness h(test::Load(LadderSources()), opened);
    h.rc.SetArenaPool({0, 1, 2, 103, 200, 201, 202}, 0);
    REQUIRE(h.rc.StartRun("alpha", 5, "", "long").ok());
    double t = 0.0;
    // Arenas 1 and 2: the route's budget; arena 3 (growth_from_arena): two more enemies.
    std::vector<int> budgets;
    for (int arena = 1; arena <= 3; ++arena) {
        h.rc.SetMapStage(-1);
        REQUIRE(h.rc.BeginArena(t).ok());
        budgets.push_back(h.rc.state().arenas.back().budget);
        test::KillN(h.rc, h.rc.RemainingEnemies(), t + 1.0);
        REQUIRE(h.rc.CompleteArena(t + 50.0).ok());
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(0).ok());
        REQUIRE(h.rc.LeaveShop().ok());
        REQUIRE(h.rc.ChooseRoute(0).ok());
        t += 100.0;
    }
    const RouteDef* r3 = h.rc.content().FindRoute(h.rc.state().arenas[2].route_id);
    REQUIRE(r3 != nullptr);
    CHECK(budgets[2] == r3->enemy_budget + 2);
    // The boss of a long run: 1.5x its map's health.
    const int boss = h.rc.PlannedMapKey();
    h.rc.SetMapStage(boss);
    REQUIRE(h.rc.BeginArena(t).ok());
    const auto spawns = h.rc.DirectorTick(t + 1.0, std::array<double, 3>{0, 0, 0});
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].hp == 7500);  // Beast 5000 x 1.5
}

TEST_CASE("a skin can be unlocked by beating a boss", "[ladder]") {
    ContentSources src = LadderSources();
    nlohmann::json characters = nlohmann::json::parse(src.characters);
    for (auto& c : characters["characters"]) {
        if (c["id"] != "alpha") continue;
        c["skins"] = nlohmann::json::array(
            {{{"id", "alpha_plain"}, {"name", "Plain"}},
             {{"id", "alpha_secret"}, {"name", "Secret"}, {"unlock_kind", "boss_beaten"}, {"unlock_boss", "boss:uEm81"},
              {"unlock_text", "A secret."}}});
    }
    src.characters = characters.dump();
    test::Harness locked(test::Load(src));
    CHECK(locked.rc.StartRun("alpha", 1, "alpha_secret").code == ErrorCode::Locked);
    Profile beaten;
    beaten.stats.bosses_beaten = {"boss:uEm81"};
    test::Harness h(test::Load(src), beaten);
    (void)h.rc.Characters();  // unlocks are evaluated with the profile
    REQUIRE(h.rc.StartRun("alpha", 1, "alpha_secret").ok());
}
