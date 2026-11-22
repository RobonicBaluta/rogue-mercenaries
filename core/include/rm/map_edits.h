#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/director.h"

namespace rm {

/// The map editor's changes to one map (2026-11-20, the user: "a tool so I can add spawners on
/// maps"; a spawner is one more spawn point the director's waves use). Spawn points added where
/// the player stood, the map's own points taken out (by position: every point within
/// kMapEditMatchRadius of a removed one), the arena's start, and objects not built (a door that
/// never opens; set record names, MapDef::hide_models) and ones built again (`shown`: the map's
/// own hide_models entries the editor put back).
struct MapEdit {
    std::vector<std::array<double, 3>> added;
    std::vector<std::array<double, 3>> removed;
    /// Every one of the map's own points out (the user: "a button to delete all spawnpoints at a
    /// time"): only the added ones are used.
    bool clear = false;
    std::optional<std::array<double, 3>> start;
    /// Player 2's start in a co-op run (the user, 2026-11-21: "set the player spawn point for both
    /// players").
    std::optional<std::array<double, 3>> start2;
    std::vector<std::string> hidden;
    std::vector<std::string> shown;
    [[nodiscard]] bool empty() const {
        return added.empty() && removed.empty() && !clear && !start && !start2 && hidden.empty() && shown.empty();
    }
};

/// By map key (a Mercenaries index 0..7 or a campaign stage number, MapDef::stage).
using MapEdits = std::map<int, MapEdit>;

inline constexpr double kMapEditMatchRadius = 50.0;  // engine units (half a metre)

/// Whether `edit` takes out a map point at `p`.
[[nodiscard]] bool RemovesPoint(const MapEdit& edit, const std::array<double, 3>& p);

/// The map with the edit: its removed points out (all of them with `clear`), the added ones in,
/// the starts replaced. A map the edit would leave without a point keeps its own (an arena needs
/// one).
[[nodiscard]] MapDef ApplyMapEdit(const MapDef& map, const MapEdit& edit);

/// map_edits.json: {"schema_version": 1, "maps": {"505": {"added": [[x, y, z], ...],
/// "removed": [[x, y, z], ...], "start": [x, y, z], "start2": [x, y, z], "hidden": ["om0955_ido00_u005"],
/// "shown": []}}}.
/// Empty edits are left out when written.
[[nodiscard]] std::optional<MapEdits> ParseMapEdits(std::string_view text, std::string* error = nullptr);
[[nodiscard]] std::string SerializeMapEdits(const MapEdits& edits);

}  // namespace rm
