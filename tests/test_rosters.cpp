#include <algorithm>
#include <set>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/rosters.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

// Four maps, each with its own Majini kind; three have a special of their own.
constexpr const char* kMaps = R"({"schema_version": 1, "maps": [
  {"id": "town", "name": "Town", "stage": 0, "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]],
   "roster": [{"class": "uEm10", "weight": 10}, {"class": "uEm22", "special": true, "weight": 2}]},
  {"id": "village", "name": "Village", "stage": 1, "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]],
   "roster": [{"class": "uEm12", "weight": 10, "profiles": [{"weight": 1, "fields": {"arm": 459103, "plaga": 1}}]},
              {"class": "uEm0b", "special": true}]},
  {"id": "lab", "name": "Lab", "stage": 2, "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]],
   "roster": [{"class": "uEm14", "weight": 10}, {"class": "uEm32", "special": true, "weight": 4}]},
  {"id": "dock", "name": "Dock", "stage": 3, "points": [[0, 0, 0], [1000, 0, 0], [0, 0, 1000], [2000, 0, 2000]],
   "roster": [{"class": "uEm10", "weight": 6}]}]})";

json Rosters() {
    return {{"enabled", true},      {"from_arena", 2},      {"normal_guests", 1}, {"special_guests", 1},
            {"max_classes", 4},     {"normal_share", 0.35}, {"special_share", 0.5},
            {"guests", {{"uEm10", 0}, {"uEm12", 1}, {"uEm14", 2}, {"uEm22", 0}, {"uEm0b", 1}, {"uEm32", 2}}}};
}

ContentSources Sources(const json& rosters = Rosters()) {
    ContentSources src = test::EditFixture([&](auto&, auto&, auto&, auto&, json& economy) { economy["run_rosters"] = rosters; });
    src.maps = kMaps;
    return src;
}

bool Has(const MapDef& m, const std::string& unit_class) {
    return std::any_of(m.roster.begin(), m.roster.end(), [&](const MapEnemy& e) { return e.unit_class == unit_class; });
}

const MapEnemy* Entry(const MapDef& m, const std::string& unit_class) {
    for (const MapEnemy& e : m.roster) {
        if (e.unit_class == unit_class) return &e;
    }
    return nullptr;
}

std::vector<int> Rotation(const Content& c) {
    std::vector<int> out;
    for (const MapDef& m : c.maps()) {
        if (m.rotation) out.push_back(m.stage);
    }
    return out;
}

}  // namespace

TEST_CASE("a guest is its home map's roster entry", "[rosters]") {
    const auto c = test::Load(Sources());
    const std::optional<MapEnemy> village = GuestEntry(*c, "uEm12");
    REQUIRE(village);
    CHECK(village->home == 1);
    REQUIRE(village->profiles.size() == 1);
    CHECK(ProfileArm(village->profiles[0]) == 459103);
    CHECK(GuestEntry(*c, "uEm32")->special);
    CHECK_FALSE(GuestEntry(*c, "uEm40"));  // not listed: it does not travel
    CHECK(GuestHomes(*c, {"uEm12", "uEm0b", "uEm32"}) == "Village and Lab");
    CHECK(GuestHomes(*c, {"uEm12", "uEm14", "uEm22"}) == "Village, Lab and Town");
    CHECK(GuestHomes(*c, {}).empty());
}

TEST_CASE("guests join a map's roster with their share of the picks", "[rosters]") {
    const auto c = test::Load(Sources());
    const MapDef& town = *c->FindMapByStage(0);
    const MapDef arena = WithGuests(*c, town, {"uEm12", "uEm32", "uEm10", "uEm40"});
    REQUIRE(arena.roster.size() == 4);  // its own uEm10 stays as it is, uEm40 does not travel
    const MapEnemy* tribal = Entry(arena, "uEm12");
    const MapEnemy* licker = Entry(arena, "uEm32");
    REQUIRE(tribal != nullptr);
    REQUIRE(licker != nullptr);
    CHECK(tribal->home == 1);
    CHECK(Entry(arena, "uEm10")->home == -1);
    // 35 percent of the normal picks: 10 * 0.35 / 0.65; half of the special ones: 2 * 0.5 / 0.5.
    CHECK(tribal->weight == Catch::Approx(10.0 * 0.35 / 0.65));
    CHECK(licker->weight == Catch::Approx(2.0));
    CHECK(licker->special);
    // A map without specials of its own: the special guest keeps its home weight.
    const MapDef dock = WithGuests(*c, *c->FindMapByStage(3), {"uEm32"});
    CHECK(Entry(dock, "uEm32")->weight == Catch::Approx(4.0));
    // No guests: the map as it is.
    CHECK(WithGuests(*c, town, {}).roster.size() == 2);
}

TEST_CASE("guests come from the second arena on, never one the map has, within the class cap", "[rosters]") {
  // max_classes 4: every map takes a normal and a special guest; 3: the maps with two classes
  // of their own only the normal one.
  for (const int cap : {4, 3}) {
    json rosters = Rosters();
    rosters["max_classes"] = cap;
    const auto c = test::Load(Sources(rosters));
    const std::vector<int> pool = Rotation(*c);
    int planned = 0;
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng route = Rng::Derive(seed, streams::kRoute);
        RunMap map = GenerateRunMap(*c, route, pool, -1);
        Rng roster = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, roster, map);
        for (const RunMapNode& n : map.layers[0]) CHECK(n.guests.empty());
        for (std::size_t l = 1; l < map.layers.size(); ++l) {
            for (const RunMapNode& n : map.layers[l]) {
                const MapDef& m = *c->FindMapByStage(n.map_key);
                INFO("seed " << seed << " layer " << l << " " << m.name);
                int normal = 0, special = 0;
                std::set<std::string> distinct(n.guests.begin(), n.guests.end());
                CHECK(distinct.size() == n.guests.size());
                for (const std::string& g : n.guests) {
                    CHECK_FALSE(Has(m, g));
                    (GuestEntry(*c, g)->special ? special : normal) += 1;
                }
                CHECK(normal == 1);  // every map can take one: three Majini kinds, one its own
                CHECK(static_cast<int>(m.roster.size() + n.guests.size()) <= cap);
                CHECK(special == (static_cast<int>(m.roster.size()) + 1 < cap ? 1 : 0));
                planned += static_cast<int>(n.guests.size());
            }
        }
    }
    CHECK(planned > 1000);
  }
    // Disabled: no guests at all.
    json off = Rosters();
    off["enabled"] = false;
    const auto plain = test::Load(Sources(off));
    Rng route = Rng::Derive(1, streams::kRoute);
    RunMap map = GenerateRunMap(*plain, route, Rotation(*plain), -1);
    Rng roster = Rng::Derive(1, streams::kRoster);
    PlanRunRosters(*plain, roster, map);
    for (const auto& layer : map.layers) {
        for (const RunMapNode& n : layer) CHECK(n.guests.empty());
    }
}

TEST_CASE("the plan prefers classes the run has not fielded yet", "[rosters]") {
    const auto c = test::Load(Sources());
    // Village (uEm12) first, then Town (uEm10): of the Majini kinds only the Lab's soldiers are new.
    for (std::uint64_t seed = 0; seed < 50; ++seed) {
        RunMap map;
        map.layers = {{RunMapNode{1, "r1a", {0}, {}}}, {RunMapNode{0, "r2", {}, {}}}};
        Rng rng = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, rng, map);
        REQUIRE_FALSE(map.layers[1][0].guests.empty());
        CHECK(map.layers[1][0].guests[0] == "uEm14");
        // Its special: the Village's uEm0b was fielded, the Lab's Licker was not.
        REQUIRE(map.layers[1][0].guests.size() == 2);
        CHECK(map.layers[1][0].guests[1] == "uEm32");
    }
}

TEST_CASE("the plan follows each path: what one way in fielded does not hold back another", "[rosters]") {
    json rosters = Rosters();
    rosters["from_arena"] = 3;  // only the last layer: the paths into it are the maps' own enemies
    const auto c = test::Load(Sources(rosters));
    for (std::uint64_t seed = 0; seed < 50; ++seed) {
        // Dock -> Village -> Town, and Dock -> Lab -> Dock.
        RunMap map;
        map.layers = {{RunMapNode{3, "r1a", {0, 1}, {}}},
                      {RunMapNode{1, "r1a", {0}, {}}, RunMapNode{2, "r2", {1}, {}}},
                      {RunMapNode{0, "r2", {}, {}}, RunMapNode{3, "r3", {}, {}}}};
        Rng rng = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, rng, map);
        CHECK(map.layers[1][0].guests.empty());
        const auto& town = map.layers[2][0].guests;
        const auto& dock = map.layers[2][1].guests;
        REQUIRE(town.size() == 2);
        CHECK(town[0] == "uEm14");  // the Village way fielded the tribal Majini, not the soldiers
        CHECK(town[1] == "uEm32");  // nor the Licker
        REQUIRE(dock.size() == 2);
        CHECK(dock[0] == "uEm12");  // the Lab way fielded the soldiers
        CHECK(dock[1] != "uEm32");  // and the Licker
    }
}

TEST_CASE("guests stay at or under the arena's threat level, specials only where they can join", "[rosters]") {
    // Threat levels: the soldiers (2) wait for level 2, every special for level 3.
    const json threat = {{"enabled", true}, {"later_arena_bonus", 1}, {"max_level", 4}, {"specials_from_level", 3},
                         {"weapons", {{"none", 1}, {"5f", 1}}},
                         {"classes", {{"uEm10", 1}, {"uEm12", 1}, {"uEm14", 2}, {"uEm0b", 2}, {"uEm22", 4}, {"uEm32", 3}}}};
    ContentSources src = test::EditFixture([&](auto&, auto&, auto&, auto&, json& economy) {
        economy["run_rosters"] = Rosters();
        economy["enemy_threat"] = threat;
    });
    src.maps = kMaps;
    const auto c = test::Load(src);
    const auto& t = c->economy().enemy_threat;
    std::set<std::string> seen;
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng route = Rng::Derive(seed, streams::kRoute);
        RunMap map = GenerateRunMap(*c, route, Rotation(*c), -1);
        Rng roster = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, roster, map);
        for (std::size_t l = 1; l < map.layers.size(); ++l) {
            for (const RunMapNode& n : map.layers[l]) {
                const int level = t.Level(static_cast<int>(l) + 1, c->FindRoute(n.route_id)->difficulty_tier);
                for (const std::string& g : n.guests) {
                    const MapEnemy e = *GuestEntry(*c, g);
                    INFO("seed " << seed << " level " << level << " guest " << g);
                    CHECK(t.table.Lowest(e) <= level);
                    if (e.special) CHECK(level >= 3);
                    seen.insert(g);
                }
            }
        }
    }
    CHECK(seen.count("uEm14") == 1);
    CHECK(seen.count("uEm32") == 1);
    CHECK(seen.count("uEm22") == 1);  // at level 4
}

TEST_CASE("the same seed plans the same guests, and planning leaves the run map as it was", "[rosters]") {
    const auto c = test::Load(Sources());
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        Rng a = Rng::Derive(seed, streams::kRoute);
        Rng b = Rng::Derive(seed, streams::kRoute);
        RunMap first = GenerateRunMap(*c, a, Rotation(*c), 0);
        const RunMap untouched = GenerateRunMap(*c, b, Rotation(*c), 0);
        RunMap second = untouched;
        Rng r1 = Rng::Derive(seed, streams::kRoster);
        Rng r2 = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, r1, first);
        PlanRunRosters(*c, r2, second);
        for (std::size_t l = 0; l < first.layers.size(); ++l) {
            for (std::size_t n = 0; n < first.layers[l].size(); ++n) {
                CHECK(first.layers[l][n].guests == second.layers[l][n].guests);
                CHECK(first.layers[l][n].map_key == untouched.layers[l][n].map_key);
                CHECK(first.layers[l][n].route_id == untouched.layers[l][n].route_id);
                CHECK(first.layers[l][n].next == untouched.layers[l][n].next);
            }
        }
    }
}

TEST_CASE("a run's later arena fields its guests through the director", "[rosters]") {
    test::Harness h(test::Load(Sources()));
    h.rc.SetArenaPool({0, 1, 2, 3}, 3);  // Dock first: one class of its own
    REQUIRE(h.rc.StartRun("alpha", 9).ok());
    CHECK(h.rc.PlannedGuests().empty());  // the first arena: its own enemies
    test::ClearArena(h.rc, 0.0);          // no stage set: played without the director
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    REQUIRE(h.rc.LeaveShop().ok());
    // The NEXT ROUTE help names where the guests come from (when it fits).
    bool named = false;
    for (const OfferView& o : h.rc.Snapshot(31.0).route_offers) named = named || o.detail.find("Also enemies from ") != std::string::npos;
    CHECK(named);
    REQUIRE(h.rc.ChooseRoute(0).ok());
    const int stage = h.rc.PlannedMapKey();
    const std::vector<std::string> guests = h.rc.PlannedGuests();
    REQUIRE_FALSE(guests.empty());
    const RunSnapshot snap = h.rc.Snapshot(32.0);
    bool listed = false;
    for (const RunMapNodeView& v : snap.run_map[1]) {
        if (v.current) listed = v.guests == guests && !v.guests_from.empty();
    }
    CHECK(listed);
    // A stage other than the planned one (a developer pick) is played without them.
    const std::optional<MapDef> other = h.rc.ArenaMap(stage == 0 ? 1 : 0);
    REQUIRE(other);
    for (const MapEnemy& e : other->roster) CHECK(e.home == -1);

    h.sink.Clear();
    h.rc.SetMapStage(stage);
    REQUIRE(h.rc.BeginArena(40.0).ok());
    REQUIRE(h.rc.DirectorActive());
    for (const std::string& g : guests) CHECK(Has(h.rc.director()->map(), g));
    std::string logged;
    for (const Event& e : h.sink.events) {
        if (e.type != EventType::ArenaStarted) continue;
        if (const FieldValue* v = e.Find("guests")) logged = std::get<std::string>(*v);
    }
    CHECK(logged.find(guests[0]) != std::string::npos);
    // Over the arena the normal guest is spawned among the map's own.
    std::set<std::string> spawned;
    double now = 41.0;
    int unit = 0;
    while (!h.rc.IsArenaComplete() && now < 400.0) {
        for (const SpawnRequest& r : h.rc.DirectorTick(now, std::nullopt)) {
            spawned.insert(r.unit_class);
            const std::string id = "u" + std::to_string(++unit);
            h.rc.DirectorSpawned(r.ticket, id);
            KillEvent k;
            k.now = now;
            k.by_player = true;
            k.enemy_id = id;
            h.rc.OnEnemyKilled(k);
        }
        now += 1.0;
    }
    CHECK(spawned.count(guests[0]) == 1);
}

TEST_CASE("a class the adapter cannot create is not picked again that arena", "[rosters]") {
    MapDef map;
    map.id = "m";
    map.name = "M";
    map.stage = 0;
    map.max_alive = 4;
    map.min_player_distance = 0.0;
    map.points = {{0, 0, 0}, {1000, 0, 0}};
    map.roster = {MapEnemy{"uEm10", {}, {}, 1, 1.0}, MapEnemy{"uEm12", {}, {}, 1, 5.0}};
    MapEnemy licker{"uEm32", {}, {}, 1, 1.0};
    licker.special = true;
    map.roster.push_back(licker);
    map.special_after = 0.0;
    Director d(map, 60, 1, 4);
    d.Unavailable("uEm12");
    d.Unavailable("uEm32");
    double now = 0.0;
    int unit = 0;
    std::set<std::string> spawned;
    while (!d.complete() && now < 300.0) {
        for (const SpawnRequest& r : d.Tick(now, std::nullopt)) {
            spawned.insert(r.unit_class);
            const std::string id = "u" + std::to_string(++unit);
            d.Spawned(r.ticket, id);
            d.Died(id);
        }
        now += 1.0;
    }
    CHECK(d.complete());
    CHECK(spawned == std::set<std::string>{"uEm10"});
}

TEST_CASE("the shipped rosters plan guests from across the game for every later arena", "[rosters]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    for (const std::string& e : r.errors) UNSCOPED_INFO(e);
    REQUIRE(r.ok());
    const Content& c = *r.content;
    const auto& cfg = c.economy().run_rosters;
    REQUIRE(cfg.enabled);
    // Every listed guest resolves to its home map's entry, and the threat table grades it.
    for (const auto& [unit_class, home] : cfg.guests) {
        INFO(unit_class);
        const std::optional<MapEnemy> e = GuestEntry(c, unit_class);
        REQUIRE(e);
        // From a Mercenaries map (their rosters are surveyed), but the dogs: no Mercenaries stage has them, so their
        // entry is the Bridge's (s104), whose campaign area places them (2026-11-24).
        if (unit_class != "uEm30") CHECK(c.FindMapByStage(home)->stage < 100);
        CHECK(c.economy().enemy_threat.table.classes.count(unit_class) == 1);
    }
    const std::vector<int> pool = Rotation(c);
    std::set<std::string> seen;
    int nodes = 0, withGuests = 0;
    for (std::uint64_t seed = 0; seed < 300; ++seed) {
        Rng route = Rng::Derive(seed, streams::kRoute);
        RunMap map = GenerateRunMap(c, route, pool, -1);
        Rng roster = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(c, roster, map);
        for (std::size_t l = 0; l < map.layers.size(); ++l) {
            for (const RunMapNode& n : map.layers[l]) {
                const MapDef& m = *c.FindMapByStage(n.map_key);
                if (l == 0 || m.boss) {
                    CHECK(n.guests.empty());
                    continue;
                }
                ++nodes;
                withGuests += n.guests.empty() ? 0 : 1;
                CHECK(static_cast<int>(m.roster.size() + n.guests.size()) <= std::max(cfg.max_classes, static_cast<int>(m.roster.size())));
                for (const std::string& g : n.guests) {
                    CHECK_FALSE(Has(m, g));
                    seen.insert(g);
                }
            }
        }
    }
    CHECK(withGuests * 10 >= nodes * 9);  // nearly every later arena brings someone new
    CHECK(seen.size() >= cfg.guests.size() - 1);
}

TEST_CASE("economy run_rosters is validated", "[rosters]") {
    auto errors = [](const json& rosters) {
        ContentSources src = test::EditFixture([&](auto&, auto&, auto&, auto&, json& economy) { economy["run_rosters"] = rosters; });
        src.maps = kMaps;
        return Content::LoadFromStrings(src).errors;
    };
    CHECK(errors(Rosters()).empty());
    json bad = Rosters();
    bad["guests"] = json::object();
    CHECK_FALSE(errors(bad).empty());  // enabled without guests
    bad = Rosters();
    bad["guests"]["uEm14"] = 9;  // no map 9
    CHECK_FALSE(errors(bad).empty());
    bad = Rosters();
    bad["guests"]["uEm14"] = 0;  // Town has no uEm14
    CHECK_FALSE(errors(bad).empty());
    bad = Rosters();
    bad["guests"]["uPl00"] = 0;
    CHECK_FALSE(errors(bad).empty());
    bad = Rosters();
    bad["normal_share"] = 1.0;
    CHECK_FALSE(errors(bad).empty());
    bad = Rosters();
    bad["max_classes"] = 0;
    CHECK_FALSE(errors(bad).empty());
    bad = Rosters();
    bad["unknown"] = 1;
    CHECK_FALSE(errors(bad).empty());
}

TEST_CASE("a guest always_from_level joins every arena from that level, whatever the class cap", "[rosters]") {
    // The soldiers (threat 2) always from level 2: every arena from the second (its route's tier plus one).
    const json threat = {{"enabled", true}, {"later_arena_bonus", 1}, {"max_level", 4}, {"specials_from_level", 3},
                         {"weapons", {{"none", 1}, {"5f", 1}}},
                         {"classes", {{"uEm10", 1}, {"uEm12", 1}, {"uEm14", 2}, {"uEm0b", 2}, {"uEm22", 4}, {"uEm32", 3}}}};
    json rosters = Rosters();
    rosters["always_from_level"] = {{"uEm14", 2}};
    rosters["max_classes"] = 2;  // the maps' own two (or one) leave no room for it otherwise
    ContentSources src = test::EditFixture([&](auto&, auto&, auto&, auto&, json& economy) {
        economy["run_rosters"] = rosters;
        economy["enemy_threat"] = threat;
    });
    src.maps = kMaps;
    const auto c = test::Load(src);
    REQUIRE(c->economy().run_rosters.always_from_level.at("uEm14") == 2);
    int arenas = 0;
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        Rng route = Rng::Derive(seed, streams::kRoute);
        RunMap map = GenerateRunMap(*c, route, Rotation(*c), -1);
        Rng roster = Rng::Derive(seed, streams::kRoster);
        PlanRunRosters(*c, roster, map);
        for (std::size_t l = 0; l < map.layers.size(); ++l) {
            for (const RunMapNode& n : map.layers[l]) {
                const MapDef* m = c->FindMapByStage(n.map_key);
                if (m == nullptr || m->boss) continue;
                const bool has = std::find(n.guests.begin(), n.guests.end(), "uEm14") != n.guests.end();
                INFO("seed " << seed << " arena " << l + 1 << " map " << m->id);
                if (l == 0) CHECK_FALSE(has);  // the first arena keeps its own
                else if (!Has(*m, "uEm14")) {
                    CHECK(has);
                    ++arenas;
                }
            }
        }
    }
    CHECK(arenas > 50);

    // Validated: it must be a guest, at a level.
    json notGuest = Rosters();
    notGuest["always_from_level"] = {{"uEm30", 3}};
    const auto errors = Content::LoadFromStrings(Sources(notGuest)).errors;
    CHECK(std::any_of(errors.begin(), errors.end(), [](const std::string& e) { return e.find("must be one of the guests") != std::string::npos; }));
    json badLevel = Rosters();
    badLevel["always_from_level"] = {{"uEm14", 0}};
    const auto errors2 = Content::LoadFromStrings(Sources(badLevel)).errors;
    CHECK(std::any_of(errors2.begin(), errors2.end(), [](const std::string& e) { return e.find("must be a level") != std::string::npos; }));
}

TEST_CASE("the shipped dogs: a guest from the Bridge, from level 2, in every arena from level 3, likelier as it grows", "[rosters]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const EconomyConfig& eco = r.content->economy();
    CHECK(eco.enemy_threat.table.Class("uEm30") == 2);
    CHECK(eco.enemy_threat.table.ClassWeight("uEm30", 4) > eco.enemy_threat.table.ClassWeight("uEm30", 2));
    CHECK(eco.run_rosters.guests.at("uEm30") == 104);
    CHECK(eco.run_rosters.always_from_level.at("uEm30") == 3);
    const std::optional<MapEnemy> dog = GuestEntry(*r.content, "uEm30");
    REQUIRE(dog);
    CHECK_FALSE(dog->special);
    for (const int stage : {104, 500}) {
        const MapDef* m = r.content->FindMapByStage(stage);
        REQUIRE(m != nullptr);
        CHECK(Has(*m, "uEm30"));
    }
}

