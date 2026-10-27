#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rm/common.h"

namespace rm {

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
    /// 0 = unlocked by default.
    std::int64_t unlock_cost_credits = 0;
};

enum class CharacterGate { Selectable, Unknown, Disabled, Locked };
std::string_view CharacterGateName(CharacterGate g);

/// Selectability for StartRun: unknown/disabled/locked characters are rejected.
CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters);

}  // namespace rm
