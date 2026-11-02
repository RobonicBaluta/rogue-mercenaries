#include <algorithm>
#include <cmath>
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
    map.roster = {MapEnemy{"uEm10", {0x70100, 0x70101}, {}, 1, 3.0}, MapEnemy{"uEm22", {}, {}, 3, 1.0}};
    return map;
}

constexpr const char* kMapsJson = R"({"schema_version": 1, "maps": [
  {"id": "test_map", "name": "Test Map", "stage": 4, "max_alive": 3, "min_player_distance": 500,
   "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]], "chests": [[10, 0, 10], [20, 0, 20]],
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
    map.roster = {MapEnemy{"uEm22", {}, {}, 3, 1.0}};
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
        if (m.start) {  // the player is moved there: it must be where the map's points are
            const auto& s = *m.start;
            CHECK(std::any_of(m.points.begin(), m.points.end(), [&](const std::array<double, 3>& p) {
                return std::hypot(p[0] - s[0], p[1] - s[1], p[2] - s[2]) < 2500.0;
            }));
        }
        if (m.boss) continue;  // a boss arena spawns its boss alone
        CHECK(std::any_of(m.roster.begin(), m.roster.end(), [](const MapEnemy& e) { return e.min_tier <= 1 && e.weight > 0; }));
    }
}

TEST_CASE("profiles are picked by weight and carry their fields and weapon", "[director]") {
    MapDef map = TestMap(20);
    map.roster = {MapEnemy{"uEm10", {0x70100}, {EnemyProfile{3.0, {{"arm", 0x70150}, {"plaga", 1}}}, EnemyProfile{1.0, {{"arm", 0x70172}}}}, 1, 1.0}};
    Director d(map, 200, 1, 11);
    int first = 0;
    int second = 0;
    for (int i = 0; i < 10; ++i) {
        for (const auto& r : d.Tick(static_cast<double>(i), std::nullopt)) {
            if (r.arm == 0x70150) {
                ++first;
                REQUIRE(r.fields.size() == 2);
                CHECK(r.fields[1].first == "plaga");
            } else {
                CHECK(r.arm == 0x70172);
                ++second;
            }
            d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
            d.Died("u" + std::to_string(r.ticket));
        }
    }
    CHECK(first > second);  // 3:1
    CHECK(second > 0);
}

TEST_CASE("the stage's special enemy joins after a share of the budget, one at a time", "[director]") {
    MapDef map = TestMap(4);
    map.special_after = 0.25;
    MapEnemy special{"uEm0b", {}, {}, 1, 1.0};
    special.special = true;
    map.roster = {MapEnemy{"uEm12", {}, {}, 1, 50.0}, special};
    Director d(map, 20, 1, 5);
    int specials = 0;
    int maxSpecialsAlive = 0;
    int deadBeforeFirstSpecial = -1;
    std::vector<std::string> alive;
    std::vector<std::string> aliveSpecials;
    double now = 0.0;
    while (!d.complete() && now < 400.0) {
        for (const auto& r : d.Tick(now, std::nullopt)) {
            const std::string id = "u" + std::to_string(r.ticket);
            d.Spawned(r.ticket, id);
            if (r.unit_class == "uEm0b") {
                ++specials;
                if (deadBeforeFirstSpecial < 0) deadBeforeFirstSpecial = d.dead();
                aliveSpecials.push_back(id);
            } else {
                alive.push_back(id);
            }
        }
        maxSpecialsAlive = std::max(maxSpecialsAlive, static_cast<int>(aliveSpecials.size()));
        // Normal enemies die first; a special dies every few seconds.
        if (!alive.empty()) {
            d.Died(alive.back());
            alive.pop_back();
        } else if (!aliveSpecials.empty() && static_cast<int>(now) % 3 == 0) {
            d.Died(aliveSpecials.back());
            aliveSpecials.pop_back();
        }
        now += 1.0;
    }
    CHECK(d.complete());
    CHECK(deadBeforeFirstSpecial >= 5);  // 25% of 20
    CHECK(specials >= 1);
    CHECK(maxSpecialsAlive == 1);
}

TEST_CASE("several special types share the map's special limit, which grows with the tier", "[director]") {
    MapDef map = TestMap(12);
    map.special_after = 0.0;
    MapEnemy a{"uEm20", {}, {}, 1, 1.0};
    a.special = true;
    MapEnemy b{"uEm21", {}, {}, 1, 1.0};
    b.special = true;
    map.roster = {MapEnemy{"uEm10", {}, {}, 1, 10.0}, a, b};
    auto specialsAtStart = [&](int tier) {
        Director d(map, 40, tier, 3);
        int n = 0;
        for (const auto& r : d.Tick(0.0, std::nullopt)) n += r.unit_class != "uEm10" ? 1 : 0;
        return n;
    };
    CHECK(specialsAtStart(1) == 1);
    CHECK(specialsAtStart(2) == 2);
    CHECK(specialsAtStart(3) == 2);  // each type is still one at a time
}

TEST_CASE("an arena places a few of the map's chest positions, distinct and repeatable", "[director]") {
    MapDef map = TestMap(3);
    map.chests = {{0, 0, 0}, {100, 0, 0}, {200, 0, 0}, {300, 0, 0}, {400, 0, 0}};
    Director a(map, 10, 1, 9);
    const auto picked = a.PickChests(3);
    REQUIRE(picked.size() == 3);
    CHECK(a.chests() == picked);
    for (std::size_t i = 0; i < picked.size(); ++i) {
        CHECK(std::find(map.chests.begin(), map.chests.end(), picked[i]) != map.chests.end());
        for (std::size_t j = i + 1; j < picked.size(); ++j) CHECK(picked[i] != picked[j]);
    }
    Director b(map, 10, 1, 9);
    CHECK(b.PickChests(3) == picked);  // same seed, same chests
    Director c(map, 10, 1, 9);
    CHECK(c.PickChests(9).size() == 5);  // never more than the map has
    CHECK(c.PickChests(0).empty());
    MapDef bare = TestMap(3);
    Director d(bare, 10, 1, 9);
    CHECK(d.PickChests(3).empty());
}

TEST_CASE("on a large map spawn points within reach of the player are preferred", "[director]") {
    MapDef map = TestMap(1);
    map.min_player_distance = 500.0;
    map.max_player_distance = 3000.0;
    map.points = {{0, 0, 0}, {1500, 0, 0}, {20000, 0, 0}, {0, 0, 25000}};
    Director d(map, 40, 1, 11);
    const std::array<double, 3> player{0, 0, 0};
    for (double now = 0.0; now < 20.0; now += 1.0) {
        for (const auto& r : d.Tick(now, player)) {
            CHECK(r.pos == map.points[1]);  // the only point between 5 and 30 m
            d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
            CHECK(d.Died("u" + std::to_string(r.ticket)));
        }
    }
    // Nothing in reach: the far points are used rather than none.
    map.points = {{0, 0, 0}, {20000, 0, 0}};
    Director far(map, 5, 1, 11);
    const auto spawns = far.Tick(0.0, player);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].pos == map.points[1]);
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
    REQUIRE(h.rc.director() != nullptr);
    CHECK(h.rc.director()->chests().size() == 2);  // the test map has two chest positions (per_arena 3)
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
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "special_after": 2, "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "special_after": 0.5, "points": [[0,0,0]], "roster": [{"class": "uEm10"}, {"class": "uEm0b", "special": true, "max_alive": 1}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]], "roster": [{"class": "uEm10", "profiles": [{"fields": {"Arm": 1}}]}]}]})").empty());
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]], "roster": [{"class": "uEm10", "profiles": [{"weight": 2, "fields": {"arm": 459088, "plaga": 1}}]}]}]})").empty());
    // A boss map needs no roster; its boss must be an enemy class with an hp the game can hold.
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "b", "name": "B", "stage": 103, "points": [[0,0,0]], "boss": {"class": "uEm40", "name": "Uroboros", "hp": 6000}}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "b", "name": "B", "stage": 103, "points": [[0,0,0]], "boss": {"class": "uPl00", "name": "X"}}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "b", "name": "B", "stage": 103, "points": [[0,0,0]], "boss": {"class": "uEm40", "name": "U", "hp": 40000}}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "b", "name": "B", "stage": 103, "points": [[0,0,0]], "boss": {"class": "uEm40"}}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]]}]})").empty());  // no roster, no boss
    // A start is a position.
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 104, "start": [1, -2, 3], "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 104, "start": [1, 2], "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]], "chests": [[1,2,3]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 503, "rotation": false, "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 503, "preload": ["uWp6f", "uEm10Eff"], "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 503, "preload": ["../x"], "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 503, "rotation": "no", "points": [[0,0,0]], "roster": [{"class": "uEm10"}]}]})").empty());
    CHECK_FALSE(errors(R"({"schema_version": 1, "maps": [{"id": "a", "name": "A", "stage": 1, "points": [[0,0,0]], "chests": [[1,2]], "roster": [{"class": "uEm10"}]}]})").empty());
}

TEST_CASE("on a boss map the director spawns the boss alone and its death clears the arena", "[director]") {
    MapDef map = TestMap(5);
    map.boss = MapBoss{"uEm40", "Uroboros", 6000};
    Director d(map, 30, 3, 9);  // the route's budget does not apply
    CHECK(d.budget() == 1);
    const auto first = d.Tick(0.0, std::array<double, 3>{0, 0, 0});
    REQUIRE(first.size() == 1);
    CHECK(first[0].unit_class == "uEm40");
    CHECK(first[0].boss);
    CHECK(first[0].hp == 6000);
    CHECK(first[0].arm == -1);
    CHECK(d.Tick(5.0, std::nullopt).empty());  // never a second unit
    d.Spawned(first[0].ticket, "boss");
    CHECK(d.Tick(10.0, std::nullopt).empty());
    CHECK_FALSE(d.complete());
    CHECK(d.Died("boss"));
    CHECK(d.complete());
    CHECK(d.Tick(20.0, std::nullopt).empty());
    // A failed spawn is asked for again.
    Director retry(map, 30, 3, 9);
    const auto a = retry.Tick(0.0, std::nullopt);
    REQUIRE(a.size() == 1);
    retry.SpawnFailed(a[0].ticket);
    const auto b = retry.Tick(1.0, std::nullopt);
    REQUIRE(b.size() == 1);
    CHECK(b[0].boss);
}
