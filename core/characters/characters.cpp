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
    }
    return "unknown";
}

CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters) {
    if (def == nullptr) return CharacterGate::Unknown;
    if (!def->enabled) return CharacterGate::Disabled;
    const bool gated = def->unlock_cost_credits > 0 || def->unlock_kind != ChallengeKind::None;
    if (gated && unlocked_characters.find(def->id) == unlocked_characters.end()) {
        return CharacterGate::Locked;
    }
    return CharacterGate::Selectable;
}

ChallengeProgress CheckChallenge(const CharacterDef& def, const ProfileStats& stats) {
    ChallengeProgress p;
    if (def.unlock_kind == ChallengeKind::None || def.unlock_count <= 0) return p;
    p.target = def.unlock_count;
    std::int64_t current = 0;
    switch (def.unlock_kind) {
        case ChallengeKind::None: break;
        case ChallengeKind::RunsCompleted: current = stats.runs_completed; break;
        case ChallengeKind::RunsCompletedAs: {
            const auto it = stats.runs_completed_as.find(def.unlock_character);
            current = it == stats.runs_completed_as.end() ? 0 : it->second;
            break;
        }
        case ChallengeKind::TotalKills: current = stats.total_kills; break;
        case ChallengeKind::BestArena: current = stats.best_arena; break;
        case ChallengeKind::CreditsEarned: current = stats.credits_earned; break;
        case ChallengeKind::NoDamageArenas: current = stats.no_damage_arenas; break;
        case ChallengeKind::SRankArenas: current = stats.s_rank_arenas; break;
    }
    p.current = std::clamp<std::int64_t>(current, 0, p.target);
    return p;
}

}  // namespace rm
