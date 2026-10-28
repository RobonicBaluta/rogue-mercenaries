#include <set>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using Catch::Approx;

TEST_CASE("weapon generation follows rarity affix counts and capabilities", "[weapons]") {
    auto content = test::LoadFixture();
    const Capabilities all = Capabilities::All();
    for (Rarity r : kAllRarities) {
        const auto& rule = content->economy().weapon_rarity[RarityIndex(r)];
        for (std::uint64_t seed = 0; seed < 50; ++seed) {
            Rng rng(seed);
            const WeaponInstance w = GenerateWeapon(rng, *content, all, "pistol", r, "w1");
            CHECK(w.rarity == r);
            CHECK(w.level == 1);
            int regular = 0;
            int unique = 0;
            std::set<AffixKind> kinds;
            for (const Affix& a : w.affixes) {
                kinds.insert(a.kind);
                if (IsUniqueAffix(a.kind)) {
                    ++unique;
                    CHECK_FALSE(a.unique_id.empty());
                } else {
                    ++regular;
                }
            }
            CHECK(regular == rule.affix_count);
            CHECK(unique == rule.unique_count);
            CHECK(kinds.size() == w.affixes.size());  // no duplicate affix kinds
        }
    }
    // Deterministic for a given stream state.
    Rng a(77);
    Rng b(77);
    const WeaponInstance wa = GenerateWeapon(a, *content, all, "shotgun", Rarity::Legendary, "x");
    const WeaponInstance wb = GenerateWeapon(b, *content, all, "shotgun", Rarity::Legendary, "x");
    REQUIRE(wa.affixes.size() == wb.affixes.size());
    for (std::size_t i = 0; i < wa.affixes.size(); ++i) {
        CHECK(wa.affixes[i].kind == wb.affixes[i].kind);
        CHECK(wa.affixes[i].value == wb.affixes[i].value);
    }
    // Only kill_events: damage affixes are filtered, supplies_on_kill remains.
    const Capabilities kills_only{cap::kKillEvents};
    Rng c(5);
    const WeaponInstance plain = GenerateWeapon(c, *content, kills_only, "pistol", Rarity::Epic, "y");
    REQUIRE(plain.affixes.size() == 1);
    CHECK(plain.affixes[0].kind == AffixKind::SuppliesOnKill);
}

TEST_CASE("affix descriptions are human readable", "[weapons]") {
    auto content = test::LoadFixture();
    CHECK(DescribeAffix(Affix{AffixKind::DamagePct, 12.5, 0, ""}) == "+12.5% damage");
    CHECK(DescribeAffix(Affix{AffixKind::CritChance, 0.05, 0, ""}) == "+5% crit chance");
    CHECK(DescribeAffix(Affix{AffixKind::LowHpTargetDamagePct, 20, 0.3, ""}) ==
          "+20% damage vs targets below 30% HP");
    CHECK(DescribeAffix(Affix{AffixKind::SuppliesOnKill, 2, 0, ""}) == "+2 supplies per kill");
    CHECK(DescribeAffix(Affix{AffixKind::UniqueLowHpTargetBonus, 1.0, 0.2, "executioner_rounds"}, content.get()) ==
          "Executioner Rounds: +100% damage vs targets below 20% HP");
    CHECK(DescribeAffix(Affix{AffixKind::UniqueKillStreakExtend, 2.0, 0, "bloodhound"}, content.get()) ==
          "Bloodhound: kills extend kill-streak windows by 2s");
}

TEST_CASE("weapon_id mode applies an instance only to matching hits", "[weapons]") {
    test::Harness h;  // all capabilities -> weapon_id mode
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.state().weapons.size() == 1);  // the rolled weapon only
    REQUIRE(h.rc.BeginArena(0.0).ok());
    WeaponInstance w = MakeStartingWeapon("shotgun", "w_sg");
    w.level = 3;
    w.affixes.push_back(Affix{AffixKind::DamagePct, 10.0, 0, ""});
    REQUIRE(h.rc.DevGrantWeapon(w).ok());
    CHECK(h.rc.state().weapons.size() == 2);  // the shotgun joins the pistol

    HitContext hit;
    hit.now = 1.0;
    hit.weapon_id = "shotgun";
    OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(r.multiplier == Approx(1.3));  // level 3 (+20%) + 10%
    CHECK(r.breakdown.weapon_instance_id == "w_sg");
    hit.weapon_id = "pistol";
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).multiplier == Approx(1.0));
    hit.weapon_id = std::nullopt;
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).breakdown.weapon_instance_id.empty());
    hit.weapon_id = "shotgun";
    hit.is_melee = true;  // melee never gets weapon modifiers
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).multiplier == Approx(1.0));

    // Weapon supplies_on_kill only for kills with that weapon.
    WeaponInstance looter = MakeStartingWeapon("pistol", "w_loot");
    looter.affixes.push_back(Affix{AffixKind::SuppliesOnKill, 2.0, 0, ""});
    REQUIRE(h.rc.DevGrantWeapon(looter).ok());
    KillEvent k;
    k.by_player = true;
    k.now = 2.0;
    k.weapon_id = "pistol";
    CHECK(h.rc.OnEnemyKilled(k).supplies_awarded == 4);  // 2 base + 2 affix
    k.weapon_id = "shotgun";
    CHECK(h.rc.OnEnemyKilled(k).supplies_awarded == 2);
}

TEST_CASE("without weapon_id a single sidearm applies to all non-melee hits", "[weapons]") {
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kWeaponId);
    test::Harness h(test::LoadFixture(), {}, caps);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.state().weapons.size() == 1);  // first starting weapon only
    CHECK(h.rc.state().weapons[0].base_id == "pistol");
    REQUIRE(h.rc.BeginArena(0.0).ok());
    WeaponInstance w = MakeStartingWeapon("rifle", "w_r");
    w.affixes.push_back(Affix{AffixKind::DamagePct, 50.0, 0, ""});
    REQUIRE(h.rc.DevGrantWeapon(w).ok());
    REQUIRE(h.rc.state().weapons.size() == 1);  // replaced the sidearm
    CHECK(h.rc.state().weapons[0].instance_id == "w_r");
    HitContext hit;
    hit.now = 1.0;
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).multiplier == Approx(1.5));
    hit.weapon_id = "shotgun";  // ignored without the capability
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).multiplier == Approx(1.5));
    hit.is_melee = true;
    CHECK(h.rc.ComputeOutgoingMultiplier(hit).multiplier == Approx(1.0));
}

TEST_CASE("weapon upgrade cost and level bonus come from economy", "[weapons]") {
    auto content = test::LoadFixture();
    const EconomyConfig& eco = content->economy();
    CHECK(UpgradeCost(eco, 1) == 10);
    CHECK(UpgradeCost(eco, 4) == 40);
    CHECK_FALSE(UpgradeCost(eco, 5).has_value());
    CHECK(LevelDamageBonus(eco, 1) == 0.0);
    CHECK(LevelDamageBonus(eco, 5) == Approx(0.4));
}
