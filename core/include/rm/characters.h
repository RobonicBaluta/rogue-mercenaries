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

/// A themed unlock condition shared by characters and skins (None = unlocked from the start).
struct Challenge {
    ChallengeKind kind = ChallengeKind::None;
    std::int64_t count = 0;  // target for the counted kind
    std::string character;   // for RunsCompletedAs
    std::string text;        // shown to the player while locked
};

/// A character skin (costume). Overrides change the archetype a little: the weapons a run
/// can start with, the starting perk and extra character skills. Empty fields keep the
/// character's own values.
struct SkinDef {
    std::string id;
    std::string name;
    /// The adapter's costume/character slot for this skin (game-specific; -1 = unknown).
    std::int64_t costume_index = -1;
    std::vector<std::string> starting_weapons;
    std::string starting_passive;
    std::vector<std::string> skill_pool;
    Challenge unlock;
};

/// Playable character definition (configs/characters.json).
struct CharacterDef {
    std::string id;
    std::string name;
    /// false = reserved/future entry; never selectable or purchasable until the adapter supports it.
    bool enabled = true;
    /// Weapon base ids a run may start with (the loadout roll picks one). Without the
    /// "weapon_id" capability the pick is used as the single sidearm slot.
    std::vector<std::string> starting_weapons;
    /// Perk id granted at run start (empty = none).
    std::string starting_passive;
    /// Character-specific perk ids allowed in addition to the general pool.
    std::vector<std::string> skill_pool;
    /// Free-form mechanic id implemented by the adapter (empty = none).
    std::string exclusive_mechanic;
    /// 0 = not purchasable with credits.
    std::int64_t unlock_cost_credits = 0;
    /// Challenge that unlocks the character. A character with neither a cost nor a
    /// challenge is unlocked from the start.
    Challenge unlock;
    /// The adapter's costume/character slot for this character (game-specific; -1 = unknown).
    std::int64_t costume_index = -1;
    /// Skins; the loader guarantees at least one (the first is the default).
    std::vector<SkinDef> skins;
};

enum class CharacterGate { Selectable, Unknown, Disabled, Locked };
std::string_view CharacterGateName(CharacterGate g);

/// Selectability for StartRun: unknown/disabled/locked characters are rejected.
CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters);
/// Locked while the skin's challenge is unmet (skin ids share the unlocked_characters set).
CharacterGate CheckSkin(const SkinDef& skin, const StringSet& unlocked_characters);

/// Progress toward a challenge, clamped to the target. met() is false without a challenge.
struct ChallengeProgress {
    std::int64_t current = 0;
    std::int64_t target = 0;
    [[nodiscard]] bool met() const { return target > 0 && current >= target; }
};
ChallengeProgress CheckChallenge(const Challenge& challenge, const ProfileStats& stats);
ChallengeProgress CheckChallenge(const CharacterDef& def, const ProfileStats& stats);

/// The named skin, or the default (first) one for an empty id; nullptr if unknown.
const SkinDef* FindSkin(const CharacterDef& def, std::string_view skin_id);
/// The character with the skin's overrides applied (what a run plays).
CharacterDef ApplySkin(const CharacterDef& def, const SkinDef& skin);

}  // namespace rm
