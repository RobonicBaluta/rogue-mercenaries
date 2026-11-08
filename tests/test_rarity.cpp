// The rarity remake (2026-11-08, the user: "elementals, crazy stuff for the best rarities"): Epic
// guns carry an element, Legendary ones an element and a legendary effect, Exotic ones (refined
// from a Legendary once the Giant Uroboros is beaten) two of each; a hit or a kill asks the adapter
// for the element's status or blast (WeaponProc); a gun goes one tier up by a refine (a reward, the
// shop, a chest).
#include <algorithm>
#include <string>

#include <catch_amalgamated.hpp>
#include <nlohmann/json.hpp>

#include "rm/run_resume.h"
#include "test_support.h"

using namespace rm;
using Catch::Approx;

namespace {

ContentSources RemakeSources(const std::function<void(nlohmann::json& weapons, nlohmann::json& economy)>& more = {}) {
    return test::EditFixture([&](nlohmann::json&, nlohmann::json& weapons, nlohmann::json&, nlohmann::json&,
                                 nlohmann::json& economy) {
        weapons["unique_affixes"] = nlohmann::json::parse(R"([
          {"id": "executioner_rounds", "name": "Executioner Rounds", "description": "finishes off the weak",
           "effect": "execute", "value": 1.0, "threshold": 0.2},
          {"id": "dragons_breath", "name": "Dragons Breath", "effect": "dragons_breath", "radius": 300},
          {"id": "chain_lightning", "name": "Chain Lightning", "effect": "chain_lightning", "value": 1.0, "every": 3,
           "targets": 4, "radius": 800},
          {"id": "bottomless", "name": "Bottomless", "effect": "bottomless"},
          {"id": "overkill", "name": "Overkill", "effect": "overkill", "value": 1.0, "radius": 600},
          {"id": "last_word", "name": "Last Word", "effect": "last_word", "value": 2.0},
          {"id": "bloodthirst", "name": "Bloodthirst", "effect": "bloodthirst", "value": 0.03}])");
        weapons["elements"] = nlohmann::json::parse(R"([
          {"element": "incendiary", "name": "Incendiary", "description": "hits set enemies on fire", "seconds": 3,
           "dps_fraction": 0.25, "boss_bonus": 0.15},
          {"element": "shock", "name": "Shock", "chance": 1.0, "damage_fraction": 0.6, "radius": 500, "targets": 2,
           "boss_bonus": 0.15},
          {"element": "cryo", "name": "Cryo", "chance": 1.0, "seconds": 3, "taken_mult": 1.5, "damage_fraction": 0.4,
           "damage_cap": 1200, "radius": 300, "boss_bonus": 0.15},
          {"element": "explosive", "name": "Explosive", "damage_fraction": 0.5, "damage_cap": 1500, "radius": 350,
           "boss_bonus": 0.2},
          {"element": "toxic", "name": "Toxic", "seconds": 5, "dps_fraction": 0.12, "taken_mult": 1.25, "boss_bonus": 0.15}])");
        economy["weapon_rarity"] = nlohmann::json::parse(R"({
          "common": {"affix_count": 0, "magnitude_mult": 1.0},
          "uncommon": {"affix_count": 1, "magnitude_mult": 1.25},
          "rare": {"affix_count": 2, "magnitude_mult": 1.5},
          "epic": {"affix_count": 3, "magnitude_mult": 1.75, "element_count": 1},
          "legendary": {"affix_count": 3, "magnitude_mult": 2.0, "unique_count": 1, "element_count": 1},
          "exotic": {"affix_count": 4, "magnitude_mult": 2.5, "unique_count": 2, "element_count": 2}})");
        economy["refine"] = nlohmann::json::parse(R"({"reward_chance": 0.0, "reward_rating": "S", "shop_chance": 1.0,
          "chest_chance": 0.0, "exotic_unlocked_by": "boss:uEm43",
          "price": {"common": 0, "uncommon": 40, "rare": 65, "epic": 110, "legendary": 160, "exotic": 240}})");
        if (more) more(weapons, economy);
    });
}

WeaponInstance Gun(std::string base, std::string id, Rarity rarity, std::vector<Element> elements = {},
                   std::vector<Affix> affixes = {}) {
    WeaponInstance w = MakeStartingWeapon(base, std::move(id));
    w.rarity = rarity;
    w.elements = std::move(elements);
    w.affixes = std::move(affixes);
    return w;
}

Affix Unique(AffixKind kind, std::string id, double value = 0.0, double threshold = 0.0) {
    return Affix{kind, value, threshold, std::move(id)};
}

const WeaponProc* FindProc(const std::vector<WeaponProc>& procs, ProcKind kind) {
    for (const WeaponProc& p : procs) {
        if (p.kind == kind) return &p;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("rarity remake: content loads elements, effects, the exotic tier and refine", "[weapons][rarity]") {
    auto content = test::Load(RemakeSources());
    CHECK(content->elements().size() == 5);
    REQUIRE(content->FindElement(Element::Cryo) != nullptr);
    CHECK(content->FindElement(Element::Cryo)->taken_mult == Approx(1.5));
    const UniqueAffixDef* chain = content->FindUnique("chain_lightning");
    REQUIRE(chain != nullptr);
    CHECK(chain->kind == AffixKind::UniqueChainLightning);
    CHECK(chain->every == 3);
    CHECK(chain->targets == 4);
    CHECK(chain->radius == Approx(800.0));
    const EconomyConfig& eco = content->economy();
    CHECK(eco.weapon_rarity[RarityIndex(Rarity::Exotic)].element_count == 2);
    CHECK(eco.rewards.rarity_weights[RarityIndex(Rarity::Exotic)] == 0.0);  // nothing rolls Exotic
    CHECK(eco.refine.enabled);
    CHECK(eco.refine.price[RarityIndex(Rarity::Exotic)] == 240);
    CHECK(ParseRarity("exotic") == Rarity::Exotic);
    CHECK(ParseElement("toxic") == Element::Toxic);

    // A broken element or effect is refused.
    const auto broken = Content::LoadFromStrings(RemakeSources([](nlohmann::json& weapons, nlohmann::json&) {
        weapons["elements"][1].erase("targets");                         // shock without its arcs
        weapons["unique_affixes"][1]["every"] = 2;                       // every on dragons_breath
        weapons["elements"].push_back(weapons["elements"][0]);           // incendiary twice
    }));
    CHECK_FALSE(broken.ok());
    const auto has = [&](const char* text) {
        return std::any_of(broken.errors.begin(), broken.errors.end(),
                           [&](const std::string& e) { return e.find(text) != std::string::npos; });
    };
    CHECK(has("needs 'targets', 'radius' and 'damage_fraction'"));
    CHECK(has("only apply to chain_lightning"));
    CHECK(has("duplicate element"));
}

TEST_CASE("rarity remake: Epic, Legendary and Exotic guns carry elements and effects", "[weapons][rarity]") {
    auto content = test::Load(RemakeSources());
    const Capabilities caps = Capabilities::All();
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Rng rng(seed);
        const WeaponInstance rare = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Rare, "w1");
        CHECK(rare.elements.empty());
        const WeaponInstance epic = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Epic, "w2");
        CHECK(epic.elements.size() == 1);
        CHECK(std::none_of(epic.affixes.begin(), epic.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }));
        const WeaponInstance legendary = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Legendary, "w3");
        CHECK(legendary.elements.size() == 1);
        CHECK(std::count_if(legendary.affixes.begin(), legendary.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }) == 1);
        const WeaponInstance exotic = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Exotic, "w4");
        REQUIRE(exotic.elements.size() == 2);
        CHECK(exotic.elements[0] != exotic.elements[1]);
        CHECK(std::count_if(exotic.affixes.begin(), exotic.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }) == 2);
        CHECK(std::count_if(exotic.affixes.begin(), exotic.affixes.end(), [](const Affix& a) { return !IsUniqueAffix(a.kind); }) == 4);
    }
    // The fixture's damage_pct rolls 10 at x1; an Exotic's at x2.5.
    Rng rng(7);
    const WeaponInstance exotic = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Exotic, "w");
    for (const Affix& a : exotic.affixes) {
        if (a.kind == AffixKind::DamagePct) CHECK(a.value == Approx(25.0));
    }
    CHECK(DescribeElement(Element::Incendiary, content.get()) == "Incendiary: hits set enemies on fire");
    CHECK(DescribeAffix(Unique(AffixKind::UniqueExecute, "executioner_rounds", 1.0, 0.2), content.get()) ==
          "Executioner Rounds: finishes off the weak");
}

TEST_CASE("rarity remake: a refine keeps the gun and adds the next tier", "[weapons][rarity]") {
    auto content = test::Load(RemakeSources());
    const Capabilities caps = Capabilities::All();
    Rng rng(3);
    WeaponInstance w = GenerateWeapon(rng, *content, caps, "shotgun", Rarity::Rare, "w_sg");
    w.level = 4;
    const WeaponInstance epic = RefineWeapon(rng, *content, caps, w);
    CHECK(epic.rarity == Rarity::Epic);
    CHECK(epic.instance_id == "w_sg");
    CHECK(epic.level == 4);
    CHECK(epic.elements.size() == 1);
    REQUIRE(epic.affixes.size() == 3);
    // The two it had stay, scaled from x1.5 to x1.75; damage_pct rolled 15 at Rare.
    for (std::size_t i = 0; i < w.affixes.size(); ++i) {
        CHECK(epic.affixes[i].kind == w.affixes[i].kind);
        if (w.affixes[i].kind == AffixKind::DamagePct) CHECK(epic.affixes[i].value == Approx(17.5));
    }
    const WeaponInstance legendary = RefineWeapon(rng, *content, caps, epic);
    CHECK(legendary.elements == epic.elements);
    CHECK(std::count_if(legendary.affixes.begin(), legendary.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }) == 1);
    const WeaponInstance exotic = RefineWeapon(rng, *content, caps, legendary);
    CHECK(exotic.rarity == Rarity::Exotic);
    CHECK(exotic.elements.size() == 2);
    CHECK(exotic.elements[0] == epic.elements[0]);
    CHECK(RefineWeapon(rng, *content, caps, exotic).rarity == Rarity::Exotic);  // the top
}

TEST_CASE("rarity remake: a hit's element procs scale with its damage; a boss takes damage instead", "[weapons][rarity]") {
    test::Harness h(test::Load(RemakeSources()));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_fire", Rarity::Epic, {Element::Incendiary, Element::Shock})).ok());

    HitContext hit;
    hit.now = 1.0;
    hit.weapon_id = "shotgun";
    hit.base_damage = 200.0;
    OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(r.multiplier == Approx(1.0));
    const WeaponProc* burn = FindProc(r.procs, ProcKind::Burn);
    REQUIRE(burn != nullptr);
    CHECK(burn->dps == Approx(50.0));  // 0.25 of the 200 dealt, per second
    CHECK(burn->seconds == Approx(3.0));
    const WeaponProc* shock = FindProc(r.procs, ProcKind::Shock);
    REQUIRE(shock != nullptr);
    CHECK(shock->damage == Approx(120.0));
    CHECK(shock->targets == 2);
    CHECK(shock->radius == Approx(500.0));

    // A boss: no status, the elements' boss_bonus as damage.
    hit.target_is_boss = true;
    r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(r.procs.empty());
    CHECK(r.multiplier == Approx(1.3));

    // A frozen and poisoned target takes more from anything, melee too.
    hit.target_is_boss = false;
    hit.target_frozen = true;
    hit.target_poisoned = true;
    hit.is_melee = true;
    r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(r.multiplier == Approx(1.5 * 1.25));
    CHECK(r.procs.empty());  // melee carries no gun
}

TEST_CASE("rarity remake: Execute, Last Word, Chain Lightning and Dragons Breath", "[weapons][rarity]") {
    test::Harness h(test::Load(RemakeSources()));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_leg", Rarity::Legendary, {},
                                    {Unique(AffixKind::UniqueExecute, "executioner_rounds", 1.0, 0.2),
                                     Unique(AffixKind::UniqueLastWord, "last_word", 2.0),
                                     Unique(AffixKind::UniqueChainLightning, "chain_lightning", 1.0),
                                     Unique(AffixKind::UniqueDragonsBreath, "dragons_breath")}))
                .ok());
    HitContext hit;
    hit.now = 1.0;
    hit.weapon_id = "shotgun";
    hit.base_damage = 100.0;
    hit.target_hp = 900.0;
    hit.target_hp_max = 1000.0;
    hit.magazine_left = 4;
    OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(FindProc(r.procs, ProcKind::Execute) == nullptr);
    REQUIRE(FindProc(r.procs, ProcKind::IgniteAround) != nullptr);
    CHECK(FindProc(r.procs, ProcKind::IgniteAround)->radius == Approx(300.0));
    CHECK(FindProc(r.procs, ProcKind::Arc) == nullptr);  // hit 1 of 3
    CHECK(FindProc(h.rc.ComputeOutgoingMultiplier(hit).procs, ProcKind::Arc) == nullptr);
    const OutgoingResult third = h.rc.ComputeOutgoingMultiplier(hit);
    REQUIRE(FindProc(third.procs, ProcKind::Arc) != nullptr);  // every 3rd hit
    CHECK(FindProc(third.procs, ProcKind::Arc)->damage == Approx(100.0));
    CHECK(FindProc(third.procs, ProcKind::Arc)->targets == 4);

    hit.target_hp = 150.0;  // below 20 percent: finished off
    CHECK(FindProc(h.rc.ComputeOutgoingMultiplier(hit).procs, ProcKind::Execute) != nullptr);
    hit.target_hp = 900.0;
    hit.magazine_left = 0;  // the last round
    CHECK(FindProc(h.rc.ComputeOutgoingMultiplier(hit).procs, ProcKind::Execute) != nullptr);
    // A boss is never executed: it takes Last Word's +200 percent instead.
    hit.target_is_boss = true;
    r = h.rc.ComputeOutgoingMultiplier(hit);
    CHECK(FindProc(r.procs, ProcKind::Execute) == nullptr);
    CHECK(r.multiplier == Approx(3.0));
}

TEST_CASE("rarity remake: kill effects (blast, shatter, overkill, refill, heal)", "[weapons][rarity]") {
    test::Harness h(test::Load(RemakeSources()));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_boom", Rarity::Exotic, {Element::Explosive, Element::Toxic},
                                    {Unique(AffixKind::UniqueOverkill, "overkill", 1.0),
                                     Unique(AffixKind::UniqueBottomless, "bottomless"),
                                     Unique(AffixKind::UniqueBloodthirst, "bloodthirst", 0.03)}))
                .ok());
    KillEvent k;
    k.now = 2.0;
    k.by_player = true;
    k.weapon_id = "shotgun";
    k.enemy_id = "e1";
    k.target_hp_max = 1000.0;
    k.overkill = 80.0;
    KillOutcome out = h.rc.OnEnemyKilled(k);
    REQUIRE(out.counted);
    const WeaponProc* boom = FindProc(out.procs, ProcKind::Explode);
    REQUIRE(boom != nullptr);
    CHECK(boom->damage == Approx(500.0));
    CHECK(boom->radius == Approx(350.0));
    REQUIRE(FindProc(out.procs, ProcKind::Overkill) != nullptr);
    CHECK(FindProc(out.procs, ProcKind::Overkill)->damage == Approx(80.0));
    CHECK(FindProc(out.procs, ProcKind::Refill) != nullptr);
    CHECK(out.heal_fraction_queued == Approx(0.03));
    CHECK(FindProc(out.procs, ProcKind::Shatter) == nullptr);

    // A giant's blast is capped; a frozen one shatters; a boss never blows up.
    k.enemy_id = "e2";
    k.target_hp_max = 20000.0;
    k.overkill = 0.0;
    k.target_frozen = true;
    out = h.rc.OnEnemyKilled(k);
    REQUIRE(FindProc(out.procs, ProcKind::Explode) != nullptr);
    CHECK(FindProc(out.procs, ProcKind::Explode)->damage == Approx(1500.0));
    REQUIRE(FindProc(out.procs, ProcKind::Shatter) != nullptr);
    CHECK(FindProc(out.procs, ProcKind::Shatter)->damage == Approx(1200.0));
    CHECK(FindProc(out.procs, ProcKind::Overkill) == nullptr);
    k.enemy_id = "e3";
    k.target_is_boss = true;
    CHECK(h.rc.OnEnemyKilled(k).procs.empty());
}

TEST_CASE("rarity remake: refines from the rewards, the shop and chests", "[weapons][rarity]") {
    SECTION("an arena at the sure rating always offers a refine of the best gun") {
        test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
            economy["refine"]["reward_rating"] = "C";  // any rating
        })));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
        test::ClearArena(h.rc, 0.0);
        const auto& offer = h.rc.state().reward_offer;
        REQUIRE(offer.size() == 3);
        const RewardChoice& refine = offer.back();
        REQUIRE(refine.kind == RewardKind::Refine);
        CHECK(refine.target_instance_id == "w_sg");
        CHECK(refine.rarity == Rarity::Epic);
        const auto view = h.rc.Snapshot(1.0).reward_offers.back();
        CHECK(view.title == "Refine Shotgun");
        CHECK(view.detail.rfind("Becomes Epic.", 0) == 0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(2).ok());
        const auto it = std::find_if(h.rc.state().weapons.begin(), h.rc.state().weapons.end(),
                                     [](const WeaponInstance& w) { return w.instance_id == "w_sg"; });
        REQUIRE(it != h.rc.state().weapons.end());
        CHECK(it->rarity == Rarity::Epic);
        CHECK(it->elements.size() == 1);
        // A reroll keeps the sure refine.
        REQUIRE(h.rc.state().phase == Phase::Shop);
    }
    SECTION("no refine at a lower rating with no chance") {
        test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
            economy["refine"]["reward_rating"] = "S";
        })));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        const ArenaResult result = test::ClearArena(h.rc, 0.0);
        const bool any = std::any_of(h.rc.state().reward_offer.begin(), h.rc.state().reward_offer.end(),
                                     [](const RewardChoice& c) { return c.kind == RewardKind::Refine; });
        CHECK(any == (result.rating == Rating::S));
    }
    SECTION("the shop sells a refine at its tier's price") {
        test::Harness h(test::Load(RemakeSources()));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(0).ok());
        REQUIRE(h.rc.state().phase == Phase::Shop);
        const auto& offers = h.rc.state().shop_offers;
        const auto it = std::find_if(offers.begin(), offers.end(), [](const ShopOffer& o) { return o.kind == ShopOfferKind::Refine; });
        REQUIRE(it != offers.end());
        CHECK(it->price == 40);  // Common to Uncommon
        CHECK(it->rarity == Rarity::Uncommon);
        REQUIRE(h.rc.DevGrantSupplies(100).ok());
        REQUIRE(h.rc.Purchase(static_cast<std::size_t>(it - offers.begin())).ok());
        bool refined = false;
        for (const WeaponInstance& w : h.rc.state().weapons) refined = refined || w.rarity == Rarity::Uncommon;
        CHECK(refined);
    }
    SECTION("a chest refines the gun in hand") {
        test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
            economy["refine"]["chest_chance"] = 1.0;
        })));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        const auto chest = h.rc.OpenChest(1.0, std::string("pistol"));
        REQUIRE(chest.ok());
        CHECK(chest.value().refined_name == "Pistol");
        CHECK(chest.value().refined_rarity == "uncommon");
    }
}

TEST_CASE("rarity remake: Exotic opens with the Giant Uroboros", "[weapons][rarity]") {
    auto content = test::Load(RemakeSources());
    {
        test::Harness h(content);
        CHECK(h.rc.MaxRefineRarity() == Rarity::Legendary);
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Legendary)).ok());
        // The legendary shotgun is at the top: the pistol is the one to refine.
        REQUIRE(h.rc.RefineTarget(std::string("shotgun")) != nullptr);
        CHECK(h.rc.RefineTarget(std::string("shotgun"))->base_id == "pistol");
    }
    Profile beaten;
    beaten.stats.bosses_beaten = {"boss:uEm43"};
    test::Harness h(content, beaten);
    CHECK(h.rc.MaxRefineRarity() == Rarity::Exotic);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Legendary)).ok());
    REQUIRE(h.rc.RefineTarget(std::string("shotgun")) != nullptr);
    CHECK(h.rc.RefineTarget(std::string("shotgun"))->base_id == "shotgun");
}

TEST_CASE("rarity remake: a resumed run keeps its guns' elements", "[weapons][rarity][persistence]") {
    test::Harness h(test::Load(RemakeSources()));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_x", Rarity::Exotic, {Element::Cryo, Element::Shock},
                                    {Unique(AffixKind::UniqueLastWord, "last_word", 2.0)}))
                .ok());
    RunState back;
    REQUIRE(ParseRunResume(SerializeRunResume(h.rc.state()), back).ok());
    const auto it = std::find_if(back.weapons.begin(), back.weapons.end(), [](const WeaponInstance& w) { return w.instance_id == "w_x"; });
    REQUIRE(it != back.weapons.end());
    CHECK(it->rarity == Rarity::Exotic);
    CHECK(it->elements == std::vector<Element>{Element::Cryo, Element::Shock});
    REQUIRE(it->affixes.size() == 1);
    CHECK(it->affixes[0].kind == AffixKind::UniqueLastWord);
}
