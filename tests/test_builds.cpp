// Skin builds (ROADMAP M8): weapon filters, category damage, supplies, free shop rerolls,
// uncapped upgrades and a build's starting weapon.

#include <algorithm>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "rm/rewards.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

// Alpha gets a second skin whose build is `build`.
ContentSources BuildSources(const json& build) {
    return test::EditFixture([&build](json&, json& weapons, json& characters, json&, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "shotgun") w["game_item"] = 0x0104;  // a game item to hand over
        }
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_build"}, {"name", "Build"}, {"costume_index", 1}, {"build", build}},
        });
    });
}

std::vector<std::string> LoadErrors(const json& build) {
    const auto loaded = Content::LoadFromStrings(BuildSources(build));
    for (const std::string& e : loaded.errors) UNSCOPED_INFO("got: " << e);
    return loaded.errors;
}

bool HasError(const std::vector<std::string>& errors, std::string_view needle) {
    return std::any_of(errors.begin(), errors.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
}

// Plays arena `arena` (1-based) to its shop.
void ToShop(RunController& rc, double t) {
    test::ClearArena(rc, t);
    REQUIRE(rc.ContinueToRewards().ok());
    REQUIRE(rc.ChooseReward(0).ok());
    REQUIRE(rc.phase() == Phase::Shop);
}

}  // namespace

TEST_CASE("build blocks are validated", "[builds]") {
    CHECK(LoadErrors(json{{"summary", "ok"}, {"category_damage", {{"shotgun", 0.3}}}}).empty());
    CHECK(HasError(LoadErrors(json{{"offered_weapons", {{"categories", {"lasers"}}}}}), "unknown weapon category"));
    CHECK(HasError(LoadErrors(json{{"usable_weapons", {{"ids", {"bazooka"}}}}}), "unknown weapon"));
    CHECK(HasError(LoadErrors(json{{"category_damage", {{"lasers", 0.1}}}}), "unknown weapon category"));
    CHECK(HasError(LoadErrors(json{{"starting_weapon", "shotgun"}, {"usable_weapons", {{"categories", {"handgun"}}}}}),
                   "not one of the build's usable weapons"));
    CHECK(HasError(LoadErrors(json{{"starting_weapon_rarity", "legendary"}}), "without a starting_weapon"));
    CHECK(HasError(LoadErrors(json{{"wings", true}}), "wings"));
    CHECK(HasError(LoadErrors(json{{"stack_max", {{"mines", 10}}}}), "not a game item id"));
    CHECK(HasError(LoadErrors(json{{"stack_max", {{"0x010A", 0}}}}), "must be an integer"));

    const auto content = test::Load(BuildSources(json{{"stack_max", {{"0x010A", 10}, {"513", 120}}}}));
    const SkinDef* skin = FindSkin(*content->FindCharacter("alpha"), "alpha_build");
    REQUIRE(skin != nullptr);
    REQUIRE(skin->build.stack_max.size() == 2);
    CHECK(std::count(skin->build.stack_max.begin(), skin->build.stack_max.end(), std::pair<std::int64_t, int>{0x010A, 10}) == 1);
    CHECK(std::count(skin->build.stack_max.begin(), skin->build.stack_max.end(), std::pair<std::int64_t, int>{513, 120}) == 1);
}

TEST_CASE("a build's offered weapons limit rewards and the shop", "[builds]") {
    // The rifle (an unlocked handgun-free pool: pistol is held) would be offered without the filter.
    const auto content = test::Load(BuildSources(json{{"offered_weapons", {{"categories", {"shotgun"}}}}}));
    int weapons_seen = 0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.StartRun("alpha", seed, "alpha_build").ok());
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        for (const RewardChoice& c : h.rc.RewardOffer()) {
            if (c.kind != RewardKind::Weapon) continue;
            CHECK(c.weapon.base_id == "shotgun");
            ++weapons_seen;
        }
        REQUIRE(h.rc.ChooseReward(0).ok());
        for (const ShopOffer& o : h.rc.ShopOffers()) {
            if (o.kind != ShopOfferKind::Weapon) continue;
            CHECK(o.weapon.base_id == "shotgun");
            ++weapons_seen;
        }
    }
    CHECK(weapons_seen > 0);
}

TEST_CASE("weapons outside a build's usable set are shown in the shop but cannot be bought", "[builds]") {
    // Only the pistol is usable; the shotgun may be offered (shown, restricted).
    const auto content = test::Load(BuildSources(json{{"usable_weapons", {{"ids", {"pistol"}}}}}));
    bool saw_restricted = false;
    for (std::uint64_t seed = 1; seed < 40 && !saw_restricted; ++seed) {
        test::Harness run(content);
        REQUIRE(run.rc.StartRun("alpha", seed, "alpha_build").ok());
        test::ClearArena(run.rc, 0.0);
        REQUIRE(run.rc.ContinueToRewards().ok());
        for (const RewardChoice& c : run.rc.RewardOffer()) CHECK(c.kind != RewardKind::Weapon);
        REQUIRE(run.rc.ChooseReward(0).ok());
        REQUIRE(run.rc.phase() == Phase::Shop);
        const auto offers = run.rc.ShopOffers();
        for (std::size_t i = 0; i < offers.size(); ++i) {
            if (offers[i].kind != ShopOfferKind::Weapon) continue;
            CHECK(offers[i].weapon.base_id != "pistol");  // already held
            CHECK(offers[i].restricted);
            REQUIRE(run.rc.AdjustSupplies(1000, "test").ok());
            CHECK(run.rc.Purchase(i).code == ErrorCode::NotEligible);
            CHECK(run.rc.Snapshot(0.0).shop_offers[i].restricted);
            saw_restricted = true;
        }
    }
    CHECK(saw_restricted);
}

TEST_CASE("category damage applies to every gun of the category, never to melee", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"category_damage", {{"shotgun", 0.5}}}})));
    REQUIRE(h.rc.StartRun("alpha", 1, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    HitContext shot;
    shot.now = 1.0;
    shot.weapon_id = "shotgun";
    const OutgoingResult s = h.rc.ComputeOutgoingMultiplier(shot);
    CHECK(s.breakdown.build_bonus == Catch::Approx(0.5));
    CHECK(s.multiplier == Catch::Approx(1.5));
    HitContext pistol = shot;
    pistol.weapon_id = "pistol";
    CHECK(h.rc.ComputeOutgoingMultiplier(pistol).breakdown.build_bonus == 0.0);
    HitContext melee = shot;
    melee.is_melee = true;
    CHECK(h.rc.ComputeOutgoingMultiplier(melee).breakdown.build_bonus == 0.0);
}

TEST_CASE("a build's supplies multiplier scales supplies earned in arenas", "[builds]") {
    auto earned = [](const json& build) {
        test::Harness h(test::Load(BuildSources(build)));
        REQUIRE(h.rc.StartRun("alpha", 9, "alpha_build").ok());
        const std::int64_t start = h.rc.Snapshot(0.0).supplies;
        test::ClearArena(h.rc, 0.0);
        return h.rc.Snapshot(30.0).supplies - start;
    };
    const std::int64_t plain = earned(json{{"summary", "none"}});
    const std::int64_t doubled = earned(json{{"supplies_mult", 2.0}});
    REQUIRE(plain > 0);
    CHECK(doubled == plain * 2);
}

TEST_CASE("free shop rerolls renew the stock and reset per shop visit", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"free_shop_rerolls", 1}})));
    REQUIRE(h.rc.StartRun("alpha", 4, "alpha_build").ok());
    CHECK(h.rc.RerollShop().code == ErrorCode::WrongPhase);
    ToShop(h.rc, 0.0);
    CHECK(h.rc.Snapshot(0.0).free_shop_rerolls == 1);
    REQUIRE(h.rc.AdjustSupplies(1000, "test").ok());
    // Buy offer 0, reroll, buy offer 0 of the new stock: both purchases go through.
    REQUIRE(h.rc.Purchase(0).ok());
    REQUIRE(h.rc.RerollShop().ok());
    CHECK_FALSE(h.rc.ShopOffers().front().purchased);
    CHECK(h.rc.Snapshot(0.0).free_shop_rerolls == 0);
    CHECK(h.rc.Purchase(0).ok());
    CHECK(h.rc.RerollShop().code == ErrorCode::NotEligible);
    REQUIRE(h.rc.LeaveShop().ok());
    REQUIRE(h.rc.ChooseRoute(0).ok());
    ToShop(h.rc, 100.0);
    CHECK(h.rc.Snapshot(100.0).free_shop_rerolls == 1);

    test::Harness plain(test::Load(BuildSources(json{{"summary", "none"}})));
    REQUIRE(plain.rc.StartRun("alpha", 4, "alpha_build").ok());
    ToShop(plain.rc, 0.0);
    CHECK(plain.rc.RerollShop().code == ErrorCode::NotEligible);
}

TEST_CASE("uncapped weapons keep upgrading past the maximum level", "[builds]") {
    const auto content = test::LoadFixture();
    const EconomyConfig& eco = content->economy();
    const int max = eco.upgrades.max_level;
    REQUIRE_FALSE(UpgradeCost(eco, max).has_value());
    const auto at_max = UpgradeCost(eco, max, true);
    REQUIRE(at_max.has_value());
    const std::int64_t last = eco.upgrades.cost_to_next.back();
    CHECK(*at_max == last + last / 2 * (max - static_cast<int>(eco.upgrades.cost_to_next.size())));
    CHECK(*UpgradeCost(eco, max + 3, true) > *at_max);
    CHECK(UpgradeCost(eco, 1, true) == UpgradeCost(eco, 1));

    // A build with uncapped handguns: the held pistol at the maximum still gets an upgrade offer.
    test::Harness h(test::Load(BuildSources(json{{"uncapped_upgrades", {{"categories", {"handgun"}}}}})));
    REQUIRE(h.rc.StartRun("alpha", 2, "alpha_build").ok());
    WeaponInstance pistol = MakeStartingWeapon("pistol", "max_pistol");
    pistol.level = max;
    REQUIRE(h.rc.DevGrantWeapon(pistol).ok());
    ToShop(h.rc, 0.0);
    const auto offers = h.rc.ShopOffers();
    const auto up = std::find_if(offers.begin(), offers.end(), [](const ShopOffer& o) {
        return o.kind == ShopOfferKind::WeaponUpgrade && o.target_instance_id == "max_pistol";
    });
    REQUIRE(up != offers.end());
    REQUIRE(h.rc.AdjustSupplies(1000, "test").ok());
    REQUIRE(h.rc.Purchase(static_cast<std::size_t>(up - offers.begin())).ok());
    const auto& weapons = h.rc.state().weapons;
    const auto held = std::find_if(weapons.begin(), weapons.end(), [](const WeaponInstance& w) { return w.instance_id == "max_pistol"; });
    REQUIRE(held != weapons.end());
    CHECK(held->level == max + 1);
}

TEST_CASE("a build's starting weapon is rolled at its rarity", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"starting_weapon", "shotgun"}, {"starting_weapon_rarity", "legendary"}})));
    REQUIRE(h.rc.StartRun("alpha", 6, "alpha_build").ok());
    const auto& weapons = h.rc.state().weapons;
    const auto it = std::find_if(weapons.begin(), weapons.end(), [](const WeaponInstance& w) { return w.base_id == "shotgun"; });
    REQUIRE(it != weapons.end());
    CHECK(it->rarity == Rarity::Legendary);
    CHECK_FALSE(it->affixes.empty());
    // Queued for the adapter (placed after it trims the game's loadout), not placed at once.
    const auto grants = h.rc.ConsumeItemGrants();
    CHECK(std::any_of(grants.begin(), grants.end(), [](const ItemGrant& g) { return g.source == "build"; }));

    // The costume holds the same gun: the adapter swaps the starting weapon to it, and the
    // build's rolled instance stays (one shotgun, still legendary).
    REQUIRE(h.rc.SwapStartingWeapon("shotgun").ok());
    const auto count = std::count_if(h.rc.state().weapons.begin(), h.rc.state().weapons.end(),
                                     [](const WeaponInstance& w) { return w.base_id == "shotgun"; });
    CHECK(count == 1);
    const auto kept = std::find_if(h.rc.state().weapons.begin(), h.rc.state().weapons.end(),
                                   [](const WeaponInstance& w) { return w.base_id == "shotgun"; });
    REQUIRE(kept != h.rc.state().weapons.end());
    CHECK(kept->rarity == Rarity::Legendary);
}
