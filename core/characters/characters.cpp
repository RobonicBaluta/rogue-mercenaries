#include "rm/characters.h"

#include <algorithm>

#include "rm/profile.h"

namespace rm {

namespace {

constexpr struct {
    ChallengeKind kind;
    const char* name;
} kChallengeNames[] = {
    {ChallengeKind::None, "none"},
    {ChallengeKind::RunsCompleted, "runs_completed"},
    {ChallengeKind::RunsCompletedAs, "runs_completed_as"},
    {ChallengeKind::TotalKills, "total_kills"},
    {ChallengeKind::BestArena, "best_arena"},
    {ChallengeKind::CreditsEarned, "credits_earned"},
    {ChallengeKind::NoDamageArenas, "no_damage_arenas"},
    {ChallengeKind::SRankArenas, "s_rank_arenas"},
    {ChallengeKind::BossBeaten, "boss_beaten"},
};

}  // namespace

std::optional<ChallengeKind> ParseChallengeKind(std::string_view name) {
    for (const auto& entry : kChallengeNames) {
        if (name == entry.name) return entry.kind;
    }
    return std::nullopt;
}

std::string_view ChallengeKindName(ChallengeKind k) {
    for (const auto& entry : kChallengeNames) {
        if (entry.kind == k) return entry.name;
    }
    return "none";
}

std::string_view CharacterGateName(CharacterGate g) {
    switch (g) {
        case CharacterGate::Selectable: return "selectable";
        case CharacterGate::Unknown: return "unknown";
        case CharacterGate::Disabled: return "disabled";
        case CharacterGate::Locked: return "locked";
        case CharacterGate::NotInstalled: return "not_installed";
    }
    return "unknown";
}

CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters) {
    if (def == nullptr) return CharacterGate::Unknown;
    if (!def->enabled) return CharacterGate::Disabled;
    const bool gated = def->unlock_cost_credits > 0 || def->unlock.kind != ChallengeKind::None;
    if (gated && unlocked_characters.find(def->id) == unlocked_characters.end()) {
        return CharacterGate::Locked;
    }
    return CharacterGate::Selectable;
}

CharacterGate CheckSkin(const SkinDef& skin, const StringSet& unlocked_characters) {
    if (skin.unlock.kind != ChallengeKind::None && unlocked_characters.find(skin.id) == unlocked_characters.end()) {
        return CharacterGate::Locked;
    }
    return CharacterGate::Selectable;
}

ChallengeProgress CheckChallenge(const Challenge& challenge, const ProfileStats& stats) {
    ChallengeProgress p;
    if (challenge.kind == ChallengeKind::None || challenge.count <= 0) return p;
    p.target = challenge.count;
    std::int64_t current = 0;
    switch (challenge.kind) {
        case ChallengeKind::None: break;
        case ChallengeKind::RunsCompleted: current = stats.runs_completed; break;
        case ChallengeKind::RunsCompletedAs: {
            const auto it = stats.runs_completed_as.find(challenge.character);
            current = it == stats.runs_completed_as.end() ? 0 : it->second;
            break;
        }
        case ChallengeKind::TotalKills: current = stats.total_kills; break;
        case ChallengeKind::BestArena: current = stats.best_arena; break;
        case ChallengeKind::CreditsEarned: current = stats.credits_earned; break;
        case ChallengeKind::NoDamageArenas: current = stats.no_damage_arenas; break;
        case ChallengeKind::SRankArenas: current = stats.s_rank_arenas; break;
        case ChallengeKind::BossBeaten: current = stats.bosses_beaten.count(challenge.boss) != 0 ? 1 : 0; break;
    }
    p.current = std::clamp<std::int64_t>(current, 0, p.target);
    return p;
}

ChallengeProgress CheckChallenge(const CharacterDef& def, const ProfileStats& stats) {
    return CheckChallenge(def.unlock, stats);
}

const SkinDef* FindSkin(const CharacterDef& def, std::string_view skin_id) {
    if (def.skins.empty()) return nullptr;
    if (skin_id.empty()) return &def.skins.front();
    const auto it = std::find_if(def.skins.begin(), def.skins.end(), [&](const SkinDef& s) { return s.id == skin_id; });
    return it == def.skins.end() ? nullptr : &*it;
}

CharacterDef ApplySkin(const CharacterDef& def, const SkinDef& skin) {
    CharacterDef out = def;
    if (!skin.starting_weapons.empty()) out.starting_weapons = skin.starting_weapons;
    if (!skin.starting_passive.empty()) out.starting_passive = skin.starting_passive;
    for (const std::string& perk : skin.skill_pool) {
        if (std::find(out.skill_pool.begin(), out.skill_pool.end(), perk) == out.skill_pool.end()) {
            out.skill_pool.push_back(perk);
        }
    }
    if (skin.costume_index >= 0) out.costume_index = skin.costume_index;
    out.build = skin.build;
    return out;
}

void ApplyPerkBuild(SkinBuild& build, const SkinBuild& perk) {
    for (const auto& [category, value] : perk.category_damage) build.category_damage[category] += value;
    build.melee_damage += perk.melee_damage;
    build.rarity_shift += perk.rarity_shift;
    build.max_health_mult *= perk.max_health_mult;
    build.heal_mult *= perk.heal_mult;
    build.supplies_mult *= perk.supplies_mult;
    build.ammo_stack_mult *= perk.ammo_stack_mult;
    for (const auto& [item, mult] : perk.stack_mult) {
        const auto it = std::find_if(build.stack_mult.begin(), build.stack_mult.end(), [item = item](const auto& s) { return s.first == item; });
        if (it == build.stack_mult.end()) build.stack_mult.emplace_back(item, mult);
        else it->second *= mult;
    }
    for (const std::string& c : perk.never_offered.categories) build.never_offered.categories.push_back(c);
    for (const std::string& id : perk.never_offered.ids) build.never_offered.ids.push_back(id);
    // One conversion of drops at a time: a skin's own (Safari's mines) wins over the perk's.
    if (build.drops_as < 0) build.drops_as_held_ammo = build.drops_as_held_ammo || perk.drops_as_held_ammo;
    for (const auto& pick : perk.pickup_as) build.pickup_as.push_back(pick);
    build.break_grabs = build.break_grabs || perk.break_grabs;
}

bool WeaponFilter::Allows(std::string_view base_id, std::string_view category) const {
    if (Empty()) return true;
    return std::find(categories.begin(), categories.end(), category) != categories.end() ||
           std::find(ids.begin(), ids.end(), base_id) != ids.end();
}

}  // namespace rm
