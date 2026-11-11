// The rarity remake (2026-11-08, the user: "elementals, crazy stuff for the best rarities"): Epic
// guns carry an element, Legendary ones an element and a legendary effect, Exotic ones (refined
// from a Legendary once the Giant Uroboros is beaten) two of each; a hit or a kill asks the adapter
// for the element's status or blast (WeaponProc); a gun goes one tier up by a refine (a reward, the
// shop, a chest).
#include <algorithm>
#include <set>
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
          "legendary": {"affix_count": 3, "magnitude_mult": 2.0, "element_count": 1, "element_evolved": true},
          "exotic": {"affix_count": 4, "magnitude_mult": 2.5, "unique_count": 1, "element_count": 1, "element_evolved": true}})");
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
    CHECK(eco.weapon_rarity[RarityIndex(Rarity::Exotic)].element_count == 1);
    CHECK(eco.weapon_rarity[RarityIndex(Rarity::Legendary)].element_evolved);
    CHECK_FALSE(eco.weapon_rarity[RarityIndex(Rarity::Epic)].element_evolved);
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

TEST_CASE("rarity remake: Epic, Legendary and Exotic guns carry one element, evolved at Legendary", "[weapons][rarity]") {
    // 2026-11-11 (the user: the best tiers carried too many): one element a gun, evolved from
    // Legendary on; Legendary adds no legendary effect, Exotic one.
    auto content = test::Load(RemakeSources());
    const Capabilities caps = Capabilities::All();
    const auto uniques = [](const WeaponInstance& w) {
        return std::count_if(w.affixes.begin(), w.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); });
    };
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Rng rng(seed);
        const WeaponInstance rare = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Rare, "w1");
        CHECK(rare.elements.empty());
        const WeaponInstance epic = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Epic, "w2");
        CHECK(epic.elements.size() == 1);
        CHECK(epic.element_offer.empty());  // a found or bought gun rolls it: no choice
        CHECK_FALSE(epic.element_evolved);
        CHECK(uniques(epic) == 0);
        const WeaponInstance legendary = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Legendary, "w3");
        CHECK(legendary.elements.size() == 1);
        CHECK(legendary.element_evolved);
        CHECK(uniques(legendary) == 0);
        const WeaponInstance exotic = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Exotic, "w4");
        CHECK(exotic.elements.size() == 1);
        CHECK(exotic.element_evolved);
        CHECK(uniques(exotic) == 1);
        CHECK(std::count_if(exotic.affixes.begin(), exotic.affixes.end(), [](const Affix& a) { return !IsUniqueAffix(a.kind); }) == 4);
        // Never one of the build's own.
        const WeaponInstance other = GenerateWeapon(rng, *content, caps, "pistol", Rarity::Epic, "w5",
                                                    {Element::Incendiary, Element::Shock, Element::Cryo, Element::Toxic});
        CHECK(other.elements == std::vector<Element>{Element::Explosive});
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
    WeaponInstance epic = RefineWeapon(rng, *content, caps, w);
    CHECK(epic.rarity == Rarity::Epic);
    CHECK(epic.instance_id == "w_sg");
    CHECK(epic.level == 4);
    // Refined to Epic, the gun offers two elements to choose from instead of drawing one.
    CHECK(epic.elements.empty());
    REQUIRE(epic.element_offer.size() == 2);
    CHECK(epic.element_offer[0] != epic.element_offer[1]);
    CHECK_FALSE(epic.element_evolved);
    REQUIRE(epic.affixes.size() == 3);
    // The two it had stay, scaled from x1.5 to x1.75; damage_pct rolled 15 at Rare.
    for (std::size_t i = 0; i < w.affixes.size(); ++i) {
        CHECK(epic.affixes[i].kind == w.affixes[i].kind);
        if (w.affixes[i].kind == AffixKind::DamagePct) CHECK(epic.affixes[i].value == Approx(17.5));
    }
    // Not chosen yet: a refine to Legendary keeps the offer, and the element comes evolved.
    const WeaponInstance waiting = RefineWeapon(rng, *content, caps, epic);
    CHECK(waiting.element_offer == epic.element_offer);
    CHECK(waiting.element_evolved);
    epic.elements = {epic.element_offer[1]};
    epic.element_offer.clear();
    const WeaponInstance legendary = RefineWeapon(rng, *content, caps, epic);
    CHECK(legendary.elements == epic.elements);
    CHECK(legendary.element_evolved);
    CHECK(legendary.element_offer.empty());
    CHECK(std::count_if(legendary.affixes.begin(), legendary.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }) == 0);
    const WeaponInstance exotic = RefineWeapon(rng, *content, caps, legendary);
    CHECK(exotic.rarity == Rarity::Exotic);
    CHECK(exotic.elements == epic.elements);
    CHECK(std::count_if(exotic.affixes.begin(), exotic.affixes.end(), [](const Affix& a) { return IsUniqueAffix(a.kind); }) == 1);
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
    SECTION("an arena at the sure rating always offers a refine (seed 1 draws the shotgun)") {
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
        CHECK(it->elements.empty());  // it waits for its element: two offered, or a random one
        CHECK(it->element_offer.size() == 2);
        CHECK(h.rc.PendingElementChoices().size() == 1);
        CHECK(view.detail.find("Choose its element: ") != std::string::npos);
        // A reroll keeps the sure refine.
        REQUIRE(h.rc.state().phase == Phase::Shop);
    }
    SECTION("a refine is offered for any gun that can go up, not only the best") {
        // 2026-11-11, the user: "the flamethrower never got a chance to get refinement, only
        // upgrades": every reward refine went to the best gun.
        std::set<std::string> offered;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
                economy["refine"]["reward_rating"] = "C";  // any rating: a sure refine every arena
            })));
            REQUIRE(h.rc.StartRun("alpha", seed).ok());  // the common pistol
            REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
            REQUIRE(h.rc.DevGrantWeapon(Gun("rifle", "w_top", Rarity::Legendary)).ok());  // the top: never
            test::ClearArena(h.rc, 0.0);
            const RewardChoice& refine = h.rc.state().reward_offer.back();
            REQUIRE(refine.kind == RewardKind::Refine);
            CHECK(refine.target_instance_id != "w_top");
            offered.insert(refine.target_instance_id);
        }
        CHECK(offered.size() == 2);  // the common pistol too, not only the rare shotgun
    }
    SECTION("a top-rated arena's refine can be a chance") {
        test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
            economy["refine"]["reward_rating"] = "C";  // any rating
            economy["refine"]["reward_rated_chance"] = 0.0;
        })));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
        test::ClearArena(h.rc, 0.0);
        const bool any = std::any_of(h.rc.state().reward_offer.begin(), h.rc.state().reward_offer.end(),
                                     [](const RewardChoice& c) { return c.kind == RewardKind::Refine; });
        CHECK_FALSE(any);
    }
    SECTION("a longer run's refine chances are spread over its arenas") {
        const auto refineShops = [](int arenas_it_is_set_for) {
            test::Harness h(test::Load(RemakeSources([arenas_it_is_set_for](nlohmann::json&, nlohmann::json& economy) {
                economy["refine"]["shop_chance"] = 1.0;
                economy["refine"]["reward_rating"] = "S";
                economy["refine"]["chances_for_arenas"] = arenas_it_is_set_for;
            })));
            int seen = 0;
            for (std::uint64_t seed = 1; seed <= 60; ++seed) {
                REQUIRE(h.rc.StartRun("alpha", seed).ok());
                REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
                test::ClearArena(h.rc, 0.0);
                REQUIRE(h.rc.ContinueToRewards().ok());
                if (h.rc.state().phase == Phase::RewardSelection) REQUIRE(h.rc.ChooseReward(0).ok());
                const auto& offers = h.rc.state().shop_offers;
                seen += std::any_of(offers.begin(), offers.end(), [](const ShopOffer& o) { return o.kind == ShopOfferKind::Refine; }) ? 1 : 0;
                (void)h.rc.AbandonRun(1.0);
                REQUIRE(h.rc.EndRun().ok());
            }
            return seen;
        };
        const int arena_count = 4;  // the test run's length
        CHECK(refineShops(arena_count) == 60);
        const int spread = refineShops(1);  // set for one arena: a quarter of the chance here
        CHECK(spread > 3);
        CHECK(spread < 35);
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

TEST_CASE("rarity remake: a build's own elements, the arena's element and cheap refines", "[weapons][rarity]") {
    const auto withBuild = [](nlohmann::json build) {
        auto sources = RemakeSources();
        nlohmann::json characters = nlohmann::json::parse(sources.characters);
        characters["characters"][0]["skins"] = nlohmann::json::array({nlohmann::json{{"id", "alpha_base"}, {"name", "Base"}, {"build", build}}});
        sources.characters = characters.dump();
        return test::Load(sources);
    };
    SECTION("gun_elements: every gun burns") {
        test::Harness h(withBuild({{"name", "Burner"}, {"gun_elements", {"incendiary"}}}));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        HitContext hit;
        hit.now = 1.0;
        hit.weapon_id = "pistol";
        hit.base_damage = 100.0;
        const OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
        REQUIRE(FindProc(r.procs, ProcKind::Burn) != nullptr);
        CHECK(FindProc(r.procs, ProcKind::Burn)->dps == Approx(25.0));
    }
    SECTION("arena_element: one random element each arena, named in the snapshot") {
        test::Harness h(withBuild({{"name", "Trials"}, {"arena_element", true}}));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        CHECK(h.rc.state().arena_element >= 0);
        CHECK_FALSE(h.rc.Snapshot(1.0).arena_element.empty());
    }
    SECTION("refine_every_shop and refine_price_mult") {
        test::Harness h(test::Load(RemakeSources([](nlohmann::json&, nlohmann::json& economy) {
            economy["refine"]["shop_chance"] = 0.0;
        })));
        test::Harness cheap(withBuild({{"name", "Lab"}, {"refine_every_shop", true}, {"refine_price_mult", 0.5}}));
        for (test::Harness* x : {&h, &cheap}) {
            REQUIRE(x->rc.StartRun("alpha", 1).ok());
            test::ClearArena(x->rc, 0.0);
            REQUIRE(x->rc.ContinueToRewards().ok());
            REQUIRE(x->rc.ChooseReward(0).ok());
        }
        const auto refineOf = [](const RunController& rc) {
            const auto& offers = rc.state().shop_offers;
            return std::find_if(offers.begin(), offers.end(), [](const ShopOffer& o) { return o.kind == ShopOfferKind::Refine; });
        };
        CHECK(refineOf(h.rc) == h.rc.state().shop_offers.end());
        REQUIRE(refineOf(cheap.rc) != cheap.rc.state().shop_offers.end());
        CHECK(refineOf(cheap.rc)->price == 20);  // half of 40
    }
    SECTION("an unknown element is refused") {
        auto sources = RemakeSources();
        nlohmann::json characters = nlohmann::json::parse(sources.characters);
        characters["characters"][0]["skins"] =
            nlohmann::json::array({nlohmann::json{{"id", "alpha_base"}, {"name", "Base"}, {"build", {{"gun_elements", {"plasma"}}}}}});
        sources.characters = characters.dump();
        const auto r = Content::LoadFromStrings(sources);
        CHECK(std::any_of(r.errors.begin(), r.errors.end(), [](const std::string& e) { return e.find("unknown element") != std::string::npos; }));
    }
}

TEST_CASE("a shop's refine chance goes by the run's length", "[weapons][rarity]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const EconomyConfig::Refine& refine = r.content->economy().refine;
    CHECK(RefineShopChance(refine, "short", 4) == Catch::Approx(0.30));
    CHECK(RefineShopChance(refine, "long", 8) == Catch::Approx(0.20));
    CHECK(RefineShopChance(refine, "marathon", 12) == Catch::Approx(0.15));
    CHECK(RefineShopChance(refine, "no such length", 8) == Catch::Approx(0.15));  // no entry: spread over its arenas
}

namespace {

// The fixture's elements with an evolution each (weapons.json "evolved"), as the shipped ones.
void AddEvolutions(nlohmann::json& weapons, nlohmann::json&) {
    const nlohmann::json evolved = nlohmann::json::parse(R"({
      "incendiary": {"name": "Inferno", "seconds": 5, "dps_fraction": 0.35, "spread_radius": 300},
      "shock": {"name": "Storm", "chance": 1.0, "damage_fraction": 1.0, "radius": 800, "targets": 4},
      "cryo": {"name": "Absolute Zero", "chance": 1.0, "seconds": 4, "damage_fraction": 0.5, "damage_cap": 1500,
               "radius": 350, "shatter_freeze_seconds": 3},
      "explosive": {"name": "Chain Reaction", "damage_fraction": 0.6, "damage_cap": 1800, "radius": 450, "chain": 2},
      "toxic": {"name": "Plague", "seconds": 6, "dps_fraction": 0.15, "radius": 400, "cloud_dps_fraction": 0.03}})");
    for (auto& e : weapons["elements"]) e["evolved"] = evolved[e["element"].get<std::string>()];
}

}  // namespace

// 2026-11-11, the user: "when you reach second to last refinement you get a list of 2 random
// elementals and an option to assign random ... when you upgrade it to the last the element you
// choose gets an upgrade and it evolves".
TEST_CASE("elements: a Legendary gun's element plays evolved", "[weapons][rarity]") {
    test::Harness h(test::Load(RemakeSources(AddEvolutions)));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    WeaponInstance fire = Gun("shotgun", "w_fire", Rarity::Legendary, {Element::Incendiary});
    fire.element_evolved = true;
    REQUIRE(h.rc.DevGrantWeapon(fire).ok());
    HitContext hit;
    hit.now = 1.0;
    hit.weapon_id = "shotgun";
    hit.base_damage = 200.0;
    const OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
    const WeaponProc* inferno = FindProc(r.procs, ProcKind::IgniteAround);  // Inferno spreads
    REQUIRE(inferno != nullptr);
    CHECK(inferno->dps == Approx(70.0));
    CHECK(inferno->seconds == Approx(5.0));
    CHECK(inferno->radius == Approx(300.0));
    CHECK(inferno->element == "incendiary");
    CHECK(inferno->evolved);
    CHECK(FindProc(r.procs, ProcKind::Burn) == nullptr);

    // Storm: the evolved Shock's numbers.
    WeaponInstance storm = Gun("pistol", "w_storm", Rarity::Legendary, {Element::Shock});
    storm.element_evolved = true;
    REQUIRE(h.rc.DevGrantWeapon(storm).ok());
    hit.weapon_id = "pistol";
    const WeaponProc* arc = FindProc(h.rc.ComputeOutgoingMultiplier(hit).procs, ProcKind::Shock);
    REQUIRE(arc != nullptr);
    CHECK(arc->damage == Approx(200.0));
    CHECK(arc->targets == 4);
    CHECK(arc->radius == Approx(800.0));

    // Chain Reaction, Absolute Zero's shatter and Plague's cloud on a kill.
    WeaponInstance boom = Gun("shotgun", "w_boom", Rarity::Legendary, {Element::Explosive});
    boom.element_evolved = true;
    REQUIRE(h.rc.DevGrantWeapon(boom).ok());
    KillEvent k;
    k.now = 2.0;
    k.by_player = true;
    k.weapon_id = "shotgun";
    k.enemy_id = "e1";
    k.target_hp_max = 1000.0;
    KillOutcome out = h.rc.OnEnemyKilled(k);
    const WeaponProc* blast = FindProc(out.procs, ProcKind::Explode);
    REQUIRE(blast != nullptr);
    CHECK(blast->damage == Approx(600.0));
    CHECK(blast->radius == Approx(450.0));
    CHECK(blast->chain == 2);

    WeaponInstance ice = Gun("rifle", "w_ice", Rarity::Legendary, {Element::Cryo});
    ice.element_evolved = true;
    REQUIRE(h.rc.DevGrantWeapon(ice).ok());
    k.weapon_id = "rifle";
    k.enemy_id = "e2";
    k.target_frozen = true;
    out = h.rc.OnEnemyKilled(k);
    const WeaponProc* shatter = FindProc(out.procs, ProcKind::Shatter);
    REQUIRE(shatter != nullptr);
    CHECK(shatter->freeze_seconds == Approx(3.0));
    CHECK(shatter->damage == Approx(500.0));
    // Another gun's kill of a frozen enemy: the plain shatter, no freeze.
    k.weapon_id = "pistol";
    k.enemy_id = "e3";
    out = h.rc.OnEnemyKilled(k);
    REQUIRE(FindProc(out.procs, ProcKind::Shatter) != nullptr);
    CHECK(FindProc(out.procs, ProcKind::Shatter)->freeze_seconds == Approx(0.0));

    // Plague: its poison carries the cloud the poisoned enemy's death lets out (the adapter's, as a
    // poison tick's kill has no gun).
    WeaponInstance plague = Gun("rifle", "w_plague", Rarity::Legendary, {Element::Toxic});
    plague.element_evolved = true;
    REQUIRE(h.rc.DevGrantWeapon(plague).ok());
    hit.weapon_id = "rifle";
    const WeaponProc* poison = FindProc(h.rc.ComputeOutgoingMultiplier(hit).procs, ProcKind::Poison);
    REQUIRE(poison != nullptr);
    CHECK(poison->dps == Approx(30.0));  // 0.15 of the 200 dealt
    CHECK(poison->seconds == Approx(6.0));
    CHECK(poison->cloud_dps_fraction == Approx(0.03));
    CHECK(poison->cloud_radius == Approx(400.0));
    k.weapon_id = "rifle";
    k.enemy_id = "e4";
    k.target_frozen = false;
    CHECK(FindProc(h.rc.OnEnemyKilled(k).procs, ProcKind::PoisonCloud) == nullptr);
    // The flamethrower's stream asks for a gun's elements.
    CHECK(h.rc.ElementsOf("rifle") == std::vector<std::pair<Element, bool>>{{Element::Toxic, true}});
    CHECK(h.rc.ElementsOf("nothing").empty());
}

TEST_CASE("elements: a gun refined to Epic waits for its element, chosen or random", "[weapons][rarity]") {
    const auto content = test::Load(RemakeSources(AddEvolutions));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 5).ok());
    REQUIRE(h.rc.DevGrantWeapon(Gun("shotgun", "w_sg", Rarity::Rare)).ok());
    CHECK(h.rc.PendingElementChoices().empty());
    REQUIRE(h.rc.DevRefineWeapon(std::string("shotgun")).ok());
    auto pending = h.rc.PendingElementChoices();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].instance_id == "w_sg");
    CHECK_FALSE(pending[0].evolved);
    REQUIRE(pending[0].offer.size() == 2);
    const Element second = pending[0].offer[1];
    CHECK(h.rc.ChooseElement("w_sg", 2).code == ErrorCode::InvalidIndex);
    CHECK(h.rc.ChooseElement("nope", 0).code == ErrorCode::UnknownId);
    REQUIRE(h.rc.ChooseElement("w_sg", 1).ok());
    CHECK(h.rc.PendingElementChoices().empty());
    const auto gun = [&h](const char* id) {
        return *std::find_if(h.rc.state().weapons.begin(), h.rc.state().weapons.end(),
                             [id](const WeaponInstance& w) { return w.instance_id == id; });
    };
    CHECK(gun("w_sg").elements == std::vector<Element>{second});
    CHECK(h.rc.ChooseElement("w_sg", 0).code == ErrorCode::NotEligible);  // chosen already
    // Then Legendary: it evolves.
    REQUIRE(h.rc.DevRefineWeapon(std::string("shotgun")).ok());
    CHECK(gun("w_sg").elements == std::vector<Element>{second});
    CHECK(gun("w_sg").element_evolved);

    // Random: any element, each as likely, never the build's own.
    std::set<Element> drawn;
    for (int i = 0; i < 40; ++i) {
        const std::string id = "w_r" + std::to_string(i);
        WeaponInstance w = Gun("pistol", id, Rarity::Epic);
        w.element_offer = {Element::Incendiary, Element::Toxic};
        REQUIRE(h.rc.DevGrantWeapon(w).ok());
        REQUIRE(h.rc.ChooseElement(id, -1).ok());
        drawn.insert(gun(id.c_str()).elements.at(0));
    }
    CHECK(drawn.size() == 5);  // the three not offered come up too
}

TEST_CASE("elements: the offer leans to the common ones and skips the build's", "[weapons][rarity]") {
    const auto content = test::Load(RemakeSources([](nlohmann::json& weapons, nlohmann::json& economy) {
        AddEvolutions(weapons, economy);
        for (auto& e : weapons["elements"]) e["weight"] = e["element"] == "explosive" ? 1 : 30;
    }));
    const Capabilities caps = Capabilities::All();
    Rng rng(11);
    int explosive = 0;
    for (int i = 0; i < 400; ++i) {
        const std::vector<Element> offer = OfferElements(rng, *content, {Element::Cryo}, 2);
        REQUIRE(offer.size() == 2);
        CHECK(offer[0] != offer[1]);
        CHECK(std::find(offer.begin(), offer.end(), Element::Cryo) == offer.end());
        explosive += static_cast<int>(std::count(offer.begin(), offer.end(), Element::Explosive));
    }
    CHECK(explosive < 40);  // weight 1 of 91: in about 2 of 100 offers' first pick, a few more second
    (void)caps;
}

TEST_CASE("elements: a resumed run keeps a gun's evolution and its waiting offer", "[weapons][rarity][persistence]") {
    test::Harness h(test::Load(RemakeSources(AddEvolutions)));
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    WeaponInstance w = Gun("shotgun", "w_wait", Rarity::Legendary);
    w.element_evolved = true;
    w.element_offer = {Element::Shock, Element::Toxic};
    REQUIRE(h.rc.DevGrantWeapon(w).ok());
    RunState back;
    REQUIRE(ParseRunResume(SerializeRunResume(h.rc.state()), back).ok());
    const auto it = std::find_if(back.weapons.begin(), back.weapons.end(), [](const WeaponInstance& x) { return x.instance_id == "w_wait"; });
    REQUIRE(it != back.weapons.end());
    CHECK(it->element_evolved);
    CHECK(it->element_offer == std::vector<Element>{Element::Shock, Element::Toxic});
}

TEST_CASE("elements: the shipped ones each evolve, the rarer ones weigh less", "[weapons][rarity][content]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const Content& c = *r.content;
    for (Element e : kAllElements) {
        INFO(ElementName(e));
        const ElementDef* base = c.FindElement(e);
        const ElementDef* evolved = c.FindElement(e, true);
        REQUIRE(base != nullptr);
        REQUIRE(evolved != nullptr);
        CHECK(evolved != base);
        CHECK(evolved->name != base->name);
    }
    CHECK(c.FindElement(Element::Explosive)->weight < c.FindElement(Element::Incendiary)->weight);
    CHECK(c.FindElement(Element::Cryo)->weight < c.FindElement(Element::Toxic)->weight);
    // Dragon's Breath and Chain Lightning are Inferno and Storm now.
    CHECK(c.FindUnique("dragons_breath") == nullptr);
    CHECK(c.FindUnique("chain_lightning") == nullptr);
    const auto& rules = c.economy().weapon_rarity;
    CHECK(rules[RarityIndex(Rarity::Legendary)].unique_count == 0);
    CHECK(rules[RarityIndex(Rarity::Exotic)].unique_count == 1);
    CHECK(rules[RarityIndex(Rarity::Exotic)].element_count == 1);
}
