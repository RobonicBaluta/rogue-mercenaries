#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/capabilities.h"
#include "rm/common.h"
#include "rm/rarity.h"
#include "rm/rng.h"
#include "rm/weapons.h"

namespace rm {

class Content;
struct CharacterDef;
struct PerkDef;

enum class RewardKind { Perk, Weapon, Supplies, Item, Refine };
std::string_view RewardKindName(RewardKind k);

struct RewardChoice {
    RewardKind kind = RewardKind::Supplies;
    Rarity rarity = Rarity::Common;
    std::string perk_id;        // kind == Perk
    WeaponInstance weapon;      // kind == Weapon (fully rolled; what you see is what you get)
    std::int64_t supplies = 0;  // kind == Supplies
    std::string item_id;        // kind == Item: loadouts.json equipment id
    int item_count = 0;         // kind == Item
    // kind == Refine: `weapon` is the held gun `target_instance_id` one tier up (rm::RefineWeapon,
    // rolled when offered: what you see is what you get); `rarity` the tier it reaches.
    std::string target_instance_id;
};

struct EquipmentDef;

/// Run/profile facts used by reward and shop generation.
struct OfferContext {
    const Content* content = nullptr;
    const CharacterDef* character = nullptr;
    const Capabilities* capabilities = nullptr;
    const StringSet* unlocked_perks = nullptr;
    const StringSet* unlocked_weapons = nullptr;
    const StringMap<int>* owned_perks = nullptr;
    double rarity_shift = 0.0;  // route rarity_bonus (+ rating bonus for post-arena rewards)
    double reward_mult = 1.0;   // route reward_mult (scales supplies bundles)
    /// Weapon base ids the adapter can put in the game right now (null = no restriction).
    const StringSet* placeable_weapons = nullptr;
    /// Weapons the run already holds: never offered again (rewards, shop, chests).
    const std::vector<WeaponInstance>* owned_weapons = nullptr;
    /// M10: what the other players were offered ("weapon:<id>", "perk:<id>", "item:<id>"; 2026-11-22, the
    /// user: "stores and rewards should be separated for each player"): skipped while anything else is left.
    const StringSet* avoid = nullptr;
};

/// True when `kind`:`id` is one of ctx.avoid's.
bool OfferAvoided(const OfferContext& ctx, std::string_view kind, std::string_view id);

/// `pool` without what ctx.avoid names (`key` gives an entry's kind and id), unless that leaves nothing.
template <class T, class KeyFn>
std::vector<T> WithoutAvoided(const OfferContext& ctx, std::vector<T> pool, std::string_view kind, KeyFn&& key) {
    if (ctx.avoid == nullptr || ctx.avoid->empty()) return pool;
    std::vector<T> kept;
    for (const T& entry : pool) {
        if (!OfferAvoided(ctx, kind, key(entry))) kept.push_back(entry);
    }
    return kept.empty() ? pool : kept;
}

using InstanceIdSource = std::function<std::string()>;

/// Perks that can be offered now: enabled, not a curse, allowed for the character, required
/// capabilities present, unlocked, below max stacks and not excluded by an owned perk.
std::vector<const PerkDef*> OfferablePerks(const OfferContext& ctx);

/// Enabled weapon bases that are free or unlocked in the profile and among the skin build's
/// offered weapons.
std::vector<const WeaponBaseDef*> OfferableWeapons(const OfferContext& ctx);

/// False for a gun the skin's build cannot use (the shop shows it, rewards and chests skip it).
bool UsableWeapon(const OfferContext& ctx, const WeaponBaseDef& w);

/// The next upgrade's price for a held weapon (nullopt at its maximum; a build's uncapped
/// weapons have none).
std::optional<std::int64_t> WeaponUpgradeCost(const OfferContext& ctx, const WeaponInstance& w);

/// Equipment the shop and the rewards may hand out: priced, backed by a game item, and only
/// when the adapter can write the inventory.
std::vector<const EquipmentDef*> OfferableItems(const OfferContext& ctx);

Rarity RollRarity(Rng& rng, const RarityWeights& weights);

/// Picks a perk of `rarity` from `pool` (skipping ids in `taken`); if none exists, searches
/// the nearest rarity, lower tiers first. Returns nullptr when nothing is left.
const PerkDef* PickPerkNearRarity(Rng& rng, const std::vector<const PerkDef*>& pool,
                                  Rarity rarity, const StringSet& taken);

/// Post-arena offer of economy rewards.choices distinct choices: unique perk ids, at most
/// one weapon and one supplies bundle (further slots fall back to larger supplies bundles
/// if the pools run dry, so the offer is never short). Deterministic for a given rng state.
std::vector<RewardChoice> GenerateRewardOffer(Rng& rng, const OfferContext& ctx,
                                              const InstanceIdSource& next_instance_id);

}  // namespace rm
