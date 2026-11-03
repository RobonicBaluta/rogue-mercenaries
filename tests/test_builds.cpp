// Skin builds (ROADMAP M8): weapon filters, category damage, supplies, free shop rerolls,
// uncapped upgrades and a build's starting weapon.

#include <algorithm>
#include <functional>
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

// Alpha's build skin, with more edits to the weapons and the economy.
ContentSources BuildSourcesWith(const json& build, const std::function<void(json& weapons, json& economy)>& more) {
    return test::EditFixture([&build, &more](json&, json& weapons, json& characters, json&, json& economy) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "shotgun") w["game_item"] = 0x0104;
        }
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_build"}, {"name", "Build"}, {"costume_index", 1}, {"build", build}},
        });
        more(weapons, economy);
    });
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
    CHECK(plain.rc.ShopRestockPrice() == -1);
}

TEST_CASE("the shop's paid new stock comes after the free ones and costs more each time", "[builds]") {
    auto sources = test::EditFixture([](auto&, auto&, auto&, auto&, json& economy) {
        economy["shop"]["restock_price"] = 15;
        economy["shop"]["restock_price_step"] = 10;
    });
    test::Harness h(test::Load(sources));
    REQUIRE(h.rc.StartRun("alpha", 4).ok());
    CHECK(h.rc.ShopRestockPrice() == -1);  // not in the shop
    ToShop(h.rc, 0.0);
    REQUIRE(h.rc.AdjustSupplies(100, "test").ok());
    const std::int64_t before = h.rc.Snapshot(0.0).supplies;
    CHECK(h.rc.ShopRestockPrice() == 15);
    REQUIRE(h.rc.Purchase(0).ok());
    const std::int64_t afterBuy = h.rc.Snapshot(0.0).supplies;
    REQUIRE(h.rc.RerollShop().ok());
    CHECK_FALSE(h.rc.ShopOffers().front().purchased);  // a fresh stock
    CHECK(h.rc.Snapshot(0.0).supplies == afterBuy - 15);
    CHECK(h.rc.Snapshot(0.0).restock_price == 25);
    REQUIRE(h.rc.RerollShop().ok());
    CHECK(h.rc.Snapshot(0.0).supplies == afterBuy - 40);
    REQUIRE(h.rc.AdjustSupplies(-(h.rc.Snapshot(0.0).supplies), "test").ok());
    CHECK(h.rc.RerollShop().code == ErrorCode::InsufficientFunds);
    CHECK(before > afterBuy);
    // A new shop visit starts at the first price again.
    REQUIRE(h.rc.LeaveShop().ok());
    REQUIRE(h.rc.ChooseRoute(0).ok());
    ToShop(h.rc, 100.0);
    CHECK(h.rc.ShopRestockPrice() == 15);
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

TEST_CASE("the second batch of build fields is validated", "[builds]") {
    CHECK(LoadErrors(json{{"name", "Brawler"}, {"icon", "fist"}, {"melee_damage", 2.0}}).empty());
    CHECK(HasError(LoadErrors(json{{"icon", "Fist!"}}), "not a valid name"));
    CHECK(HasError(LoadErrors(json{{"melee_kill_stats", {{"luck", 0.1}}}}), "unknown stat"));
    CHECK(HasError(LoadErrors(json{{"special", {{"kind", "fly"}, {"name", "Fly"}, {"cooldown_seconds", 5}}}}), "unknown special kind"));
    CHECK(HasError(LoadErrors(json{{"special", {{"kind", "dash"}, {"name", "Dash"}}}}), "neither a cooldown"));
    CHECK(HasError(LoadErrors(json{{"extra_weapons", {"shotgun"}}}), "not build_only"));
    CHECK(HasError(LoadErrors(json{{"drops_as", "0x0999"}}), "no loadouts.json equipment"));
    CHECK(LoadErrors(json{{"drops_as", 262}}).empty());  // the fixture's grenade
}

TEST_CASE("melee damage applies to melee hits, melee kills add run stats", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"melee_damage", 2.0}, {"melee_kill_stats", {{"damage", 0.1}}}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    HitContext melee;
    melee.now = 1.0;
    melee.is_melee = true;
    CHECK(h.rc.ComputeOutgoingMultiplier(melee).breakdown.build_bonus == Catch::Approx(2.0));
    HitContext shot;
    shot.now = 1.0;
    shot.weapon_id = "pistol";
    CHECK(h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus == 0.0);

    KillEvent bullet;
    bullet.now = 2.0;
    bullet.by_player = true;
    bullet.enemy_id = "e1";
    CHECK(h.rc.OnEnemyKilled(bullet).stat_gained.empty());
    KillEvent punch = bullet;
    punch.enemy_id = "e2";
    punch.was_melee = true;
    const KillOutcome out = h.rc.OnEnemyKilled(punch);
    CHECK(out.stat_gained == "damage");
    CHECK(h.rc.state().stats.damage == Catch::Approx(0.1));
    CHECK(h.rc.state().stats.gains == 1);
    CHECK(h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus == Catch::Approx(0.1));
    CHECK(h.rc.Snapshot(3.0).stats.damage == Catch::Approx(0.1));
}

TEST_CASE("a build's heals are stronger and a big heal starts its buffs", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"heal_mult", 1.5},
                                                 {"heal_buff_fraction", 0.6},
                                                 {"heal_invulnerable_seconds", 5.0},
                                                 {"heal_damage_bonus", 0.3},
                                                 {"heal_damage_seconds", 10.0}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    const HealOutcome herb = h.rc.OnPlayerHealed(HealContext{1.0, 200.0, 500.0, 1000.0});
    CHECK(herb.amount == Catch::Approx(300.0));
    CHECK_FALSE(herb.high_tier);
    CHECK_FALSE(h.rc.Invulnerable(1.5));

    const HealOutcome spray = h.rc.OnPlayerHealed(HealContext{2.0, 1000.0, 100.0, 1000.0});
    CHECK(spray.high_tier);
    CHECK(h.rc.Invulnerable(6.9));
    CHECK_FALSE(h.rc.Invulnerable(7.1));
    CHECK(h.rc.ComputeIncoming(IncomingContext{3.0, 200.0, 500.0, 1000.0}).multiplier == 0.0);
    CHECK(h.rc.ComputeIncoming(IncomingContext{8.0, 200.0, 500.0, 1000.0}).multiplier > 0.0);
    HitContext shot;
    shot.now = 5.0;
    CHECK(h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus == Catch::Approx(0.3));
    shot.now = 13.0;
    CHECK(h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus == 0.0);
    CHECK(h.rc.Snapshot(3.0).invulnerable_left == Catch::Approx(4.0));
}

TEST_CASE("keys drop from listed enemies and open chests; a key-exempt build needs none", "[builds]") {
    auto keys = [](json&, json& eco) {
        eco["keys"] = {{"enabled", true}, {"drop_chance", 1.0}, {"classes", {"uEm1"}}, {"max", 3}};
        eco["chests"] = {{"item_chance", 0.0}, {"weapon_chance", 0.0}};
    };
    test::Harness h(test::Load(BuildSourcesWith(json{{"summary", "none"}}, keys)));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    const auto locked = h.rc.OpenChest(1.0);
    REQUIRE(locked.ok());
    CHECK(locked->locked);
    CHECK(locked->supplies == 0);
    CHECK(h.rc.Snapshot(1.0).keys_needed);

    KillEvent dog;
    dog.now = 2.0;
    dog.by_player = true;
    dog.enemy_id = "d";
    dog.enemy_class = "uEm20";
    CHECK_FALSE(h.rc.OnEnemyKilled(dog).key_dropped);
    KillEvent majini = dog;
    majini.enemy_id = "m";
    majini.enemy_class = "uEm10";
    CHECK(h.rc.OnEnemyKilled(majini).key_dropped);
    CHECK(h.rc.state().keys == 1);
    const auto opened = h.rc.OpenChest(3.0);
    REQUIRE(opened.ok());
    CHECK_FALSE(opened->locked);
    CHECK(opened->key_used);
    CHECK(h.rc.state().keys == 0);

    test::Harness jill(test::Load(BuildSourcesWith(json{{"chest_keys_exempt", true}}, keys)));
    REQUIRE(jill.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(jill.rc.BeginArena(0.0).ok());
    const auto free = jill.rc.OpenChest(1.0);
    REQUIRE(free.ok());
    CHECK_FALSE(free->locked);
    CHECK_FALSE(free->key_used);
    CHECK_FALSE(jill.rc.Snapshot(1.0).keys_needed);
}

TEST_CASE("a build's drops_as item comes out of every chest", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"drops_as", 262}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    for (int i = 0; i < 5; ++i) {
        const auto chest = h.rc.OpenChest(1.0 + i);
        REQUIRE(chest.ok());
        CHECK(chest->item_id == "grenade");
        CHECK(chest->weapon_id.empty());
    }
}

TEST_CASE("build-only weapons are offered only to the builds that list them", "[builds]") {
    auto gun = [](json& weapons, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "shotgun") w["build_only"] = true;
        }
    };
    const auto content = test::Load(BuildSourcesWith(json{{"extra_weapons", {"shotgun"}}}, gun));
    const CharacterDef* alpha = content->FindCharacter("alpha");
    REQUIRE(alpha != nullptr);
    const CharacterDef with = ApplySkin(*alpha, *FindSkin(*alpha, "alpha_build"));
    const CharacterDef without = ApplySkin(*alpha, *FindSkin(*alpha, "alpha_base"));
    auto offered = [&content](const CharacterDef& character) {
        OfferContext ctx;
        ctx.content = content.get();
        ctx.character = &character;
        std::vector<std::string> ids;
        for (const WeaponBaseDef* w : OfferableWeapons(ctx)) ids.push_back(w->id);
        return ids;
    };
    const auto a = offered(with);
    const auto b = offered(without);
    CHECK(std::count(a.begin(), a.end(), "shotgun") == 1);
    CHECK(std::count(b.begin(), b.end(), "shotgun") == 0);
}

TEST_CASE("a special keeps its cooldown, its arena limit and a dash's invulnerability", "[builds]") {
    const json dash = {{"special", {{"kind", "dash"}, {"name", "Dash"}, {"cooldown_seconds", 3.0}, {"duration_seconds", 0.5}}}};
    test::Harness h(test::Load(BuildSources(dash)));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    CHECK(h.rc.UseSpecial(0.0).code == ErrorCode::WrongPhase);
    REQUIRE(h.rc.BeginArena(0.0).ok());
    CHECK(h.rc.Special(1.0).ready);
    REQUIRE(h.rc.UseSpecial(1.0).ok());
    CHECK(h.rc.Invulnerable(1.4));
    CHECK_FALSE(h.rc.Invulnerable(1.6));
    CHECK(h.rc.UseSpecial(2.0).code == ErrorCode::NotEligible);
    CHECK(h.rc.Special(2.0).cooldown_left == Catch::Approx(2.0));
    CHECK_FALSE(h.rc.Special(2.0).ready);
    CHECK(h.rc.UseSpecial(4.1).ok());
    // A stronger move recovers longer, and the HUD's total follows it.
    CHECK(h.rc.UseSpecial(7.2, 2.0).ok());
    CHECK(h.rc.Special(8.2).cooldown_total == Catch::Approx(6.0));
    CHECK(h.rc.Special(8.2).cooldown_left == Catch::Approx(5.0));
    CHECK(h.rc.UseSpecial(13.1).code == ErrorCode::NotEligible);
    CHECK(h.rc.UseSpecial(13.3).ok());
    CHECK(h.rc.Special(13.3).cooldown_total == Catch::Approx(3.0));
    CHECK(h.rc.UseSpecial(20.0, 0.0).code == ErrorCode::InvalidArgument);

    // A second special has its own button and cooldown.
    const json two = {{"special", {{"kind", "dash"}, {"name", "Dash"}, {"cooldown_seconds", 3.0}}},
                      {"special_2", {{"kind", "burst"}, {"name", "Burst"}, {"cooldown_seconds", 6.0}, {"duration_seconds", 0.4}}}};
    test::Harness w(test::Load(BuildSources(two)));
    REQUIRE(w.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(w.rc.BeginArena(0.0).ok());
    CHECK(w.rc.Special(1.0, 1).kind == "burst");
    REQUIRE(w.rc.UseSpecial(1.0).ok());
    CHECK(w.rc.Special(1.5, 1).ready);  // the dash's cooldown leaves the burst alone
    REQUIRE(w.rc.UseSpecial(1.5, 1.0, 1).ok());
    CHECK(w.rc.Invulnerable(1.8));
    CHECK(w.rc.Special(4.5).ready);
    CHECK_FALSE(w.rc.Special(4.5, 1).ready);
    CHECK(w.rc.Special(4.5, 1).cooldown_left == Catch::Approx(3.0));
    CHECK(w.rc.UseSpecial(4.5, 1.0, 2).code == ErrorCode::InvalidArgument);
    CHECK(w.rc.Special(4.5, 2).kind.empty());

    const json backup = {{"special", {{"kind", "backup"}, {"name", "Backup"}, {"every_arenas", 2}}}};
    test::Harness josh(test::Load(BuildSources(backup)));
    REQUIRE(josh.rc.StartRun("alpha", 5, "alpha_build").ok());
    REQUIRE(josh.rc.BeginArena(0.0).ok());
    REQUIRE(josh.rc.UseSpecial(1.0).ok());
    CHECK(josh.rc.UseSpecial(2.0).code == ErrorCode::NotEligible);
    CHECK(josh.rc.Special(2.0).arenas_left == 2);
    double t = 0.0;
    for (int arena = 2; arena <= 3; ++arena) {
        test::KillN(josh.rc, josh.rc.RemainingEnemies(), t + 1.0);
        REQUIRE(josh.rc.CompleteArena(t + 30.0).ok());
        REQUIRE(josh.rc.ContinueToRewards().ok());
        REQUIRE(josh.rc.ChooseReward(0).ok());
        REQUIRE(josh.rc.LeaveShop().ok());
        REQUIRE(josh.rc.ChooseRoute(0).ok());
        t += 100.0;
        REQUIRE(josh.rc.BeginArena(t).ok());
        if (arena == 2) CHECK(josh.rc.UseSpecial(t + 1.0).code == ErrorCode::NotEligible);
    }
    CHECK(josh.rc.Special(t + 1.0).ready);  // arena 3: two arenas after its use
    CHECK(josh.rc.UseSpecial(t + 1.0).ok());
}
