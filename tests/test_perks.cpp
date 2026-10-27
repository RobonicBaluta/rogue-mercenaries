#include <cmath>

#include <catch_amalgamated.hpp>

#include "rm/rewards.h"
#include "test_support.h"

using namespace rm;
using Catch::Approx;

namespace {

HitContext Hit(double now) {
    HitContext h;
    h.now = now;
    h.base_damage = 100.0;
    return h;
}

HitContext HitTarget(double now, double hp, double max) {
    HitContext h = Hit(now);
    h.target_hp = hp;
    h.target_hp_max = max;
    return h;
}

/// Started run with an active first arena and no starting weapons in play (melee hits).
void StartArena(test::Harness& h, const char* character = "alpha") {
    REQUIRE(h.rc.StartRun(character, 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
}

HitContext Melee(double now) {
    HitContext h = Hit(now);
    h.is_melee = true;
    return h;
}

}  // namespace

TEST_CASE("outgoing_damage_mult always: trigger and stacking", "[perks]") {
    test::Harness h;
    StartArena(h);
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(1)).multiplier == Approx(1.0));
    REQUIRE(h.rc.DevGrantPerk("dmg_up").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(1)).multiplier == Approx(1.1));
    REQUIRE(h.rc.DevGrantPerk("dmg_up").ok());
    REQUIRE(h.rc.DevGrantPerk("dmg_up").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(1)).multiplier == Approx(1.3));
    const Status fourth = h.rc.DevGrantPerk("dmg_up");
    CHECK(fourth.code == ErrorCode::NotEligible);
    CHECK(fourth.message.find("max_stacks") != std::string::npos);
    CHECK(h.rc.state().perks.at("dmg_up") == 3);
}

TEST_CASE("target_hp_below needs target hp below the threshold", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("execute").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(HitTarget(1, 20, 100)).multiplier == Approx(1.5));
    CHECK(h.rc.ComputeOutgoingMultiplier(HitTarget(1, 50, 100)).multiplier == Approx(1.0));
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(1)).multiplier == Approx(1.0));  // hp unknown -> no bonus
}

TEST_CASE("kill_streak bonus stacks per kill and expires with injected time", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("streak").ok());
    KillEvent k;
    k.by_player = true;
    k.now = 10.0;
    h.rc.OnEnemyKilled(k);
    k.now = 11.0;
    h.rc.OnEnemyKilled(k);
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(12.0)).breakdown.kill_streak_bonus == Approx(0.2));
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(15.5)).breakdown.kill_streak_bonus == Approx(0.1));  // first expired at 15
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(16.5)).breakdown.kill_streak_bonus == Approx(0.0));
    CHECK(h.rc.Snapshot(16.5).kill_streak_bonus == Approx(0.0));

    // Cap: max_bonus 0.3 with 5 kills inside the window.
    for (int i = 0; i < 5; ++i) {
        k.now = 20.0 + i * 0.1;
        h.rc.OnEnemyKilled(k);
    }
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(21.0)).breakdown.kill_streak_bonus == Approx(0.3));
    // Second stack: per_kill 0.15, max 0.5.
    REQUIRE(h.rc.DevGrantPerk("streak").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(21.0)).breakdown.kill_streak_bonus == Approx(0.5));
    const RunSnapshot snap = h.rc.Snapshot(21.0);
    CHECK(snap.kill_streak_bonus == Approx(0.5));
    CHECK(snap.kill_streak_kills == 5);
}

TEST_CASE("bloodhound kills extend kill-streak windows", "[perks][weapons]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("streak").ok());
    WeaponInstance w = MakeStartingWeapon("rifle", "w_bh");
    w.affixes.push_back(Affix{AffixKind::UniqueKillStreakExtend, 2.0, 0.0, "bloodhound"});
    REQUIRE(h.rc.DevGrantWeapon(w).ok());
    KillEvent k;
    k.by_player = true;
    k.now = 10.0;
    k.weapon_id = "pistol";
    h.rc.OnEnemyKilled(k);  // plain kill: window 10..15
    k.now = 12.0;
    k.weapon_id = "rifle";
    const KillOutcome out = h.rc.OnEnemyKilled(k);  // extends both by 2s
    CHECK(out.triggered == std::vector<std::string>{"bloodhound"});
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(16.0)).breakdown.kill_streak_bonus == Approx(0.2));  // 10..17, 12..19
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(18.0)).breakdown.kill_streak_bonus == Approx(0.1));
}

TEST_CASE("player_hp_below uses reported player health", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("desperate").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(1)).multiplier == Approx(1.0));  // hp unknown
    h.rc.OnPlayerDamaged(DamageEvent{1.0, 80.0, 100.0, 20.0, 100.0});
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(2)).multiplier == Approx(1.4));
    HitContext healthy = Melee(3);
    healthy.player_hp = 90.0;
    healthy.player_hp_max = 100.0;
    CHECK(h.rc.ComputeOutgoingMultiplier(healthy).multiplier == Approx(1.0));
}

TEST_CASE("after_melee only works when hits report melee", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("brawl").ok());
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(4.0)).multiplier == Approx(1.0));  // no melee yet
    CHECK(h.rc.ComputeOutgoingMultiplier(Melee(5.0)).multiplier == Approx(1.0));  // the melee hit itself
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(6.0)).multiplier == Approx(1.6));
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(8.0)).multiplier == Approx(1.6));
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(8.5)).multiplier == Approx(1.0));  // window 3s expired
    HitContext unknown = Hit(9.0);
    unknown.is_melee = std::nullopt;  // unknown is not melee
    h.rc.ComputeOutgoingMultiplier(unknown);
    CHECK(h.rc.ComputeOutgoingMultiplier(Hit(9.5)).multiplier == Approx(1.0));
}

TEST_CASE("incoming_damage_mult: always, stacking and player_hp_below", "[perks]") {
    test::Harness h;
    StartArena(h);
    IncomingContext in{1.0, 10.0, 100.0, 100.0};
    CHECK(h.rc.ComputeIncoming(in).multiplier == Approx(1.0));
    REQUIRE(h.rc.DevGrantPerk("armor").ok());
    CHECK(h.rc.ComputeIncoming(in).multiplier == Approx(0.8));
    REQUIRE(h.rc.DevGrantPerk("armor").ok());
    CHECK(h.rc.ComputeIncoming(in).multiplier == Approx(0.7));
    REQUIRE(h.rc.DevGrantPerk("low_armor").ok());
    CHECK(h.rc.ComputeIncoming(in).multiplier == Approx(0.7));
    IncomingContext low{2.0, 10.0, 20.0, 100.0};
    CHECK(h.rc.ComputeIncoming(low).multiplier == Approx(0.25));  // 1 - 0.3 - 0.5 = 0.2 -> clamped
    CHECK(h.rc.ComputeIncoming(low).raw_multiplier == Approx(0.2));
    REQUIRE(h.rc.DevGrantPerk("curse_weak").ok());
    CHECK(h.rc.ComputeIncoming(in).multiplier == Approx(0.95));
}

TEST_CASE("crit rolls on the combat stream deterministically", "[perks]") {
    auto crit_pattern = [](std::uint64_t seed) {
        test::Harness h;
        REQUIRE(h.rc.StartRun("alpha", seed).ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        REQUIRE(h.rc.DevGrantPerk("crit_up").ok());
        std::vector<bool> crits;
        int count = 0;
        for (int i = 0; i < 4000; ++i) {
            const OutgoingResult r = h.rc.ComputeOutgoingMultiplier(Melee(1.0 + i));
            crits.push_back(r.crit);
            if (r.crit) {
                ++count;
                CHECK(r.multiplier == Approx(2.0));
            } else {
                CHECK(r.multiplier == Approx(1.0));
            }
        }
        CHECK(count > 900);  // 25% of 4000 = 1000
        CHECK(count < 1100);
        return crits;
    };
    CHECK(crit_pattern(5) == crit_pattern(5));
    CHECK(crit_pattern(5) != crit_pattern(6));
}

TEST_CASE("heal_on_kill queues heal requests for the adapter", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("medic").ok());
    const KillOutcome k = test::KillN(h.rc, 2, 1.0);
    CHECK(k.heal_fraction_queued == Approx(0.05));
    CHECK(h.rc.PendingHeal() == Approx(0.1));
    const std::vector<HealRequest> heals = h.rc.ConsumeHealRequests();
    REQUIRE(heals.size() == 2);
    CHECK(heals[0].source == "medic");
    CHECK(h.rc.PendingHeal() == 0.0);
    CHECK(h.rc.ConsumeHealRequests().empty());
}

TEST_CASE("supplies_on_kill rolls the loot stream", "[perks]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 9).ok());
    REQUIRE(h.rc.DevGrantPerk("scav").ok());
    int procs = 0;
    for (int arena = 0; arena < 1; ++arena) {
        REQUIRE(h.rc.BeginArena(0.0).ok());
        for (int i = 0; i < 5; ++i) {
            KillEvent k;
            k.by_player = true;
            k.now = 1.0 + i;
            k.was_melee = true;  // no weapon affixes involved
            const KillOutcome out = h.rc.OnEnemyKilled(k);
            CHECK((out.supplies_awarded == 2 || out.supplies_awarded == 5));  // base 2, +3 on proc
            if (out.supplies_awarded == 5) ++procs;
        }
    }
    CHECK(h.rc.state().supplies.balance() == 50 + 5 * 2 + procs * 3);
    // Statistically: 50% over many draws (separate harness, long arena).
    auto content = test::Load(test::EditFixture([](auto&, auto&, auto&, auto& routes, auto&) {
        routes["routes"][0]["enemy_budget"] = 1000;
    }));
    test::Harness big(content);
    REQUIRE(big.rc.StartRun("alpha", 9).ok());
    REQUIRE(big.rc.DevGrantPerk("scav").ok());
    REQUIRE(big.rc.BeginArena(0.0).ok());
    int big_procs = 0;
    for (int i = 0; i < 1000; ++i) {
        KillEvent k;
        k.by_player = true;
        k.now = 1.0 + i;
        k.was_melee = true;
        if (big.rc.OnEnemyKilled(k).supplies_awarded == 5) ++big_procs;
    }
    CHECK(big_procs > 440);
    CHECK(big_procs < 560);
}

TEST_CASE("supplies_on_arena_clear honours require_no_damage", "[perks]") {
    for (const bool take_damage : {false, true}) {
        test::Harness h;
        REQUIRE(h.rc.StartRun("alpha", 3).ok());
        REQUIRE(h.rc.DevGrantPerk("sweeper").ok());
        REQUIRE(h.rc.DevGrantPerk("stocker").ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        if (take_damage) h.rc.OnPlayerDamaged(DamageEvent{1.0, 5.0, 100.0, 95.0, 100.0});
        KillEvent k;
        k.by_player = true;
        k.was_melee = true;
        for (int i = 0; i < 5; ++i) {
            k.now = 2.0 + i;
            h.rc.OnEnemyKilled(k);
        }
        const Result<ArenaResult> r = h.rc.CompleteArena(20.0);
        REQUIRE(r.ok());
        // kills 5*2 + clear 25 + stocker 10 (+ sweeper 20 only without damage)
        CHECK(r->supplies_awarded == (take_damage ? 45 : 65));
        CHECK(r->no_damage == !take_damage);
    }
}

TEST_CASE("cheat_death prevents one lethal hit per arena", "[perks]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 4).ok());
    REQUIRE(h.rc.DevGrantPerk("phoenix").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    CHECK(h.rc.Snapshot(0.0).cheat_death_remaining == 1);
    const IncomingResult safe = h.rc.ComputeIncoming(IncomingContext{1.0, 10.0, 100.0, 100.0});
    CHECK_FALSE(safe.prevent_death);
    const IncomingResult first = h.rc.ComputeIncoming(IncomingContext{2.0, 500.0, 40.0, 100.0});
    CHECK(first.prevent_death);
    CHECK(first.leave_hp == Approx(30.0));
    CHECK(h.rc.Snapshot(2.0).cheat_death_remaining == 0);
    const IncomingResult second = h.rc.ComputeIncoming(IncomingContext{3.0, 500.0, 30.0, 100.0});
    CHECK_FALSE(second.prevent_death);

    // Recharges in the next arena.
    test::KillN(h.rc, h.rc.RemainingEnemies(), 4.0);
    REQUIRE(h.rc.CompleteArena(10.0).ok());
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    REQUIRE(h.rc.LeaveShop().ok());
    REQUIRE(h.rc.ChooseRoute(0).ok());
    REQUIRE(h.rc.BeginArena(20.0).ok());
    CHECK(h.rc.ComputeIncoming(IncomingContext{21.0, 500.0, 40.0, 100.0}).prevent_death);
}

TEST_CASE("extreme multipliers are clamped and both values reported", "[perks]") {
    test::Harness h;
    StartArena(h);
    for (const char* id : {"dmg_up", "dmg_up", "dmg_up", "berserk", "execute", "jackpot"}) {
        h.rc.DevGrantPerk(id);  // jackpot is locked -> rejected, others apply
    }
    CHECK(h.rc.state().perks.count("jackpot") == 0);
    REQUIRE(h.rc.DevGrantPerk("streak").ok());
    REQUIRE(h.rc.DevGrantPerk("streak").ok());
    test::KillN(h.rc, 4, 1.0);
    WeaponInstance w = MakeStartingWeapon("pistol", "w_big");
    w.level = 5;
    w.affixes.push_back(Affix{AffixKind::DamagePct, 300.0, 0.0, ""});
    w.affixes.push_back(Affix{AffixKind::UniqueLowHpTargetBonus, 1.0, 0.2, "executioner_rounds"});
    REQUIRE(h.rc.DevGrantWeapon(w).ok());
    HitContext hit = HitTarget(2.0, 5.0, 100.0);
    hit.weapon_id = "pistol";
    const OutgoingResult r = h.rc.ComputeOutgoingMultiplier(hit);
    // perks: 1 + 0.3 + 1.0 + 0.5 = 2.8, streak 0.5 -> 3.3; weapon: 1 + 0.4 + 3.0 + 1.0 = 5.4
    CHECK(r.breakdown.raw == Approx(3.3 * 5.4));
    CHECK(r.breakdown.clamped == Approx(5.0));
    CHECK(r.multiplier == Approx(5.0));
    CHECK(h.sink.Count(EventType::DamageModified) == 1);  // first clamped hit is always logged

    test::Harness low;
    StartArena(low);
    REQUIRE(low.rc.DevGrantPerk("curse_soft").ok());
    WeaponInstance neg = MakeStartingWeapon("pistol", "w_neg");
    neg.affixes.push_back(Affix{AffixKind::DamagePct, -95.0, 0.0, ""});
    REQUIRE(low.rc.DevGrantWeapon(neg).ok());
    HitContext lh = Hit(1.0);
    lh.weapon_id = "pistol";
    const OutgoingResult lr = low.rc.ComputeOutgoingMultiplier(lh);
    CHECK(lr.breakdown.raw == Approx(0.8 * 0.05));
    CHECK(lr.multiplier == Approx(0.25));
}

TEST_CASE("mutual exclusion blocks grants and offers", "[perks]") {
    test::Harness h;
    StartArena(h);
    REQUIRE(h.rc.DevGrantPerk("berserk").ok());
    const Status s = h.rc.DevGrantPerk("turtle");
    CHECK(s.code == ErrorCode::NotEligible);
    CHECK(s.message.find("excluded") != std::string::npos);
    OfferContext ctx;
    ctx.content = &h.rc.content();
    ctx.character = h.rc.content().FindCharacter("alpha");
    ctx.capabilities = &h.rc.capabilities();
    ctx.unlocked_perks = &h.rc.profile().unlocked_perks;
    ctx.owned_perks = &h.rc.state().perks;
    for (const PerkDef* p : OfferablePerks(ctx)) {
        CHECK(p->id != "turtle");
        CHECK(p->id != "berserk");  // epic max_stacks 1 already owned
    }
}

TEST_CASE("character restrictions and skill pools", "[perks]") {
    auto content = test::LoadFixture();
    const PerkDef& alpha_only = *content->FindPerk("alpha_only");
    CHECK(CharacterMayUsePerk(alpha_only, *content->FindCharacter("alpha")));
    CHECK_FALSE(CharacterMayUsePerk(alpha_only, *content->FindCharacter("beta")));

    test::Harness beta;
    REQUIRE(beta.rc.StartRun("beta", 1).ok());
    CHECK(beta.rc.state().perks.at("beta_passive") == 1);  // starting passive
    CHECK(beta.rc.DevGrantPerk("alpha_only").code == ErrorCode::NotEligible);
    test::Harness alpha;
    REQUIRE(alpha.rc.StartRun("alpha", 1).ok());
    CHECK(alpha.rc.DevGrantPerk("alpha_only").ok());
    CHECK(alpha.rc.DevGrantPerk("beta_passive").code == ErrorCode::NotEligible);

    // Skill pool grants access without listing the character on the perk.
    ContentSources src = test::EditFixture([](auto& perks, auto&, auto& chars, auto&, auto&) {
        for (auto& p : perks["perks"]) {
            if (p["id"] == "alpha_only") p["characters"] = nlohmann::json::array({"alpha"});
        }
        chars["characters"][1]["skill_pool"] = nlohmann::json::array({"alpha_only"});
    });
    auto edited = test::Load(src);
    CHECK(CharacterMayUsePerk(*edited->FindPerk("alpha_only"), *edited->FindCharacter("beta")));
}

TEST_CASE("missing capabilities filter perks from offers and grants", "[perks]") {
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kMeleeDetect);
    caps.Remove(cap::kHeal);
    caps.Remove(cap::kEnemyHp);
    test::Harness h(test::LoadFixture(), {}, caps);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    CHECK(h.rc.DevGrantPerk("brawl").message.find("missing_capability") != std::string::npos);
    CHECK(h.rc.DevGrantPerk("medic").code == ErrorCode::NotEligible);
    CHECK(h.rc.DevGrantPerk("execute").code == ErrorCode::NotEligible);
    OfferContext ctx;
    ctx.content = &h.rc.content();
    ctx.character = h.rc.content().FindCharacter("alpha");
    ctx.capabilities = &h.rc.capabilities();
    ctx.unlocked_perks = &h.rc.profile().unlocked_perks;
    ctx.owned_perks = &h.rc.state().perks;
    for (const PerkDef* p : OfferablePerks(ctx)) {
        CHECK(p->id != "brawl");
        CHECK(p->id != "medic");
        CHECK(p->id != "execute");
    }
    // Across many generated offers none of them appears either.
    for (std::uint64_t seed = 0; seed < 300; ++seed) {
        Rng rng(seed);
        int serial = 0;
        for (const RewardChoice& c : GenerateRewardOffer(rng, ctx, [&serial] { return "w" + std::to_string(++serial); })) {
            CHECK(c.perk_id != "brawl");
            CHECK(c.perk_id != "medic");
            CHECK(c.perk_id != "execute");
            if (c.kind == RewardKind::Weapon) {
                for (const Affix& a : c.weapon.affixes) {
                    CHECK(a.kind != AffixKind::LowHpTargetDamagePct);  // needs enemy_hp
                    CHECK(a.kind != AffixKind::UniqueLowHpTargetBonus);
                }
            }
        }
    }
}
