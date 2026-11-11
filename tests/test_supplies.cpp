// The supplies economy by the numbers (the user, 2026-11-11: "my friend was able to get basically all
// guns, upgrade them and still have supplies on a marathon"; scripts/economy_math.py works out what a
// run earns). Per-kill amounts add up exactly, a sale pays half the shop price, and so does what has
// no room.
#include <memory>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <nlohmann/json.hpp>

#include "rm/content.h"
#include "rm/run_controller.h"
#include "rm/weapons.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

KillOutcome Kill(RunController& rc, int i, const char* weapon = nullptr, bool melee = false) {
    KillEvent k;
    k.now = 1.0 + i;
    k.by_player = true;
    k.was_melee = melee;
    k.enemy_id = "e" + std::to_string(i);
    if (weapon != nullptr) k.weapon_id = weapon;
    return rc.OnEnemyKilled(k);
}

}  // namespace

TEST_CASE("a kill's supplies add up exactly, with no rounding per kill", "[economy]") {
    const auto content = test::Load(test::EditFixture([](json&, json&, json&, json&, json& eco) { eco["supplies"]["per_kill"] = 1.25; }));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());  // the starting route pays x1.0
    std::vector<std::int64_t> paid;
    for (int i = 0; i < 4; ++i) paid.push_back(Kill(h.rc, i).supplies_awarded);
    CHECK(paid == std::vector<std::int64_t>{1, 1, 1, 2});  // 1.25, 2.5, 3.75, 5: four kills pay 5, not 4 (or 8)
}

TEST_CASE("a perk's and a gun's supplies per kill may be fractions, paid exactly", "[economy]") {
    const auto content = test::Load(test::EditFixture([](json& perks, json&, json& characters, json&, json&) {
        perks["perks"].push_back(json{{"id", "trickle"}, {"name", "Trickle"}, {"category", "style"}, {"rarity", "common"},
                                      {"effects", {{{"kind", "supplies_on_kill"}, {"amount", 0.5}}}}});
        characters["characters"][0]["starting_passive"] = "trickle";
    }));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    std::vector<std::int64_t> paid;
    for (int i = 0; i < 4; ++i) paid.push_back(Kill(h.rc, i).supplies_awarded);
    CHECK(paid == std::vector<std::int64_t>{2, 3, 2, 3});  // 2 a kill, and the perk's 0.5 every other kill

    // A gun's +0.5 supplies per kill: one supply every other kill made with it.
    test::Harness g;
    REQUIRE(g.rc.StartRun("alpha", 3).ok());
    REQUIRE(g.rc.BeginArena(0.0).ok());
    WeaponInstance looter = MakeStartingWeapon("pistol", "w_loot");
    looter.affixes.push_back(Affix{AffixKind::SuppliesOnKill, 0.5, 0, ""});
    REQUIRE(g.rc.DevGrantWeapon(looter).ok());
    std::vector<std::int64_t> affix;  // the fixture arena's 5 enemies: 4 kills, all paying
    for (int i = 0; i < 4; ++i) affix.push_back(Kill(g.rc, i, "pistol").supplies_awarded - 2);
    CHECK(affix == std::vector<std::int64_t>{0, 1, 0, 1});
    CHECK(DescribeAffix(looter.affixes[0]) == "+0.5 supplies per kill");
    CHECK(DescribeAffix(Affix{AffixKind::SuppliesOnKill, 1.0, 0, ""}) == "+1 supply per kill");
}

TEST_CASE("a sale is worth half the shop price", "[economy]") {
    const auto content = test::Load(test::EditFixture([](json&, json& weapons, json&, json&, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "pistol") w["game_item"] = 0x0102;
            if (w["id"] == "shotgun") w["game_item"] = 0x0104;
            if (w["id"] == "rifle") w["game_item"] = 0x011C;
        }
    }));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    WeaponInstance rifle = MakeStartingWeapon("rifle", "w_r");
    rifle.rarity = Rarity::Legendary;
    REQUIRE(h.rc.DevGrantWeapon(rifle).ok());
    const auto& price = content->economy().shop.weapon_price;  // the fixture's: 10 common .. 50 legendary
    CHECK(h.rc.SaleValue(0x011C, 1) == price[RarityIndex(Rarity::Legendary)] / 2);  // the run's legendary rifle
    CHECK(h.rc.SaleValue(0x0102, 30) == price[RarityIndex(Rarity::Common)] / 2);    // its rounds do not count
    CHECK(h.rc.SaleValue(0x0104, 1) == price[RarityIndex(Rarity::Common)] / 2);     // not the run's: as a common one
    CHECK(h.rc.SaleValue(262, 2) == 5);   // grenades: 10 for a bundle of 2
    CHECK(h.rc.SaleValue(262, 3) == 7);   // 7.5, the whole part
    CHECK(h.rc.SaleValue(772, 1) == 10);  // a spray: 20 for one
    CHECK(h.rc.SaleValue(0x0999, 5) == 0);  // nothing the shop prices
    CHECK(h.rc.SaleValue(262, 0) == 0);
}

TEST_CASE("the shipped economy: a legendary gun sells for 75, a kill pays 1.25 at the route's rate", "[economy]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const EconomyConfig& eco = r.content->economy();
    CHECK(eco.shop.weapon_price[RarityIndex(Rarity::Legendary)] / 2 == 75);
    CHECK(eco.supplies.per_kill == Catch::Approx(1.25));
}
