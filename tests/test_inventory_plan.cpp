#include <algorithm>
#include <map>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rm/inventory_plan.h"

using namespace rm;

namespace {

constexpr std::uint32_t kPistol = 0x0102, kMagnum = 0x010B, kAmmo = 0x0201, kHerb = 0x0301, kGrenade = 0x0106, kKnife = 0x0101;

GridRecord Rec(int handle, std::uint32_t item, int cell, int count, bool held = false) { return GridRecord{handle, item, cell, count, held}; }
WantedCell Stack(std::uint32_t item, int count) { return WantedCell{item, count, false}; }
WantedCell Gun(std::uint32_t item, int rounds) { return WantedCell{item, rounds, true}; }

bool Has(const std::vector<int>& handles, int h) { return std::find(handles.begin(), handles.end(), h) != handles.end(); }

// What the grid holds once a plan is carried out: cell -> (item, count). Asks that no two records end in one cell.
std::map<int, std::pair<std::uint32_t, int>> Apply(std::vector<GridRecord> records, const GridPlan& plan) {
    std::erase_if(records, [&](const GridRecord& r) { return Has(plan.clear, r.handle) || Has(plan.out_of_hand, r.handle); });
    for (const auto& [handle, cell] : plan.move) {
        for (GridRecord& r : records) {
            if (r.handle == handle) r.cell = cell;
        }
    }
    for (const auto& [handle, count] : plan.set_count) {
        for (GridRecord& r : records) {
            if (r.handle == handle) r.count = count;
        }
    }
    std::map<int, std::pair<std::uint32_t, int>> grid;
    for (const GridRecord& r : records) {
        if (r.cell < 0 || r.cell >= kGridCellCount) continue;
        CHECK(grid.count(r.cell) == 0);
        grid[r.cell] = {r.item, r.count};
    }
    for (const auto& [cell, what] : plan.create) {
        CHECK(grid.count(cell) == 0);
        grid[cell] = {what.item, what.count};
    }
    return grid;
}

}  // namespace

TEST_CASE("a grid that already matches needs nothing", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kPistol, 0, 15, true), Rec(1, kAmmo, 1, 30), Rec(2, kKnife, 9, 1)};
    GridLayout layout{};
    layout[0] = Gun(kPistol, 15);
    layout[1] = Stack(kAmmo, 30);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.clear.empty());
    CHECK(plan.out_of_hand.empty());
    CHECK(plan.move.empty());
    CHECK(plan.set_count.empty());
    CHECK(plan.create.empty());
    CHECK(plan.surplus.empty());
}

TEST_CASE("a sold item goes and the knife outside the grid stays", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kPistol, 0, 15, true), Rec(1, kHerb, 1, 1), Rec(2, kKnife, 9, 1)};
    GridLayout layout{};
    layout[0] = Gun(kPistol, 15);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.clear == std::vector<int>{1});
    CHECK(plan.out_of_hand.empty());
}

TEST_CASE("the gun in the hands that was sold or stored must leave them first", "[inventory]") {
    // The user's report: the screen put a herb in its cell, the gun stayed there as well.
    const std::vector<GridRecord> records = {Rec(0, kMagnum, 0, 6, true), Rec(1, kPistol, 1, 15), Rec(2, kHerb, 2, 1)};
    GridLayout layout{};
    layout[0] = Stack(kHerb, 1);
    layout[1] = Gun(kPistol, 15);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.out_of_hand == std::vector<int>{0});
    CHECK(plan.clear.empty());
    const auto grid = Apply(records, plan);
    REQUIRE(grid.count(0) == 1);
    CHECK(grid.at(0).first == kHerb);
    CHECK(grid.at(1).first == kPistol);
    CHECK(grid.size() == 2);
}

TEST_CASE("the gun in the hands moved to another cell stays in the hands", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kMagnum, 0, 6, true), Rec(1, kAmmo, 4, 12)};
    GridLayout layout{};
    layout[4] = Gun(kMagnum, 6);
    layout[0] = Stack(kAmmo, 12);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.out_of_hand.empty());
    CHECK(plan.clear.empty());
    const auto grid = Apply(records, plan);
    CHECK(grid.at(4).first == kMagnum);
    CHECK(grid.at(0).first == kAmmo);
}

TEST_CASE("three items moved in a circle end where the layout has them", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kPistol, 0, 15), Rec(1, kAmmo, 1, 30), Rec(2, kHerb, 2, 1)};
    GridLayout layout{};
    layout[1] = Gun(kPistol, 15);
    layout[2] = Stack(kAmmo, 30);
    layout[0] = Stack(kHerb, 1);
    const auto grid = Apply(records, PlanGrid(records, layout));
    CHECK(grid.at(1).first == kPistol);
    CHECK(grid.at(2).first == kAmmo);
    CHECK(grid.at(0).first == kHerb);
}

TEST_CASE("a stack split in the screen becomes two records with their own counts", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kAmmo, 1, 50)};
    GridLayout layout{};
    layout[1] = Stack(kAmmo, 30);
    layout[5] = Stack(kAmmo, 20);
    const GridPlan plan = PlanGrid(records, layout);
    const auto grid = Apply(records, plan);
    CHECK(grid.at(1) == std::pair<std::uint32_t, int>{kAmmo, 30});
    CHECK(grid.at(5) == std::pair<std::uint32_t, int>{kAmmo, 20});
    CHECK(plan.create.size() == 1);
}

TEST_CASE("stacks merged in the screen leave one record with the sum", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kAmmo, 1, 30), Rec(1, kAmmo, 4, 10)};
    GridLayout layout{};
    layout[4] = Stack(kAmmo, 40);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.clear == std::vector<int>{0});
    const auto grid = Apply(records, plan);
    CHECK(grid.size() == 1);
    CHECK(grid.at(4) == std::pair<std::uint32_t, int>{kAmmo, 40});
}

TEST_CASE("part of a stack sold keeps the record of the cell it stayed in", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kAmmo, 0, 50), Rec(1, kAmmo, 4, 10)};
    GridLayout layout{};
    layout[4] = Stack(kAmmo, 10);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.clear == std::vector<int>{0});
    CHECK(plan.set_count.empty());
    CHECK(plan.move.empty());
}

TEST_CASE("an item taken out of the storage is made in its cell", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kPistol, 0, 15, true)};
    GridLayout layout{};
    layout[0] = Gun(kPistol, 15);
    layout[7] = Gun(kMagnum, 3);
    const GridPlan plan = PlanGrid(records, layout);
    REQUIRE(plan.create.size() == 1);
    CHECK(plan.create[0].first == 7);
    CHECK(plan.create[0].second.item == kMagnum);
    CHECK(plan.create[0].second.count == 3);
}

TEST_CASE("a second of the same gun waits in the storage", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kMagnum, 0, 6)};
    GridLayout layout{};
    layout[0] = Gun(kMagnum, 6);
    layout[3] = Gun(kMagnum, 2);
    const GridPlan plan = PlanGrid(records, layout);
    REQUIRE(plan.surplus.size() == 1);
    CHECK(plan.surplus[0].item == kMagnum);
    CHECK(plan.surplus[0].count == 2);
    CHECK(plan.create.empty());
}

TEST_CASE("a record outside the grid comes in when the layout wants it", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, 0x0121, 0xA, 100), Rec(1, kKnife, 9, 1)};
    GridLayout layout{};
    layout[2] = Gun(0x0121, 100);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.create.empty());
    REQUIRE(plan.move.size() == 1);
    CHECK(plan.move[0] == std::pair<int, int>{0, 2});
    CHECK(plan.clear.empty());
}

TEST_CASE("a stack in the hands is the copy that stays", "[inventory]") {
    const std::vector<GridRecord> records = {Rec(0, kGrenade, 3, 2), Rec(1, kGrenade, 6, 1, true)};
    GridLayout layout{};
    layout[3] = Stack(kGrenade, 3);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.out_of_hand.empty());
    CHECK(plan.clear == std::vector<int>{0});
    const auto grid = Apply(records, plan);
    CHECK(grid.at(3) == std::pair<std::uint32_t, int>{kGrenade, 3});
}

TEST_CASE("a full grid rearranged with a held gun sold ends exact", "[inventory]") {
    std::vector<GridRecord> records;
    for (int c = 0; c < kGridCellCount; ++c) records.push_back(Rec(c, 0x0300u + static_cast<std::uint32_t>(c), c, 1, c == 4));
    GridLayout layout{};
    for (int c = 0; c < kGridCellCount; ++c) {
        layout[static_cast<std::size_t>(c)] = c == 4 ? Stack(0x0399, 2) : Stack(0x0300u + static_cast<std::uint32_t>(8 - c), 1);
    }
    layout[4] = Stack(0x0399, 2);
    const GridPlan plan = PlanGrid(records, layout);
    CHECK(plan.out_of_hand == std::vector<int>{4});
    const auto grid = Apply(records, plan);
    CHECK(grid.size() == static_cast<std::size_t>(kGridCellCount));
    CHECK(grid.at(4).first == 0x0399u);
    CHECK(grid.at(0).first == 0x0308u);
}
