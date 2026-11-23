#include <catch_amalgamated.hpp>

#include "rm/gun_levels.h"

using namespace rm;

namespace {

// Rows of the game's own tables (research/re5-engine-map.md "Gun stat tables").
GunLevelTables M92F() { return {{10, 13, 16, 20, 25, 30, 33, 37, 40, 45, 50, 60, 70, 100}, {1, .95f, .9f, .8f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}; }
GunLevelTables M500() { return {{5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, .9f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}; }
GunLevelTables Mp5() { return {{45, 55, 65, 80, 90, 100, 110, 120, 130, 150, 0, 0, 0, 0}, {.9f, .85f, .8f, .7f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}; }
GunLevelTables Px4() { return {{25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}; }

}  // namespace

TEST_CASE("each upgrade moves capacity and reload one step", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(M92F(), {0, 0}, 2);
    CHECK(l.capacity == 2);  // 16 rounds
    CHECK(l.reload == 2);    // 0.9
}

TEST_CASE("reload stops at the gun's last real level and capacity goes on", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(M92F(), {0, 0}, 4);
    CHECK(l.capacity == 4);
    CHECK(l.reload == 3);  // the next row reads 1.0: slower, not a level
}

TEST_CASE("a magnum with one step left takes it and stops", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(M500(), {0, 0}, 4);
    CHECK(l.capacity == 1);  // 6, the next reads 0
    CHECK(l.reload == 1);
}

TEST_CASE("a gun whose first level is already quicker still climbs", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(Mp5(), {0, 0}, 5);
    CHECK(l.capacity == 5);
    CHECK(l.reload == 3);
}

TEST_CASE("a gun with no levels is left alone", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(Px4(), {0, 0}, 4);
    CHECK(l.capacity == 0);
    CHECK(l.reload == 0);
}

TEST_CASE("a gun that came upgraded stops at the end of its table", "[gun-levels]") {
    const GunStatLevels l = RaiseGunLevels(M92F(), {10, 3}, 9);  // Chris's Mercenaries M92F holds 50 rounds
    CHECK(l.capacity == 13);                                     // 100, the last row
    CHECK(l.reload == 3);
}

TEST_CASE("no steps change nothing, and levels from a bad record stay out of the tables", "[gun-levels]") {
    CHECK(RaiseGunLevels(M92F(), {3, 1}, 0).capacity == 3);
    const GunStatLevels past = RaiseGunLevels(M92F(), {13, 13}, 3);
    CHECK(past.capacity == 13);
    CHECK(past.reload == 13);
    const GunStatLevels negative = RaiseGunLevels(M92F(), {-1, -1}, 3);
    CHECK(negative.capacity == -1);
    CHECK(negative.reload == -1);
}

// Fire speed and piercing (2026-11-23, the user: "new upgrade lines (fire speed, piercing)"): the game's own
// piercing levels (the H&K P8: 2, 3, 4, 5 enemies) and the run's fire speed levels the adapter writes.
TEST_CASE("each upgrade also moves fire speed and piercing, as far as their levels go", "[gun-levels]") {
    GunLevelTables p8 = M92F();
    p8.fire = {26, 24, 22, 20, 18.7f, 17.2f, 0, 0, 0, 0, 0, 0, 0, 0};  // the run's five, then none
    p8.pierce = {2, 3, 4, 5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};           // the game's: four levels, then 1s
    const GunStatLevels two = RaiseGunLevels(p8, {0, 0, 0, 0}, 2);
    CHECK(two.fire == 2);
    CHECK(two.pierce == 2);
    const GunStatLevels nine = RaiseGunLevels(p8, {0, 0, 0, 0}, 9);
    CHECK(nine.fire == 5);    // stops where the written levels end (0 is no level)
    CHECK(nine.pierce == 3);  // stops at 5 enemies: the 1 after it is no gain
    CHECK(nine.capacity == 9);
}

TEST_CASE("the game's untouched fire table (1.0 past its first level) is never stepped onto", "[gun-levels]") {
    GunLevelTables stock = M92F();
    stock.fire = {26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // as the adapter reports a gun without the run's curve
    stock.pierce = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const GunStatLevels l = RaiseGunLevels(stock, {0, 0, 0, 0}, 5);
    CHECK(l.fire == 0);
    CHECK(l.pierce == 0);
    CHECK(l.capacity == 5);
}
