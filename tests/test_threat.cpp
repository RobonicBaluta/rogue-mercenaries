#include <algorithm>
#include <set>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/content.h"
#include "rm/director.h"
#include "rm/routes.h"
#include "test_support.h"

using namespace rm;

namespace {

// Weapons as the shipped table grades them: 0x50 a blade (1), 0x65 a stun rod (3), 0x76 a
// machine gun (4); classes: uEm10 a Majini (1), uEm14 a soldier (2), uEm32 a Licker (3).
ThreatTable Table() {
    ThreatTable t;
    t.none = 1;
    t.fallback = 2;
    t.weapons = {{0x50, 1}, {0x65, 3}, {0x76, 4}};
    t.classes["uEm10"] = 1;
    t.classes["uEm14"] = 2;
    t.classes["uEm32"] = 3;
    t.classes["uEm22"] = 4;
    return t;
}

ArenaThreat Threat(int level, int maxAlive = 0) {
    ArenaThreat a;
    a.level = level;
    a.max_alive = maxAlive;
    a.specials_from = 3;
    a.table = Table();
    return a;
}

EnemyProfile Armed(std::int64_t arm, double weight = 1.0) {
    EnemyProfile p;
    p.weight = weight;
    p.fields = {{"arm", static_cast<double>(arm)}};
    return p;
}

MapDef Map() {
    MapDef map;
    map.id = "m";
    map.name = "M";
    map.stage = 1;
    map.max_alive = 12;
    map.min_player_distance = 0.0;
    map.points = {{0, 0, 0}, {1000, 0, 0}, {0, 0, 1000}, {2000, 0, 2000}};
    MapEnemy majini{"uEm10", {}, {Armed(0x70150), Armed(0x70165), Armed(0x70176)}, 1, 6.0};
    MapEnemy soldier{"uEm14", {}, {Armed(0x70150)}, 1, 2.0};
    map.roster = {majini, soldier};
    return map;
}

// Every spawn of a whole arena, its deaths in order.
std::vector<SpawnRequest> RunArena(Director& d, int* maxAlive = nullptr) {
    std::vector<SpawnRequest> all;
    std::vector<std::string> alive;
    int peak = 0;
    double now = 0.0;
    while (!d.complete() && now < 600.0) {
        for (const SpawnRequest& r : d.Tick(now, std::nullopt)) {
            d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
            alive.push_back("u" + std::to_string(r.ticket));
            all.push_back(r);
        }
        peak = std::max(peak, static_cast<int>(alive.size()));
        if (!alive.empty()) {
            d.Died(alive.front());
            alive.erase(alive.begin());
        }
        now += 1.0;
    }
    if (maxAlive != nullptr) *maxAlive = peak;
    return all;
}

std::int64_t ArmOf(const SpawnRequest& r) { return r.arm; }

}  // namespace

TEST_CASE("a spawn's threat is the higher of its class's and its weapon's", "[threat]") {
    const ThreatTable t = Table();
    CHECK(t.Weapon(-1) == 1);
    CHECK(t.Weapon(0x70100) == 1);  // no weapon
    CHECK(t.Weapon(0x70150) == 1);
    CHECK(t.Weapon(0x70176) == 4);
    CHECK(t.Weapon(0x70199) == 2);  // not listed
    CHECK(t.Weapon(458992) == 2);   // not a 0x701NN weapon
    const MapDef map = Map();
    CHECK(t.Of(map.roster[0], 0x70165) == 3);
    CHECK(t.Of(map.roster[1], 0x70150) == 2);  // the soldier's class
    CHECK(t.Lowest(map.roster[0]) == 1);
    CHECK(t.Lowest(map) == 1);
    MapDef soldiers = map;
    soldiers.roster = {map.roster[1]};
    CHECK(t.Lowest(soldiers) == 2);
}

TEST_CASE("an arena's threat level holds the director to enemies at or under it", "[threat]") {
    auto spawned = [](int level) {
        Director d(Map(), 40, 1, 11, Threat(level));
        return RunArena(d);
    };
    for (const SpawnRequest& r : spawned(1)) {
        CHECK(r.unit_class == "uEm10");
        CHECK(ArmOf(r) == 0x70150);
    }
    std::set<std::int64_t> arms3;
    std::set<std::string> classes3;
    for (const SpawnRequest& r : spawned(3)) {
        arms3.insert(ArmOf(r));
        classes3.insert(r.unit_class);
    }
    CHECK(arms3.count(0x70165) == 1);  // the stun rod joins at 3
    CHECK(arms3.count(0x70176) == 0);  // the machine gun waits for 4
    CHECK(classes3.count("uEm14") == 1);
    std::set<std::int64_t> arms4;
    for (const SpawnRequest& r : spawned(4)) arms4.insert(ArmOf(r));
    CHECK(arms4.count(0x70176) == 1);
    // Without a level, every variant (as before).
    Director open(Map(), 40, 1, 11);
    std::set<std::int64_t> armsAny;
    for (const SpawnRequest& r : RunArena(open)) armsAny.insert(ArmOf(r));
    CHECK(armsAny.size() == 3);
}

TEST_CASE("a low threat level keeps fewer enemies alive at once", "[threat]") {
    Director low(Map(), 30, 1, 3, Threat(1, 6));
    int peak = 0;
    RunArena(low, &peak);
    CHECK(peak <= 6);
    CHECK(low.Tick(0.0, std::nullopt).empty());  // spent
    Director first(Map(), 30, 1, 3, Threat(1, 6));
    CHECK(first.Tick(0.0, std::nullopt).size() == 6);  // the first fill
    Director open(Map(), 30, 1, 3, Threat(4, 0));
    CHECK(open.Tick(0.0, std::nullopt).size() == 12);  // the map's own max_alive
}

TEST_CASE("a map with nothing that mild fields its mildest enemies", "[threat]") {
    MapDef map = Map();
    map.roster = {MapEnemy{"uEm14", {}, {Armed(0x70150), Armed(0x70165)}, 1, 1.0}};
    Director d(map, 20, 1, 5, Threat(1));
    CHECK(d.threat_level() == 1);
    CHECK(d.pick_level() == 2);
    const auto spawns = RunArena(d);
    REQUIRE_FALSE(spawns.empty());
    for (const SpawnRequest& r : spawns) CHECK(ArmOf(r) == 0x70150);  // the stun rod (3) stays out
    CHECK(d.complete());
}

TEST_CASE("specials join only from the threat's specials level, one more per level above it", "[threat]") {
    MapDef map = Map();
    map.special_after = 0.0;
    MapEnemy licker{"uEm32", {}, {}, 1, 1.0};
    licker.special = true;
    MapEnemy chainsaw{"uEm22", {}, {}, 1, 1.0};
    chainsaw.special = true;
    map.roster = {MapEnemy{"uEm10", {}, {Armed(0x70150)}, 1, 10.0}, licker, chainsaw};
    auto specialsAtStart = [&](int level) {
        Director d(map, 40, 1, 7, Threat(level));
        int n = 0;
        for (const SpawnRequest& r : d.Tick(0.0, std::nullopt)) n += r.unit_class != "uEm10" ? 1 : 0;
        return n;
    };
    CHECK(specialsAtStart(1) == 0);
    CHECK(specialsAtStart(2) == 0);
    CHECK(specialsAtStart(3) == 1);  // the Licker; the chainsaw (4) waits
    CHECK(specialsAtStart(4) == 2);
}

TEST_CASE("the first arena is level 1 and later ones their route's tier plus one", "[threat]") {
    EconomyConfig::EnemyThreat t;
    CHECK(t.Level(1, 1) == 0);  // disabled
    t.enabled = true;
    t.later_arena_bonus = 1;
    t.max_level = 4;
    t.max_alive_by_level = {6, 8, 10, 12};
    CHECK(t.Level(1, 1) == 1);
    CHECK(t.Level(2, 1) == 2);
    CHECK(t.Level(2, 2) == 3);
    CHECK(t.Level(3, 2) == 3);
    CHECK(t.Level(3, 3) == 4);
    CHECK(t.Level(3, 7) == 4);
    CHECK(t.ForArena(1, 1).max_alive == 6);
    CHECK(t.ForArena(3, 3).max_alive == 12);
}

TEST_CASE("the shipped threat table grades every enemy the maps field", "[threat]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const auto& t = r.content->economy().enemy_threat;
    REQUIRE(t.enabled);
    CHECK(t.max_alive_by_level.size() == static_cast<std::size_t>(t.max_level));
    int mild = 0;
    for (const MapDef& m : r.content->maps()) {
        INFO(m.name);
        for (const MapEnemy& e : m.roster) {
            INFO(e.unit_class);
            CHECK(t.table.classes.count(e.unit_class) == 1);
            for (const EnemyProfile& p : e.profiles) {
                const std::int64_t arm = ProfileArm(p);
                if (arm < 0 || (arm & 0xFF) == 0) continue;
                CHECK(t.table.weapons.count(static_cast<int>(arm & 0xFF)) == 1);
            }
        }
        if (!m.boss && t.table.Lowest(m) <= 1) ++mild;
    }
    CHECK(mild >= 6);  // plenty of maps for a first arena
    // The soldier maps (stun rods, a Licker) are never a first arena.
    for (const MapDef& m : r.content->maps()) {
        if (m.name == "Experimental Facility" || m.name == "Underground Facility") CHECK(t.table.Lowest(m) == 3);
    }
}

TEST_CASE("the run map only puts a map on a node when it can field enemies at that node's level", "[threat]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const Content& c = *r.content;
    const auto& t = c.economy().enemy_threat;
    std::vector<int> arenas;
    for (const MapDef& m : c.maps()) {
        if (m.rotation) arenas.push_back(m.stage);
    }
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        const RunMap map = GenerateRunMap(c, rng, arenas, -1);
        for (std::size_t l = 0; l < map.layers.size(); ++l) {
            for (const RunMapNode& n : map.layers[l]) {
                const MapDef* m = c.FindMapByStage(n.map_key);
                REQUIRE(m != nullptr);
                if (m->boss) continue;
                const int level = t.Level(static_cast<int>(l) + 1, c.FindRoute(n.route_id)->difficulty_tier);
                INFO("seed " << seed << " arena " << l + 1 << " " << m->name);
                CHECK(t.table.Lowest(*m) <= level);
            }
        }
        CHECK(t.table.Lowest(*c.FindMapByStage(map.layers[0][0].map_key)) == 1);
    }
}

TEST_CASE("economy enemy_threat is validated", "[threat]") {
    auto errors = [](const nlohmann::json& threat) {
        return Content::LoadFromStrings(test::EditFixture([&](auto&, auto&, auto&, auto&, nlohmann::json& economy) {
                   economy["enemy_threat"] = threat;
               })).errors;
    };
    const nlohmann::json ok = {{"enabled", true}, {"max_level", 4}, {"max_alive_by_level", {6, 8, 10, 12}},
                               {"specials_from_level", 3}, {"weapons", {{"none", 1}, {"50", 1}, {"76", 4}}},
                               {"classes", {{"uEm10", 1}}}};
    CHECK(errors(ok).empty());
    nlohmann::json bad = ok;
    bad["weapons"]["7G"] = 2;
    CHECK_FALSE(errors(bad).empty());
    bad = ok;
    bad["weapons"]["50"] = 5;  // above max_level
    CHECK_FALSE(errors(bad).empty());
    bad = ok;
    bad["max_alive_by_level"] = {6, 8};
    CHECK_FALSE(errors(bad).empty());
    bad = ok;
    bad["classes"] = {{"uPl00", 1}};
    CHECK_FALSE(errors(bad).empty());
    bad = ok;
    bad["unknown"] = 1;
    CHECK_FALSE(errors(bad).empty());
}

TEST_CASE("a class's weight by level makes it likelier at the bigger tiers", "[threat]") {
    ThreatTable t = Table();
    t.class_weight["uEm14"] = {1.0, 1.0, 1.0, 6.0};
    CHECK(t.ClassWeight("uEm14", 1) == Catch::Approx(1.0));
    CHECK(t.ClassWeight("uEm14", 4) == Catch::Approx(6.0));
    CHECK(t.ClassWeight("uEm14", 5) == Catch::Approx(1.0));  // past the list
    CHECK(t.ClassWeight("uEm14", 0) == Catch::Approx(1.0));  // no threat level
    CHECK(t.ClassWeight("uEm10", 4) == Catch::Approx(1.0));  // not listed
    const auto soldiers = [&](int level, bool weighted) {
        ArenaThreat a = Threat(level);
        if (weighted) a.table = t;
        Director d(Map(), 60, 1, 11, a);
        int n = 0, all = 0;
        for (const SpawnRequest& r : RunArena(d)) {
            ++all;
            n += r.unit_class == "uEm14" ? 1 : 0;
        }
        return all > 0 ? static_cast<double>(n) / all : 0.0;
    };
    // Level 4 fields every Majini (weight 6) and the soldiers (2): about a quarter; six times the soldiers' weight
    // makes them about two thirds. Level 2 (one Majini variant in three fits) is the same either way.
    CHECK(soldiers(4, false) < 0.4);
    CHECK(soldiers(4, true) > 0.55);
    CHECK(soldiers(2, true) == Catch::Approx(soldiers(2, false)));
}

TEST_CASE("economy enemy_threat's class_weight_by_level is validated", "[threat]") {
    const auto errors = [](const nlohmann::json& weights) {
        ContentSources src = test::EditFixture([&](auto&, auto&, auto&, auto&, nlohmann::json& economy) {
            economy["enemy_threat"] = {{"enabled", true}, {"max_level", 4}, {"classes", {{"uEm10", 1}}}, {"class_weight_by_level", weights}};
        });
        return Content::LoadFromStrings(src).errors;
    };
    CHECK(errors({{"uEm30", {1.0, 1.0, 2.0, 3.5}}}).empty());
    for (const nlohmann::json& bad : {nlohmann::json{{"uEm30", {1, 1, 1, 1, 1}}}, nlohmann::json{{"uEm30", nlohmann::json::array()}},
                                      nlohmann::json{{"uEm30", {1, -1}}}, nlohmann::json{{"dog", {1}}}, nlohmann::json{{"uEm30", 2}}}) {
        INFO(bad.dump());
        const auto e = errors(bad);
        CHECK(std::any_of(e.begin(), e.end(), [](const std::string& x) { return x.find("class_weight_by_level") != std::string::npos; }));
    }
}

TEST_CASE("a normal enemy's max_alive caps how many of it are alive at once", "[threat]") {
    MapDef map = Map();
    MapEnemy dog{"uEm30", {}, {}, 1, 50.0};  // by far the likeliest pick
    dog.max_alive = 3;
    map.roster.push_back(dog);
    Director d(map, 60, 1, 11);
    std::vector<std::pair<std::string, std::string>> alive;  // id, class
    int peakDogs = 0, dogs = 0;
    double now = 0.0;
    while (!d.complete() && now < 600.0) {
        for (const SpawnRequest& r : d.Tick(now, std::nullopt)) {
            const std::string id = "u" + std::to_string(r.ticket);
            d.Spawned(r.ticket, id);
            alive.emplace_back(id, r.unit_class);
            dogs += r.unit_class == "uEm30" ? 1 : 0;
        }
        int n = 0;
        for (const auto& [id, cls] : alive) n += cls == "uEm30" ? 1 : 0;
        peakDogs = std::max(peakDogs, n);
        if (now >= 5.0 && !alive.empty()) {  // a crowd builds up first, then one dies a second
            d.Died(alive.front().first);
            alive.erase(alive.begin());
        }
        now += 1.0;
    }
    CHECK(dogs > 10);
    CHECK(peakDogs == 3);
}

