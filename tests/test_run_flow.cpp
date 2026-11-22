#include <algorithm>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using Catch::Approx;

namespace {

std::size_t CountEvents(const MemorySink& sink, EventType type, int arena = -1) {
    return static_cast<std::size_t>(std::count_if(sink.events.begin(), sink.events.end(), [&](const Event& e) {
        return e.type == type && (arena < 0 || e.arena == arena);
    }));
}

}  // namespace

TEST_CASE("full three-arena run driven by a fake event script", "[run]") {
    test::Harness h;
    RunController& rc = h.rc;
    REQUIRE(rc.StartRun("alpha", 42).ok());
    CHECK(rc.state().run_id == "run_0001");
    CHECK(rc.phase() == Phase::Idle);
    CHECK(rc.profile().stats.runs_started == 1);
    CHECK(rc.state().supplies.balance() == 50);

    double t = 0.0;
    std::int64_t expected_kills = 0;
    for (int arena = 1; arena <= 3; ++arena) {
        INFO("arena " << arena);
        REQUIRE(rc.BeginArena(t).ok());
        CHECK(rc.phase() == Phase::ArenaActive);
        CHECK(rc.Snapshot(t).arena == arena);
        const int budget = rc.RemainingEnemies();
        REQUIRE(budget > 0);

        // Script: partner kill (ignored for budget), duplicate report, hits, damage, player kills.
        KillEvent partner;
        partner.now = t + 1.0;
        partner.by_player = false;
        partner.enemy_id = "e_partner";
        CHECK_FALSE(rc.OnEnemyKilled(partner).counted);
        CHECK(rc.RemainingEnemies() == budget);
        KillEvent dup = partner;
        dup.by_player = true;  // same enemy reported again by a different hook
        CHECK(rc.OnEnemyKilled(dup).duplicate);
        CHECK(rc.CompleteArena(t + 2.0).code() == ErrorCode::ArenaNotComplete);

        const IncomingResult inc = rc.ComputeIncoming(IncomingContext{t + 2.0, 10.0, 100.0, 100.0});
        rc.OnPlayerDamaged(DamageEvent{t + 2.0, 10.0 * inc.multiplier, 100.0, 100.0 - 10.0 * inc.multiplier, 100.0});
        for (int i = 0; i < budget; ++i) {
            HitContext hit;
            hit.now = t + 3.0 + i;
            hit.weapon_id = "pistol";
            hit.target_hp = 10.0;
            hit.target_hp_max = 100.0;
            CHECK(rc.ComputeOutgoingMultiplier(hit).multiplier >= 0.25);
            KillEvent k;
            k.now = t + 3.0 + i;
            k.by_player = true;
            k.weapon_id = "pistol";
            k.enemy_id = "e" + std::to_string(arena) + "_" + std::to_string(i);
            const KillOutcome out = rc.OnEnemyKilled(k);
            CHECK(out.counted);
            CHECK(out.remaining == budget - i - 1);
        }
        expected_kills += budget;
        REQUIRE(rc.IsArenaComplete());
        const Result<ArenaResult> result = rc.CompleteArena(t + 60.0);
        REQUIRE(result.ok());
        CHECK(result->arena == arena);
        CHECK(result->kills == budget);
        CHECK_FALSE(result->no_damage);
        CHECK(result->time_seconds == Approx(60.0));
        CHECK(result->supplies_awarded > 0);
        CHECK(result->credits_accrued > 0);
        CHECK(rc.phase() == Phase::ArenaResults);
        CHECK(rc.state().arenas.back().non_player_kills == 1);
        CHECK(rc.state().arenas.back().duplicate_kills == 1);

        // Rewards, except after the last arena (nothing is left to use one on).
        CHECK(rc.ChooseReward(0).code == ErrorCode::WrongPhase);
        if (arena == 3) {
            CHECK(rc.RewardOffer().empty());
            REQUIRE(rc.ContinueToRewards().ok());
            CHECK(rc.phase() == Phase::RunComplete);
            CHECK(rc.ChooseReward(0).code == ErrorCode::WrongPhase);
        } else {
            REQUIRE(rc.ContinueToRewards().ok());
            REQUIRE(rc.RewardOffer().size() == 3);
            REQUIRE(rc.ChooseReward(1).ok());
        }
        if (arena < 3) {
            CHECK(rc.phase() == Phase::Shop);
            CHECK(rc.BeginArena(t).code == ErrorCode::WrongPhase);
            CHECK(rc.ChooseRoute(0).code == ErrorCode::WrongPhase);
            REQUIRE(rc.LeaveShop().ok());
            CHECK(rc.phase() == Phase::RouteSelection);
            CHECK(rc.BeginArena(t).code == ErrorCode::WrongPhase);  // route not chosen yet
            REQUIRE(rc.RouteChoices().size() >= 2);
            REQUIRE(rc.ChooseRoute(rc.RouteChoices().size() - 1).ok());
            CHECK(rc.ChooseRoute(0).code == ErrorCode::AlreadyChosen);
        }
        t += 100.0;
    }
    CHECK(rc.phase() == Phase::RunComplete);
    CHECK(rc.BeginArena(t).code == ErrorCode::WrongPhase);
    const std::int64_t pending = rc.state().credits_pending;
    const Result<RunSummary> summary = rc.EndRun();
    REQUIRE(summary.ok());
    CHECK(summary->completed);
    CHECK(summary->arenas_cleared == 3);
    CHECK(summary->total_kills == expected_kills);
    CHECK(summary->credits_banked == pending + 50);
    CHECK(summary->previous_best_arena == 0);
    double time = 0.0;
    for (const ArenaResult& a : summary->arenas) time += a.time_seconds;
    CHECK(summary->time_seconds == time);
    CHECK(summary->time_seconds > 0.0);
    CHECK(rc.profile().credits.balance() == pending + 50);
    CHECK(rc.profile().stats.runs_completed == 1);
    CHECK(rc.profile().stats.best_arena == 3);
    CHECK(rc.profile().stats.total_kills == expected_kills);
    CHECK(rc.profile().IsRunBanked("run_0001"));
    CHECK_FALSE(rc.HasActiveRun());
    CHECK(rc.phase() == Phase::Idle);

    CHECK(CountEvents(h.sink, EventType::RunStarted) == 1);
    CHECK(CountEvents(h.sink, EventType::ArenaStarted) == 3);
    CHECK(CountEvents(h.sink, EventType::ArenaCompleted) == 3);
    CHECK(CountEvents(h.sink, EventType::RewardSelected) == 2);  // none after the last arena
    CHECK(CountEvents(h.sink, EventType::RouteSelected) == 2);
    CHECK(CountEvents(h.sink, EventType::RunEnded) == 1);
    for (const Event& e : h.sink.events) {
        if (e.type == EventType::ArenaStarted) CHECK(e.run_id == "run_0001");
    }
}

TEST_CASE("ChooseReward succeeds exactly once per arena", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 8).ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    auto weapon_ids = [&h] {
        std::string ids;
        for (const WeaponInstance& w : h.rc.state().weapons) ids += w.instance_id + ",";
        return ids;
    };
    const std::int64_t supplies_before = h.rc.state().supplies.balance();
    const StringMap<int> perks_before = h.rc.state().perks;
    const std::string weapons_before = weapon_ids();
    CHECK(h.rc.ChooseReward(99).code == ErrorCode::InvalidIndex);
    REQUIRE(h.rc.ChooseReward(0).ok());
    const std::int64_t supplies_after = h.rc.state().supplies.balance();
    const StringMap<int> perks_after = h.rc.state().perks;
    const std::string weapons_after = weapon_ids();
    CHECK((supplies_after != supplies_before || perks_after != perks_before || weapons_after != weapons_before));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(h.rc.ChooseReward(i).code == ErrorCode::AlreadyChosen);
    }
    CHECK(h.rc.RerollReward().code == ErrorCode::WrongPhase);
    CHECK(h.rc.state().supplies.balance() == supplies_after);
    CHECK(h.rc.state().perks == perks_after);
    CHECK(weapon_ids() == weapons_after);
    CHECK(h.sink.Count(EventType::RewardSelected) == 1);
}

TEST_CASE("reroll tokens regenerate the offer before choosing", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 8).ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    CHECK(h.rc.RerollReward().code == ErrorCode::NoRerollTokens);
    REQUIRE(h.rc.ChooseReward(0).ok());
    // Buy a token in the shop and use it on the next arena's offer.
    const auto& offers = h.rc.ShopOffers();
    std::size_t token = offers.size();
    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (offers[i].kind == ShopOfferKind::RerollToken) token = i;
    }
    REQUIRE(token < offers.size());
    REQUIRE(h.rc.Purchase(token).ok());
    REQUIRE(h.rc.LeaveShop().ok());
    REQUIRE(h.rc.ChooseRoute(0).ok());
    test::ClearArena(h.rc, 100.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.RerollReward().ok());
    CHECK(h.rc.state().reroll_tokens == 0);
    CHECK(h.rc.RewardOffer().size() == 3);
    CHECK(h.sink.Count(EventType::RewardRerolled) == 1);
}

TEST_CASE("death fails the run and banks credits at the death rate", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 5).ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    REQUIRE(h.rc.LeaveShop().ok());
    REQUIRE(h.rc.ChooseRoute(0).ok());
    REQUIRE(h.rc.BeginArena(100.0).ok());
    test::KillN(h.rc, 2, 101.0);
    CHECK(h.rc.EndRun().code() == ErrorCode::WrongPhase);
    REQUIRE(h.rc.OnPlayerDied(105.0).ok());
    CHECK(h.rc.phase() == Phase::RunFailed);
    CHECK(h.rc.state().arenas.back().deaths == 1);
    CHECK(h.rc.BeginArena(110.0).code == ErrorCode::WrongPhase);
    CHECK(h.rc.OnEnemyKilled(KillEvent{110.0, true, {}, {}, {}}).ignored);
    CHECK(h.rc.ComputeOutgoingMultiplier(HitContext{}).multiplier == 1.0);
    CHECK(h.rc.OnPlayerDied(111.0).code == ErrorCode::WrongPhase);
    const std::int64_t pending = h.rc.state().credits_pending;
    REQUIRE(pending > 0);
    const Result<RunSummary> s = h.rc.EndRun();
    REQUIRE(s.ok());
    CHECK_FALSE(s->completed);
    CHECK(s->end_reason == "died");
    CHECK(s->arenas_cleared == 1);
    CHECK(s->credits_banked == pending / 2);
    // The results screen's time counts the arena the player died in (100 s to 105 s).
    CHECK(s->time_seconds == s->arenas.front().time_seconds + 5.0);
    CHECK(s->previous_best_arena == 0);
    CHECK(h.rc.profile().credits.balance() == pending / 2);
    CHECK(h.rc.profile().stats.runs_failed == 1);
    CHECK(h.rc.profile().stats.runs_completed == 0);
}

TEST_CASE("StartRun resets temporary state while the profile persists", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.DevGrantPerk("dmg_up").ok());
    REQUIRE(h.rc.DevGrantPerk("medic").ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    CHECK(h.rc.StartRun("alpha", 2).code == ErrorCode::RunInProgress);
    REQUIRE(h.rc.AbandonRun(50.0).ok());
    CHECK(h.rc.AbandonRun(51.0).code == ErrorCode::WrongPhase);
    REQUIRE(h.rc.EndRun().ok());
    const std::int64_t credits = h.rc.profile().credits.balance();
    CHECK(credits > 0);

    REQUIRE(h.rc.StartRun("beta", 2).ok());
    const RunState& s = h.rc.state();
    CHECK(s.run_id == "run_0002");
    CHECK(s.arena_index == 0);
    CHECK(s.phase == Phase::Idle);
    CHECK(s.supplies.balance() == 50);
    CHECK(s.supplies_earned == 0);
    CHECK(s.supplies_spent == 0);
    CHECK(s.credits_pending == 0);
    CHECK(s.perks == StringMap<int>{{"beta_passive", 1}});
    REQUIRE(s.weapons.size() == 1);
    CHECK(s.weapons[0].level == 1);
    CHECK(s.weapons[0].affixes.empty());
    CHECK(s.reward_offer.empty());
    CHECK(s.shop_offers.empty());
    CHECK(s.results.empty());
    CHECK(s.kill_records.empty());
    CHECK(s.reroll_tokens == 0);
    CHECK(h.rc.PendingHeal() == 0.0);
    CHECK(h.rc.profile().credits.balance() == credits);
    CHECK(h.rc.profile().stats.runs_started == 2);
    CHECK(h.rc.profile().stats.runs_failed == 1);
}

TEST_CASE("non-player kills can be configured to count toward the budget", "[run]") {
    auto content = test::Load(test::EditFixture([](auto&, auto&, auto&, auto&, auto& eco) {
        eco["run"]["non_player_kills_count_toward_budget"] = true;
    }));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 1).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    KillEvent k;
    k.now = 1.0;
    k.by_player = false;
    const KillOutcome out = h.rc.OnEnemyKilled(k);
    CHECK(out.counted);
    CHECK(out.supplies_awarded > 0);  // a counted kill pays out (solo: the player caused it)
    CHECK(h.rc.RemainingEnemies() == 4);
    CHECK(h.rc.state().arenas.back().player_kills == 0);  // but it is not a player kill (no streaks)
}

TEST_CASE("route curses are granted when a cursed route is chosen", "[run]") {
    bool cursed = false;
    for (std::uint64_t seed = 0; seed < 40 && !cursed; ++seed) {
        test::Harness run;
        REQUIRE(run.rc.StartRun("alpha", seed).ok());
        test::ClearArena(run.rc, 0.0);
        REQUIRE(run.rc.ContinueToRewards().ok());
        REQUIRE(run.rc.ChooseReward(0).ok());
        REQUIRE(run.rc.LeaveShop().ok());
        const auto& choices = run.rc.RouteChoices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (choices[i].curse_perk_id.empty()) continue;
            const std::string curse = choices[i].curse_perk_id;
            REQUIRE(run.rc.ChooseRoute(i).ok());
            CHECK(run.rc.state().perks.at(curse) == 1);
            CHECK(run.rc.state().route_id == choices[i].route_id);
            const RunSnapshot snap = run.rc.Snapshot(0.0);
            CHECK(std::any_of(snap.perks.begin(), snap.perks.end(), [&](const PerkView& p) { return p.id == curse; }));
            cursed = true;
            break;
        }
    }
    CHECK(cursed);
}

TEST_CASE("snapshot exposes UI state", "[run]") {
    test::Harness h;
    RunSnapshot idle = h.rc.Snapshot(0.0);
    CHECK_FALSE(idle.run_active);
    CHECK(idle.phase_name == "idle");
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    test::KillN(h.rc, 2, 1.0);
    RunSnapshot s = h.rc.Snapshot(2.0);
    CHECK(s.run_active);
    CHECK(s.phase_name == "arena_active");
    CHECK(s.arena == 1);
    CHECK(s.arena_count == 3);
    CHECK(s.kills == 2);
    CHECK(s.budget == 5);
    CHECK(s.remaining == 3);
    CHECK(s.supplies == 54);
    CHECK(s.route_name == "R1A");
    REQUIRE(s.weapons.size() == 1);
    CHECK(s.weapons[0].name == "Pistol");
    test::KillN(h.rc, 3, 3.0);
    REQUIRE(h.rc.CompleteArena(10.0).ok());
    s = h.rc.Snapshot(10.0);
    REQUIRE(s.last_result.has_value());
    CHECK(s.last_result->rating == Rating::S);
    CHECK(s.reward_offers.size() == 3);
    for (const OfferView& o : s.reward_offers) CHECK_FALSE(o.title.empty());
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(2).ok());
    s = h.rc.Snapshot(10.0);
    CHECK(s.reward_chosen);
    CHECK_FALSE(s.shop_offers.empty());
    for (const OfferView& o : s.shop_offers) {
        CHECK_FALSE(o.title.empty());
        CHECK(o.price > 0);
    }
    REQUIRE(h.rc.LeaveShop().ok());
    s = h.rc.Snapshot(10.0);
    CHECK(s.route_offers.size() >= 2);
    for (const OfferView& o : s.route_offers) CHECK(o.detail.find("Tier") == 0);
}

TEST_CASE("operations outside a run fail cleanly", "[run]") {
    test::Harness h;
    CHECK(h.rc.BeginArena(0.0).code == ErrorCode::NoActiveRun);
    CHECK(h.rc.CompleteArena(0.0).code() == ErrorCode::NoActiveRun);
    CHECK(h.rc.ChooseReward(0).code == ErrorCode::NoActiveRun);
    CHECK(h.rc.LeaveShop().code == ErrorCode::NoActiveRun);
    CHECK(h.rc.ChooseRoute(0).code == ErrorCode::NoActiveRun);
    CHECK(h.rc.EndRun().code() == ErrorCode::NoActiveRun);
    CHECK(h.rc.OnPlayerDied(0.0).code == ErrorCode::NoActiveRun);
    CHECK(h.rc.OnEnemyKilled(KillEvent{}).ignored);
    CHECK(h.rc.ComputeIncoming(IncomingContext{0.0, 10.0, 5.0, 100.0}).multiplier == 1.0);
    CHECK_FALSE(h.rc.ComputeIncoming(IncomingContext{0.0, 10.0, 5.0, 100.0}).prevent_death);
    CHECK(h.sink.Count(EventType::Error) >= 7);
}

// The info panel (2026-11-22): the arena under way in the snapshot.
TEST_CASE("the snapshot carries the arena's time, par, damage taken and the rating it would get now", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 42).ok());
    CHECK_FALSE(h.rc.Snapshot(0.0).rating_now);  // no arena yet
    REQUIRE(h.rc.BeginArena(10.0).ok());
    const RunSnapshot s = h.rc.Snapshot(25.0);
    CHECK(s.arena_seconds == Catch::Approx(15.0));
    CHECK(s.par_seconds > 0.0);
    CHECK(s.damage_taken == Catch::Approx(0.0));
    REQUIRE(s.rating_now);
    CHECK(*s.rating_now == ComputeRating(h.rc.content().economy().rating, 15.0, s.par_seconds, 0.0, 0.0, 0));
}
