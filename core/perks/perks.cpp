#include "rm/perks.h"

#include <algorithm>
#include <array>

#include "rm/characters.h"

namespace rm {
namespace {

struct ParamEntry {
    std::string_view name;
    double EffectParams::*field;
};

constexpr std::array<ParamEntry, 13> kParamFields = {{
    {"value", &EffectParams::value},
    {"fraction", &EffectParams::fraction},
    {"window_seconds", &EffectParams::window_seconds},
    {"per_kill", &EffectParams::per_kill},
    {"max_bonus", &EffectParams::max_bonus},
    {"chance", &EffectParams::chance},
    {"multiplier", &EffectParams::multiplier},
    {"amount_fraction_of_max", &EffectParams::amount_fraction_of_max},
    {"amount", &EffectParams::amount},
    {"per_arena", &EffectParams::per_arena},
    {"leave_hp_fraction", &EffectParams::leave_hp_fraction},
    {"game_item", &EffectParams::game_item},
    {"every_kills", &EffectParams::every_kills},
}};

bool Contains(const std::vector<std::string>& list, std::string_view item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

}  // namespace

std::string_view PerkCategoryName(PerkCategory c) {
    switch (c) {
        case PerkCategory::Offensive: return "offensive";
        case PerkCategory::Defensive: return "defensive";
        case PerkCategory::Precision: return "precision";
        case PerkCategory::Economy: return "economy";
        case PerkCategory::Utility: return "utility";
        case PerkCategory::Cooperative: return "cooperative";
        case PerkCategory::Curse: return "curse";
    }
    return "offensive";
}

std::optional<PerkCategory> ParsePerkCategory(std::string_view name) {
    for (PerkCategory c : {PerkCategory::Offensive, PerkCategory::Defensive, PerkCategory::Precision,
                           PerkCategory::Economy, PerkCategory::Utility, PerkCategory::Cooperative,
                           PerkCategory::Curse}) {
        if (PerkCategoryName(c) == name) return c;
    }
    return std::nullopt;
}

std::string_view EffectKindName(EffectKind k) {
    switch (k) {
        case EffectKind::OutgoingDamageMult: return "outgoing_damage_mult";
        case EffectKind::IncomingDamageMult: return "incoming_damage_mult";
        case EffectKind::Crit: return "crit";
        case EffectKind::HealOnKill: return "heal_on_kill";
        case EffectKind::SuppliesOnKill: return "supplies_on_kill";
        case EffectKind::SuppliesOnArenaClear: return "supplies_on_arena_clear";
        case EffectKind::CheatDeath: return "cheat_death";
        case EffectKind::ItemOnKill: return "item_on_kill";
        case EffectKind::ItemOnArenaClear: return "item_on_arena_clear";
    }
    return "outgoing_damage_mult";
}

std::optional<EffectKind> ParseEffectKind(std::string_view name) {
    for (EffectKind k : {EffectKind::OutgoingDamageMult, EffectKind::IncomingDamageMult,
                         EffectKind::Crit, EffectKind::HealOnKill, EffectKind::SuppliesOnKill,
                         EffectKind::SuppliesOnArenaClear, EffectKind::CheatDeath, EffectKind::ItemOnKill,
                         EffectKind::ItemOnArenaClear}) {
        if (EffectKindName(k) == name) return k;
    }
    return std::nullopt;
}

std::string_view ConditionKindName(ConditionKind k) {
    switch (k) {
        case ConditionKind::Always: return "always";
        case ConditionKind::TargetHpBelow: return "target_hp_below";
        case ConditionKind::KillStreak: return "kill_streak";
        case ConditionKind::PlayerHpBelow: return "player_hp_below";
        case ConditionKind::AfterMelee: return "after_melee";
    }
    return "always";
}

std::optional<ConditionKind> ParseConditionKind(std::string_view name) {
    for (ConditionKind k : {ConditionKind::Always, ConditionKind::TargetHpBelow,
                            ConditionKind::KillStreak, ConditionKind::PlayerHpBelow,
                            ConditionKind::AfterMelee}) {
        if (ConditionKindName(k) == name) return k;
    }
    return std::nullopt;
}

std::optional<double EffectParams::*> EffectParamField(std::string_view name) {
    for (const ParamEntry& e : kParamFields) {
        if (e.name == name) return e.field;
    }
    return std::nullopt;
}

EffectParams EffectDef::AtStacks(int stacks) const {
    const double extra = stacks > 1 ? static_cast<double>(stacks - 1) : 0.0;
    EffectParams out = params;
    for (const ParamEntry& e : kParamFields) {
        out.*(e.field) += per_stack.*(e.field) * extra;
    }
    return out;
}

std::vector<std::string> ImpliedCapabilities(const EffectDef& effect) {
    std::vector<std::string> caps;
    auto add = [&caps](std::string_view c) { caps.emplace_back(c); };
    switch (effect.kind) {
        case EffectKind::OutgoingDamageMult:
            add(cap::kOutgoingDamage);
            switch (effect.condition) {
                case ConditionKind::TargetHpBelow: add(cap::kEnemyHp); break;
                case ConditionKind::KillStreak: add(cap::kKillEvents); break;
                case ConditionKind::PlayerHpBelow: add(cap::kIncomingDamage); break;
                case ConditionKind::AfterMelee: add(cap::kMeleeDetect); break;
                case ConditionKind::Always: break;
            }
            break;
        case EffectKind::IncomingDamageMult: add(cap::kIncomingDamage); break;
        case EffectKind::Crit: add(cap::kOutgoingDamage); break;
        case EffectKind::HealOnKill:
            add(cap::kKillEvents);
            add(cap::kHeal);
            break;
        case EffectKind::SuppliesOnKill: add(cap::kKillEvents); break;
        case EffectKind::SuppliesOnArenaClear:
            if (effect.require_no_damage) add(cap::kIncomingDamage);
            break;
        case EffectKind::CheatDeath: add(cap::kIncomingDamage); break;
        case EffectKind::ItemOnKill:
            add(cap::kKillEvents);
            add(cap::kInventory);
            break;
        case EffectKind::ItemOnArenaClear: add(cap::kInventory); break;
    }
    return caps;
}

std::string_view PerkGateName(PerkGate g) {
    switch (g) {
        case PerkGate::Eligible: return "eligible";
        case PerkGate::Disabled: return "disabled";
        case PerkGate::CurseNotAllowed: return "curse_not_allowed";
        case PerkGate::NotCurse: return "not_curse";
        case PerkGate::WrongCharacter: return "wrong_character";
        case PerkGate::MissingCapability: return "missing_capability";
        case PerkGate::Locked: return "locked";
        case PerkGate::MaxStacks: return "max_stacks";
        case PerkGate::Excluded: return "excluded";
    }
    return "eligible";
}

bool CharacterMayUsePerk(const PerkDef& perk, const CharacterDef& character) {
    if (perk.characters.empty()) return true;
    if (Contains(perk.characters, character.id)) return true;
    return Contains(character.skill_pool, perk.id);
}

PerkGate CheckPerk(const PerkDef& perk, const PerkGateInput& input) {
    if (!perk.enabled) return PerkGate::Disabled;
    if (perk.IsCurse() && input.curses == CursePolicy::Exclude) return PerkGate::CurseNotAllowed;
    if (!perk.IsCurse() && input.curses == CursePolicy::Only) return PerkGate::NotCurse;
    if (input.character != nullptr && !CharacterMayUsePerk(perk, *input.character)) {
        return PerkGate::WrongCharacter;
    }
    if (input.capabilities != nullptr && !input.capabilities->HasAll(perk.requires_capabilities)) {
        return PerkGate::MissingCapability;
    }
    if (!input.ignore_unlock && perk.unlock_cost_credits > 0) {
        if (input.unlocked_perks == nullptr ||
            input.unlocked_perks->find(perk.id) == input.unlocked_perks->end()) {
            return PerkGate::Locked;
        }
    }
    if (input.owned != nullptr) {
        const auto it = input.owned->find(perk.id);
        if (it != input.owned->end() && it->second >= perk.max_stacks) return PerkGate::MaxStacks;
        for (const std::string& other : perk.exclusive_with) {
            const auto o = input.owned->find(other);
            if (o != input.owned->end() && o->second > 0) return PerkGate::Excluded;
        }
    }
    return PerkGate::Eligible;
}

}  // namespace rm
