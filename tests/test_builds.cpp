// Skin builds (ROADMAP M8): weapon filters, category damage, supplies, free shop rerolls,
// uncapped upgrades and a build's starting weapon.

#include <algorithm>
#include <functional>
#include <set>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "rm/rewards.h"
#include "rm/unlock_all.h"
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
    CHECK(HasError(LoadErrors(json{{"melee_kill_caps", {{"luck", 0.1}}}}), "unknown stat"));
    CHECK(HasError(LoadErrors(json{{"special", {{"kind", "fly"}, {"name", "Fly"}, {"cooldown_seconds", 5}}}}), "unknown special kind"));
    CHECK(HasError(LoadErrors(json{{"special", {{"kind", "dash"}, {"name", "Dash"}}}}), "neither a cooldown"));
    CHECK(HasError(LoadErrors(json{{"extra_weapons", {"shotgun"}}}), "not build_only"));
    CHECK(HasError(LoadErrors(json{{"drops_as", "0x0999"}}), "no loadouts.json equipment"));
    CHECK(LoadErrors(json{{"drops_as", 262}}).empty());  // the fixture's grenade
    CHECK(LoadErrors(json{{"extra_chests", 3}}).empty());
    CHECK_FALSE(LoadErrors(json{{"extra_chests", 9}}).empty());
}

TEST_CASE("a build's extra chests are placed in every arena it plays", "[builds]") {
    // A map with eight chest positions; the economy places 3 per arena (its default).
    auto run = [](const char* skin) {
        ContentSources src = BuildSources(json{{"name", "Locksmith"}, {"chest_keys_exempt", true}, {"extra_chests", 2}});
        src.maps = R"({"schema_version": 1, "maps": [{"id": "m", "name": "M", "stage": 4,
          "points": [[0, 0, 0], [1000, 0, 0]], "roster": [{"class": "uEm10"}],
          "chests": [[1, 0, 0], [2, 0, 0], [3, 0, 0], [4, 0, 0], [5, 0, 0], [6, 0, 0], [7, 0, 0], [8, 0, 0]]}]})";
        test::Harness h(test::Load(src));
        h.rc.SetMapStage(4);
        REQUIRE(h.rc.StartRun("alpha", 3, skin).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        REQUIRE(h.rc.director() != nullptr);
        return h.rc.director()->chests().size();
    };
    CHECK(run("alpha_base") == 3);
    CHECK(run("alpha_build") == 5);
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

TEST_CASE("melee kills raise run stats up to their caps, and the new stats work", "[builds]") {
    // Five melee kills (the fixture arena's budget) for each build.
    auto meleeKills = [](test::Harness& h) {
        REQUIRE(h.rc.StartRun("alpha", 4, "alpha_build").ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        for (int i = 0; h.rc.RemainingEnemies() > 0 && i < 20; ++i) {
            KillEvent k;
            k.now = 1.0 + i;
            k.by_player = true;
            k.was_melee = true;
            k.enemy_id = "melee" + std::to_string(i);
            h.rc.OnEnemyKilled(k);
        }
    };
    // Speed alone: 0.1, 0.2, then 0.25 (its cap), then no more gains.
    test::Harness speed(test::Load(BuildSources(json{{"melee_kill_stats", {{"move_speed", 0.1}}},
                                                     {"melee_kill_caps", {{"move_speed", 0.25}}}})));
    meleeKills(speed);
    CHECK(speed.rc.state().stats.move_speed == Catch::Approx(0.25));
    CHECK(speed.rc.state().stats.gains == 3);
    // Healing: +20 percent on every heal.
    test::Harness heal(test::Load(BuildSources(json{{"melee_kill_stats", {{"heal", 0.2}}}, {"melee_kill_caps", {{"heal", 0.2}}}})));
    meleeKills(heal);
    CHECK(heal.rc.state().stats.heal == Catch::Approx(0.2));
    HealContext ctx;
    ctx.now = 50.0;
    ctx.amount = 100.0;
    ctx.hp = 100.0;
    ctx.hp_max = 1000.0;
    CHECK(heal.rc.OnPlayerHealed(ctx).amount == Catch::Approx(120.0));
    // Supplies: every gain stays within its cap.
    test::Harness supplies(test::Load(BuildSources(json{{"melee_kill_stats", {{"supplies", 0.25}}},
                                                        {"melee_kill_caps", {{"supplies", 0.5}}}})));
    meleeKills(supplies);
    CHECK(supplies.rc.state().stats.supplies == Catch::Approx(0.5));
    // A stat that reached its cap is never rolled again, even when its steps add up to a hair
    // under it (0.15 three times is 0.44999999999999996): a cap of 0.45 takes three gains, not a
    // fourth of nothing (Excella's healing, 0.05 ten times, rolled once more for +0.00, user
    // playtest 2026-11-07).
    test::Harness exact(test::Load(BuildSources(json{{"melee_kill_stats", {{"defense", 0.15}}},
                                                     {"melee_kill_caps", {{"defense", 0.45}}}})));
    meleeKills(exact);
    CHECK(exact.rc.state().stats.defense == Catch::Approx(0.45));
    CHECK(exact.rc.state().stats.gains == 3);
}

namespace {

// Alpha's build skin (`build`) plus a free style perk for Alpha (`perk`, its id "trait").
ContentSources PerkBuildSources(const json& build, const json& perk) {
    return test::EditFixture([&build, &perk](json& perks, json& weapons, json& characters, json&, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "shotgun") w["game_item"] = 0x0104;
        }
        json p = perk;
        p["id"] = "trait";
        p["name"] = "Trait";
        p["category"] = "style";
        p["rarity"] = "common";
        p["characters"] = json::array({"alpha"});
        if (!p.contains("effects")) p["effects"] = json::array();
        perks["perks"].push_back(p);
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_build"}, {"name", "Build"}, {"costume_index", 1}, {"build", build}},
        });
    });
}

}  // namespace

TEST_CASE("a style perk's build adds to the skin's (a rule with its cost)", "[builds]") {
    const json build = {{"category_damage", {{"handgun", 0.1}}}, {"max_health_mult", 1.5}, {"drops_as", 262}};
    const json perk = {{"build", {{"melee_damage", 1.5}, {"category_damage", {{"handgun", -0.25}}}, {"max_health_mult", 1.2},
                                   {"heal_mult", 0.75}, {"supplies_mult", 0.8}, {"stack_mult", {{"0x0204", 2}}},
                                   {"never_offered", {{"categories", {"shotgun"}}}}, {"drops_as_held_ammo", true},
                                   {"pickup_as", {{"0x0301", "0x0309"}}}, {"break_grabs", true}}}};
    const auto content = test::Load(PerkBuildSources(build, perk));
    int weapons_seen = 0;
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.EquipPerk("alpha", "trait").ok());
        REQUIRE(h.rc.StartRun("alpha", seed, "alpha_build").ok());
        const SkinBuild& b = h.rc.state().character.build;
        CHECK(b.melee_damage == Catch::Approx(1.5));
        CHECK(b.category_damage.at("handgun") == Catch::Approx(-0.15));
        CHECK(b.max_health_mult == Catch::Approx(1.8));
        CHECK(b.heal_mult == Catch::Approx(0.75));
        CHECK(b.supplies_mult == Catch::Approx(0.8));
        REQUIRE(b.stack_mult.size() == 1);
        CHECK(b.stack_mult[0] == std::pair<std::int64_t, double>{0x0204, 2.0});
        CHECK_FALSE(b.drops_as_held_ammo);  // the skin's own drops (mines) win
        REQUIRE(b.pickup_as.size() == 1);
        CHECK(b.pickup_as[0] == std::pair<std::int64_t, std::int64_t>{0x0301, 0x0309});
        CHECK(b.break_grabs);
        // Never offered: rewards and the shop leave the shotgun out.
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        for (const RewardChoice& c : h.rc.RewardOffer()) {
            if (c.kind != RewardKind::Weapon) continue;
            CHECK(c.weapon.base_id != "shotgun");
            ++weapons_seen;
        }
        REQUIRE(h.rc.ChooseReward(0).ok());
        for (const ShopOffer& o : h.rc.ShopOffers()) {
            if (o.kind == ShopOfferKind::Weapon) CHECK(o.weapon.base_id != "shotgun");
        }
    }
    CHECK(weapons_seen == 0);  // the rifle is locked and the pistol held: nothing else to offer
    // Without the perk the skin's build is as it was, and the shotgun is offered.
    test::Harness plain(content);
    REQUIRE(plain.rc.StartRun("alpha", 1, "alpha_build").ok());
    CHECK(plain.rc.state().character.build.max_health_mult == Catch::Approx(1.5));
    CHECK(plain.rc.state().character.build.melee_damage == Catch::Approx(0.0));
    CHECK_FALSE(plain.rc.state().character.build.break_grabs);
    int shotguns = 0;
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.StartRun("alpha", seed, "alpha_build").ok());
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        for (const RewardChoice& c : h.rc.RewardOffer()) shotguns += c.kind == RewardKind::Weapon && c.weapon.base_id == "shotgun";
    }
    CHECK(shotguns > 0);
}

TEST_CASE("a perk's build is limited to what adds into a run's", "[builds]") {
    const auto errors = [](const json& perk) {
        const auto loaded = Content::LoadFromStrings(PerkBuildSources(json{{"summary", "ok"}}, perk));
        for (const std::string& e : loaded.errors) UNSCOPED_INFO("got: " << e);
        return loaded.errors;
    };
    CHECK(errors(json{{"build", {{"melee_damage", 1.0}}}}).empty());
    CHECK(HasError(errors(json{{"build", {{"special", {{"kind", "dash"}}}}}}), "special"));
    CHECK(HasError(errors(json{{"build", {{"never_offered", {{"ids", {"bazooka"}}}}}}}), "unknown weapon"));
    CHECK(HasError(errors(json{{"build", {{"stack_mult", {{"0x0204", 0.5}}}}}}), "stack_mult"));
    CHECK(HasError(errors(json{{"build", {{"pickup_as", {{"0x0301", "0x0301"}}}}}}), "pickup_as"));
    CHECK(HasError(errors(json{{"effects", json::array()}}), "non-empty"));
}

TEST_CASE("a melee-only kill effect pays for melee kills alone", "[builds]") {
    const json perk = {{"effects", {{{"kind", "supplies_on_kill"}, {"amount", 15}, {"melee_only", true}}}}};
    const auto content = test::Load(PerkBuildSources(json{{"summary", "ok"}}, perk));
    test::Harness h(content);
    REQUIRE(h.rc.EquipPerk("alpha", "trait").ok());
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    auto kill = [&](std::optional<bool> melee, double t) {
        KillEvent k;
        k.now = t;
        k.by_player = true;
        k.weapon_id = std::string("pistol");
        k.was_melee = melee;
        return h.rc.OnEnemyKilled(k).supplies_awarded;
    };
    const std::int64_t shot = kill(false, 1.0);
    const std::int64_t unknown = kill(std::nullopt, 2.0);
    const std::int64_t melee = kill(true, 3.0);
    CHECK(shot == unknown);
    CHECK(melee == shot + 15);
}

TEST_CASE("a launcher's rounds are offered only while the run holds the launcher", "[builds]") {
    // Rounds for the shotgun only: the run holds the pistol, so no offer has them, and the
    // starting loadout never rolls them.
    ContentSources src = test::FixtureSources();
    json loadouts = json::parse(src.loadouts);
    loadouts["equipment"].push_back(json{{"id", "rounds"}, {"name", "Rounds"}, {"cost", 1}, {"max", 3}, {"game_item", 0x0206},
                                         {"price", 1}, {"bundle", 2}, {"for_weapons", {"shotgun"}}});
    src.loadouts = loadouts.dump();
    const auto content = test::Load(src);
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.StartRun("beta", seed).ok());
        for (const LoadoutItem& item : h.rc.state().loadout.items) CHECK(item.id != "rounds");
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        for (const RewardChoice& c : h.rc.RewardOffer()) CHECK(c.item_id != "rounds");
    }
    json bad = loadouts;
    bad["equipment"].back()["for_weapons"] = json::array({"bazooka"});
    src.loadouts = bad.dump();
    const auto loaded = Content::LoadFromStrings(src);
    CHECK(HasError(loaded.errors, "for_weapons references unknown weapon"));
}

TEST_CASE("a build's shop weapon is in every shop, with no unlock, until the run holds it", "[builds]") {
    // Heavy Metal's Gatling (user playtest 2026-11-04: never seen without its CREDITS unlock).
    // The fixture's rifle needs an unlock: the build's shop sells it anyway, at the build's price.
    const auto content = test::Load(BuildSources(json{{"shop_weapon", "rifle"}, {"shop_weapon_price", 70}}));
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.StartRun("alpha", seed, "alpha_build").ok());
        ToShop(h.rc, 0.0);
        int rifles = 0;
        std::size_t at = 0;
        for (std::size_t i = 0; i < h.rc.ShopOffers().size(); ++i) {
            const ShopOffer& o = h.rc.ShopOffers()[i];
            if (o.kind != ShopOfferKind::Weapon || o.weapon.base_id != "rifle") continue;
            ++rifles;
            at = i;
            CHECK(o.price == 70);
        }
        REQUIRE(rifles == 1);
        // Bought, it is not offered again.
        REQUIRE(h.rc.AdjustSupplies(200, "test").ok());
        REQUIRE(h.rc.Purchase(at).ok());
        REQUIRE(h.rc.LeaveShop().ok());
        REQUIRE(h.rc.ChooseRoute(0).ok());
        ToShop(h.rc, 200.0);
        for (const ShopOffer& o : h.rc.ShopOffers()) CHECK_FALSE((o.kind == ShopOfferKind::Weapon && o.weapon.base_id == "rifle"));
    }
    CHECK(Content::LoadFromStrings(BuildSources(json{{"shop_weapon", "bazooka"}})).errors.size() > 0);
}

namespace {

// The fewest enemies arena `arena` (1-based) of a run of `length` can field: none on a boss arena (a
// ladder length's last, its own or a shorter one's), the starting route on the first, else the
// smallest budget of the lower tier the run map offers there (routes.cpp: layer d's tiers are the
// d-th and (d+1)-th lowest), with the ladder's growth.
int FewestEnemies(const Content& c, const EconomyConfig::Ladder::Length& length, int arena) {
    const EconomyConfig& eco = c.economy();
    for (const auto* tier : eco.ladder.BossTiers(length.id)) {
        if (tier->arenas == arena) return 0;
    }
    const auto playable = [](const RouteDef& r) { return r.enabled && r.enemy_budget > 0; };
    int budget = 0;
    if (arena == 1) {
        budget = c.FindRoute(eco.run.starting_route)->enemy_budget;
    } else {
        std::set<int> tierSet;
        for (const RouteDef& r : c.routes()) {
            if (playable(r)) tierSet.insert(r.difficulty_tier);
        }
        const std::vector<int> tiers(tierSet.begin(), tierSet.end());
        const int low = tiers[static_cast<std::size_t>(std::min<int>(arena - 2, static_cast<int>(tiers.size()) - 1))];
        budget = 1 << 30;
        for (const RouteDef& r : c.routes()) {
            if (playable(r) && r.difficulty_tier == low) budget = std::min(budget, r.enemy_budget);
        }
    }
    return budget + eco.ladder.budget_per_arena * eco.ladder.Steps(arena);
}

const SkinBuild* ExcellaAmbition(const Content& c) {
    const CharacterDef* excella = c.FindCharacter("excella");
    if (excella == nullptr) return nullptr;
    for (const SkinDef& s : excella->skins) {
        if (s.id == "excella_dress") return &s.build;
    }
    return nullptr;
}

}  // namespace

// Excella's pace (the user, 2026-11-11: "excella is too powerful"): killing every enemy by melee on the
// smallest arenas a run can draw, every stat is full by mid arena 3 of a short run, the start of arena
// 7 of a long one and mid arena 10 of a marathon. Her gains for each length add up to those kills.
TEST_CASE("Excella's stats fill by mid arena 3, the start of arena 7 and mid arena 10", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const Content& c = *r.content;
    const SkinBuild* ambition = ExcellaAmbition(c);
    REQUIRE(ambition != nullptr);
    const SkinBuild& build = *ambition;
    const std::vector<std::pair<std::string, double>> fullAt = {{"short", 2.5}, {"long", 6.0}, {"marathon", 9.5}};
    for (const auto& [id, arenas] : fullAt) {
        INFO(id);
        const auto* length = c.economy().ladder.Find(id);
        REQUIRE(length != nullptr);
        const int whole = static_cast<int>(arenas);
        int kills = 0;
        for (int a = 1; a <= whole; ++a) kills += FewestEnemies(c, *length, a);
        if (arenas > whole) kills += FewestEnemies(c, *length, whole + 1) / 2;
        const auto pace = build.melee_kill_gains_by_length.find(id);
        REQUIRE(pace != build.melee_kill_gains_by_length.end());
        int gains = 0;
        for (const auto& [stat, n] : pace->second) gains += n;
        CHECK(gains == kills);
        // Every stat that rises has its gains here, so none fills later or never.
        for (const auto& [stat, step] : build.melee_kill_stats) CHECK(pace->second.count(stat) == 1);
    }
    // The caps: 10 percent under those of 2026-11-07.
    CHECK(build.melee_kill_caps.at("damage") == Catch::Approx(0.9));
    CHECK(build.melee_kill_caps.at("max_health") == Catch::Approx(0.9));
    CHECK(build.melee_kill_caps.at("crit") == Catch::Approx(0.27));
}

// Sheva in Hound Wolf Squad gear (Scorched Earth) starts with a rare flamethrower and a pistol in
// hand (2026-11-11, the user: "get sheva's mod skin a better flamethrower, make it be rare from the
// beggining", "she also starts with an M3, make her start with a pistol").
TEST_CASE("Hound Wolf Sheva starts with a rare flamethrower and a pistol in hand", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    Profile open;
    UnlockEverything(open, *r.content);
    test::Harness h(r.content, open);
    h.rc.SetInstalledModSkins({"sheva_hound_wolf"});
    REQUIRE(h.rc.StartRun("sheva", 3, "sheva_hound_wolf").ok());
    const auto& weapons = h.rc.state().weapons;
    const auto it = std::find_if(weapons.begin(), weapons.end(), [](const WeaponInstance& w) { return w.base_id == "flamethrower"; });
    REQUIRE(it != weapons.end());
    CHECK(it->rarity == Rarity::Rare);
    CHECK(it->affixes.size() == 2);  // the rare tier's affix count
    // The adapter turns her costume's M3 in hand into its handgun; the run's starting gun is a handgun.
    CHECK(h.rc.state().character.build.pistol_in_hand);
    const WeaponBaseDef* start = r.content->FindWeapon(h.rc.state().loadout.weapon_id);
    REQUIRE(start != nullptr);
    CHECK(start->category == WeaponCategory::Handgun);
    // Fairy Tale Sheva, whose M3 it is, keeps hers.
    test::Harness fairy(r.content, open);
    REQUIRE(fairy.rc.StartRun("sheva", 3, "sheva_fairy_tale").ok());
    CHECK_FALSE(fairy.rc.state().character.build.pistol_in_hand);
}

// Wesker in his RE4 clothes (2026-11-17): a mod skin in S.T.A.R.S. Wesker's slot, his build Mastermind.
TEST_CASE("RE4 Wesker wears S.T.A.R.S. Wesker's slot and starts with a rare Lightning Hawk", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const CharacterDef* wesker = r.content->FindCharacter("wesker");
    REQUIRE(wesker != nullptr);
    const SkinDef* re4 = nullptr;
    const SkinDef* stars = nullptr;
    for (const SkinDef& s : wesker->skins) {
        if (s.id == "wesker_re4") re4 = &s;
        if (s.id == "wesker_stars") stars = &s;
    }
    REQUIRE(re4 != nullptr);
    REQUIRE(stars != nullptr);
    REQUIRE(re4->mod.has_value());
    CHECK(re4->costume_index == stars->costume_index);
    CHECK(re4->build.name == "Mastermind");
    Profile open;
    UnlockEverything(open, *r.content);
    test::Harness h(r.content, open);
    h.rc.SetInstalledModSkins({"wesker_re4"});
    REQUIRE(h.rc.StartRun("wesker", 5, "wesker_re4").ok());
    const auto& weapons = h.rc.state().weapons;
    const auto hawk = std::find_if(weapons.begin(), weapons.end(), [](const WeaponInstance& w) { return w.base_id == "l_hawk"; });
    REQUIRE(hawk != weapons.end());
    CHECK(hawk->rarity == Rarity::Rare);
    // Without the mod he cannot wear it.
    test::Harness without(r.content, open);
    CHECK_FALSE(without.rc.StartRun("wesker", 5, "wesker_re4").ok());
}

// Ada Wong (2026-11-17): a character of her own in Sheva's Default, Clubbin and Tribal outfits.
TEST_CASE("Ada Wong wears Sheva's three outfits, each skin with its build", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const CharacterDef* ada = r.content->FindCharacter("ada");
    const CharacterDef* sheva = r.content->FindCharacter("sheva");
    REQUIRE(ada != nullptr);
    REQUIRE(sheva != nullptr);
    CHECK(ada->starting_passive == "ada_butterfly");
    // Each skin on the slot whose archive its mod replaces: the bundle's Cos1 is Assignment, its
    // Cos2 the red dress, and slot 4 (Clubbin') loads Cos2, slot 5 (Tribal) Cos1 (seen in game
    // 2026-11-21: Assignment on slot 4 was the game's Clubbin' Sheva).
    const std::vector<std::pair<std::string, std::string>> slots = {
        {"ada_agent", "sheva_bsaa"}, {"ada_assignment", "sheva_tribal"}, {"ada_red_dress", "sheva_clubbin"}};
    for (const auto& [mine, base] : slots) {
        INFO(mine);
        const SkinDef* a = FindSkin(*ada, mine);
        const SkinDef* b = FindSkin(*sheva, base);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(a->mod.has_value());
        CHECK(a->costume_index == b->costume_index);
        CHECK_FALSE(a->build.name.empty());
    }
    Profile open;
    UnlockEverything(open, *r.content);
    const auto has = [](const RunState& s, const char* base) {
        return std::find_if(s.weapons.begin(), s.weapons.end(), [base](const WeaponInstance& w) { return w.base_id == base; });
    };
    {
        test::Harness h(r.content, open);
        h.rc.SetInstalledModSkins({"ada_agent"});
        REQUIRE(h.rc.StartRun("ada", 7, "ada_agent").ok());
        CHECK(has(h.rc.state(), "px4") != h.rc.state().weapons.end());  // her own gun, a gun of its own
        CHECK(h.rc.state().character.build.pistol_in_hand);
    }
    {
        test::Harness h(r.content, open);
        h.rc.SetInstalledModSkins({"ada_red_dress"});
        REQUIRE(h.rc.StartRun("ada", 7, "ada_red_dress").ok());
        const auto rifle = has(h.rc.state(), "psg1");  // RE4's semi-auto rifle, no bow (2026-11-23)
        REQUIRE(rifle != h.rc.state().weapons.end());
        CHECK(rifle->rarity == Rarity::Rare);
        CHECK(has(h.rc.state(), "longbow") == h.rc.state().weapons.end());
        // Her pickups stay what they are; a rifle headshot gives its round back (2026-11-24).
        CHECK_FALSE(h.rc.state().character.build.drops_as_held_ammo);
        CHECK(h.rc.state().character.build.headshot_refund.at("rifle") == Catch::Approx(1.0));
    }
    test::Harness without(r.content, open);
    CHECK_FALSE(without.rc.StartRun("ada", 7, "ada_agent").ok());
}

TEST_CASE("a run length's pace fills every stat on its last gain, never before", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const Content& c = *r.content;
    const SkinBuild* ambition = ExcellaAmbition(c);
    REQUIRE(ambition != nullptr);
    const SkinBuild& build = *ambition;
    Profile open;
    UnlockEverything(open, c);
    for (const auto& [id, pace] : build.melee_kill_gains_by_length) {
        INFO(id);
        int total = 0;
        for (const auto& [stat, n] : pace) total += n;
        test::Harness h(r.content, open);
        REQUIRE(h.rc.StartRun("excella", 11, "excella_dress", id).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        const auto allFull = [&h, &build] {
            for (const auto& [stat, cap] : build.melee_kill_caps) {
                if (h.rc.state().stats.Get(stat) < cap - 1e-9) return false;
            }
            return true;
        };
        for (int i = 0; i <= total; ++i) {
            if (i == total - 1) CHECK_FALSE(allFull());
            KillEvent k;
            k.now = 1.0 + i * 0.01;
            k.by_player = true;
            k.was_melee = true;
            k.enemy_id = "melee" + std::to_string(i);
            h.rc.OnEnemyKilled(k);
        }
        CHECK(allFull());
        CHECK(h.rc.state().stats.gains == total);  // the kill after the last gain raised nothing
        for (const auto& [stat, cap] : build.melee_kill_caps) CHECK(h.rc.state().stats.Get(stat) == Catch::Approx(cap));
    }
    // A pace's stat needs a cap, and its gains are whole numbers.
    CHECK(HasError(LoadErrors(json{{"melee_kill_stats", {{"heal", 0.1}}}, {"melee_kill_gains_by_length", {{"short", {{"heal", 4}}}}}}),
                   "needs a cap"));
    CHECK(HasError(LoadErrors(json{{"melee_kill_stats", {{"heal", 0.1}}},
                                   {"melee_kill_caps", {{"heal", 0.4}}},
                                   {"melee_kill_gains_by_length", {{"short", {{"heal", 2.5}}}}}}),
                   "whole number"));
}

// The tester (the user, 2026-11-22: "can you give me a character just for me, who has infinite rocket
// launcher and infinite machinegun to test bosses and stuff myself"): more starting guns, guns that
// never run out, and a character only an install that asks for it lists.
TEST_CASE("a build's more starting weapons are handed over and its infinite guns are named", "[builds]") {
    CHECK(LoadErrors(json{{"more_starting_weapons", {"shotgun", "rifle"}}, {"infinite_ammo", {{"ids", {"rifle"}}}}}).empty());
    CHECK(HasError(LoadErrors(json{{"more_starting_weapons", {"bazooka"}}}), "more_starting_weapons references unknown weapon"));
    CHECK(HasError(LoadErrors(json{{"infinite_ammo", {{"ids", {"bazooka"}}}}}), "references unknown weapon"));
    CHECK(HasError(LoadErrors(json{{"more_starting_weapons", {"rifle"}}, {"usable_weapons", {{"categories", {"handgun", "shotgun"}}}}}),
                   "not one of the build's usable weapons"));
    auto buildOnly = [](json& weapons, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "rifle") w["build_only"] = true;
        }
    };
    const auto missing = Content::LoadFromStrings(BuildSourcesWith(json{{"more_starting_weapons", {"rifle"}}}, buildOnly));
    CHECK(HasError(missing.errors, "build_only and not in the build's extra_weapons"));
    const auto listed = Content::LoadFromStrings(
        BuildSourcesWith(json{{"more_starting_weapons", {"rifle"}}, {"extra_weapons", {"rifle"}}}, buildOnly));
    for (const std::string& e : listed.errors) UNSCOPED_INFO("got: " << e);
    CHECK(listed.errors.empty());

    test::Harness h(test::Load(BuildSources(json{{"more_starting_weapons", {"shotgun", "rifle"}}, {"infinite_ammo", {{"ids", {"rifle"}}}}})));
    REQUIRE(h.rc.StartRun("alpha", 6, "alpha_build").ok());
    const auto& weapons = h.rc.state().weapons;
    auto count = [&weapons](const char* id) { return std::count_if(weapons.begin(), weapons.end(), [id](const WeaponInstance& w) { return w.base_id == id; }); };
    CHECK(count("rifle") == 1);
    CHECK(count("shotgun") == 1);  // once, whether the roll's own gun was the shotgun or not
    if (h.rc.state().loadout.weapon_id != "shotgun") {
        // The shotgun has a game item: queued for the adapter like a build's starting weapon.
        const auto grants = h.rc.ConsumeItemGrants();
        CHECK(std::any_of(grants.begin(), grants.end(), [](const ItemGrant& g) { return g.source == "build" && g.game_item == 0x0104; }));
    }
    const SkinBuild& build = h.rc.state().character.build;
    CHECK(build.infinite_ammo.Allows("rifle", "rifle"));
    CHECK_FALSE(build.infinite_ammo.Allows("shotgun", "shotgun"));
    // An empty filter allows every weapon, so an adapter asks whether it lists any first.
    CHECK(SkinBuild{}.infinite_ammo.Empty());
}

TEST_CASE("a tester_only character is listed and playable only when the install asks for it", "[builds]") {
    const auto content = test::Load(test::EditFixture([](json&, json&, json& characters, json&, json&) {
        characters["characters"].push_back(json{{"id", "tester"}, {"name", "Tester"}, {"tester_only", true}, {"starting_weapons", {"pistol"}}});
    }));
    auto listed = [](const RunController& rc) {
        const auto all = rc.Characters();
        return std::any_of(all.begin(), all.end(), [](const CharacterAvailability& a) { return a.id == "tester"; });
    };
    test::Harness off(content);
    CHECK_FALSE(listed(off.rc));
    CHECK_FALSE(off.rc.StartRun("tester", 1).ok());
    test::Harness on(content);
    on.rc.SetTesterCharacters(true);
    CHECK(listed(on.rc));
    CHECK(on.rc.StartRun("tester", 1).ok());
    // A tester's character is never the one a player starts with.
    const auto alone = Content::LoadFromStrings(test::EditFixture([](json&, json&, json& characters, json&, json&) {
        characters["characters"] = json::array({json{{"id", "tester"}, {"name", "Tester"}, {"tester_only", true}, {"starting_weapons", {"pistol"}}}});
    }));
    CHECK(HasError(alone.errors, "no enabled character is unlocked by default"));
}

TEST_CASE("the shipped tester starts with the flamethrower only and picks its gun's element", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const CharacterDef* tester = r.content->FindCharacter("tester");
    REQUIRE(tester != nullptr);
    CHECK(tester->tester_only);
    test::Harness hidden(r.content);
    CHECK_FALSE(hidden.rc.StartRun("tester", 2).ok());
    test::Harness h(r.content);
    h.rc.SetTesterCharacters(true);
    REQUIRE(h.rc.StartRun("tester", 2).ok());
    const SkinBuild& build = h.rc.state().character.build;
    // 2026-11-22, the user: "make my tester character have only the flamethrower".
    CHECK(build.more_starting_weapons == std::vector<std::string>{"flamethrower"});
    CHECK(build.only_build_weapons);
    const auto& weapons = h.rc.state().weapons;
    CHECK(std::count_if(weapons.begin(), weapons.end(), [](const WeaponInstance& w) { return w.base_id == "flamethrower"; }) == 1);
    // Its fuel is never made infinite: the adapter tells the flame is firing by the fuel going down.
    // (An empty filter is "no infinite guns" to the adapter: InfiniteForBuild.)
    CHECK(build.infinite_ammo.Empty());
    // "let me select what element my gun has from the special menu".
    CHECK(build.Special(0).kind == "element_pick");
    REQUIRE(h.rc.TesterSetElement("flamethrower", Element::Shock, true).ok());
    const auto gun = [&]() -> const WeaponInstance& {
        return *std::find_if(h.rc.state().weapons.begin(), h.rc.state().weapons.end(),
                             [](const WeaponInstance& w) { return w.base_id == "flamethrower"; });
    };
    CHECK(gun().elements == std::vector<Element>{Element::Shock});
    CHECK(gun().element_evolved);
    const auto procs = h.rc.ElementsOf("flamethrower");
    REQUIRE(procs.size() == 1);
    CHECK(procs[0].first == Element::Shock);
    CHECK(procs[0].second);  // Storm
    REQUIRE(h.rc.TesterSetElement("flamethrower", Element::Cryo, false).ok());
    CHECK(gun().elements == std::vector<Element>{Element::Cryo});
    CHECK_FALSE(gun().element_evolved);
    REQUIRE(h.rc.TesterSetElement("flamethrower", std::nullopt, true).ok());
    CHECK(gun().elements.empty());
    CHECK_FALSE(gun().element_evolved);
    CHECK_FALSE(h.rc.TesterSetElement("rocket_launcher", Element::Toxic, false).ok());  // no such gun in the run
    // Anyone else: refused.
    test::Harness other(r.content);
    REQUIRE(other.rc.StartRun("chris", 2).ok());
    CHECK_FALSE(other.rc.TesterSetElement(other.rc.state().weapons.front().base_id, Element::Shock, false).ok());
}

TEST_CASE("the tester picks the gun it holds and its level", "[builds]") {
    // 2026-11-22, the user: "add dev command and something to select what weapon I have so I can test everything".
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    test::Harness h(r.content);
    h.rc.SetTesterCharacters(true);
    REQUIRE(h.rc.StartRun("tester", 2).ok());
    REQUIRE(h.rc.TesterSetElement("flamethrower", Element::Shock, false).ok());
    REQUIRE(h.rc.TesterSetWeapon("grenade_launcher").ok());
    REQUIRE(h.rc.state().weapons.size() == 1);  // that one alone
    CHECK(h.rc.state().weapons.front().base_id == "grenade_launcher");
    CHECK(h.rc.state().weapons.front().level == 1);
    REQUIRE(h.rc.DevSetWeaponLevel("grenade_launcher", 5).ok());
    CHECK(h.rc.state().weapons.front().level == 5);
    CHECK_FALSE(h.rc.DevSetWeaponLevel("grenade_launcher", 6).ok());  // past the economy's maximum
    CHECK_FALSE(h.rc.DevSetWeaponLevel("grenade_launcher", 0).ok());
    CHECK_FALSE(h.rc.DevSetWeaponLevel("flamethrower", 3).ok());      // no longer the run's
    // The same gun picked again keeps its level; a gun that is no game weapon is refused.
    REQUIRE(h.rc.TesterSetWeapon("grenade_launcher").ok());
    CHECK(h.rc.state().weapons.front().level == 5);
    CHECK_FALSE(h.rc.TesterSetWeapon("no_such_gun").ok());
    CHECK(h.rc.state().weapons.front().base_id == "grenade_launcher");
    // Anyone else: no pick (the level is a dev command, any run).
    test::Harness other(r.content);
    REQUIRE(other.rc.StartRun("chris", 2).ok());
    CHECK_FALSE(other.rc.TesterSetWeapon("gatling").ok());
    const std::string pistol = other.rc.state().weapons.front().base_id;
    REQUIRE(other.rc.DevSetWeaponLevel(pistol, 3).ok());
    CHECK(other.rc.state().weapons.front().level == 3);
}

// Safari Chris (the user, 2026-11-23: "safari chris should pick only change mines from grenades and stuff, also
// when bought from shop or given by chest, but should get ammo"): grenades become bombs wherever they come from,
// and nothing else does.
TEST_CASE("Safari Chris's grenades are bombs wherever they come from, and nothing else is", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const CharacterDef* chris = r.content->FindCharacter("chris");
    REQUIRE(chris != nullptr);
    const SkinDef* safari = FindSkin(*chris, "chris_safari");
    REQUIRE(safari != nullptr);
    const SkinBuild& b = safari->build;
    CHECK(b.drops_as < 0);  // ammo and the rest stay what they are
    CHECK(b.items_as.size() == 3);
    CHECK(b.mine_damage > 0.0);
    CHECK(b.mine_blast_m > 0.0);
    CHECK(std::count(b.stack_max.begin(), b.stack_max.end(), std::pair<std::int64_t, int>{0x010A, 10}) == 1);

    // The shop, rewards and chests take their items from OfferableItems: bombs in the grenades' place.
    const Capabilities caps = Capabilities::All();
    OfferContext ctx;
    ctx.content = r.content.get();
    ctx.capabilities = &caps;
    const auto has = [](const std::vector<const EquipmentDef*>& items, std::int64_t item) {
        return std::any_of(items.begin(), items.end(), [item](const EquipmentDef* e) { return e->game_item == item; });
    };
    const auto plain = OfferableItems(ctx);
    CHECK(has(plain, 0x0106));
    ctx.build = &b;
    const auto items = OfferableItems(ctx);
    CHECK_FALSE(has(items, 0x0106));
    CHECK_FALSE(has(items, 0x0107));
    CHECK_FALSE(has(items, 0x0108));
    CHECK(has(items, 0x010A));
    CHECK(std::count_if(items.begin(), items.end(), [](const EquipmentDef* e) { return e->game_item == 0x010A; }) == 1);
    CHECK(has(items, 0x0201));  // handgun rounds stay rounds
    CHECK(items.size() + 3 >= plain.size());

    // A perk's grenades too (Grenade Pouch: two at the run's start).
    Profile open;
    UnlockEverything(open, *r.content);
    test::Harness h(r.content, open);
    REQUIRE(h.rc.EquipPerk("chris", "supply_grenades").ok());
    REQUIRE(h.rc.StartRun("chris", 5, "chris_safari").ok());
    const auto& grants = h.rc.state().item_grants;
    CHECK(std::none_of(grants.begin(), grants.end(), [](const ItemGrant& g) { return g.game_item == 0x0106; }));
    CHECK(std::any_of(grants.begin(), grants.end(), [](const ItemGrant& g) { return g.game_item == 0x010A && g.count == 2; }));
}

TEST_CASE("items_as is checked when loaded", "[builds]") {
    CHECK(HasError(LoadErrors(json{{"items_as", {{"263", "0x0999"}}}}), "items_as gives a game item no loadouts.json equipment"));
    CHECK(HasError(LoadErrors(json{{"items_as", {{"263", 263}}}}), "must name another game item id"));
    CHECK(HasError(LoadErrors(json{{"mine_blast_fraction", 2.0}}), "mine_blast_fraction"));
    CHECK(LoadErrors(json{{"items_as", {{"263", 262}}}, {"mine_damage", 0.5}, {"mine_blast_m", 2.5}, {"mine_blast_fraction", 0.6}}).empty());
}

// Safari Chris's own bombs, his guns weaker, explosions on him halved (the user, 2026-11-23: "he should also get
// less damage from explosions and some way to generate mines, also he should deal less damage from other weapon types").
TEST_CASE("Safari Chris makes a bomb every 8 kills, takes half from explosions, and his guns are weaker", "[builds]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const SkinDef* safari = FindSkin(*r.content->FindCharacter("chris"), "chris_safari");
    REQUIRE(safari != nullptr);
    const SkinBuild& b = safari->build;
    CHECK(b.kill_item == 0x010A);
    CHECK(b.kill_item_every == 8);
    CHECK(b.explosion_damage_mult == Catch::Approx(0.5));
    for (const char* gun : {"handgun", "shotgun", "smg", "machine_gun", "rifle", "magnum", "bow", "launcher", "flamethrower"}) {
        INFO(gun);
        const auto it = b.category_damage.find(gun);
        REQUIRE(it != b.category_damage.end());
        CHECK(it->second == Catch::Approx(-0.25));
    }
    CHECK(b.category_damage.find("melee") == b.category_damage.end());  // kicks and the knife as they were
}

TEST_CASE("a build's kill_item comes every so many of the player's kills", "[builds]") {
    test::Harness h(test::Load(BuildSources(json{{"kill_item", 262}, {"kill_item_every", 2}, {"kill_item_amount", 3}})));
    REQUIRE(h.rc.StartRun("alpha", 4, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    const auto given = [&h] {
        const auto& grants = h.rc.state().item_grants;
        return std::count_if(grants.begin(), grants.end(), [](const ItemGrant& g) { return g.game_item == 262 && g.count == 3 && g.source == "build"; });
    };
    for (int i = 0; i < 4; ++i) {
        KillEvent k;
        k.now = 1.0 + i;
        k.by_player = true;
        k.enemy_id = "kill" + std::to_string(i);
        h.rc.OnEnemyKilled(k);
        CHECK(given() == (i + 1) / 2);
    }
    KillEvent other;  // a kill that is no player's counts for nothing
    other.now = 9.0;
    other.by_player = false;
    other.enemy_id = "other";
    h.rc.OnEnemyKilled(other);
    CHECK(given() == 2);
    CHECK(HasError(LoadErrors(json{{"kill_item", 262}}), "kill_item and kill_item_every go together"));
    CHECK(HasError(LoadErrors(json{{"kill_item", "0x0999"}, {"kill_item_every", 3}}), "kill_item names a game item no loadouts.json equipment"));
    CHECK(HasError(LoadErrors(json{{"explosion_damage_mult", 1.5}}), "explosion_damage_mult"));
}
