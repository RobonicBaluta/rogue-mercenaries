#include <array>
#include <cmath>
#include <set>

#include <catch_amalgamated.hpp>

#include "rm/rewards.h"
#include "test_support.h"

using namespace rm;

namespace {

OfferContext FixtureContext(const Content& content, const StringSet& unlocked, const StringMap<int>& owned,
                            const Capabilities& caps, const char* character = "alpha") {
    OfferContext ctx;
    ctx.content = &content;
    ctx.character = content.FindCharacter(character);
    ctx.capabilities = &caps;
    ctx.unlocked_perks = &unlocked;
    ctx.unlocked_weapons = &unlocked;
    ctx.owned_perks = &owned;
    return ctx;
}

std::string Describe(const RewardChoice& c) {
    switch (c.kind) {
        case RewardKind::Perk: return "perk:" + c.perk_id;
        case RewardKind::Weapon: return "weapon:" + c.weapon.base_id + ":" + std::string(RarityName(c.rarity));
        case RewardKind::Supplies: return "supplies:" + std::to_string(c.supplies);
    }
    return "?";
}

}  // namespace

TEST_CASE("reward offer for a fixed seed matches the snapshot", "[rewards]") {
    auto content = test::LoadFixture();
    const StringSet unlocked;
    const StringMap<int> owned;
    const Capabilities caps = Capabilities::All();
    const OfferContext ctx = FixtureContext(*content, unlocked, owned, caps);
    std::vector<std::string> got;
    for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
        Rng rng = Rng::Derive(seed, streams::kReward);
        int serial = 0;
        for (const RewardChoice& c : GenerateRewardOffer(rng, ctx, [&serial] { return "w" + std::to_string(++serial); })) {
            got.push_back(Describe(c));
        }
    }
    // Pinned with kRngVersion 1 and the test fixture; update intentionally if the generator changes.
    const std::vector<std::string> expected = {
#include "reward_snapshot.inc"
    };
    for (const std::string& g : got) UNSCOPED_INFO(g);
    CHECK(got == expected);
}

TEST_CASE("reward offers are distinct and respect pools", "[rewards]") {
    auto content = test::LoadFixture();
    const StringSet unlocked;
    StringMap<int> owned = {{"dmg_up", 3}, {"berserk", 1}};
    const Capabilities caps = Capabilities::All();
    const OfferContext ctx = FixtureContext(*content, unlocked, owned, caps);
    for (std::uint64_t seed = 0; seed < 2000; ++seed) {
        Rng rng(seed);
        int serial = 0;
        const auto offer = GenerateRewardOffer(rng, ctx, [&serial] { return "w" + std::to_string(++serial); });
        REQUIRE(offer.size() == 3);
        std::set<std::string> seen;
        int weapons = 0;
        int supplies = 0;
        for (const RewardChoice& c : offer) {
            CHECK(seen.insert(Describe(c)).second);
            if (c.kind == RewardKind::Weapon) ++weapons;
            if (c.kind == RewardKind::Supplies) {
                ++supplies;
                CHECK(c.supplies > 0);
            }
            if (c.kind == RewardKind::Perk) {
                CHECK(c.perk_id != "dmg_up");       // max stacks
                CHECK(c.perk_id != "turtle");       // excluded by berserk
                CHECK(c.perk_id != "jackpot");      // locked
                CHECK(c.perk_id != "beta_passive"); // other character
                CHECK(c.perk_id != "coop_reserved");
                CHECK(content->FindPerk(c.perk_id)->category != PerkCategory::Curse);
            }
            if (c.kind == RewardKind::Weapon) CHECK(c.weapon.base_id != "rifle");  // locked weapon
        }
        CHECK(weapons <= 1);
        CHECK(supplies <= 1);
    }
}

TEST_CASE("offers are padded with distinct supplies when pools are empty", "[rewards]") {
    ContentSources src = test::EditFixture([](auto& perks, auto&, auto&, auto&, auto&) {
        for (auto& p : perks["perks"]) {
            if (p["id"] != "curse_weak" && p["id"] != "beta_passive" && p["id"] != "dmg_up") p["enabled"] = false;
        }
    });
    auto content = test::Load(src);
    const StringSet unlocked;
    const StringMap<int> owned = {{"dmg_up", 3}};
    Capabilities caps;  // nothing verified: no perk or weapon can work
    caps.Add(cap::kKillEvents);
    const OfferContext ctx = FixtureContext(*content, unlocked, owned, caps);
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        Rng rng(seed);
        const auto offer = GenerateRewardOffer(rng, ctx, {});
        REQUIRE(offer.size() == 3);
        std::set<std::int64_t> amounts;
        for (const RewardChoice& c : offer) {
            CHECK(c.kind == RewardKind::Supplies);
            CHECK(amounts.insert(c.supplies).second);
        }
    }
}

TEST_CASE("rarity distribution matches weights over 100k draws", "[rewards]") {
    auto content = test::LoadFixture();
    const RarityWeights base = content->economy().rewards.rarity_weights;
    for (const double shift : {0.0, 0.4, -0.3}) {
        const RarityWeights w = ShiftRarityWeights(base, shift);
        double total = 0.0;
        for (double v : w) total += v;
        std::array<int, kRarityCount> counts{};
        Rng rng = Rng::Derive(2026, "rarity_test");
        constexpr int kDraws = 100000;
        for (int i = 0; i < kDraws; ++i) ++counts[RarityIndex(RollRarity(rng, w))];
        for (std::size_t i = 0; i < kRarityCount; ++i) {
            const double expected = w[i] / total;
            const double observed = static_cast<double>(counts[i]) / kDraws;
            const double sigma = std::sqrt(expected * (1.0 - expected) / kDraws);
            INFO("shift " << shift << " rarity " << i << " expected " << expected << " observed " << observed);
            CHECK(std::fabs(observed - expected) < 5.0 * sigma + 1e-4);
        }
    }
    // Positive shift strictly favours rarer tiers.
    const RarityWeights up = ShiftRarityWeights(base, 0.4);
    const RarityWeights flat = ShiftRarityWeights(base, 0.0);
    auto share = [](const RarityWeights& w, std::size_t i) {
        double t = 0;
        for (double v : w) t += v;
        return w[i] / t;
    };
    CHECK(share(up, RarityIndex(Rarity::Legendary)) > share(flat, RarityIndex(Rarity::Legendary)));
    CHECK(share(up, RarityIndex(Rarity::Common)) < share(flat, RarityIndex(Rarity::Common)));
}

