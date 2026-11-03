#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/capabilities.h"
#include "rm/rarity.h"
#include "rm/rng.h"

namespace rm {

class Content;

enum class WeaponCategory { Handgun, Shotgun, Smg, Rifle, MachineGun, Magnum, Bow };
std::string_view WeaponCategoryName(WeaponCategory c);
std::optional<WeaponCategory> ParseWeaponCategory(std::string_view name);

/// An existing game weapon; the physical weapon stays the game's own.
struct WeaponBaseDef {
    std::string id;  // stable id the adapter reports in HitContext/KillEvent ("m92f", ...)
    std::string name;
    WeaponCategory category = WeaponCategory::Handgun;
    double base_damage_mult = 1.0;
    std::int64_t unlock_cost_credits = 0;
    bool enabled = true;
    /// RE5 inventory item id of this weapon (-1 = unknown; the adapter then only applies the damage class).
    std::int64_t game_item = -1;
    /// Offered only to a skin build that lists it in extra_weapons (the Gatling, the longbow).
    bool build_only = false;
};

enum class AffixKind {
    DamagePct,               // +value % damage
    CritChance,              // +value crit chance (fraction)
    CritMultPct,             // +value % crit multiplier
    LowHpTargetDamagePct,    // +value % damage vs targets below `threshold` hp fraction
    SuppliesOnKill,          // +value supplies per kill with this weapon
    UniqueLowHpTargetBonus,  // legendary: +value (fraction) damage vs targets below `threshold`
    UniqueKillStreakExtend,  // legendary: kills extend kill-streak windows by value seconds
};
std::string_view AffixKindName(AffixKind k);
std::optional<AffixKind> ParseAffixKind(std::string_view name);         // regular affixes
std::optional<AffixKind> ParseUniqueEffect(std::string_view name);      // legendary effects
bool IsUniqueAffix(AffixKind k);

/// Roll range for a regular affix (weapons.json "affixes"); scaled by rarity magnitude_mult.
struct AffixDef {
    AffixKind kind = AffixKind::DamagePct;
    double min = 0.0;
    double max = 0.0;
    double threshold = 0.0;  // LowHpTargetDamagePct only
    std::vector<std::string> requires_capabilities;  // declared plus implied
};

/// Fixed legendary effect (weapons.json "unique_affixes").
struct UniqueAffixDef {
    std::string id;
    std::string name;
    std::string description;
    AffixKind kind = AffixKind::UniqueLowHpTargetBonus;
    double value = 0.0;
    double threshold = 0.0;
    std::vector<std::string> requires_capabilities;
};

/// Rolled affix on an instance. Units: *_pct are percent, crit chance is a fraction,
/// supplies are whole units, unique low-HP bonus is a fraction (1.0 = +100 %),
/// kill-streak extension is seconds.
struct Affix {
    AffixKind kind = AffixKind::DamagePct;
    double value = 0.0;
    double threshold = 0.0;
    std::string unique_id;  // set for legendary effects
};

/// A run-scoped upgradeable copy of a base weapon.
///
/// Application rule (enforced by RunController):
///  * capability "weapon_id" present: an instance's modifiers apply only to hits/kills whose
///    weapon_id equals its base_id; the run holds at most one instance per base id (a new
///    instance of the same base replaces the old one).
///  * capability "weapon_id" missing: the run has a single "sidearm" slot holding one
///    instance whose modifiers apply to every player hit/kill; a new weapon replaces it.
///  * hits/kills flagged is_melee/was_melee == true never receive weapon modifiers.
struct WeaponInstance {
    std::string instance_id;
    std::string base_id;
    int level = 1;  // 1..max_level (economy.json upgrades.max_level)
    Rarity rarity = Rarity::Common;
    std::vector<Affix> affixes;
};

inline constexpr std::string_view kSidearmSlot = "sidearm";

/// Level-1 common instance with no affixes (starting gear).
WeaponInstance MakeStartingWeapon(std::string_view base_id, std::string instance_id);

/// Rolls a new instance: affix_count distinct regular affixes plus unique_count legendary
/// effects for the rarity (economy.json weapon_rarity), skipping affixes whose capabilities
/// are missing. Values are rounded to 2 decimals so they print and persist stably.
WeaponInstance GenerateWeapon(Rng& rng, const Content& content, const Capabilities& caps,
                              std::string_view base_id, Rarity rarity, std::string instance_id);

/// Human-readable affix line ("+12% damage", "Executioner Rounds: ...").
std::string DescribeAffix(const Affix& affix, const Content* content = nullptr);

}  // namespace rm
