#include "rm/inventory_plan.h"

#include <algorithm>
#include <cstddef>

namespace rm {

namespace {

bool InGrid(const GridRecord& r) { return r.cell >= 0 && r.cell < kGridCellCount; }

}  // namespace

GridPlan PlanGrid(const std::vector<GridRecord>& records, const GridLayout& wanted) {
    GridPlan plan;
    GridLayout layout = wanted;
    // One of each gun: a second cell of it goes to the storage.
    for (std::size_t c = 0; c < layout.size(); ++c) {
        if (layout[c].item == 0 || !layout[c].weapon) continue;
        for (std::size_t earlier = 0; earlier < c; ++earlier) {
            if (layout[earlier].item != layout[c].item) continue;
            plan.surplus.push_back(layout[c]);
            layout[c] = {};
            break;
        }
    }

    std::array<const GridRecord*, kGridCellCount> assigned{};
    std::vector<const GridRecord*> taken;
    const auto isTaken = [&taken](const GridRecord& r) { return std::find(taken.begin(), taken.end(), &r) != taken.end(); };
    const auto assign = [&](std::size_t cell, const GridRecord& r) {
        assigned[cell] = &r;
        taken.push_back(&r);
    };
    const auto wants = [&](std::size_t cell, const GridRecord& r) {
        return r.item != 0 && layout[cell].item == r.item && assigned[cell] == nullptr;
    };

    // What is in the hands first: its own cell when that still wants it, or the first that does.
    for (const GridRecord& r : records) {
        if (!r.in_hand) continue;
        if (InGrid(r) && wants(static_cast<std::size_t>(r.cell), r)) {
            assign(static_cast<std::size_t>(r.cell), r);
            continue;
        }
        for (std::size_t c = 0; c < layout.size(); ++c) {
            if (!wants(c, r)) continue;
            assign(c, r);
            break;
        }
    }
    // Then every record already where it should be.
    for (const GridRecord& r : records) {
        if (!isTaken(r) && InGrid(r) && wants(static_cast<std::size_t>(r.cell), r)) assign(static_cast<std::size_t>(r.cell), r);
    }
    // The rest of each item fill its remaining cells: those in the grid before those outside it.
    for (const bool fromGrid : {true, false}) {
        for (const GridRecord& r : records) {
            if (isTaken(r) || InGrid(r) != fromGrid) continue;
            for (std::size_t c = 0; c < layout.size(); ++c) {
                if (!wants(c, r)) continue;
                assign(c, r);
                break;
            }
        }
    }

    for (std::size_t c = 0; c < layout.size(); ++c) {
        if (layout[c].item == 0) continue;
        const GridRecord* r = assigned[c];
        if (r == nullptr) {
            plan.create.emplace_back(static_cast<int>(c), layout[c]);
            continue;
        }
        if (r->cell != static_cast<int>(c)) plan.move.emplace_back(r->handle, static_cast<int>(c));
        if (r->count != layout[c].count) plan.set_count.emplace_back(r->handle, layout[c].count);
    }
    for (const GridRecord& r : records) {
        if (isTaken(r) || !InGrid(r)) continue;
        (r.in_hand ? plan.out_of_hand : plan.clear).push_back(r.handle);
    }
    return plan;
}

}  // namespace rm
