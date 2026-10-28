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
            if (UpgradeCost(eco, w.level).has_value()) upgradeable.push_back(&w);
        }
        if (!upgradeable.empty()) {
            const WeaponInstance* w = upgradeable[rng.Index(upgradeable.size())];
            ShopOffer o;
            o.kind = ShopOfferKind::WeaponUpgrade;
            o.price = *UpgradeCost(eco, w->level);
            o.target_instance_id = w->instance_id;
            o.target_level = w->level;
            o.rarity = w->rarity;
            out.push_back(std::move(o));
        }
    }

    const RarityWeights weights = ShiftRarityWeights(eco.rewards.rarity_weights, ctx.rarity_shift);
    // Never sell a gun the player already carries.
    std::vector<const WeaponBaseDef*> bases = OfferableWeapons(ctx);
    std::erase_if(bases, [&owned_weapons](const WeaponBaseDef* b) {
        return std::any_of(owned_weapons.begin(), owned_weapons.end(),
                           [b](const WeaponInstance& w) { return w.base_id == b->id; });
    });
    if (!bases.empty()) {
        const Rarity rarity = RollRarity(rng, weights);
        const WeaponBaseDef* base = bases[rng.Index(bases.size())];
        ShopOffer o;
        o.kind = ShopOfferKind::Weapon;
        o.rarity = rarity;
        o.price = eco.shop.weapon_price[RarityIndex(rarity)];
        o.weapon = GenerateWeapon(rng, *ctx.content, caps, base->id, rarity,
                                  next_instance_id ? next_instance_id() : std::string("w"));
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
