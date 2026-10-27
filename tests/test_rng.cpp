#include <array>
#include <cstdint>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rm/rng.h"
#include "test_support.h"

using namespace rm;

// Golden values come from an independent Python implementation of splitmix64 seeding +
// xoshiro256** + FNV-1a stream derivation. They must hold on x86 and x64 builds alike.
TEST_CASE("rng golden sequence (version 1)", "[rng]") {
    REQUIRE(kRngVersion == 1);
    Rng r(1);
    const std::array<std::uint64_t, 5> expected = {0xb3f2af6d0fc710c5ull, 0x853b559647364ceaull,
                                                   0x92f89756082a4514ull, 0x642e1c7bc266a3a7ull,
                                                   0xb27a48e29a233673ull};
    for (std::uint64_t v : expected) CHECK(r.Next() == v);

    Rng zero(0);
    CHECK(zero.Next() == 0x99ec5f36cb75f2b4ull);
    CHECK(zero.Next() == 0xbf6e1f784956452aull);

    CHECK(Fnv1a64("reward") == 0xac09659fa3d48bfaull);
    Rng reward = Rng::Derive(42, streams::kReward);
    CHECK(reward.Next() == 0x90eb8eec0de96695ull);
    CHECK(reward.Next() == 0x367c8344a08f9e19ull);
    CHECK(reward.Next() == 0xcca557076a85be38ull);
    Rng combat = Rng::Derive(42, streams::kCombat);
    CHECK(combat.Next() == 0x806af79376f5cb81ull);

    Rng ints(7);
    const std::array<std::int64_t, 10> expected_ints = {94, 74, 38, 64, 64, 21, 16, 96, 8, 19};
    for (std::int64_t v : expected_ints) CHECK(ints.UniformInt(0, 99) == v);

    Rng doubles(7);
    CHECK(doubles.NextDouble() == 0.7005764821796896);
    CHECK(doubles.NextDouble() == 0.2787512294737843);
    CHECK(doubles.NextDouble() == 0.8396274618764198);
    CHECK(doubles.draws() == 3);
}

TEST_CASE("rng is deterministic and range-safe", "[rng]") {
    Rng a(123456789);
    Rng b(123456789);
    for (int i = 0; i < 1000; ++i) REQUIRE(a.Next() == b.Next());

    Rng r(99);
    for (int i = 0; i < 10000; ++i) {
        const std::int64_t v = r.UniformInt(-3, 3);
        REQUIRE(v >= -3);
        REQUIRE(v <= 3);
        const double d = r.NextDouble();
        REQUIRE(d >= 0.0);
        REQUIRE(d < 1.0);
    }
    CHECK(r.UniformInt(5, 5) == 5);
    CHECK(r.UniformInt(5, 1) == 5);
    const std::uint64_t before = r.draws();
    const std::vector<double> none = {0.0, -1.0};
    CHECK(r.WeightedIndex(none) == none.size());
    CHECK(r.draws() == before);  // no draw consumed when nothing is pickable
    const std::vector<double> one = {0.0, 2.0, 0.0};
    for (int i = 0; i < 100; ++i) CHECK(r.WeightedIndex(one) == 1);
}

TEST_CASE("named streams are independent", "[rng]") {
    // Consuming the combat stream must not shift the reward stream.
    Rng reward_a = Rng::Derive(77, streams::kReward);
    Rng combat = Rng::Derive(77, streams::kCombat);
    for (int i = 0; i < 500; ++i) combat.Next();
    Rng reward_b = Rng::Derive(77, streams::kReward);
    for (int i = 0; i < 100; ++i) REQUIRE(reward_a.Next() == reward_b.Next());

    // Different names and different seeds give different sequences.
    CHECK(Rng::Derive(77, streams::kReward).Next() != Rng::Derive(77, streams::kRoute).Next());
    CHECK(Rng::Derive(77, streams::kShop).Next() != Rng::Derive(78, streams::kShop).Next());
}

TEST_CASE("controller streams: crit rolls do not shift reward offers", "[rng]") {
    auto offer_after_hits = [](int hits) {
        test::Harness h;
        REQUIRE(h.rc.StartRun("alpha", 2024).ok());
        REQUIRE(h.rc.DevGrantPerk("crit_up").ok());
        REQUIRE(h.rc.BeginArena(0.0).ok());
        for (int i = 0; i < hits; ++i) {
            HitContext hit;
            hit.now = 1.0 + i * 0.01;
            hit.weapon_id = "pistol";
            h.rc.ComputeOutgoingMultiplier(hit);
        }
        test::KillN(h.rc, h.rc.RemainingEnemies(), 5.0);
        REQUIRE(h.rc.CompleteArena(20.0).ok());
        std::vector<std::string> ids;
        for (const RewardChoice& c : h.rc.RewardOffer()) {
            ids.push_back(std::string(RewardKindName(c.kind)) + ":" + c.perk_id + c.weapon.base_id +
                          std::to_string(c.supplies));
        }
        return ids;
    };
    CHECK(offer_after_hits(0) == offer_after_hits(250));
}
