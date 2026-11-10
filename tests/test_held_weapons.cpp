// The run's guns follow what the player holds (2026-11-10, the user: "I get upgrades for weapons I
// don't own anymore"), and a gun that "restocks" (the rocket launcher) comes back every arena.
#include <algorithm>
#include <optional>
#include <set>

#include <catch_amalgamated.hpp>

#include "rm/content.h"
#include "rm/profile.h"
#include "rm/run_controller.h"
#include "test_support.h"

using namespace rm;

namespace {

// The adapter's inventory: what it was given, and whatever the test takes away (fired, sold).
struct FakeInventory final : InventoryPort {
    std::set<std::int64_t> held;
    bool knows = true;
    int Give(std::int64_t game_item, int count, std::string_view) override {
        held.insert(game_item);
        return count;
    }
    std::optional<bool> Holds(std::int64_t game_item) override {
        if (!knows) return std::nullopt;
        return held.count(game_item) > 0;
    }
};

std::shared_ptr<const Content> Shipped() {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    return r.content;
}

std::int64_t ItemOf(const Content& c, const std::string& id) {
    const WeaponBaseDef* w = c.FindWeapon(id);
    REQUIRE(w != nullptr);
    return w->game_item;
}

bool Has(const RunController& rc, const std::string& base) {
    const auto& w = rc.state().weapons;
    return std::any_of(w.begin(), w.end(), [&base](const WeaponInstance& i) { return i.base_id == base; });
}

// A run with the starting pistol, an M29 and a rocket launcher, all in the inventory.
void Start(test::Harness& h, FakeInventory& inv, const Content& c) {
    h.rc.SetInventory(&inv);
    REQUIRE(h.rc.StartRun("chris", 7).ok());
    for (const WeaponInstance& w : h.rc.state().weapons) inv.held.insert(ItemOf(c, w.base_id));
    REQUIRE(h.rc.DevGrantWeapon(MakeStartingWeapon("m29", "w_m29")).ok());
    REQUIRE(h.rc.DevGrantWeapon(MakeStartingWeapon("rocket_launcher", "w_rl")).ok());
    inv.held.insert(ItemOf(c, "m29"));
    inv.held.insert(ItemOf(c, "rocket_launcher"));
}

}  // namespace

TEST_CASE("only the rocket launcher comes back every arena", "[held-weapons]") {
    const auto c = Shipped();
    for (const WeaponBaseDef& w : c->weapons()) {
        INFO(w.id);
        CHECK(w.restocks == (w.id == "rocket_launcher"));
    }
}

TEST_CASE("a gun the player no longer holds leaves the run before the rewards and the shop", "[held-weapons]") {
    const auto c = Shipped();
    test::Harness h(c);
    FakeInventory inv;
    Start(h, inv, *c);
    REQUIRE(Has(h.rc, "m29"));
    inv.held.erase(ItemOf(*c, "m29"));              // sold
    inv.held.erase(ItemOf(*c, "rocket_launcher"));  // fired: the game discards it
    test::ClearArena(h.rc, 0.0);
    CHECK_FALSE(Has(h.rc, "m29"));
    CHECK(Has(h.rc, "rocket_launcher"));  // it restocks
    const auto restock = h.rc.RestockWeaponItems();
    CHECK(restock == std::vector<std::int64_t>{ItemOf(*c, "rocket_launcher")});
    REQUIRE(h.rc.ContinueToRewards().ok());
    if (h.rc.state().phase == Phase::RewardSelection) REQUIRE(h.rc.ChooseReward(0).ok());
    REQUIRE(h.rc.state().phase == Phase::Shop);
    for (const ShopOffer& o : h.rc.state().shop_offers) {
        CHECK(o.target_instance_id != "w_m29");
        if (o.kind == ShopOfferKind::Weapon) CHECK(o.weapon.base_id != "rocket_launcher");  // owned: never sold again
    }
}

TEST_CASE("a gun the inventory cannot vouch for stays", "[held-weapons]") {
    const auto c = Shipped();
    test::Harness h(c);
    FakeInventory inv;
    Start(h, inv, *c);
    inv.knows = false;  // no player in a match, the Organize screen open
    inv.held.clear();
    test::ClearArena(h.rc, 0.0);
    CHECK(Has(h.rc, "m29"));
    CHECK(Has(h.rc, "rocket_launcher"));
}

TEST_CASE("without an inventory nothing is dropped", "[held-weapons]") {
    const auto c = Shipped();
    test::Harness h(c);
    REQUIRE(h.rc.StartRun("chris", 7).ok());
    REQUIRE(h.rc.DevGrantWeapon(MakeStartingWeapon("m29", "w_m29")).ok());
    test::ClearArena(h.rc, 0.0);
    CHECK(Has(h.rc, "m29"));
}

TEST_CASE("a sold rocket launcher is given up and no longer comes back", "[held-weapons]") {
    const auto c = Shipped();
    test::Harness h(c);
    FakeInventory inv;
    Start(h, inv, *c);
    CHECK_FALSE(h.rc.DropWeapon(ItemOf(*c, "m500"), "sold").ok());  // not one of the run's
    REQUIRE(h.rc.DropWeapon(ItemOf(*c, "rocket_launcher"), "sold").ok());
    CHECK_FALSE(Has(h.rc, "rocket_launcher"));
    CHECK(h.rc.RestockWeaponItems().empty());
    CHECK(Has(h.rc, "m29"));
}
