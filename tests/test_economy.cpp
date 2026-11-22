#include <algorithm>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using nlohmann::json;

TEST_CASE("ledger is idempotent per transaction id and rejects overdrafts", "[economy]") {
    Ledger l(10);
    CHECK(l.Credit("a", 5) == TxResult::Applied);
    CHECK(l.balance() == 15);
    CHECK(l.Credit("a", 5) == TxResult::AlreadyApplied);
    CHECK(l.Credit("a", 999) == TxResult::AlreadyApplied);
    CHECK(l.balance() == 15);
    CHECK(l.Debit("b", 20) == TxResult::InsufficientFunds);
    CHECK_FALSE(l.Contains("b"));
    CHECK(l.Debit("b", 15) == TxResult::Applied);
    CHECK(l.balance() == 0);
    CHECK(l.Debit("b", 15) == TxResult::AlreadyApplied);
    CHECK(l.Debit("c", 1) == TxResult::InsufficientFunds);
    CHECK(l.Credit("d", -1) == TxResult::InvalidAmount);
    CHECK(l.Debit("e", -1) == TxResult::InvalidAmount);
    CHECK(l.Credit("", 1) == TxResult::InvalidId);
    CHECK(l.balance() == 0);
    CHECK(l.Credit("big", std::numeric_limits<std::int64_t>::max()) == TxResult::Applied);
    CHECK(l.Credit("overflow", 1) == TxResult::InvalidAmount);

    Ledger r(0);
    REQUIRE(r.Credit("in", 10) == TxResult::Applied);
    REQUIRE(r.Debit("out", 4) == TxResult::Applied);
    CHECK(r.Rollback("out"));
    CHECK(r.balance() == 10);
    CHECK_FALSE(r.Contains("out"));
    CHECK(r.Debit("spend", 10) == TxResult::Applied);
    CHECK_FALSE(r.Rollback("in"));  // would go negative
    CHECK_FALSE(r.Rollback("unknown"));
}

TEST_CASE("rating grades time against par and damage", "[economy]") {
    const EconomyConfig::RatingRules rules;
    CHECK(ComputeRating(rules, 90, 100, 0, 100, 0) == Rating::S);
    CHECK(ComputeRating(rules, 140, 100, 0, 100, 0) == Rating::A);
    CHECK(ComputeRating(rules, 90, 100, 20, 100, 0) == Rating::A);
    CHECK(ComputeRating(rules, 140, 100, 20, 100, 0) == Rating::B);
    CHECK(ComputeRating(rules, 200, 100, 80, 100, 0) == Rating::C);
    CHECK(ComputeRating(rules, 10, 100, 0, 100, 1) == Rating::C);
}

TEST_CASE("character unlocks spend credits transactionally", "[economy]") {
    Profile profile;
    test::Harness h(test::LoadFixture(), profile);
    CHECK(h.rc.StartRun("gamma", 1).code == ErrorCode::Locked);
    CHECK(h.rc.StartRun("delta", 1).code == ErrorCode::Disabled);
    CHECK(h.rc.StartRun("nobody", 1).code == ErrorCode::UnknownId);
    CHECK(h.rc.PurchaseUnlock(UnlockKind::Character, "gamma").code() == ErrorCode::InsufficientFunds);
    CHECK(h.rc.PurchaseUnlock(UnlockKind::Character, "delta").code() == ErrorCode::Disabled);
    CHECK(h.rc.PurchaseUnlock(UnlockKind::Character, "alpha").code() == ErrorCode::NotUnlockable);

    // Earn credits through a full run.
    REQUIRE(h.rc.StartRun("alpha", 11).ok());
    CHECK(h.rc.PurchaseUnlock(UnlockKind::Character, "gamma").code() == ErrorCode::RunInProgress);
    for (int arena = 0; arena < 3; ++arena) {
        test::ClearArena(h.rc, arena * 100.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        if (arena < 2) {
            REQUIRE(h.rc.ChooseReward(0).ok());
            REQUIRE(h.rc.LeaveShop().ok());
            REQUIRE(h.rc.ChooseRoute(0).ok());
        }
    }
    REQUIRE(h.rc.phase() == Phase::RunComplete);
    const Result<RunSummary> summary = h.rc.EndRun();
    REQUIRE(summary.ok());
    const std::int64_t balance = h.rc.profile().credits.balance();
    REQUIRE(balance >= 100);

    const Result<UnlockReceipt> receipt = h.rc.PurchaseUnlock(UnlockKind::Character, "gamma");
    REQUIRE(receipt.ok());
    CHECK(receipt->cost == 100);
    CHECK(h.rc.profile().credits.balance() == balance - 100);
    CHECK(h.rc.profile().unlocked_characters.count("gamma") == 1);
    CHECK(h.rc.profile().credits.Contains("unlock:character:gamma"));
    CHECK(h.rc.PurchaseUnlock(UnlockKind::Character, "gamma").code() == ErrorCode::AlreadyUnlocked);
    CHECK(h.rc.profile().credits.balance() == balance - 100);
    CHECK(h.rc.StartRun("gamma", 2).ok());
    CHECK(h.sink.Count(EventType::UnlockPurchased) == 1);
}

TEST_CASE("perk and weapon unlocks gate offers", "[economy]") {
    Profile rich;
    rich.credits = Ledger(1000);
    test::Harness h(test::LoadFixture(), rich);
    REQUIRE(h.rc.PurchaseUnlock(UnlockKind::Perk, "jackpot").ok());
    REQUIRE(h.rc.PurchaseUnlock(UnlockKind::Weapon, "rifle").ok());
    CHECK(h.rc.profile().credits.balance() == 850);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    // Unlocked but not equipped: not in the run until granted.
    CHECK(h.rc.state().perks.count("jackpot") == 0);
    CHECK(h.rc.DevGrantPerk("jackpot").ok());
}

TEST_CASE("developer supplies are credited once per call", "[economy]") {
    test::Harness h(test::LoadFixture());
    CHECK(h.rc.DevGrantSupplies(50).code == ErrorCode::NoActiveRun);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    const std::int64_t before = h.rc.state().supplies.balance();
    REQUIRE(h.rc.DevGrantSupplies(50).ok());
    REQUIRE(h.rc.DevGrantSupplies(50).ok());
    CHECK(h.rc.state().supplies.balance() == before + 100);
    CHECK(h.rc.DevGrantSupplies(0).code == ErrorCode::InvalidAmount);
}

TEST_CASE("supplies from the game's own shop screen are credited or debited between arenas", "[economy]") {
    test::Harness h(test::LoadFixture());
    CHECK(h.rc.AdjustSupplies(10, "native_shop").code == ErrorCode::NoActiveRun);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    CHECK(h.rc.AdjustSupplies(10, "native_shop").code == ErrorCode::WrongPhase);  // not before an arena
    test::ClearArena(h.rc, 0.0);
    CHECK(h.rc.AdjustSupplies(10, "native_shop").ok());  // results screen onwards
    const std::int64_t balance = h.rc.state().supplies.balance();
    REQUIRE(h.rc.AdjustSupplies(25, "native_shop").ok());
    REQUIRE(h.rc.AdjustSupplies(-15, "native_shop").ok());
    CHECK(h.rc.state().supplies.balance() == balance + 10);
    CHECK(h.rc.AdjustSupplies(-(balance + 1000), "native_shop").code == ErrorCode::InsufficientFunds);
    CHECK(h.rc.state().supplies.balance() == balance + 10);
    CHECK(h.rc.AdjustSupplies(0, "native_shop").ok());
}

TEST_CASE("unlock is rolled back when the profile cannot be saved", "[economy]") {
    test::TempDir dir("unlock_rollback");
    // A directory where the profile file should be makes the final rename fail.
    const std::filesystem::path main = dir.path() / "profile.json";
    std::filesystem::create_directories(main);
    ProfileStore store(main);
    Profile rich;
    rich.credits = Ledger(500);
    test::Harness h(test::LoadFixture(), rich, Capabilities::All(), &store);
    const Result<UnlockReceipt> r = h.rc.PurchaseUnlock(UnlockKind::Character, "gamma");
    CHECK(r.code() == ErrorCode::PersistenceFailed);
    CHECK(h.rc.profile().credits.balance() == 500);
    CHECK(h.rc.profile().unlocked_characters.count("gamma") == 0);
    CHECK_FALSE(h.rc.profile().credits.Contains("unlock:character:gamma"));
}

namespace {

/// Plays the first arena and enters the shop.
void ToShop(test::Harness& h, std::uint64_t seed) {
    REQUIRE(h.rc.StartRun("alpha", seed).ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    REQUIRE(h.rc.phase() == Phase::Shop);
}

std::size_t FindOffer(const RunController& rc, ShopOfferKind kind, std::size_t start = 0) {
    const auto& offers = rc.ShopOffers();
    for (std::size_t i = start; i < offers.size(); ++i) {
        if (offers[i].kind == kind) return i;
    }
    return offers.size();
}

}  // namespace

TEST_CASE("shop purchases validate phase, index, stock and balance", "[shop]") {
    test::Harness h;
    CHECK(h.rc.Purchase(0).code == ErrorCode::NoActiveRun);
    ToShop(h, 21);
    const auto& offers = h.rc.ShopOffers();
    REQUIRE_FALSE(offers.empty());
    CHECK(FindOffer(h.rc, ShopOfferKind::Heal) < offers.size());
    CHECK(FindOffer(h.rc, ShopOfferKind::RerollToken) < offers.size());
    CHECK(h.rc.Purchase(offers.size()).code == ErrorCode::InvalidIndex);

    const std::size_t reroll = FindOffer(h.rc, ShopOfferKind::RerollToken);
    const std::int64_t before = h.rc.state().supplies.balance();
    REQUIRE(h.rc.Purchase(reroll).ok());
    CHECK(h.rc.state().supplies.balance() == before - offers[reroll].price);
    CHECK(h.rc.state().reroll_tokens == 1);
    CHECK(h.rc.Purchase(reroll).code == ErrorCode::AlreadyBought);

    const std::size_t heal = FindOffer(h.rc, ShopOfferKind::Heal);
    REQUIRE(h.rc.Purchase(heal).ok());
    CHECK(h.rc.PendingHeal() == Catch::Approx(0.5));

    // Spend down until something is unaffordable.
    bool saw_insufficient = false;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (offers[i].purchased) continue;
        const Status s = h.rc.Purchase(i);
        if (s.code == ErrorCode::InsufficientFunds) saw_insufficient = true;
        CHECK(h.rc.state().supplies.balance() >= 0);
    }
    REQUIRE(h.rc.LeaveShop().ok());
    CHECK(h.rc.Purchase(0).code == ErrorCode::WrongPhase);
    (void)saw_insufficient;
}

TEST_CASE("shop heal offer requires the heal capability", "[shop]") {
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kHeal);
    test::Harness h(test::LoadFixture(), {}, caps);
    ToShop(h, 21);
    CHECK(FindOffer(h.rc, ShopOfferKind::Heal) == h.rc.ShopOffers().size());
}

TEST_CASE("shop purchase is rolled back when the grant fails", "[shop]") {
    // Only the mutually exclusive pair is offerable, so the second perk purchase must fail.
    ContentSources src = test::EditFixture([](json& perks, json&, json&, json&, json& eco) {
        for (json& p : perks["perks"]) {
            const std::string id = p["id"];
            if (id != "berserk" && id != "turtle" && id != "curse_weak" && id != "beta_passive") p["enabled"] = false;
        }
        eco["run"]["starting_supplies"] = 500;
        eco["rewards"]["kind_weights"] = {{"perk", 0}, {"weapon", 0}, {"supplies", 1}};
    });
    test::Harness h(test::Load(src));
    ToShop(h, 3);
    const std::size_t first = FindOffer(h.rc, ShopOfferKind::Perk);
    const std::size_t second = FindOffer(h.rc, ShopOfferKind::Perk, first + 1);
    REQUIRE(second < h.rc.ShopOffers().size());
    REQUIRE(h.rc.Purchase(first).ok());
    const std::int64_t balance = h.rc.state().supplies.balance();
    const std::int64_t spent = h.rc.state().supplies_spent;
    const Status s = h.rc.Purchase(second);
    CHECK(s.code == ErrorCode::GrantFailed);
    CHECK(s.message.find("excluded") != std::string::npos);
    CHECK(h.rc.state().supplies.balance() == balance);
    CHECK(h.rc.state().supplies_spent == spent);
    CHECK_FALSE(h.rc.ShopOffers()[second].purchased);
    CHECK(h.rc.state().perks.size() == 1);
    // The rollback is visible in telemetry.
    bool rollback_event = false;
    for (const Event& e : h.sink.events) {
        const FieldValue* reason = e.Find("reason");
        if (e.type == EventType::CurrencyChanged && reason != nullptr &&
            std::get<std::string>(*reason) == "rollback") {
            rollback_event = true;
        }
    }
    CHECK(rollback_event);
}

TEST_CASE("weapon upgrade purchase raises the level", "[shop]") {
    test::Harness h;
    ToShop(h, 21);
    const std::size_t up = FindOffer(h.rc, ShopOfferKind::WeaponUpgrade);
    REQUIRE(up < h.rc.ShopOffers().size());
    const std::string target = h.rc.ShopOffers()[up].target_instance_id;
    REQUIRE(h.rc.Purchase(up).ok());
    for (const WeaponInstance& w : h.rc.state().weapons) {
        if (w.instance_id == target) CHECK(w.level == 2);
    }
}

TEST_CASE("the flamethrower's upgrade says what its level adds besides damage", "[shop]") {
    // 2026-11-22, the user: "level ups get more fuel and faster refil and longer reach" (weapons.json per_level).
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const WeaponBaseDef* flame = r.content->FindWeapon("flamethrower");
    REQUIRE(flame != nullptr);
    CHECK(flame->per_level.fuel_pct > 0.0);
    CHECK(flame->per_level.refill_pct > 0.0);
    CHECK(flame->per_level.reach_m > 0.0);
    CHECK_FALSE(r.content->FindWeapon("m92f")->per_level.Any());
    // The tester holds the flamethrower alone, so the shop's upgrade is its own (a few seeds: an upgrade
    // is not in every shop).
    bool seen = false;
    for (std::uint64_t seed = 1; seed < 40 && !seen; ++seed) {
        test::Harness h(r.content);
        h.rc.SetTesterCharacters(true);
        REQUIRE(h.rc.StartRun("tester", seed).ok());
        test::ClearArena(h.rc, 0.0);
        REQUIRE(h.rc.ContinueToRewards().ok());
        REQUIRE(h.rc.ChooseReward(0).ok());
        if (h.rc.phase() != Phase::Shop) continue;
        for (const auto& offer : h.rc.Snapshot(1.0).shop_offers) {
            if (offer.title.rfind("Upgrade Flamethrower", 0) != 0) continue;
            seen = true;
            INFO(offer.detail);
            CHECK(offer.detail.find("fuel +") != std::string::npos);
            CHECK(offer.detail.find("refill +") != std::string::npos);
            CHECK(offer.detail.find("reach +1.25 m") != std::string::npos);  // 10 m at level 5 (the user, 2026-11-22)
        }
    }
    CHECK(seen);
    // Out of range: refused.
    const ContentLoadResult bad = Content::LoadFromStrings(test::EditFixture([](json&, json& weapons, json&, json&, json&) {
        weapons["weapons"][0]["per_level"] = json{{"reach_m", 9.0}};
    }));
    CHECK(std::any_of(bad.errors.begin(), bad.errors.end(), [](const std::string& e) { return e.find("reach_m") != std::string::npos; }));
}

TEST_CASE("the special guns' levels: crit and supplies by level, the shipped perks, the loader's checks", "[shop][weapons]") {
    // 2026-11-22, the user's picks for the guns the game gives no levels (weapons.json per_level).
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const auto perks = [&](const char* id) { return r.content->FindWeapon(id)->per_level; };
    CHECK(perks("flamethrower").reach_m * 4 + 5.0 == Catch::Approx(10.0));  // "10m at 5"
    CHECK(perks("rocket_launcher").extra_every_levels == 2);                // 2 rockets at level 3, 3 at 5
    CHECK(perks("rocket_launcher").blast_m > 0.0);
    CHECK(perks("rocket_launcher").free_round_pct == 0.0);                  // no refund
    for (const char* id : {"grenade_launcher"}) {
        CHECK(perks(id).free_round_pct > 0.0);
        CHECK(perks(id).blast_m > 0.0);
        CHECK(perks(id).status_pct > 0.0);
        CHECK(perks(id).magazine > 0);
    }
    CHECK(perks("gatling").ramp_pct > 0.0);
    CHECK(perks("gatling").stagger_pct > 0.0);
    CHECK(perks("gatling").crit_pct > 0.0);
    CHECK(perks("longbow").pierce_pct > 0.0);
    CHECK(perks("longbow").explode_every > 0);
    CHECK(perks("longbow").headshot_refund_pct == 0.0);                     // its arrows are infinite
    CHECK(perks("stun_rod").chain_targets > 0);
    CHECK(perks("stun_rod").stun_s > 0.0);
    CHECK(perks("stun_rod").stunned_damage_pct > 0.0);
    for (const char* id : {"px4", "samurai_edge"}) {
        CHECK(perks(id).crit_pct > 0.0);
        CHECK(perks(id).headshot_refund_pct > 0.0);
        CHECK(perks(id).supplies_per_kill > 0.0);
        CHECK(perks(id).magazine > 0);
    }

    // The core's two: a level 3 gun has 2 levels' crit chance and supplies a kill more than at level 1.
    const auto sources = test::EditFixture([](json&, json& weapons, json&, json&, json&) {
        for (json& w : weapons["weapons"]) {
            if (w["id"] == "rifle") w["per_level"] = json{{"crit_pct", 5}, {"supplies_per_kill", 1.0}};
        }
    });
    double crit[2] = {};
    std::int64_t supplies[2] = {};
    for (int i = 0; i < 2; ++i) {
        test::Harness h(test::Load(sources));
        REQUIRE(h.rc.StartRun("alpha", 1).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        WeaponInstance w = MakeStartingWeapon("rifle", "w_lv");
        w.level = i == 0 ? 1 : 3;
        REQUIRE(h.rc.DevGrantWeapon(w).ok());
        HitContext hit;
        hit.now = 1.0;
        hit.weapon_id = "rifle";  // not one alpha starts with
        hit.base_damage = 100.0;
        crit[i] = h.rc.ComputeOutgoingMultiplier(hit).breakdown.crit_chance;
        KillEvent k;
        k.now = 2.0;
        k.by_player = true;
        k.weapon_id = "rifle";
        k.enemy_id = "e1";
        k.target_hp_max = 1000.0;
        const KillOutcome out = h.rc.OnEnemyKilled(k);
        REQUIRE(out.counted);
        supplies[i] = out.supplies_awarded;
    }
    CHECK(crit[1] - crit[0] == Catch::Approx(0.10));
    CHECK(supplies[1] - supplies[0] == 2);

    // The loader: a blast needs its share, a chain its reach; magazines have a ceiling.
    const auto errorsWith = [](const json& perLevel, const char* word) {
        const ContentLoadResult bad = Content::LoadFromStrings(test::EditFixture([&](json&, json& weapons, json&, json&, json&) {
            weapons["weapons"][0]["per_level"] = perLevel;
        }));
        return std::any_of(bad.errors.begin(), bad.errors.end(), [&](const std::string& e) { return e.find(word) != std::string::npos; });
    };
    CHECK(errorsWith(json{{"blast_m", 1.0}}, "blast_fraction"));
    CHECK(errorsWith(json{{"chain_targets", 2}}, "chain_m"));
    CHECK(errorsWith(json{{"magazine", 99}}, "magazine"));
    CHECK(errorsWith(json{{"ramp_pct", 5}}, "ramp_seconds"));
    CHECK(errorsWith(json{{"pierce", 5}}, "pierce"));  // an unknown key
    CHECK_FALSE(errorsWith(json{{"crit_pct", 2.5}, {"headshot_refund_pct", 10}}, "per_level"));
}
