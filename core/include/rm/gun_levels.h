#pragma once

#include <array>

namespace rm {

/// The game keeps 14 levels per stat and gun; levels the gun does not have read as 0 (capacity) or
/// go back to 1.0 (reload's time multiplier), so the next step is only taken when it is a real gain.
inline constexpr int kGunStatLevels = 14;

struct GunLevelTables {
    std::array<int, kGunStatLevels> capacity{};  ///< rounds at each level
    std::array<float, kGunStatLevels> reload{};  ///< reload time multiplier at each level
    /// The time between shots at each level (the game's LvFireSpeed table; 0: no such level). The game has one
    /// level per gun; the adapter writes the run's faster ones (2026-11-23, the user: "new upgrade lines (fire
    /// speed, piercing)") and reports only those.
    std::array<float, kGunStatLevels> fire{};
    std::array<float, kGunStatLevels> pierce{};  ///< enemies a shot goes through at each level (LvThrough)
};

struct GunStatLevels {
    int capacity = 0;
    int reload = 0;
    int fire = 0;
    int pierce = 0;
};

/// `steps` SHOP upgrades on the game's own levels: each moves capacity, reload, fire speed and piercing one
/// step, each stopping where the gun's table ends. A gun with no levels left is unchanged.
inline GunStatLevels RaiseGunLevels(const GunLevelTables& tables, GunStatLevels levels, int steps) {
    for (int i = 0; i < steps; ++i) {
        const int nextFire = levels.fire + 1;  // a shorter time between shots
        if (levels.fire >= 0 && nextFire < kGunStatLevels) {
            const float next = tables.fire[static_cast<std::size_t>(nextFire)];
            if (next > 0.0f && next < tables.fire[static_cast<std::size_t>(levels.fire)]) levels.fire = nextFire;
        }
        const int nextPierce = levels.pierce + 1;  // more enemies a shot goes through
        if (levels.pierce >= 0 && nextPierce < kGunStatLevels &&
            tables.pierce[static_cast<std::size_t>(nextPierce)] > tables.pierce[static_cast<std::size_t>(levels.pierce)]) {
            levels.pierce = nextPierce;
        }
        const int nextCapacity = levels.capacity + 1;
        if (levels.capacity >= 0 && nextCapacity < kGunStatLevels &&
            tables.capacity[static_cast<std::size_t>(nextCapacity)] > tables.capacity[static_cast<std::size_t>(levels.capacity)]) {
            levels.capacity = nextCapacity;
        }
        const int nextReload = levels.reload + 1;
        if (levels.reload >= 0 && nextReload < kGunStatLevels) {
            const float next = tables.reload[static_cast<std::size_t>(nextReload)];
            if (next > 0.0f && next < tables.reload[static_cast<std::size_t>(levels.reload)]) levels.reload = nextReload;
        }
    }
    return levels;
}

}  // namespace rm
