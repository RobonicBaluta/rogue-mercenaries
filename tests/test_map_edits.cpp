#include <array>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/director.h"
#include "rm/map_edits.h"
#include "rm/run_controller.h"
#include "test_support.h"

using namespace rm;

namespace {

MapDef EditMap() {
    MapDef map;
    map.id = "edit_map";
    map.name = "Edit Map";
    map.stage = 505;
    map.max_alive = 2;
    map.min_player_distance = 0.0;
    map.points = {{0, 0, 0}, {1000, 0, 0}, {2000, 0, 0}};
    map.start = std::array<double, 3>{5, 5, 5};
    map.roster = {MapEnemy{"uEm10", {}, {}, 1, 1.0}};
    return map;
}

}  // namespace

TEST_CASE("a map edit takes out points near a removed one and adds its own", "[director]") {
    MapEdit edit;
    edit.removed = {{1010, 0, -20}};  // within half a metre of (1000, 0, 0)
    edit.added = {{3000, 0, 0}};
    edit.start = std::array<double, 3>{7, 8, 9};
    const MapDef out = ApplyMapEdit(EditMap(), edit);
    REQUIRE(out.points.size() == 3);
    CHECK(out.points[0] == std::array<double, 3>{0, 0, 0});
    CHECK(out.points[1] == std::array<double, 3>{2000, 0, 0});
    CHECK(out.points[2] == std::array<double, 3>{3000, 0, 0});
    REQUIRE(out.start);
    CHECK((*out.start)[2] == 9);
    CHECK(RemovesPoint(edit, {1000, 0, 0}));
    CHECK_FALSE(RemovesPoint(edit, {1100, 0, 0}));  // a metre off
}

TEST_CASE("a map edit never leaves a map without a point", "[director]") {
    MapEdit edit;
    edit.removed = {{0, 0, 0}, {1000, 0, 0}, {2000, 0, 0}};
    const MapDef out = ApplyMapEdit(EditMap(), edit);
    CHECK(out.points.size() == 3);  // its own kept
}

TEST_CASE("map edits survive a write and a read", "[director]") {
    MapEdits edits;
    edits[505].added = {{-1782.34, 301.0, 1999.0}};
    edits[505].removed = {{-3475.0, 2.0, 2513.0}, {-911.0, 138.0, 3521.0}};
    edits[3].start = std::array<double, 3>{1, 2, 3};
    edits[7];  // empty: left out
    const std::string text = SerializeMapEdits(edits);
    std::string error;
    const auto back = ParseMapEdits(text, &error);
    REQUIRE(back);
    CHECK(error.empty());
    REQUIRE(back->size() == 2);
    CHECK(back->at(505).added.size() == 1);
    CHECK(back->at(505).added[0][0] == Catch::Approx(-1782.3));
    CHECK(back->at(505).removed.size() == 2);
    REQUIRE(back->at(3).start);
    CHECK((*back->at(3).start)[1] == 2);
}

TEST_CASE("broken map edits are refused with a reason", "[director]") {
    std::string error;
    CHECK_FALSE(ParseMapEdits("{", &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ParseMapEdits(R"({"maps": {"x": {}}})", &error));
    CHECK(error.find("not a number") != std::string::npos);
    CHECK_FALSE(ParseMapEdits(R"({"maps": {"505": {"added": [[1, 2]]}}})", &error));
    const auto empty = ParseMapEdits(R"({"schema_version": 1})");
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("the director spawns at points the editor gives it during the arena", "[director]") {
    Director d(EditMap(), 10, 1, 7);
    const auto first = d.Tick(0.0, std::nullopt);
    REQUIRE_FALSE(first.empty());
    for (const auto& r : first) d.Spawned(r.ticket, "u" + std::to_string(r.ticket));
    d.ReplacePoints({{9000, 0, 0}, {9500, 0, 0}});
    // A unit from before the change is relocated safely (its old point is forgotten).
    const auto moved = d.Relocate("u" + std::to_string(first[0].ticket), std::nullopt);
    REQUIRE(moved);
    CHECK((*moved)[0] >= 9000);
    // Later spawns use only the new points.
    for (const auto& r : first) d.Died("u" + std::to_string(r.ticket));
    const auto later = d.Tick(100.0, std::nullopt);
    REQUIRE_FALSE(later.empty());
    for (const auto& r : later) CHECK(r.pos[0] >= 9000);
    d.ReplacePoints({});  // empty: unchanged
    const auto again = d.Tick(200.0, std::nullopt);
    for (const auto& r : again) CHECK(r.pos[0] >= 9000);
}

TEST_CASE("a map edit takes objects out and puts the map's own back", "[director]") {
    MapDef map = EditMap();
    map.hide_models = {"om0955_IDo00", "om0959_IDo00"};
    MapEdit edit;
    edit.hidden = {"OM0960_IDo00_U013", "om0955_ido00"};  // the second is the map's already
    edit.shown = {"OM0959_IDO00"};
    const MapDef out = ApplyMapEdit(map, edit);
    REQUIRE(out.hide_models.size() == 2);
    CHECK(out.hide_models[0] == "om0955_IDo00");
    CHECK(out.hide_models[1] == "OM0960_IDo00_U013");
    MapEdits edits;
    edits[505] = edit;
    const auto back = ParseMapEdits(SerializeMapEdits(edits));
    REQUIRE(back);
    CHECK(back->at(505).hidden.size() == 2);
    CHECK(back->at(505).shown.size() == 1);
    CHECK_FALSE(ParseMapEdits(R"({"maps": {"505": {"hidden": [3]}}})"));
}

TEST_CASE("the map editor's jump plays the same arena again", "[run]") {
    test::Harness h;
    RunController& rc = h.rc;
    REQUIRE(rc.StartRun("alpha", 42).ok());
    CHECK_FALSE(rc.EditorRestartArena().ok());  // no arena yet
    REQUIRE(rc.BeginArena(0.0).ok());
    REQUIRE(rc.state().arena_index == 1);
    REQUIRE(rc.EditorRestartArena().ok());
    CHECK(rc.phase() == Phase::RouteSelection);
    CHECK(rc.state().arena_index == 0);
    CHECK(rc.state().arenas.empty());
    CHECK(rc.director() == nullptr);
    REQUIRE(rc.BeginArena(5.0).ok());  // the same arena, on whatever map the adapter loads
    CHECK(rc.state().arena_index == 1);
    CHECK(rc.phase() == Phase::ArenaActive);
    // Once cleared there is nothing to take back.
    test::KillN(rc, rc.RemainingEnemies(), 6.0);
    REQUIRE(rc.IsArenaComplete());
    REQUIRE(rc.CompleteArena(30.0).ok());
    CHECK_FALSE(rc.EditorRestartArena().ok());
}

TEST_CASE("a map edit can take out every one of the map's points", "[director]") {
    MapEdit edit;
    edit.clear = true;
    CHECK(ApplyMapEdit(EditMap(), edit).points.size() == 3);  // none of its own left and none added: its own
    edit.added = {{7000, 0, 0}};
    const MapDef out = ApplyMapEdit(EditMap(), edit);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0][0] == 7000);
    MapEdits edits;
    edits[3] = edit;
    const auto back = ParseMapEdits(SerializeMapEdits(edits));
    REQUIRE(back);
    CHECK(back->at(3).clear);
    MapEdits only_clear;
    only_clear[4].clear = true;
    CHECK(ParseMapEdits(SerializeMapEdits(only_clear))->size() == 1);  // not an empty edit
}
