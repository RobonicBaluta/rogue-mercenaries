#include <algorithm>
#include <set>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/routes.h"
#include "test_support.h"

using namespace rm;

namespace {

// Four arenas with names: the snapshot titles route offers with them.
constexpr const char* kArenas = R"({"schema_version": 1, "maps": [
  {"id": "a0", "name": "Arena Zero", "stage": 0, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a1", "name": "Arena One", "stage": 1, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a2", "name": "Arena Two", "stage": 2, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]},
  {"id": "a3", "name": "Arena Three", "stage": 103, "points": [[0, 0, 0]], "roster": [{"class": "uEm10"}]}]})";

int Tier(const Content& c, const RunMapNode& n) { return c.FindRoute(n.route_id)->difficulty_tier; }

}  // namespace

TEST_CASE("the run map has one layer per arena, a single start and two links per later node", "[routes]") {
    const auto content = test::LoadFixture();
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        const RunMap map = GenerateRunMap(*content, rng, {0, 1, 2, 3, 103, 104}, 2);
        REQUIRE(map.layers.size() == static_cast<std::size_t>(content->economy().run.arena_count));
        REQUIRE(map.layers[0].size() == 1);
        CHECK(map.layers[0][0].route_id == content->economy().run.starting_route);
        CHECK(map.layers[0][0].map_key == 2);
        CHECK(map.layers[0][0].next == std::vector<int>{0, 1, 2});  // the start reaches all of layer 1
        for (std::size_t l = 1; l < map.layers.size(); ++l) {
            REQUIRE(map.layers[l].size() == 3);
            std::vector<int> parents(3, 0);
            for (const RunMapNode& p : map.layers[l - 1]) {
                for (const int n : p.next) ++parents[static_cast<std::size_t>(n)];
            }
            for (const int count : parents) CHECK(count > 0);  // every node can be reached
            for (const RunMapNode& n : map.layers[l]) {
                if (l + 1 == map.layers.size()) {
                    CHECK(n.next.empty());
                } else {
                    REQUIRE(n.next.size() == 2);
                    CHECK(n.next[1] == n.next[0] + 1);  // neighbours
                }
            }
        }
    }
}

TEST_CASE("run map tiers rise with depth and every choice has a safer and a harder option", "[routes]") {
    const auto content = test::LoadFixture();  // tiers 1, 2, 3; three arenas
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        const RunMap map = GenerateRunMap(*content, rng, {}, -1);
        for (const RunMapNode& n : map.layers[1]) CHECK((Tier(*content, n) == 1 || Tier(*content, n) == 2));
        for (const RunMapNode& n : map.layers[2]) CHECK((Tier(*content, n) == 2 || Tier(*content, n) == 3));
        for (std::size_t l = 0; l + 1 < map.layers.size(); ++l) {
            for (const RunMapNode& from : map.layers[l]) {
                std::set<int> tiers;
                for (const int n : from.next) tiers.insert(Tier(*content, map.layers[l + 1][static_cast<std::size_t>(n)]));
                CHECK(tiers.size() == 2);
            }
        }
        // Without a pool the adapter picks the arenas.
        for (const auto& layer : map.layers) {
            for (const RunMapNode& n : layer) CHECK(n.map_key == -1);
        }
    }
}

TEST_CASE("run map arenas differ within a layer and from the arena before", "[routes]") {
    const auto content = test::LoadFixture();
    const std::vector<int> pool{0, 1, 2, 3, 4, 5, 103, 104};
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        const RunMap map = GenerateRunMap(*content, rng, pool, -1);
        CHECK(std::find(pool.begin(), pool.end(), map.layers[0][0].map_key) != pool.end());
        for (std::size_t l = 1; l < map.layers.size(); ++l) {
            std::set<int> keys;
            for (const RunMapNode& n : map.layers[l]) {
                CHECK(std::find(pool.begin(), pool.end(), n.map_key) != pool.end());
                CHECK(keys.insert(n.map_key).second);
            }
            for (const RunMapNode& p : map.layers[l - 1]) {
                for (const int n : p.next) CHECK(map.layers[l][static_cast<std::size_t>(n)].map_key != p.map_key);
            }
        }
    }
    // A pool smaller than a layer repeats arenas rather than leaving nodes without one.
    Rng rng = Rng::Derive(7, streams::kRoute);
    const RunMap small = GenerateRunMap(*content, rng, {5}, -1);
    for (const auto& layer : small.layers) {
        for (const RunMapNode& n : layer) CHECK(n.map_key == 5);
    }
}

TEST_CASE("the run map is the same for the same seed", "[routes]") {
    const auto content = test::LoadFixture();
    auto make = [&](std::uint64_t seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        return GenerateRunMap(*content, rng, {0, 1, 2, 3, 103}, -1);
    };
    auto flat = [](const RunMap& m) {
        std::string s;
        for (const auto& layer : m.layers) {
            for (const RunMapNode& n : layer) s += std::to_string(n.map_key) + n.route_id + ";";
        }
        return s;
    };
    CHECK(flat(make(11)) == flat(make(11)));
    bool differs = false;
    for (std::uint64_t seed = 12; seed < 40 && !differs; ++seed) differs = flat(make(seed)) != flat(make(11));
    CHECK(differs);
}

TEST_CASE("route choices are the run map's next nodes and the run follows the chosen one", "[routes]") {
    ContentSources src = test::FixtureSources();
    src.maps = kArenas;
    test::Harness h(test::Load(src));
    h.rc.SetArenaPool({0, 1, 2, 103}, 1);
    REQUIRE(h.rc.StartRun("alpha", 99).ok());
    CHECK(h.rc.PlannedMapKey() == 1);
    CHECK(h.rc.Snapshot(0.0).arena_name == "Arena One");
    double t = 0.0;
    for (int arena = 1; arena <= 3; ++arena) {
        test::ClearArena(h.rc, t);
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(0).ok());
        if (arena == 3) break;
        REQUIRE(h.rc.LeaveShop().ok());
        const RunState& st = h.rc.state();
        const RunMapNode& here = st.run_map.layers[static_cast<std::size_t>(st.map_layer)][static_cast<std::size_t>(st.map_node)];
        const auto& choices = h.rc.RouteChoices();
        REQUIRE(choices.size() == here.next.size());
        const RunSnapshot snap = h.rc.Snapshot(t);
        REQUIRE(snap.route_offers.size() == choices.size());
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const RunMapNode& node = st.run_map.layers[static_cast<std::size_t>(st.map_layer) + 1][static_cast<std::size_t>(choices[i].node)];
            CHECK(choices[i].node == here.next[i]);
            CHECK(choices[i].map_key == node.map_key);
            CHECK(choices[i].route_id == node.route_id);
            // Titled with the arena and its tier; the detail names the route and what follows.
            const RouteDef& route = *h.rc.content().FindRoute(node.route_id);
            CHECK(snap.route_offers[i].title == h.rc.content().FindMapByStage(node.map_key)->name);
            CHECK(snap.route_offers[i].tier == route.difficulty_tier);
            CHECK(snap.route_offers[i].detail.find(route.name + ", " + std::to_string(route.enemy_budget) + " enemies") == 0);
            CHECK(snap.route_offers[i].detail.find(arena == 1 ? "\nThen " : "\nThe last arena of the run.") != std::string::npos);
            if (arena == 1) {
                // The two arenas after this choice, by name.
                const auto& after = st.run_map.layers[2];
                const std::string a = h.rc.content().FindMapByStage(after[static_cast<std::size_t>(node.next[0])].map_key)->name;
                const std::string b = h.rc.content().FindMapByStage(after[static_cast<std::size_t>(node.next[1])].map_key)->name;
                CHECK(snap.route_offers[i].detail.find("\nThen " + a + " or " + b + ".") != std::string::npos);
            }
            // Each line fits the game's help box (two lines of 56 characters).
            const std::string& d = snap.route_offers[i].detail;
            CHECK(d.find('\n') <= 56);
            CHECK(d.size() - d.find('\n') - 1 <= 56);
        }
        const std::size_t pick = choices.size() - 1;
        const int key = choices[pick].map_key;
        const std::string route = choices[pick].route_id;
        REQUIRE(h.rc.ChooseRoute(pick).ok());
        CHECK(h.rc.PlannedMapKey() == key);
        CHECK(h.rc.state().route_id == route);
        CHECK(h.rc.state().map_layer == arena);
        t += 100.0;
    }
    CHECK(h.rc.phase() == Phase::RunComplete);
    // The snapshot's run map marks where the run is.
    const RunSnapshot end = h.rc.Snapshot(t);
    REQUIRE(end.run_map.size() == 3);
    int current = 0;
    for (const auto& layer : end.run_map) {
        for (const RunMapNodeView& v : layer) current += v.current ? 1 : 0;
    }
    CHECK(current == 1);
    CHECK(end.run_map[2][static_cast<std::size_t>(h.rc.state().map_node)].current);
}

TEST_CASE("route choices roll their routes' curses, never the same one twice", "[routes]") {
    const auto content = test::LoadFixture();  // r2 has curse_chance 1.0, r1a none
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        Rng rng = Rng::Derive(seed, streams::kRoute);
        std::vector<RouteChoice> choices(3);
        choices[0].route_id = "r1a";
        choices[1].route_id = "r2";
        choices[2].route_id = "r2";
        RollRouteCurses(*content, rng, choices, {});
        CHECK(choices[0].curse_perk_id.empty());
        REQUIRE_FALSE(choices[1].curse_perk_id.empty());
        CHECK(content->FindPerk(choices[1].curse_perk_id)->IsCurse());
        CHECK(choices[2].curse_perk_id != choices[1].curse_perk_id);
    }
}

TEST_CASE("with the shipped content every route offer fits the game's help box", "[routes]") {
    ContentLoadResult loaded = Content::LoadFromDirectory(test::ConfigDir());
    for (const std::string& e : loaded.errors) UNSCOPED_INFO(e);
    REQUIRE(loaded.ok());
    std::vector<int> pool;
    for (const MapDef& m : loaded.content->maps()) {
        if (m.rotation) pool.push_back(m.stage);
    }
    test::Harness h(loaded.content);
    int offers = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        h.rc.SetArenaPool(pool, pool[seed % pool.size()]);
        REQUIRE(h.rc.StartRun("chris", seed).ok());
        double t = 0.0;
        for (int arena = 1; arena <= h.rc.state().arena_count; ++arena) {
            test::ClearArena(h.rc, t);
            REQUIRE(h.rc.ContinueToRewards().ok());
            REQUIRE(h.rc.ChooseReward(0).ok());
            if (h.rc.phase() == Phase::RunComplete) break;
            REQUIRE(h.rc.LeaveShop().ok());
            for (const OfferView& o : h.rc.Snapshot(t).route_offers) {
                INFO(o.title << " | " << o.detail);
                const std::size_t nl = o.detail.find('\n');
                REQUIRE(nl != std::string::npos);
                CHECK(nl <= 56);
                CHECK(o.detail.size() - nl - 1 <= 56);
                CHECK_FALSE(o.title.empty());
                // The game's menu font has no ':' or apostrophe.
                CHECK((o.title + o.detail).find_first_of(":'") == std::string::npos);
                ++offers;
            }
            REQUIRE(h.rc.ChooseRoute(seed % h.rc.RouteChoices().size()).ok());
            t += 100.0;
        }
        REQUIRE(h.rc.EndRun().ok());
    }
    CHECK(offers > 200);
}
