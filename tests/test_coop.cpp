// Local co-op (ROADMAP M10, F1): a run with a seat per player. Each seat has its own character,
// build, loadout, perks, weapons, rewards and shop; the seed, run map, route and supplies are the
// players' together.
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rm/unlock_all.h"
#include "test_support.h"

using namespace rm;

namespace {

// Both seats choose their first reward and leave their shop, then the first route is taken and the
// next arena begins at `t`.
void NextArenaTogether(RunController& rc, double t) {
    REQUIRE(rc.ContinueToRewards().ok());
    for (int seat = 0; seat < rc.SeatCount(); ++seat) {
        SeatScope s(rc, seat);
        REQUIRE(s.ok());
        if (!rc.state().reward_offer.empty()) REQUIRE(rc.ChooseReward(0).ok());
    }
    if (rc.state().phase != Phase::Shop) return;  // the run ended on its last arena
    for (int seat = 0; seat < rc.SeatCount(); ++seat) {
        SeatScope s(rc, seat);
        REQUIRE(rc.LeaveShop().ok());
    }
    REQUIRE(rc.state().phase == Phase::RouteSelection);
    REQUIRE(rc.ChooseRoute(0).ok());
    REQUIRE(rc.BeginArena(t).ok());
}

}  // namespace

TEST_CASE("co-op: each seat has its own character, weapons and perks", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 7).ok());
    CHECK(h.rc.SeatCount() == 1);
    REQUIRE(h.rc.AddSeat("beta", "").ok());
    CHECK(h.rc.SeatCount() == 2);
    CHECK(h.rc.ActiveSeat() == 0);
    CHECK(h.rc.state().character_id == "alpha");
    const std::size_t alpha_weapons = h.rc.state().weapons.size();
    std::set<std::string> ids;
    for (const WeaponInstance& w : h.rc.state().weapons) ids.insert(w.instance_id);
    {
        SeatScope seat(h.rc, 1);
        REQUIRE(seat.ok());
        CHECK(h.rc.ActiveSeat() == 1);
        CHECK(h.rc.state().character_id == "beta");
        CHECK(h.rc.state().perks.count("beta_passive") == 1);  // beta's own trait
        CHECK_FALSE(h.rc.state().weapons.empty());
        for (const WeaponInstance& w : h.rc.state().weapons) CHECK(ids.insert(w.instance_id).second);  // ids never shared
        const RunSnapshot snap = h.rc.Snapshot(0.0);
        CHECK(snap.seat == 1);
        CHECK(snap.seat_count == 2);
        CHECK(snap.character_id == "beta");
    }
    CHECK(h.rc.ActiveSeat() == 0);
    CHECK(h.rc.state().character_id == "alpha");
    CHECK(h.rc.state().perks.count("beta_passive") == 0);
    CHECK(h.rc.state().weapons.size() == alpha_weapons);
    CHECK(h.sink.Count(EventType::PlayerJoined) == 1);
    bool seated = false;
    for (const Event& e : h.sink.events) {
        if (e.type != EventType::PlayerJoined) continue;
        const FieldValue* seat = e.Find("seat");
        seated = seat != nullptr && std::get<std::int64_t>(*seat) == 1;
    }
    CHECK(seated);
    CHECK(h.rc.ActivateSeat(2).code == ErrorCode::InvalidIndex);
}

TEST_CASE("co-op: a player joins before the first arena, gated like StartRun, up to the limit", "[run]") {
    test::Harness none;
    CHECK(none.rc.AddSeat("alpha", "").code == ErrorCode::NoActiveRun);

    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 7).ok());
    CHECK(h.rc.AddSeat("gamma", "").code == ErrorCode::Locked);
    CHECK(h.rc.AddSeat("delta", "").code == ErrorCode::Disabled);
    CHECK(h.rc.AddSeat("nobody", "").code == ErrorCode::UnknownId);
    REQUIRE(h.rc.AddSeat("alpha", "").ok());  // the same character twice is the game's to allow
    CHECK(h.rc.AddSeat("beta", "").code == ErrorCode::NotEligible);  // two players: the limit

    test::Harness late;
    REQUIRE(late.rc.StartRun("alpha", 7).ok());
    test::ClearArena(late.rc, 0.0);
    CHECK(late.rc.AddSeat("beta", "").code == ErrorCode::WrongPhase);
}

TEST_CASE("co-op: the shop opens once every player chose, the route once every player left it", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 8).ok());
    REQUIRE(h.rc.AddSeat("beta", "").ok());
    test::ClearArena(h.rc, 0.0);
    REQUIRE(h.rc.ContinueToRewards().ok());
    REQUIRE(h.rc.ChooseReward(0).ok());
    CHECK(h.rc.state().phase == Phase::RewardSelection);  // beta has not chosen yet
    CHECK(h.rc.ChooseReward(0).code == ErrorCode::AlreadyChosen);
    {
        SeatScope beta(h.rc, 1);
        CHECK_FALSE(h.rc.state().reward_offer.empty());
        CHECK_FALSE(h.rc.state().reward_chosen);
        REQUIRE(h.rc.ChooseReward(0).ok());
    }
    REQUIRE(h.rc.state().phase == Phase::Shop);
    CHECK_FALSE(h.rc.ShopOffers().empty());
    {
        SeatScope beta(h.rc, 1);
        CHECK_FALSE(h.rc.ShopOffers().empty());  // beta's own shop
    }
    REQUIRE(h.rc.LeaveShop().ok());
    CHECK(h.rc.state().phase == Phase::Shop);  // beta is still shopping
    CHECK(h.rc.Snapshot(0.0).shop_done);
    CHECK(h.rc.Purchase(0).code == ErrorCode::WrongPhase);  // alpha left
    {
        SeatScope beta(h.rc, 1);
        CHECK_FALSE(h.rc.Snapshot(0.0).shop_done);
        REQUIRE(h.rc.LeaveShop().ok());
    }
    CHECK(h.rc.state().phase == Phase::RouteSelection);
}

TEST_CASE("co-op: a perk pays each player who has it", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 8).ok());
    REQUIRE(h.rc.AddSeat("beta", "").ok());
    REQUIRE(h.rc.DevGrantPerk("stocker").ok());
    {
        SeatScope beta(h.rc, 1);
        REQUIRE(h.rc.DevGrantPerk("stocker").ok());
    }
    const std::size_t before = h.sink.events.size();
    test::ClearArena(h.rc, 0.0);
    int paid = 0;
    for (std::size_t i = before; i < h.sink.events.size(); ++i) {
        const Event& e = h.sink.events[i];
        const FieldValue* reason = e.Find("reason");
        if (e.type == EventType::CurrencyChanged && reason != nullptr && std::get<std::string>(*reason) == "stocker") ++paid;
    }
    CHECK(paid == 2);
}

TEST_CASE("co-op: a player down comes back next arena; the run fails when both are down", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 9).ok());
    REQUIRE(h.rc.AddSeat("beta", "").ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    {
        SeatScope beta(h.rc, 1);
        REQUIRE(h.rc.OnPlayerDied(5.0).ok());
        CHECK(h.rc.Snapshot(5.0).seat_down);
    }
    CHECK(h.rc.SeatDown(1));
    CHECK_FALSE(h.rc.SeatDown(0));
    CHECK(h.rc.state().phase == Phase::ArenaActive);
    test::KillN(h.rc, h.rc.RemainingEnemies(), 6.0);
    REQUIRE(h.rc.CompleteArena(30.0).ok());
    NextArenaTogether(h.rc, 40.0);
    CHECK_FALSE(h.rc.SeatDown(1));
    CHECK(h.rc.ConsumeRevivedSeats() == std::vector<int>{1});
    CHECK(h.rc.ConsumeRevivedSeats().empty());
    {
        SeatScope beta(h.rc, 1);
        REQUIRE(h.rc.OnPlayerDied(45.0).ok());
    }
    CHECK(h.rc.state().phase == Phase::ArenaActive);
    REQUIRE(h.rc.OnPlayerDied(46.0).ok());  // alpha too
    CHECK(h.rc.state().phase == Phase::RunFailed);
    REQUIRE(h.rc.EndRun().ok());
    CHECK(h.rc.SeatCount() == 0);
    CHECK(h.rc.ActiveSeat() == 0);
}

TEST_CASE("co-op: more enemies an arena, and no resume point", "[run]") {
    test::Harness solo;
    REQUIRE(solo.rc.StartRun("alpha", 9).ok());
    REQUIRE(solo.rc.BeginArena(0.0).ok());
    const int solo_budget = solo.rc.state().arenas.back().budget;

    test::Harness duo;
    REQUIRE(duo.rc.StartRun("alpha", 9).ok());
    REQUIRE(duo.rc.AddSeat("beta", "").ok());
    CHECK(duo.rc.SaveResumePoint("adapter").code == ErrorCode::NotEligible);
    REQUIRE(duo.rc.BeginArena(0.0).ok());
    CHECK(duo.rc.state().arenas.back().budget ==
          static_cast<int>(std::lround(solo_budget * duo.rc.content().economy().coop.enemy_mult)));
    CHECK(duo.rc.state().arenas.back().budget > solo_budget);
}

TEST_CASE("co-op: a finished run counts for both players' characters", "[run]") {
    test::Harness h;
    REQUIRE(h.rc.StartRun("alpha", 10).ok());
    REQUIRE(h.rc.AddSeat("beta", "").ok());
    double t = 0.0;
    REQUIRE(h.rc.BeginArena(t).ok());
    for (int guard = 0; guard < 20 && h.rc.state().phase == Phase::ArenaActive; ++guard) {
        test::KillN(h.rc, h.rc.RemainingEnemies(), t + 1.0);
        REQUIRE(h.rc.CompleteArena(t + 30.0).ok());
        t += 100.0;
        NextArenaTogether(h.rc, t);
    }
    REQUIRE(h.rc.state().phase == Phase::RunComplete);
    REQUIRE(h.rc.EndRun().ok());
    const auto& as = h.rc.profile().stats.runs_completed_as;
    CHECK(as.count("alpha") == 1);
    CHECK(as.count("beta") == 1);
    CHECK(as.at("alpha") == 1);
    CHECK(as.at("beta") == 1);
}

TEST_CASE("co-op: two outfits of one game costume slot cannot be worn together when one is a mod's", "[run]") {
    const ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    Profile open;
    UnlockEverything(open, *r.content);
    test::Harness h(r.content, open);
    h.rc.SetInstalledModSkins({"ada_agent"});
    REQUIRE(h.rc.StartRun("sheva", 7, "sheva_bsaa").ok());
    CHECK(h.rc.AddSeat("ada", "ada_agent").code == ErrorCode::NotEligible);  // Sheva BSAA's slot, Ada's mod file
    REQUIRE(h.rc.AddSeat("chris", "chris_bsaa").ok());
}
