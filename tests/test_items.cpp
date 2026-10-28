#include <string>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

std::shared_ptr<const Content> ItemEconomy() {
    return test::Load(test::EditFixture([](json&, json&, json&, json&, json& eco) {
        eco["rewards"]["kind_weights"] = {{"perk", 0}, {"weapon", 0}, {"supplies", 0}, {"item", 1}};
        eco["shop"]["perk_offers"] = 0;
        eco["shop"]["item_offers"] = 2;
    }));
}

void ClearFirstArena(test::Harness& h) {
    REQUIRE(h.rc.StartRun("alpha", 7).ok());
    REQUIRE(h.rc.BeginArena(0.0).ok());
    test::KillN(h.rc, h.rc.RemainingEnemies(), 1.0);
    REQUIRE(h.rc.CompleteArena(20.0).ok());
    REQUIRE(h.rc.ContinueToRewards().ok());
}

}  // namespace

TEST_CASE("loadout equipment doubles as the item catalog", "[items]") {
    const auto content = test::LoadFixture();
    const LoadoutConfig& cfg = content->loadouts();
    REQUIRE(cfg.FindEquipment("grenade") != nullptr);
    CHECK(cfg.FindEquipment("grenade")->Offerable());
    CHECK(cfg.FindEquipment("grenade")->bundle == 2);
    CHECK(cfg.FindEquipment("spray")->bundle == 1);
    CHECK_FALSE(cfg.FindEquipment("herb")->Offerable());  // no game item, no price
    CHECK(cfg.FindEquipment("missing") == nullptr);

    // A priced item must be backed by a game item.
    ContentSources bad = test::FixtureSources();
    bad.loadouts = R"({"schema_version": 1, "equipment": [{"id": "x", "name": "X", "cost": 1, "price": 5}]})";
    const auto r = Content::LoadFromStrings(bad);
    REQUIRE_FALSE(r.ok());
    CHECK(r.errors.front().find("game_item") != std::string::npos);
}

TEST_CASE("rewards and the shop hand out inventory items instead of perks", "[items]") {
    const auto content = ItemEconomy();
    test::Harness h(content);
    ClearFirstArena(h);

    const auto& offers = h.rc.state().reward_offer;
    REQUIRE(offers.size() == 3);
    int items = 0;
    std::size_t pick = offers.size();
    for (std::size_t i = 0; i < offers.size(); ++i) {
        CHECK(offers[i].kind != RewardKind::Perk);
        if (offers[i].kind == RewardKind::Item) {
            ++items;
            pick = i;
        }
    }
    CHECK(items == 2);  // two offerable items; the third choice pads with supplies
    REQUIRE(pick < offers.size());
    const std::string chosen = offers[pick].item_id;
    const int count = offers[pick].item_count;
    CHECK(count == content->loadouts().FindEquipment(chosen)->bundle);
    const RunSnapshot before = h.rc.Snapshot(21.0);
    CHECK(before.reward_offers[pick].kind == "item");
    CHECK(before.reward_offers[pick].title.find(content->loadouts().FindEquipment(chosen)->name) == 0);

    (void)h.rc.ConsumeItemGrants();
    REQUIRE(h.rc.ChooseReward(pick).ok());
    auto grants = h.rc.ConsumeItemGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].count == count);
    CHECK(grants[0].source == "reward");
    CHECK(grants[0].game_item == content->loadouts().FindEquipment(chosen)->game_item);

    REQUIRE(h.rc.phase() == Phase::Shop);
    const auto& shop = h.rc.state().shop_offers;
    int shop_items = 0;
    std::size_t item_index = shop.size();
    for (std::size_t i = 0; i < shop.size(); ++i) {
        CHECK(shop[i].kind != ShopOfferKind::Perk);
        if (shop[i].kind == ShopOfferKind::Item) {
            ++shop_items;
            item_index = i;
        }
    }
    CHECK(shop_items == 2);
    REQUIRE(item_index < shop.size());
    const std::int64_t balance = h.rc.state().supplies.balance();
    const std::int64_t price = shop[item_index].price;
    CHECK(price == content->loadouts().FindEquipment(shop[item_index].item_id)->price);
    REQUIRE(h.rc.Purchase(item_index).ok());
    CHECK(h.rc.state().supplies.balance() == balance - price);
    grants = h.rc.ConsumeItemGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].count == 1);
    CHECK(grants[0].source == "shop");
    CHECK(h.rc.Purchase(item_index).code == ErrorCode::AlreadyBought);
}

namespace {

struct FakeInventory final : InventoryPort {
    int room = 0;  // units that still fit
    std::vector<std::pair<std::int64_t, int>> placed;
    int Give(std::int64_t game_item, int count, std::string_view) override {
        const int n = std::min(count, room);
        room -= n;
        if (n > 0) placed.emplace_back(game_item, n);
        return n;
    }
};

}  // namespace

TEST_CASE("with an inventory port, items are placed at once and a full inventory refunds", "[items]") {
    const auto content = ItemEconomy();
    test::Harness h(content);
    FakeInventory inv;
    h.rc.SetInventory(&inv);
    ClearFirstArena(h);
    const auto& offers = h.rc.state().reward_offer;
    std::size_t pick = 0;
    while (offers[pick].kind != RewardKind::Item) ++pick;
    const std::int64_t game_item = content->loadouts().FindEquipment(offers[pick].item_id)->game_item;

    // Nothing fits: the run must not stall, so the item's value is paid in supplies.
    const std::int64_t before = h.rc.state().supplies.balance();
    const std::int64_t value = content->loadouts().FindEquipment(offers[pick].item_id)->price * offers[pick].item_count;
    REQUIRE(h.rc.ChooseReward(pick).ok());
    CHECK(h.rc.state().reward_chosen);
    CHECK(inv.placed.empty());
    CHECK(h.rc.ConsumeItemGrants().empty());
    CHECK(h.rc.state().supplies.balance() == before + value);
    bool converted = false;
    for (const Event& ev : h.sink.events) {
        if (ev.type == EventType::RewardSelected && ev.Find("inventory_full_supplies") != nullptr) converted = true;
    }
    CHECK(converted);

    // With room, the item is placed at once and nothing is queued.
    test::Harness roomy(content);
    FakeInventory spare;
    spare.room = 10;
    roomy.rc.SetInventory(&spare);
    ClearFirstArena(roomy);
    const auto& again = roomy.rc.state().reward_offer;
    std::size_t p2 = 0;
    while (again[p2].kind != RewardKind::Item) ++p2;
    const int wanted = again[p2].item_count;
    REQUIRE(roomy.rc.ChooseReward(p2).ok());
    REQUIRE(spare.placed.size() == 1);
    CHECK(spare.placed[0].second == wanted);
    CHECK(roomy.rc.ConsumeItemGrants().empty());
    (void)game_item;

    REQUIRE(h.rc.phase() == Phase::Shop);
    const auto& shop = h.rc.state().shop_offers;
    std::size_t item_index = 0;
    while (shop[item_index].kind != ShopOfferKind::Item) ++item_index;
    inv.room = 0;
    const std::int64_t balance = h.rc.state().supplies.balance();
    const Status full = h.rc.Purchase(item_index);
    CHECK(full.code == ErrorCode::GrantFailed);
    CHECK(h.rc.state().supplies.balance() == balance);  // refunded
    CHECK_FALSE(shop[item_index].purchased);
    inv.room = 1;
    REQUIRE(h.rc.Purchase(item_index).ok());
    CHECK(h.rc.state().supplies.balance() == balance - shop[item_index].price);
    CHECK(inv.placed.size() == 1);  // only the shop item (the reward was converted)
}

TEST_CASE("items are never offered without the inventory capability", "[items]") {
    const auto content = ItemEconomy();
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kInventory);
    test::Harness h(content, {}, caps);
    ClearFirstArena(h);
    for (const RewardChoice& c : h.rc.state().reward_offer) {
        CHECK(c.kind == RewardKind::Supplies);  // only padding is left
    }
    REQUIRE(h.rc.ChooseReward(0).ok());
    for (const ShopOffer& o : h.rc.state().shop_offers) CHECK(o.kind != ShopOfferKind::Item);
    CHECK(h.rc.ConsumeItemGrants().empty());
}

TEST_CASE("the loadout roll respects the distinct-item cap", "[items]") {
    ContentSources src = test::FixtureSources();
    src.loadouts = R"({"schema_version": 1, "budget": 60, "default_weapon_cost": 0, "max_items": 1,
      "equipment": [
        {"id": "a", "name": "A", "cost": 5, "max": 2},
        {"id": "b", "name": "B", "cost": 5, "max": 2},
        {"id": "c", "name": "C", "cost": 5, "max": 2}]})";
    const auto content = test::Load(src);
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        test::Harness h(content);
        REQUIRE(h.rc.StartRun("alpha", seed).ok());
        const Loadout& l = h.rc.state().loadout;
        CHECK(l.items.size() == 1);
        if (!l.items.empty()) CHECK(l.items[0].count == 2);  // tops up the one entry it picked
    }
}

TEST_CASE("supply perks queue their start items when granted", "[items]") {
    const auto content = test::Load(test::EditFixture([](json& perks, json&, json& characters, json&, json&) {
        perks["perks"].push_back(json{{"id", "shotgun_licence"},
                                      {"name", "Shotgun Licence"},
                                      {"description", "Start with a shotgun and two grenades."},
                                      {"category", "supply"},
                                      {"rarity", "rare"},
                                      {"max_stacks", 1},
                                      {"effects", json::array({json{{"kind", "item_on_start"}, {"game_item", 260}, {"amount", 1}},
                                                               json{{"kind", "item_on_start"}, {"game_item", 262}, {"amount", 2}}})}});
        characters["characters"][0]["starting_passive"] = "shotgun_licence";
    }));
    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 5).ok());
    const auto grants = h.rc.ConsumeItemGrants();
    REQUIRE(grants.size() == 2);
    CHECK(grants[0].game_item == 260);
    CHECK(grants[0].count == 1);
    CHECK(grants[1].game_item == 262);
    CHECK(grants[1].count == 2);
    CHECK(grants[1].source == "shotgun_licence");

    // Granted mid-run (dev / future equip) the items are queued too; without the inventory
    // capability the perk itself is not eligible.
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kInventory);
    test::Harness quiet(content, {}, caps);
    REQUIRE(quiet.rc.StartRun("alpha", 5).ok());
    CHECK(quiet.rc.ConsumeItemGrants().empty());
    CHECK(quiet.rc.state().perks.count("shotgun_licence") == 0);
}

TEST_CASE("one equipped perk per category drives the start of a run", "[items]") {
    const auto content = test::Load(test::EditFixture([](json& perks, json&, json&, json&, json&) {
        perks["perks"].push_back(json{{"id", "pouch"}, {"name", "Pouch"}, {"description", "Two grenades."}, {"category", "supply"},
                                      {"rarity", "common"}, {"max_stacks", 1},
                                      {"effects", json::array({json{{"kind", "item_on_start"}, {"game_item", 262}, {"amount", 2}}})}});
        perks["perks"].push_back(json{{"id", "medkit"}, {"name", "Medkit"}, {"description", "A spray."}, {"category", "supply"},
                                      {"rarity", "common"}, {"max_stacks", 1}, {"unlock_cost_credits", 50},
                                      {"effects", json::array({json{{"kind", "item_on_start"}, {"game_item", 772}, {"amount", 1}}})}});
    }));
    Profile profile;
    profile.unlocked_perks.insert("jackpot");
    test::Harness h(content, profile);

    // Locked, wrong category and foreign perks are refused.
    CHECK(h.rc.EquipPerk("alpha", "medkit").code == ErrorCode::NotEligible);
    CHECK(h.rc.EquipPerk("alpha", "curse_weak").code == ErrorCode::NotEligible);
    CHECK(h.rc.EquipPerk("alpha", "beta_passive").code == ErrorCode::NotEligible);
    CHECK(h.rc.EquipPerk("alpha", "nobody").code == ErrorCode::UnknownId);

    REQUIRE(h.rc.EquipPerk("alpha", "pouch").ok());
    REQUIRE(h.rc.EquipPerk("alpha", "jackpot").ok());  // style slot (fixture category)
    REQUIRE(h.rc.EquippedPerk("alpha", PerkCategory::Supply) != nullptr);
    CHECK(h.rc.EquippedPerk("alpha", PerkCategory::Supply)->id == "pouch");
    CHECK(h.rc.EquippedPerk("alpha", PerkCategory::Support) == nullptr);
    // A second style perk replaces the first within its category.
    REQUIRE(h.rc.EquipPerk("alpha", "dmg_up").ok());
    CHECK(h.rc.EquippedPerk("alpha", PerkCategory::Style)->id == "dmg_up");
    CHECK(h.rc.profile().equipped_perks.at("alpha").size() == 2);

    // The selection survives a profile round trip.
    const auto parsed = ParseProfile(SerializeProfile(h.rc.profile()));
    REQUIRE(parsed.ok());
    CHECK(parsed->profile.equipped_perks.at("alpha") == h.rc.profile().equipped_perks.at("alpha"));

    REQUIRE(h.rc.StartRun("alpha", 9).ok());
    CHECK(h.rc.state().perks.count("dmg_up") == 1);
    CHECK(h.rc.state().perks.count("pouch") == 1);
    CHECK(h.rc.state().perks.count("jackpot") == 0);
    const auto grants = h.rc.ConsumeItemGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].game_item == 262);

    // Clearing a slot: the style slot falls back to the character's own trait.
    test::Harness fresh(content, profile);
    REQUIRE(fresh.rc.EquipPerk("alpha", "jackpot").ok());
    REQUIRE(fresh.rc.UnequipPerk("alpha", PerkCategory::Style).ok());
    const PerkDef* style = fresh.rc.EquippedPerk("alpha", PerkCategory::Style);
    const CharacterDef* alpha = content->FindCharacter("alpha");
    REQUIRE(alpha != nullptr);
    if (alpha->starting_passive.empty()) {
        CHECK(style == nullptr);
    } else {
        REQUIRE(style != nullptr);
        CHECK(style->id == alpha->starting_passive);
    }
}

TEST_CASE("a bought weapon is placed in the inventory or the purchase is refunded", "[items]") {
    const auto content = test::Load(test::EditFixture([](json&, json& weapons, json&, json&, json& eco) {
        for (auto& w : weapons["weapons"]) w["game_item"] = 258 + static_cast<int>(w["name"].get<std::string>().size());
        eco["rewards"]["kind_weights"] = {{"perk", 0}, {"weapon", 1}, {"supplies", 0}, {"item", 0}};
        eco["shop"]["perk_offers"] = 0;
    }));
    test::Harness h(content);
    FakeInventory inv;
    h.rc.SetInventory(&inv);
    ClearFirstArena(h);
    const auto& offers = h.rc.state().reward_offer;
    REQUIRE_FALSE(offers.empty());
    REQUIRE(offers[0].kind == RewardKind::Weapon);
    // No room: the weapon reward pays its shop value instead.
    const std::int64_t before = h.rc.state().supplies.balance();
    const std::size_t held = h.rc.state().weapons.size();
    REQUIRE(h.rc.ChooseReward(0).ok());
    CHECK(inv.placed.empty());
    CHECK(h.rc.state().supplies.balance() > before);
    CHECK(h.rc.state().weapons.size() == held);  // nothing added

    REQUIRE(h.rc.phase() == Phase::Shop);
    const auto& shop = h.rc.state().shop_offers;
    std::size_t w = shop.size();
    for (std::size_t i = 0; i < shop.size(); ++i) {
        if (shop[i].kind == ShopOfferKind::Weapon) w = i;
    }
    REQUIRE(w < shop.size());
    const std::int64_t balance = h.rc.state().supplies.balance();
    CHECK(h.rc.Purchase(w).code == ErrorCode::GrantFailed);
    CHECK(h.rc.state().supplies.balance() == balance);  // refunded
    CHECK_FALSE(shop[w].purchased);
    inv.room = 1;
    REQUIRE(h.rc.Purchase(w).ok());
    REQUIRE(inv.placed.size() == 1);
    CHECK(inv.placed[0].first == content->FindWeapon(shop[w].weapon.base_id)->game_item);
    CHECK(h.rc.state().weapons.size() >= held);  // added, or replaced an instance of the same base
}
