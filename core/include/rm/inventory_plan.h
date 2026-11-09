#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace rm {

/// The game's item grid has nine cells.
inline constexpr int kGridCellCount = 9;

/// One inventory record as the game holds it now.
struct GridRecord {
    int handle = 0;  ///< the caller's name for it
    std::uint32_t item = 0;
    int cell = -1;         ///< 0..8 in the grid, anything else outside it (the knife, bare hands)
    int count = 0;         ///< a stack's size, a gun's rounds
    bool in_hand = false;  ///< the game hangs if the record goes while it is held
};

/// What one grid cell should hold (item 0: nothing).
struct WantedCell {
    std::uint32_t item = 0;
    int count = 0;
    bool weapon = false;  ///< a gun: a run holds one of each
};

using GridLayout = std::array<WantedCell, kGridCellCount>;

/// How to turn the records into a layout, cell by cell. Reconciling by item totals could not tell
/// which record a change was about: a sold or stored gun that was in the hands stayed in its cell
/// beside whatever took that cell, and a stack split in two stayed one (2026-11-09).
struct GridPlan {
    std::vector<int> clear;                          ///< records that go
    std::vector<int> out_of_hand;                    ///< records that go, once they are out of the hands
    std::vector<std::pair<int, int>> move;           ///< record -> cell
    std::vector<std::pair<int, int>> set_count;      ///< record -> count
    std::vector<std::pair<int, WantedCell>> create;  ///< cell -> what to make there
    std::vector<WantedCell> surplus;                 ///< a second of the same gun: for the storage
};

/// Records outside the grid are left alone unless the layout wants their item. What is in the hands
/// keeps its place in the plan first, so the gun being held is never the copy that goes.
GridPlan PlanGrid(const std::vector<GridRecord>& records, const GridLayout& layout);

}  // namespace rm
