#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/common.h"

namespace rm {

struct ProfileStats;

/// What a themed unlock challenge counts (configs/characters.json "unlock_kind").
enum class ChallengeKind {
    None,
    RunsCompleted,    // completed runs with any character
    RunsCompletedAs,  // completed runs as "unlock_character"
    TotalKills,
    BestArena,        // most arenas cleared in one run
    CreditsEarned,    // lifetime banked credits
    NoDamageArenas,   // arenas cleared without taking damage
    SRankArenas,      // arenas rated S
};
std::optional<ChallengeKind> ParseChallengeKind(std::string_view name);
std::string_view ChallengeKindName(ChallengeKind k);

/// Playable character definition (configs/characters.json).
struct CharacterDef {
    std::string id;
    std::string name;
    /// false = reserved/future entry; never selectable or purchasable until the adapter supports it.
    bool enabled = true;
    /// Weapon base ids granted at run start. Without the "weapon_id" capability only the
    /// first one is used (as the single sidearm slot).
    std::vector<std::string> starting_weapons;
    /// Perk id granted at run start (empty = none).
    std::string starting_passive;
    /// Character-specific perk ids allowed in addition to the general pool.
    std::vector<std::string> skill_pool;
    /// Free-form mechanic id implemented by the adapter (empty = none).
    std::string exclusive_mechanic;
    /// 0 = not purchasable with credits.
    std::int64_t unlock_cost_credits = 0;
    /// Challenge that unlocks the character (None = no challenge). A character with neither
    /// a cost nor a challenge is unlocked from the start.
    ChallengeKind unlock_kind = ChallengeKind::None;
    std::int64_t unlock_count = 0;  // target for the counted kind
    std::string unlock_character;   // for RunsCompletedAs
    std::string unlock_text;        // shown to the player while locked
};

enum class CharacterGate { Selectable, Unknown, Disabled, Locked };
std::string_view CharacterGateName(CharacterGate g);

/// Selectability for StartRun: unknown/disabled/locked characters are rejected.
CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters);

/// Progress toward a character's challenge, clamped to the target. met() is false for a
/// character without a challenge.
struct ChallengeProgress {
    std::int64_t current = 0;
    std::int64_t target = 0;
    [[nodiscard]] bool met() const { return target > 0 && current >= target; }
};
ChallengeProgress CheckChallenge(const CharacterDef& def, const ProfileStats& stats);

}  // namespace rm
