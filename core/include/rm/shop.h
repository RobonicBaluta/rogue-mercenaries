#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rm/rarity.h"
#include "rm/rewards.h"
#include "rm/rng.h"
#include "rm/weapons.h"

namespace rm {

enum class ShopOfferKind { Heal, RerollToken, WeaponUpgrade, Weapon, Perk, Item, Refine };
std::string_view ShopOfferKindName(ShopOfferKind k);

struct ShopOffer {
    ShopOfferKind kind = ShopOfferKind::Heal;
    std::int64_t price = 0;  // supplies
    bool purchased = false;
    Rarity rarity = Rarity::Common;
    double heal_fraction = 0.0;      // Heal: fraction of max hp queued as a heal request
    std::string perk_id;             // Perk
    std::string item_id;             // Item: loadouts.json equipment id (one bundle)
    std::string target_instance_id;  // WeaponUpgrade, Refine
    int target_level = 0;            // WeaponUpgrade: level before the upgrade
    WeaponInstance weapon;           // Weapon; Refine: the target one tier up (rm::RefineWeapon)
    bool restricted = false;         // Weapon: the skin's build cannot use it (not for sale)
};

/// Shop stock drawn from the "shop" stream: heal (only with the "heal" capability), a reroll
/// token (if below the cap), one upgrade for a random upgradeable instance, one weapon and
/// economy shop.perk_offers distinct perks. Prices come from economy.json.
std::vector<ShopOffer> GenerateShopOffers(Rng& rng, const OfferContext& ctx,
                                          const std::vector<WeaponInstance>& owned_weapons,
                                          int reroll_tokens,
                                          const InstanceIdSource& next_instance_id);

}  // namespace rm
