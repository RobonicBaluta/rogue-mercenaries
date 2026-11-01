#include <algorithm>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/director.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

MapDef TestMap(int max_alive = 3) {
    MapDef map;
    map.id = "test_map";
    map.name = "Test Map";
    map.stage = 4;
    map.max_alive = max_alive;
    map.min_player_distance = 500.0;
    map.points = {{0, 0, 0}, {1000, 0, 0}, {0, 0, 1000}, {2000, 0, 2000}};
    map.roster = {MapEnemy{"uEm10", {0x70100, 0x70101}, 1, 3.0}, MapEnemy{"uEm22", {}, 3, 1.0}};
    return map;
}

constexpr const char* kMapsJson = R"({"schema_version": 1, "maps": [
  {"id": "test_map", "name": "Test Map", "stage": 4, "max_alive": 3, "min_player_distance": 500,
   "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]],
   "roster": [{"class": "uEm10", "arms": [458992, 458993], "weight": 3}, {"class": "uEm22", "min_tier": 3}]}]})";

}  // namespace

TEST_CASE("the director keeps at most max_alive enemies and refills until the budget is dead", "[director]") {
    Director d(TestMap(3), 7, 1, 42);
    auto first = d.Tick(0.0, std::nullopt);
    REQUIRE(first.size() == 3);  // the first fill comes at once
    for (const auto& r : first) {
        CHECK(r.unit_class == "uEm10");  // uEm22 needs tier 3
        CHECK((r.arm == 0x70100 || r.arm == 0x70101));
        d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
    }
    CHECK(d.Tick(0.1, std::nullopt).empty());  // at the cap
    CHECK(d.alive() == 3);
    CHECK_FALSE(d.Died("stranger"));  // not one of ours
    REQUIRE(d.Died("u1"));
    CHECK_FALSE(d.Died("u1"));  // counted once
    auto refill = d.Tick(1.0, std::nullopt);
    REQUIRE(refill.size() == 1);
    d.Spawned(refill[0].ticket, "u" + std::to_string(refill[0].ticket));
    // Kill everything as it comes; the budget caps the total.
    int spawned = 4;
    double now = 2.0;
    while (!d.complete() && now < 100.0) {
        for (const std::string id : {"u2", "u3", "u4", "u5", "u6", "u7", "u8", "u9"}) d.Died(id);
        for (const auto& r : d.Tick(now, std::nullopt)) {
            d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
            ++spawned;
        }
        now += 1.0;
    }
    CHECK(d.complete());
    CHECK(spawned == 7);
    CHECK(d.dead() == 7);
    CHECK(d.remaining() == 0);
    CHECK(d.Tick(now, std::nullopt).empty());
}

TEST_CASE("failed spawns go back to the budget and bad points are avoided", "[director]") {
    Director d(TestMap(1), 2, 1, 7);
    for (int i = 0; i < 10; ++i) {
        auto r = d.Tick(static_cast<double>(i), std::nullopt);
        REQUIRE(r.size() == 1);
        d.SpawnFailed(r[0].ticket);
    }
    CHECK(d.dead() == 0);
    CHECK(d.alive() == 0);
    auto r = d.Tick(20.0, std::nullopt);
    REQUIRE(r.size() == 1);  // still trying, every point included again once all failed
    d.SpawnFailed(r[0].ticket);
    CHECK(d.Tick(20.1, std::nullopt).empty());  // no retry in the same breath
    CHECK(d.Tick(20.7, std::nullopt).size() == 1);
}

TEST_CASE("a roster with nothing at the run's tier still fills the arena", "[director]") {
    MapDef map = TestMap(2);
    map.roster = {MapEnemy{"uEm22", {}, 3, 1.0}};
    Director d(map, 4, 1, 3);
    const auto r = d.Tick(0.0, std::nullopt);
    REQUIRE(r.size() == 2);
    CHECK(r[0].unit_class == "uEm22");
}

TEST_CASE("shipped maps load and every map can spawn at tier 1", "[director]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    for (const std::string& e : r.errors) UNSCOPED_INFO(e);
    REQUIRE(r.ok());
    for (const MapDef& m : r.content->maps()) {
        INFO(m.id);
        CHECK(m.points.size() >= 4);
        CHECK(std::any_of(m.roster.begin(), m.roster.end(), [](const MapEnemy& e) { return e.min_tier <= 1 && e.weight > 0; }));
    }
}

TEST_CASE("spawn points away from the player are preferred", "[director]") {
    Director d(TestMap(1), 50, 1, 9);
    const std::array<double, 3> player{0, 0, 0};
    for (int i = 0; i < 20; ++i) {
        auto r = d.Tick(static_cast<double>(i), player);
        REQUIRE(r.size() == 1);
        CHECK_FALSE((r[0].pos[0] == 0.0 && r[0].pos[2] == 0.0));  // never on the player
        d.Spawned(r[0].ticket, "u" + std::to_string(r[0].ticket));
        d.Died("u" + std::to_string(r[0].ticket));
    }
}

TEST_CASE("arenas on a listed map are run by the director", "[director]") {
    ContentSources src = test::FixtureSources();
    src.maps = kMapsJson;
    const auto content = test::Load(src);
    REQUIRE(content->FindMapByStage(4) != nullptr);
    CHECK(content->FindMapByStage(4)->roster.size() == 2);

    test::Harness h(content);
    h.rc.SetMapStage(4);
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    REQUIRE(h.rc.DirectorActive());
    const int budget = h.rc.RemainingEnemies();
    REQUIRE(budget > 0);
    auto spawns = h.rc.DirectorTick(0.0, std::nullopt);
    REQUIRE_FALSE(spawns.empty());

    // A kill of something the director did not spawn does not count.
    KillEvent stray;
    stray.now = 1.0;
    stray.by_player = true;
    stray.enemy_id = "native";
    CHECK(h.rc.OnEnemyKilled(stray).ignored);
    CHECK(h.rc.RemainingEnemies() == budget);

    // Its own units count whatever killed them (explosions, falls: by_player false).
    h.rc.DirectorSpawned(spawns[0].ticket, "unit_a");
    KillEvent fall;
    fall.now = 2.0;
    fall.by_player = false;
    fall.enemy_id = "unit_a";
    CHECK(h.rc.OnEnemyKilled(fall).counted);
    CHECK(h.rc.RemainingEnemies() == budget - 1);
    CHECK_FALSE(h.rc.OnEnemyKilled(fall).counted);  // the same death reported twice

    // The adapter may hand out an id again for a later spawn: it counts again.
    auto more = h.rc.DirectorTick(10.0, std::nullopt);
    REQUIRE_FALSE(more.empty());
    h.rc.DirectorSpawned(more[0].ticket, "unit_a");
    fall.now = 11.0;
    CHECK(h.rc.OnEnemyKilled(fall).counted);
    CHECK(h.rc.RemainingEnemies() == budget - 2);

    // Without the capability or on an unknown map the old budget path runs.
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kDirector);
    test::Harness plain(content, {}, caps);
    plain.rc.SetMapStage(4);
    REQUIRE(plain.rc.StartRun("alpha", 3).ok());
    REQUIRE(plain.rc.BeginArena(0.0).ok());
    CHECK_FALSE(plain.rc.DirectorActive());
    test::Harness elsewhere(content);
    elsewhere.rc.SetMapStage(1);
    REQUIRE(elsewhere.rc.StartRun("alpha", 3).ok());
    REQUIRE(elsewhere.rc.BeginArena(0.0).ok());
    CHECK_FALSE(elsewhere.rc.DirectorActive());
}

TEST_CASE("maps.json is validated", "[director]") {
    auto errors = [](const char* maps) {
        ContentSources src = test::FixtureSources();
        src.maps = maps;
        return Content::LoadFromStrings(src).errors;
    };
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]], "roster": [{"class": "uPl00"}]}]})").empty());
    CHECK(errors(kMapsJson).empty());
}
