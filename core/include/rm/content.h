#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rm/characters.h"
#include "rm/common.h"
#include "rm/economy.h"
#include "rm/perks.h"
#include "rm/routes.h"
#include "rm/weapons.h"

namespace rm {

class Content;

/// Raw JSON text of the five content files (used by tests and LoadFromDirectory).
struct ContentSources {
    std::string perks;       // perks.json
    std::string weapons;     // weapons.json
    std::string characters;  // characters.json
    std::string routes;      // routes.json
    std::string economy;     // economy.json
    std::string loadouts;    // loadouts.json (optional; defaults apply when empty)
};

/// Starting loadout roll (configs/loadouts.json). A run spends `budget` points: the weapon
/// first (by category), then random equipment while points remain; the rest becomes supplies.
/// Without a loadouts file the budget is 0: one starting weapon is still rolled, nothing else.
struct EquipmentDef {
    std::string id;
    std::string name;
    std::int64_t cost = 0;
    int max_count = 1;
    /// Adapter item id to put into the game's inventory (-1 = tracked by the run only).
    std::int64_t game_item = -1;
};
struct LoadoutConfig {
    std::int64_t budget = 0;
    std::int64_t default_weapon_cost = 0;
    StringMap<std::int64_t> weapon_cost;  // by weapon category name
    std::vector<EquipmentDef> equipment;
};

/// `content` is set only when there are no errors. Errors are human-readable, one problem
/// per line, prefixed with the file and the offending entry.
struct ContentLoadResult {
    std::shared_ptr<const Content> content;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    [[nodiscard]] bool ok() const { return content != nullptr && errors.empty(); }
};

/// Immutable, validated game content. Validation rejects duplicate ids, unknown references,
/// out-of-range numbers, unknown keys, unsupported effect/condition/affix kinds, unknown
/// capability names and exclusion setups that make a perk impossible to pick.
/// exclusive_with is normalised to be symmetric; requires_capabilities gains implied entries.
class Content {
public:
    static ContentLoadResult LoadFromDirectory(const std::filesystem::path& directory);
    static ContentLoadResult LoadFromStrings(const ContentSources& sources);

    [[nodiscard]] const std::vector<PerkDef>& perks() const { return perks_; }
    [[nodiscard]] const std::vector<WeaponBaseDef>& weapons() const { return weapons_; }
    [[nodiscard]] const std::vector<AffixDef>& affixes() const { return affixes_; }
    [[nodiscard]] const std::vector<UniqueAffixDef>& unique_affixes() const { return uniques_; }
    [[nodiscard]] const std::vector<CharacterDef>& characters() const { return characters_; }
    [[nodiscard]] const std::vector<RouteDef>& routes() const { return routes_; }
    [[nodiscard]] const EconomyConfig& economy() const { return economy_; }
    [[nodiscard]] const LoadoutConfig& loadouts() const { return loadouts_; }

    [[nodiscard]] const PerkDef* FindPerk(std::string_view id) const;
    [[nodiscard]] const WeaponBaseDef* FindWeapon(std::string_view id) const;
    [[nodiscard]] const CharacterDef* FindCharacter(std::string_view id) const;
    [[nodiscard]] const RouteDef* FindRoute(std::string_view id) const;
    [[nodiscard]] const UniqueAffixDef* FindUnique(std::string_view id) const;

    /// Highest difficulty tier among enabled routes.
    [[nodiscard]] int MaxRouteTier() const;

private:
    friend class ContentBuilder;

    std::vector<PerkDef> perks_;
    std::vector<WeaponBaseDef> weapons_;
    std::vector<AffixDef> affixes_;
    std::vector<UniqueAffixDef> uniques_;
    std::vector<CharacterDef> characters_;
    std::vector<RouteDef> routes_;
    EconomyConfig economy_;
    LoadoutConfig loadouts_;

    StringMap<std::size_t> perk_index_;
    StringMap<std::size_t> weapon_index_;
    StringMap<std::size_t> character_index_;
    StringMap<std::size_t> route_index_;
    StringMap<std::size_t> unique_index_;
};

}  // namespace rm
