#pragma once

#include <cstdint>
#include <functional>
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

enum class RewardKind { Perk, Weapon, Supplies, Item };
std::string_view RewardKindName(RewardKind k);

struct RewardChoice {
    RewardKind kind = RewardKind::Supplies;
    Rarity rarity = Rarity::Common;
    std::string perk_id;        // kind == Perk
    WeaponInstance weapon;      // kind == Weapon (fully rolled; what you see is what you get)
    std::int64_t supplies = 0;  // kind == Supplies
    std::string item_id;        // kind == Item: loadouts.json equipment id
    int item_count = 0;         // kind == Item
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
};

using InstanceIdSource = std::function<std::string()>;

/// Perks that can be offered now: enabled, not a curse, allowed for the character, required
/// capabilities present, unlocked, below max stacks and not excluded by an owned perk.
std::vector<const PerkDef*> OfferablePerks(const OfferContext& ctx);

/// Enabled weapon bases that are free or unlocked in the profile.
std::vector<const WeaponBaseDef*> OfferableWeapons(const OfferContext& ctx);

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
