#pragma once

#include <array>
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

/// Launcher: the grenade and rocket launchers; melee: the stun rod.
enum class WeaponCategory { Handgun, Shotgun, Smg, Rifle, MachineGun, Magnum, Bow, Launcher, Melee, Flamethrower };
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

/// The kinds are stored by number in a run's resume point: new kinds go at the end.
enum class AffixKind {
    DamagePct,               // +value % damage
    CritChance,              // +value crit chance (fraction)
    CritMultPct,             // +value % crit multiplier
    LowHpTargetDamagePct,    // +value % damage vs targets below `threshold` hp fraction
    SuppliesOnKill,          // +value supplies per kill with this weapon
    UniqueLowHpTargetBonus,  // legendary: +value (fraction) damage vs targets below `threshold`
    UniqueKillStreakExtend,  // legendary: kills extend kill-streak windows by value seconds
    // Legendary effects of the rarity remake (2026-11-08); their numbers are the content's
    // (UniqueAffixDef, looked up by the affix's unique_id).
    UniqueExecute,           // a non-boss below `threshold` health dies; a boss takes +value
    UniqueDragonsBreath,     // a hit sets every enemy within `radius` of the target burning
    UniqueChainLightning,    // every `every`-th hit arcs to `targets` enemies within `radius` (value: of the hit)
    UniqueBottomless,        // a kill refills the magazine
    UniqueOverkill,          // damage past a kill (times value) goes to the nearest enemy within `radius`
    UniqueLastWord,          // the magazine's last round kills a non-boss; a boss takes +value
    UniqueBloodthirst,       // a kill heals value of the player's maximum health
};
std::string_view AffixKindName(AffixKind k);
std::optional<AffixKind> ParseAffixKind(std::string_view name);         // regular affixes
std::optional<AffixKind> ParseUniqueEffect(std::string_view name);      // legendary effects
bool IsUniqueAffix(AffixKind k);

/// Elements (weapons.json "elements"): an Epic gun has one, a Legendary one, an Exotic two.
/// Their statuses and reactions are the adapter's to play (WeaponProc); a boss takes
/// ElementDef::boss_bonus more damage from the gun instead.
enum class Element { Incendiary, Shock, Cryo, Explosive, Toxic };
inline constexpr std::array<Element, 5> kAllElements = {Element::Incendiary, Element::Shock, Element::Cryo,
                                                        Element::Explosive, Element::Toxic};
std::string_view ElementName(Element e);
std::optional<Element> ParseElement(std::string_view name);

struct ElementDef {
    Element element = Element::Incendiary;
    std::string name;         // "Incendiary"
    std::string description;  // one line for the menus
    double chance = 1.0;      // per hit (Shock, Cryo); 1: every hit (Incendiary, Toxic)
    double seconds = 0.0;     // burn, poison or freeze time
    double dps_fraction = 0.0;     // burn or poison damage per second, as a fraction of the hit's damage
    double damage_fraction = 0.0;  // Shock: an arc's damage, of the hit's; Explosive, Cryo (a shatter): of the victim's max health
    double damage_cap = 0.0;       // Explosive, Cryo: the most a blast deals (0: no cap)
    double radius = 0.0;           // engine units (centimetres) around the target or the victim
    int targets = 0;               // Shock: enemies an arc reaches
    double taken_mult = 1.0;       // Cryo: damage a frozen target takes; Toxic: a poisoned one
    double boss_bonus = 0.0;       // against a boss: this much more damage, no status
};

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
    double radius = 0.0;  // engine units (Dragon's Breath, Chain Lightning, Overkill)
    int every = 0;        // Chain Lightning: every N-th hit
    int targets = 0;      // Chain Lightning: enemies an arc reaches
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
    std::vector<Element> elements;  // distinct
};

inline constexpr std::string_view kSidearmSlot = "sidearm";

/// Level-1 common instance with no affixes (starting gear).
WeaponInstance MakeStartingWeapon(std::string_view base_id, std::string instance_id);

/// Rolls a new instance: affix_count distinct regular affixes, element_count distinct elements
/// and unique_count legendary effects for the rarity (economy.json weapon_rarity), skipping
/// affixes whose capabilities are missing. Values are rounded to 2 decimals so they print and
/// persist stably.
WeaponInstance GenerateWeapon(Rng& rng, const Content& content, const Capabilities& caps,
                              std::string_view base_id, Rarity rarity, std::string instance_id);

/// The gun one tier up (a refine): its affixes scaled to the new tier's magnitude, then what
/// the tier adds (affixes, elements, effects) drawn as GenerateWeapon would; its id, level,
/// elements and effects stay. At Exotic it is returned as it is.
WeaponInstance RefineWeapon(Rng& rng, const Content& content, const Capabilities& caps, const WeaponInstance& w);

/// Human-readable affix line ("+12% damage", "Executioner Rounds: ...").
std::string DescribeAffix(const Affix& affix, const Content* content = nullptr);

/// "Incendiary: hits set enemies on fire" (the content's name and description).
std::string DescribeElement(Element e, const Content* content = nullptr);

}  // namespace rm
