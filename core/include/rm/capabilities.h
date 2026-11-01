#pragma once

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

#include "rm/common.h"

namespace rm {

/// Engine features an adapter can report as verified. Content declares (and the loader
/// derives) which ones it needs; anything whose requirements are missing is never offered.
namespace cap {
inline constexpr std::string_view kOutgoingDamage = "outgoing_damage";  // hit multiplier can be applied
inline constexpr std::string_view kIncomingDamage = "incoming_damage";  // player damage observed/scaled
inline constexpr std::string_view kKillEvents = "kill_events";          // enemy deaths with attribution
inline constexpr std::string_view kHeal = "heal";                       // adapter can heal the player
inline constexpr std::string_view kMeleeDetect = "melee_detect";        // hits/kills flag melee reliably
inline constexpr std::string_view kWeaponId = "weapon_id";              // hits/kills report weapon base id
inline constexpr std::string_view kEnemyHp = "enemy_hp";                // hits report target hp / max hp
inline constexpr std::string_view kInventory = "inventory";             // adapter can put items in the inventory
inline constexpr std::string_view kDirector = "director";               // adapter spawns the arena's enemies itself

inline constexpr std::array<std::string_view, 9> kAll = {
    kOutgoingDamage, kIncomingDamage, kKillEvents, kHeal, kMeleeDetect, kWeaponId, kEnemyHp, kInventory, kDirector,
};
}  // namespace cap

bool IsKnownCapability(std::string_view name);

/// Set of verified capability names supplied by the adapter (or the simulator).
class Capabilities {
public:
    Capabilities() = default;
    Capabilities(std::initializer_list<std::string_view> names);

    /// Every known capability (simulator default).
    static Capabilities All();

    void Add(std::string_view name);
    void Remove(std::string_view name);
    [[nodiscard]] bool Has(std::string_view name) const;

    template <class Range>
    [[nodiscard]] bool HasAll(const Range& names) const {
        for (const auto& n : names) {
            if (!Has(n)) return false;
        }
        return true;
    }

    [[nodiscard]] const StringSet& names() const { return names_; }
    /// Comma-separated, sorted ("" when empty).
    [[nodiscard]] std::string ToString() const;

    bool operator==(const Capabilities&) const = default;

private:
    StringSet names_;
};

}  // namespace rm
