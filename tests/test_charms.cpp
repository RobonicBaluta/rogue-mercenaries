// Charms (CharmDef), the grenade and ammo build fields, and the supply perks' guns against the
// starting gun (the Perk Workshop's picks, 2026-11-24).

#include <algorithm>
#include <filesystem>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "rm/characters.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

const json kCard = {{"id", "fidelity_card"}, {"name", "Fidelity Card"}, {"game_item", "0x0419"},
                    {"picture", "fidelity_card.png"}, {"chest_weapon_every", 3}, {"chest_weapon_rarity", "rare"}};
const json kVirus = {{"id", "uroboros_virus"}, {"name", "Uroboros Virus"}, {"game_item", "0x0422"},
                     {"kill_stack_stats", {{"damage", 0.05}, {"defense", 0.04}, {"crit", 0.02}, {"move_speed", 0.03}}},
                     {"kill_stack_max", 3}, {"stacks_end_on_hit", true}};
const json kGrapple = {{"id", "grapple_gun"}, {"name", "Grapple Gun"}, {"game_item", "0x0423"}, {"breaks_grabs", true},
                       {"special", {{"kind", "flip"}, {"name", "Flip"}, {"cooldown_seconds", 5.0}, {"duration_seconds", 0.6}, {"power", 300}}}};

// Alpha's second skin plays `build`; the economy's chests hold no weapon or item of their own, and
// the pistol, a second handgun and the shotgun have game items.
ContentSources Sources(const json& build, const json& extra_perks = json::array()) {
    return test::EditFixture([&](json& perks, json& weapons, json& characters, json&, json& economy) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "pistol") w["game_item"] = 0x0102;
            if (w["id"] == "shotgun") w["game_item"] = 0x0104;
        }
        weapons["weapons"].push_back(json{{"id", "machine_pistol"}, {"name", "Machine Pistol"}, {"category", "handgun"}, {"game_item", 0x011E}});
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_build"}, {"name", "Build"}, {"costume_index", 1}, {"build", build}},
        });
        for (const json& p : extra_perks) perks["perks"].push_back(p);
        economy["chests"] = {{"item_chance", 0.0}, {"weapon_chance", 0.0}};
    });
}

std::vector<std::string> LoadErrors(const json& build) {
    const auto loaded = Content::LoadFromStrings(Sources(build));
    for (const std::string& e : loaded.errors) UNSCOPED_INFO("got: " << e);
    return loaded.errors;
}

bool HasError(const std::vector<std::string>& errors, std::string_view needle) {
    return std::any_of(errors.begin(), errors.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
}

bool Granted(const RunController& rc, std::int64_t item, std::string_view source) {
    const auto& grants = rc.state().item_grants;
    return std::any_of(grants.begin(), grants.end(), [&](const ItemGrant& g) { return g.game_item == item && g.source == source; });
}

const WeaponInstance* Held(const RunController& rc, std::string_view base) {
    for (const WeaponInstance& w : rc.state().weapons) {
        if (w.base_id == base) return &w;
    }
    return nullptr;
}

KillEvent Kill(const char* id, double now) {
    KillEvent k;
    k.now = now;
    k.by_player = true;
    k.enemy_id = id;
    return k;
}

}  // namespace

TEST_CASE("charms are checked when loaded and handed over when the run starts", "[charms]") {
    CHECK(LoadErrors(json{{"charms", {kCard, kVirus, kGrapple}}}).empty());
    json notTreasure = kCard;
    notTreasure["game_item"] = 0x0102;
    CHECK(HasError(LoadErrors(json{{"charms", {notTreasure}}}), "must be a treasure"));
    json noMax = kVirus;
    noMax.erase("kill_stack_max");
    CHECK(HasError(LoadErrors(json{{"charms", {noMax}}}), "need a kill_stack_max"));
    json badStat = kVirus;
    badStat["kill_stack_stats"] = {{"heal", 0.1}};
    CHECK(HasError(LoadErrors(json{{"charms", {badStat}}}), "is not damage, defense, crit or move_speed"));
    json badRarity = kCard;
    badRarity["chest_weapon_rarity"] = "shiny";
    CHECK(HasError(LoadErrors(json{{"charms", {badRarity}}}), "is not a rarity"));

    test::Harness h(test::Load(Sources(json{{"charms", {kCard, kGrapple}}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    CHECK(Granted(h.rc, 0x0419, "charm:fidelity_card"));
    CHECK(Granted(h.rc, 0x0423, "charm:grapple_gun"));
    // The grapple gun's flip fills the empty special slot, and names its charm.
    CHECK(h.rc.state().character.build.special.kind == "flip");
    CHECK(h.rc.state().character.build.special_charm == "grapple_gun");
}

TEST_CASE("the Fidelity Card's every third chest holds a weapon of at least its rarity, while it is carried", "[charms]") {
    test::Harness h(test::Load(Sources(json{{"charms", {kCard}}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    // Not carried: chests hold no weapon (the fixture's chests have none of their own) and are not counted.
    for (int i = 0; i < 3; ++i) {
        const auto chest = h.rc.OpenChest(1.0 + i);
        REQUIRE(chest.ok());
        CHECK(chest->weapon_id.empty());
    }
    CHECK(h.rc.state().charm_chests == 0);
    h.rc.SetCarriedCharms({"fidelity_card"});
    CHECK(h.rc.CharmCarried("fidelity_card"));
    for (int i = 1; i <= 6; ++i) {
        INFO("chest " << i << " while carried");
        const auto chest = h.rc.OpenChest(10.0 + i);
        REQUIRE(chest.ok());
        if (i % 3 != 0) {
            CHECK(chest->weapon_id.empty());
            CHECK(chest->charm.empty());
            continue;
        }
        REQUIRE_FALSE(chest->weapon_id.empty());
        CHECK(chest->charm == "Fidelity Card");
        const WeaponInstance* w = Held(h.rc, chest->weapon_id);
        REQUIRE(w != nullptr);
        CHECK(static_cast<int>(w->rarity) >= static_cast<int>(Rarity::Rare));
    }
}

TEST_CASE("the Uroboros Virus stacks with kills while carried, adds its stats, and a hit taken clears them", "[charms]") {
    test::Harness h(test::Load(Sources(json{{"charms", {kVirus}}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    HitContext shot;
    shot.now = 1.0;
    shot.weapon_id = "pistol";
    const double plain = h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus;

    h.rc.OnEnemyKilled(Kill("a", 1.0));  // not carried: nothing
    CHECK(h.rc.CharmStacks() == 0);
    h.rc.SetCarriedCharms({"uroboros_virus"});
    for (int i = 0; i < 5; ++i) h.rc.OnEnemyKilled(Kill(("k" + std::to_string(i)).c_str(), 2.0 + i));
    CHECK(h.rc.CharmStacks() == 3);  // its most
    const RunStats s = h.rc.CharmStats();
    CHECK(s.damage == Catch::Approx(0.15));
    CHECK(s.defense == Catch::Approx(0.12));
    CHECK(s.crit == Catch::Approx(0.06));
    CHECK(s.move_speed == Catch::Approx(0.09));
    CHECK(h.rc.ComputeOutgoingMultiplier(shot).breakdown.build_bonus == Catch::Approx(plain + 0.15));

    // Put down, it adds nothing (the stacks wait); carried again, they count again.
    h.rc.SetCarriedCharms({});
    CHECK(h.rc.CharmStats().damage == 0.0);
    h.rc.SetCarriedCharms({"uroboros_virus"});
    CHECK(h.rc.CharmStats().damage == Catch::Approx(0.15));

    DamageEvent nick;
    nick.now = 9.0;
    nick.amount = 0.0;  // no damage: they stay
    h.rc.OnPlayerDamaged(nick);
    CHECK(h.rc.CharmStacks() == 3);
    nick.amount = 50.0;
    nick.hp_before = 1000.0;
    nick.hp_after = 950.0;
    nick.hp_max = 1000.0;
    h.rc.OnPlayerDamaged(nick);
    CHECK(h.rc.CharmStacks() == 0);
    CHECK(h.rc.CharmStats().damage == 0.0);
}

TEST_CASE("a charm's special and grab escape work only while it is carried", "[charms]") {
    test::Harness h(test::Load(Sources(json{{"charms", {kGrapple}}})));
    REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    CHECK_FALSE(h.rc.BreaksGrabs());
    CHECK(h.rc.UseSpecial(1.0).code == ErrorCode::NotEligible);
    h.rc.SetCarriedCharms({"grapple_gun"});
    CHECK(h.rc.BreaksGrabs());
    REQUIRE(h.rc.UseSpecial(2.0).ok());
    CHECK(h.rc.Invulnerable(2.5));  // a flip is untouchable for its duration, as a dash is
    CHECK_FALSE(h.rc.Invulnerable(2.7));

    // A build's own special stays; the charm's does not take its slot.
    const json dash = {{"special", {{"kind", "dash"}, {"name", "Dash"}, {"cooldown_seconds", 3.0}}}, {"charms", {kGrapple}}};
    test::Harness w(test::Load(Sources(dash)));
    REQUIRE(w.rc.StartRun("alpha", 3, "alpha_build").ok());
    CHECK(w.rc.state().character.build.special.kind == "dash");
    CHECK(w.rc.state().character.build.special_charm.empty());
}

TEST_CASE("a supply perk's gun takes the place of the starting gun of its kind, better", "[charms]") {
    const json spare = {{"id", "spare_gun"}, {"name", "Spare Gun"}, {"category", "supply"}, {"rarity", "common"},
                        {"effects", {{{"kind", "item_on_start"}, {"game_item", 0x011E}, {"amount", 1}}}}};
    const json same = {{"id", "same_gun"}, {"name", "Same Gun"}, {"category", "supply"}, {"rarity", "common"},
                       {"effects", {{{"kind", "item_on_start"}, {"game_item", 0x0102}, {"amount", 1}}}}};
    const json other = {{"id", "other_gun"}, {"name", "Other Gun"}, {"category", "supply"}, {"rarity", "common"},
                        {"effects", {{{"kind", "item_on_start"}, {"game_item", 0x0104}, {"amount", 1}}}}};

    SECTION("another gun of the kind: it replaces the starting gun, rare") {
        test::Harness h(test::Load(Sources(json{{"summary", "none"}}, json::array({spare}))));
        REQUIRE(h.rc.EquipPerk("alpha", "spare_gun").ok());
        REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
        REQUIRE(h.rc.SwapStartingWeapon("pistol").ok());
        CHECK(h.rc.StartingGunReplacedBy() == "machine_pistol");
        CHECK(Held(h.rc, "pistol") == nullptr);
        REQUIRE(Held(h.rc, "machine_pistol") != nullptr);
        CHECK(Held(h.rc, "machine_pistol")->rarity == Rarity::Rare);
        CHECK(h.rc.state().loadout.weapon_id == "machine_pistol");
    }
    SECTION("an expert of the kind gets it epic") {
        test::Harness h(test::Load(Sources(json{{"category_damage", {{"handgun", 0.25}}}}, json::array({spare}))));
        REQUIRE(h.rc.EquipPerk("alpha", "spare_gun").ok());
        REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
        REQUIRE(h.rc.SwapStartingWeapon("pistol").ok());
        REQUIRE(Held(h.rc, "machine_pistol") != nullptr);
        CHECK(Held(h.rc, "machine_pistol")->rarity == Rarity::Epic);
    }
    SECTION("the starting gun itself: it only becomes the better one") {
        test::Harness h(test::Load(Sources(json{{"summary", "none"}}, json::array({same}))));
        REQUIRE(h.rc.EquipPerk("alpha", "same_gun").ok());
        REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
        REQUIRE(h.rc.SwapStartingWeapon("pistol").ok());
        CHECK(h.rc.StartingGunReplacedBy().empty());
        REQUIRE(Held(h.rc, "pistol") != nullptr);
        CHECK(Held(h.rc, "pistol")->rarity == Rarity::Rare);
    }
    SECTION("another kind: both stay as they were") {
        test::Harness h(test::Load(Sources(json{{"summary", "none"}}, json::array({other}))));
        REQUIRE(h.rc.EquipPerk("alpha", "other_gun").ok());
        REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
        REQUIRE(h.rc.SwapStartingWeapon("pistol").ok());
        CHECK(h.rc.StartingGunReplacedBy().empty());
        REQUIRE(Held(h.rc, "pistol") != nullptr);
        REQUIRE(Held(h.rc, "shotgun") != nullptr);
        CHECK(Held(h.rc, "shotgun")->rarity == Rarity::Common);
    }
    SECTION("a gun the build cannot use comes as a usable one of its kind, or not at all") {
        const json onlyPistol = {{"usable_weapons", {{"ids", {"pistol"}}}}};
        test::Harness h(test::Load(Sources(onlyPistol, json::array({spare, other}))));
        REQUIRE(h.rc.EquipPerk("alpha", "spare_gun").ok());
        REQUIRE(h.rc.StartRun("alpha", 3, "alpha_build").ok());
        CHECK(Granted(h.rc, 0x0102, "spare_gun"));        // its own handgun in the machine pistol's place
        CHECK_FALSE(Granted(h.rc, 0x011E, "spare_gun"));
        test::Harness o(test::Load(Sources(onlyPistol, json::array({spare, other}))));
        REQUIRE(o.rc.EquipPerk("alpha", "other_gun").ok());
        REQUIRE(o.rc.StartRun("alpha", 3, "alpha_build").ok());
        CHECK_FALSE(Granted(o.rc, 0x0104, "other_gun"));  // no usable shotgun: nothing of that perk
        CHECK(Held(o.rc, "shotgun") == nullptr);
    }
}

TEST_CASE("a perk's grenade, ammo and charm fields add into the run's build", "[charms]") {
    SkinBuild build;
    build.grenade_damage = 0.2;
    SkinBuild pouch;
    pouch.grenade_damage = 0.5;
    pouch.grenade_blast_m = 2.0;
    pouch.grenade_blast_fraction = 0.5;
    pouch.cluster_bomblets = 4;
    pouch.cluster_damage_fraction = 0.6;
    pouch.cluster_radius_m = 2.5;
    pouch.incendiary_burns = true;
    pouch.flash_masked = true;
    pouch.flash_extra_seconds = 2.0;
    pouch.empty_ammo_regen["handgun"] = AmmoRegen{6.0, 10};
    CharmDef card;
    card.id = "fidelity_card";
    pouch.charms.push_back(card);
    ApplyPerkBuild(build, pouch);
    ApplyPerkBuild(build, pouch);  // a charm comes once
    CHECK(build.grenade_damage == Catch::Approx(1.2));
    CHECK(build.grenade_blast_m == Catch::Approx(2.0));
    CHECK(build.cluster_bomblets == 4);
    CHECK(build.cluster_radius_m == Catch::Approx(2.5));
    CHECK(build.incendiary_burns);
    CHECK(build.flash_masked);
    CHECK(build.flash_extra_seconds == Catch::Approx(2.0));
    REQUIRE(build.empty_ammo_regen.count("handgun") == 1);
    CHECK(build.empty_ammo_regen.at("handgun").rounds == 10);
    CHECK(build.charms.size() == 1);
}

TEST_CASE("the shipped picks: the charms, the pouches, Scavenger's rounds and the Ammo Belt", "[charms]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const auto charmOf = [](const SkinBuild& b, std::string_view id) -> const CharmDef* {
        for (const CharmDef& c : b.charms) {
            if (c.id == id) return &c;
        }
        return nullptr;
    };
    // Night Shopper's Fidelity Card.
    const SkinDef* clubbin = FindSkin(*r.content->FindCharacter("sheva"), "sheva_clubbin");
    REQUIRE(clubbin != nullptr);
    const CharmDef* card = charmOf(clubbin->build, "fidelity_card");
    REQUIRE(card != nullptr);
    CHECK(card->game_item == 0x0419);
    CHECK(card->chest_weapon_every == 3);
    CHECK(card->chest_weapon_rarity == "rare");
    // Firestarter is Perfection: the Uroboros Virus, no grenades.
    const PerkDef* perfection = r.content->FindPerk("wesker_firestarter");
    REQUIRE(perfection != nullptr);
    CHECK(perfection->name == "Perfection");
    CHECK(perfection->effects.empty());
    REQUIRE(perfection->build.has_value());
    const CharmDef* virus = charmOf(*perfection->build, "uroboros_virus");
    REQUIRE(virus != nullptr);
    CHECK(virus->kill_stack_max == 10);
    CHECK(virus->stacks_end_on_hit);
    // Squad Tactics' cluster grenades and its grenade every 8 kills.
    const PerkDef* squad = r.content->FindPerk("josh_squad_tactics");
    REQUIRE(squad != nullptr);
    REQUIRE(squad->build.has_value());
    CHECK(squad->build->cluster_bomblets == 4);
    CHECK(std::any_of(squad->effects.begin(), squad->effects.end(), [](const EffectDef& e) {
        return e.kind == EffectKind::ItemOnKill && e.params.game_item == 0x0106 && e.params.every_kills == 8;
    }));
    // Scavenger's rounds: the bigger the gun, the longer the wait and the fewer the rounds.
    const PerkDef* scav = r.content->FindPerk("sheva_scavenger");
    REQUIRE(scav != nullptr);
    REQUIRE(scav->build.has_value());
    const auto& regen = scav->build->empty_ammo_regen;
    REQUIRE(regen.count("handgun") == 1);
    REQUIRE(regen.count("magnum") == 1);
    CHECK(regen.at("handgun").seconds < regen.at("magnum").seconds);
    CHECK(regen.at("handgun").rounds > regen.at("magnum").rounds);
    // The Grapple Gun: a charm with the flip, no heal penalty, no grab escape without it.
    const PerkDef* grapple = r.content->FindPerk("ada_grapple");
    REQUIRE(grapple != nullptr);
    REQUIRE(grapple->build.has_value());
    CHECK(grapple->build->heal_mult == Catch::Approx(1.0));
    CHECK_FALSE(grapple->build->break_grabs);
    const CharmDef* gun = charmOf(*grapple->build, "grapple_gun");
    REQUIRE(gun != nullptr);
    CHECK(gun->breaks_grabs);
    CHECK(gun->special.kind == "flip");
    // The pouches.
    const PerkDef* hand = r.content->FindPerk("supply_grenades");
    const PerkDef* flash = r.content->FindPerk("supply_flash");
    const PerkDef* fire = r.content->FindPerk("supply_incendiary");
    REQUIRE((hand && hand->build && flash && flash->build && fire && fire->build));
    CHECK(hand->build->grenade_damage == Catch::Approx(0.5));
    CHECK(hand->build->grenade_blast_m == Catch::Approx(2.0));
    CHECK(flash->build->flash_masked);
    CHECK(flash->build->flash_extra_seconds > 0.0);
    CHECK(fire->build->incendiary_burns);
    // Every charm's picture ships, the size of an item's picture.
    for (const auto* b : {&clubbin->build, &*perfection->build, &*grapple->build}) {
        for (const CharmDef& c : b->charms) {
            INFO(c.id);
            CHECK(std::filesystem::exists(test::ConfigDir() / "charms" / c.picture));
        }
    }
}
