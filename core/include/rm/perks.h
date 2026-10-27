#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/capabilities.h"
#include "rm/common.h"
#include "rm/rarity.h"

namespace rm {

struct CharacterDef;

enum class PerkCategory { Offensive, Defensive, Precision, Economy, Utility, Cooperative, Curse };
std::string_view PerkCategoryName(PerkCategory c);
std::optional<PerkCategory> ParsePerkCategory(std::string_view name);

/// Supported effect kinds (perks.json "kind").
enum class EffectKind {
    OutgoingDamageMult,
    IncomingDamageMult,
    Crit,
    HealOnKill,
    SuppliesOnKill,
    SuppliesOnArenaClear,
    CheatDeath,
};
std::string_view EffectKindName(EffectKind k);
std::optional<EffectKind> ParseEffectKind(std::string_view name);

/// Activation condition for damage multipliers (perks.json "condition.type").
enum class ConditionKind { Always, TargetHpBelow, KillStreak, PlayerHpBelow, AfterMelee };
std::string_view ConditionKindName(ConditionKind k);
std::optional<ConditionKind> ParseConditionKind(std::string_view name);

/// Numeric effect parameters; JSON keys use the same names. Each kind reads a subset:
///   outgoing_damage_mult  value; condition target_hp_below/player_hp_below: fraction;
///                         kill_streak: window_seconds, per_kill, max_bonus (value unused);
///                         after_melee: window_seconds
///   incoming_damage_mult  value; condition player_hp_below: fraction
///   crit                  chance, multiplier
///   heal_on_kill          amount_fraction_of_max
///   supplies_on_kill      amount, chance
///   supplies_on_arena_clear amount (+ require_no_damage flag)
///   cheat_death           per_arena, leave_hp_fraction
/// Damage "value" is an additive fraction: 0.15 = +15 %, -0.10 = -10 %.
struct EffectParams {
    double value = 0.0;
    double fraction = 0.0;
    double window_seconds = 0.0;
    double per_kill = 0.0;
    double max_bonus = 0.0;
    double chance = 0.0;
    double multiplier = 0.0;
    double amount_fraction_of_max = 0.0;
    double amount = 0.0;
    double per_arena = 0.0;
    double leave_hp_fraction = 0.0;
};

/// Maps a JSON parameter name to its EffectParams field (nullopt for unknown names).
std::optional<double EffectParams::*> EffectParamField(std::string_view name);

struct EffectDef {
    EffectKind kind = EffectKind::OutgoingDamageMult;
    ConditionKind condition = ConditionKind::Always;
    EffectParams params;
    /// Added once per stack beyond the first ("per_stack" object in JSON; omitted keys don't scale).
    EffectParams per_stack;
    bool require_no_damage = false;

    /// Parameters at the given stack count (stacks < 1 is treated as 1).
    [[nodiscard]] EffectParams AtStacks(int stacks) const;
};

/// Capabilities an effect needs to have any effect (derived from kind and condition).
std::vector<std::string> ImpliedCapabilities(const EffectDef& effect);

struct PerkDef {
    std::string id;
    std::string name;
    std::string description;
    PerkCategory category = PerkCategory::Offensive;
    Rarity rarity = Rarity::Common;
    std::vector<std::string> tags;
    int max_stacks = 1;
    /// Normalised to be symmetric by the content loader.
    std::vector<std::string> exclusive_with;
    /// Empty = available to every character (general pool).
    std::vector<std::string> characters;
    std::int64_t unlock_cost_credits = 0;
    /// Declared plus implied capabilities (sorted union).
    std::vector<std::string> requires_capabilities;
    std::vector<EffectDef> effects;
    bool enabled = true;

    [[nodiscard]] bool IsCurse() const { return category == PerkCategory::Curse; }
};

/// Why a perk can or cannot be granted to the current run.
enum class PerkGate {
    Eligible,
    Disabled,
    CurseNotAllowed,
    NotCurse,
    WrongCharacter,
    MissingCapability,
    Locked,
    MaxStacks,
    Excluded,
};
std::string_view PerkGateName(PerkGate g);

enum class CursePolicy { Exclude, Only, Any };

struct PerkGateInput {
    const CharacterDef* character = nullptr;       // null: skip the character restriction
    const Capabilities* capabilities = nullptr;    // null: assume every capability
    const StringSet* unlocked_perks = nullptr;     // null: perks with a cost count as locked
    const StringMap<int>* owned = nullptr;         // perk id -> stacks held this run
    CursePolicy curses = CursePolicy::Exclude;
    bool ignore_unlock = false;                    // starting passives are granted by the character
};

/// A character may take a perk if the perk is general (no character list), names the
/// character, or appears in the character's skill_pool.
bool CharacterMayUsePerk(const PerkDef& perk, const CharacterDef& character);

PerkGate CheckPerk(const PerkDef& perk, const PerkGateInput& input);

}  // namespace rm
