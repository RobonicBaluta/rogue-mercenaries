#include "rm/shop.h"

#include <algorithm>

#include "rm/content.h"

namespace rm {

std::string_view ShopOfferKindName(ShopOfferKind k) {
    switch (k) {
        case ShopOfferKind::Heal: return "heal";
        case ShopOfferKind::RerollToken: return "reroll";
        case ShopOfferKind::WeaponUpgrade: return "upgrade";
        case ShopOfferKind::Weapon: return "weapon";
        case ShopOfferKind::Perk: return "perk";
        case ShopOfferKind::Item: return "item";
    }
    return "heal";
}

std::vector<ShopOffer> GenerateShopOffers(Rng& rng, const OfferContext& ctx,
                                          const std::vector<WeaponInstance>& owned_weapons,
                                          int reroll_tokens,
                                          const InstanceIdSource& next_instance_id) {
    std::vector<ShopOffer> out;
    if (ctx.content == nullptr) return out;
    const EconomyConfig& eco = ctx.content->economy();
    const Capabilities all = Capabilities::All();
    const Capabilities& caps = ctx.capabilities != nullptr ? *ctx.capabilities : all;

    if (caps.Has(cap::kHeal)) {
        ShopOffer o;
        o.kind = ShopOfferKind::Heal;
        o.price = eco.shop.heal_price;
        o.heal_fraction = eco.shop.heal_fraction;
        out.push_back(std::move(o));
    }
    if (reroll_tokens < eco.run.max_reroll_tokens) {
        ShopOffer o;
        o.kind = ShopOfferKind::RerollToken;
        o.price = eco.shop.reroll_price;
        out.push_back(std::move(o));
    }
    if (caps.Has(cap::kOutgoingDamage)) {
        std::vector<const WeaponInstance*> upgradeable;
        for (const WeaponInstance& w : owned_weapons) {
            if (WeaponUpgradeCost(ctx, w).has_value()) upgradeable.push_back(&w);
        }
        if (!upgradeable.empty()) {
            const WeaponInstance* w = upgradeable[rng.Index(upgradeable.size())];
            ShopOffer o;
            o.kind = ShopOfferKind::WeaponUpgrade;
            o.price = *WeaponUpgradeCost(ctx, *w);
            o.target_instance_id = w->instance_id;
            o.target_level = w->level;
            o.rarity = w->rarity;
            out.push_back(std::move(o));
        }
    }

    const RarityWeights weights = ShiftRarityWeights(eco.rewards.rarity_weights, ctx.rarity_shift);
    const auto owned = [&owned_weapons](std::string_view id) {
        return std::any_of(owned_weapons.begin(), owned_weapons.end(), [id](const WeaponInstance& w) { return w.base_id == id; });
    };
    // A build's own gun (Heavy Metal's Gatling) is in every shop, with no unlock, until bought:
    // without it the build is the weakest there is (user playtest 2026-11-04).
    const std::string shop_weapon = ctx.character != nullptr ? ctx.character->build.shop_weapon : std::string();
    if (caps.Has(cap::kOutgoingDamage) && !shop_weapon.empty() && !owned(shop_weapon)) {
        if (const WeaponBaseDef* base = ctx.content->FindWeapon(shop_weapon); base != nullptr && base->enabled) {
            ShopOffer o;
            o.kind = ShopOfferKind::Weapon;
            o.rarity = Rarity::Common;
            const std::int64_t price = ctx.character->build.shop_weapon_price;
            o.price = price > 0 ? price : eco.shop.weapon_price[RarityIndex(Rarity::Common)];
            o.weapon = GenerateWeapon(rng, *ctx.content, caps, base->id, Rarity::Common,
                                      next_instance_id ? next_instance_id() : std::string("w"));
            o.restricted = !UsableWeapon(ctx, *base);
            out.push_back(std::move(o));
        }
    }
    // Never sell a gun the player already carries.
    std::vector<const WeaponBaseDef*> bases = OfferableWeapons(ctx);
    std::erase_if(bases, [&owned, &shop_weapon](const WeaponBaseDef* b) { return owned(b->id) || b->id == shop_weapon; });
    if (!bases.empty()) {
        const Rarity rarity = RollRarity(rng, weights);
        const WeaponBaseDef* base = bases[rng.Index(bases.size())];
        ShopOffer o;
        o.kind = ShopOfferKind::Weapon;
        o.rarity = rarity;
        o.price = eco.shop.weapon_price[RarityIndex(rarity)];
        o.weapon = GenerateWeapon(rng, *ctx.content, caps, base->id, rarity,
                                  next_instance_id ? next_instance_id() : std::string("w"));
        o.restricted = !UsableWeapon(ctx, *base);  // shown, not for sale to this build
        out.push_back(std::move(o));
    }

    const std::vector<const PerkDef*> pool = OfferablePerks(ctx);
    StringSet taken;
    for (int i = 0; i < eco.shop.perk_offers; ++i) {
        const Rarity rarity = RollRarity(rng, weights);
        const PerkDef* p = PickPerkNearRarity(rng, pool, rarity, taken);
        if (p == nullptr) break;
        taken.insert(p->id);
        ShopOffer o;
        o.kind = ShopOfferKind::Perk;
        o.perk_id = p->id;
        o.rarity = p->rarity;
        o.price = eco.shop.perk_price[RarityIndex(p->rarity)];
        out.push_back(std::move(o));
    }

    std::vector<const EquipmentDef*> items = OfferableItems(ctx);
    for (int i = 0; i < eco.shop.item_offers && !items.empty(); ++i) {
        const std::size_t pick = rng.Index(items.size());
        const EquipmentDef* e = items[pick];
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(pick));
        ShopOffer o;
        o.kind = ShopOfferKind::Item;
        o.item_id = e->id;
        o.price = e->price;
        out.push_back(std::move(o));
    }
    return out;
}

}  // namespace rm
