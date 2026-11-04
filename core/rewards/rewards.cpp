#include "rm/rewards.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "rm/content.h"

namespace rm {
namespace {

std::int64_t SuppliesBundle(const EconomyConfig& eco, Rarity rarity, double reward_mult, int pad) {
    const double tier_scale = 1.0 + 0.5 * static_cast<double>(RarityIndex(rarity));
    const double pad_scale = 1.0 + 0.5 * static_cast<double>(pad);
    const double amount =
        static_cast<double>(eco.rewards.supplies_bundle) * reward_mult * tier_scale * pad_scale;
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(amount)));
}

}  // namespace

std::string_view RewardKindName(RewardKind k) {
    switch (k) {
        case RewardKind::Perk: return "perk";
        case RewardKind::Weapon: return "weapon";
        case RewardKind::Supplies: return "supplies";
        case RewardKind::Item: return "item";
    }
    return "supplies";
}

std::vector<const PerkDef*> OfferablePerks(const OfferContext& ctx) {
    std::vector<const PerkDef*> out;
    if (ctx.content == nullptr) return out;
    PerkGateInput gate;
    gate.character = ctx.character;
    gate.capabilities = ctx.capabilities;
    gate.unlocked_perks = ctx.unlocked_perks;
    gate.owned = ctx.owned_perks;
    gate.curses = CursePolicy::Exclude;
    for (const PerkDef& p : ctx.content->perks()) {
        if (CheckPerk(p, gate) == PerkGate::Eligible) out.push_back(&p);
    }
    return out;
}

bool UsableWeapon(const OfferContext& ctx, const WeaponBaseDef& w) {
    return ctx.character == nullptr || ctx.character->build.usable_weapons.Allows(w.id, WeaponCategoryName(w.category));
}

std::optional<std::int64_t> WeaponUpgradeCost(const OfferContext& ctx, const WeaponInstance& w) {
    if (ctx.content == nullptr) return std::nullopt;
    const WeaponBaseDef* base = ctx.content->FindWeapon(w.base_id);
    const bool uncapped = base != nullptr && ctx.character != nullptr &&
                          ctx.character->build.Uncapped(base->id, WeaponCategoryName(base->category));
    return UpgradeCost(ctx.content->economy(), w.level, uncapped);
}

std::vector<const WeaponBaseDef*> OfferableWeapons(const OfferContext& ctx) {
    std::vector<const WeaponBaseDef*> out;
    if (ctx.content == nullptr) return out;
    // A weapon instance only changes outgoing damage; without that capability it is inert.
    if (ctx.capabilities != nullptr && !ctx.capabilities->Has(cap::kOutgoingDamage)) return out;
    for (const WeaponBaseDef& w : ctx.content->weapons()) {
        if (!w.enabled) continue;
        if (ctx.placeable_weapons != nullptr && ctx.placeable_weapons->find(w.id) == ctx.placeable_weapons->end()) continue;
        if (ctx.owned_weapons != nullptr &&
            std::any_of(ctx.owned_weapons->begin(), ctx.owned_weapons->end(), [&w](const WeaponInstance& o) { return o.base_id == w.id; })) {
            continue;
        }
        if (w.unlock_cost_credits > 0 &&
            (ctx.unlocked_weapons == nullptr ||
             ctx.unlocked_weapons->find(w.id) == ctx.unlocked_weapons->end())) {
            continue;
        }
        if (ctx.character != nullptr && !ctx.character->build.offered_weapons.Allows(w.id, WeaponCategoryName(w.category))) {
            continue;
        }
        if (ctx.character != nullptr && !ctx.character->build.never_offered.Empty() &&
            ctx.character->build.never_offered.Allows(w.id, WeaponCategoryName(w.category))) {
            continue;
        }
        if (w.build_only &&
            (ctx.character == nullptr || std::find(ctx.character->build.extra_weapons.begin(),
                                                   ctx.character->build.extra_weapons.end(), w.id) == ctx.character->build.extra_weapons.end())) {
            continue;
        }
        out.push_back(&w);
    }
    return out;
}

std::vector<const EquipmentDef*> OfferableItems(const OfferContext& ctx) {
    std::vector<const EquipmentDef*> out;
    if (ctx.content == nullptr) return out;
    if (ctx.capabilities != nullptr && !ctx.capabilities->Has(cap::kInventory)) return out;
    for (const EquipmentDef& e : ctx.content->loadouts().equipment) {
        if (!e.Offerable()) continue;
        // A launcher's rounds only while the run holds the launcher.
        if (!e.for_weapons.empty() &&
            (ctx.owned_weapons == nullptr ||
             std::none_of(ctx.owned_weapons->begin(), ctx.owned_weapons->end(), [&e](const WeaponInstance& w) {
                 return std::find(e.for_weapons.begin(), e.for_weapons.end(), w.base_id) != e.for_weapons.end();
             }))) {
            continue;
        }
        out.push_back(&e);
    }
    return out;
}

Rarity RollRarity(Rng& rng, const RarityWeights& weights) {
    const std::size_t i = rng.WeightedIndex(weights);
    return i < kRarityCount ? kAllRarities[i] : Rarity::Common;
}

const PerkDef* PickPerkNearRarity(Rng& rng, const std::vector<const PerkDef*>& pool, Rarity rarity,
                                  const StringSet& taken) {
    const int target = static_cast<int>(RarityIndex(rarity));
    const int count = static_cast<int>(kRarityCount);
    std::vector<const PerkDef*> candidates;
    for (int dist = 0; dist < count; ++dist) {
        for (const int sign : {-1, 1}) {
            if (dist == 0 && sign == 1) continue;
            const int r = target + sign * dist;
            if (r < 0 || r >= count) continue;
            candidates.clear();
            for (const PerkDef* p : pool) {
                if (static_cast<int>(RarityIndex(p->rarity)) == r && taken.find(p->id) == taken.end()) {
                    candidates.push_back(p);
                }
            }
            if (!candidates.empty()) return candidates[rng.Index(candidates.size())];
        }
    }
    return nullptr;
}

std::vector<RewardChoice> GenerateRewardOffer(Rng& rng, const OfferContext& ctx,
                                              const InstanceIdSource& next_instance_id) {
    std::vector<RewardChoice> out;
    if (ctx.content == nullptr) return out;
    const EconomyConfig& eco = ctx.content->economy();
    const Capabilities all = Capabilities::All();
    const Capabilities& caps = ctx.capabilities != nullptr ? *ctx.capabilities : all;
    const RarityWeights weights = ShiftRarityWeights(eco.rewards.rarity_weights, ctx.rarity_shift);
    const std::vector<const PerkDef*> perks = OfferablePerks(ctx);
    std::vector<const WeaponBaseDef*> weapons = OfferableWeapons(ctx);
    std::erase_if(weapons, [&ctx](const WeaponBaseDef* w) { return !UsableWeapon(ctx, *w); });  // rewards never offer a gun the build cannot use
    const std::vector<const EquipmentDef*> items = OfferableItems(ctx);

    StringSet taken;
    StringSet taken_items;
    bool has_weapon = false;
    bool has_supplies = false;
    int pad = 1;
    const auto wanted = static_cast<std::size_t>(std::max(1, eco.rewards.choices));
    while (out.size() < wanted) {
        const bool perk_left = std::any_of(perks.begin(), perks.end(), [&taken](const PerkDef* p) {
            return taken.find(p->id) == taken.end();
        });
        const bool item_left = std::any_of(items.begin(), items.end(), [&taken_items](const EquipmentDef* e) {
            return taken_items.find(e->id) == taken_items.end();
        });
        const std::array<double, 4> kind_weights = {
            perk_left ? eco.rewards.perk_weight : 0.0,
            (!has_weapon && !weapons.empty()) ? eco.rewards.weapon_weight : 0.0,
            !has_supplies ? eco.rewards.supplies_weight : 0.0,
            item_left ? eco.rewards.item_weight : 0.0,
        };
        const std::size_t kind = rng.WeightedIndex(kind_weights);
        const Rarity rarity = RollRarity(rng, weights);
        RewardChoice choice;
        choice.rarity = rarity;
        if (kind == 0) {
            const PerkDef* p = PickPerkNearRarity(rng, perks, rarity, taken);
            if (p == nullptr) break;  // unreachable: perk_left guarantees a candidate
            choice.kind = RewardKind::Perk;
            choice.perk_id = p->id;
            choice.rarity = p->rarity;
            taken.insert(p->id);
        } else if (kind == 1) {
            const WeaponBaseDef* base = weapons[rng.Index(weapons.size())];
            choice.kind = RewardKind::Weapon;
            choice.weapon = GenerateWeapon(rng, *ctx.content, caps, base->id, rarity,
                                           next_instance_id ? next_instance_id() : std::string("w"));
            has_weapon = true;
        } else if (kind == 2) {
            choice.kind = RewardKind::Supplies;
            choice.supplies = SuppliesBundle(eco, rarity, ctx.reward_mult, 0);
            has_supplies = true;
        } else if (kind == 3) {
            std::vector<const EquipmentDef*> left;
            for (const EquipmentDef* e : items) {
                if (taken_items.find(e->id) == taken_items.end()) left.push_back(e);
            }
            const EquipmentDef* e = left[rng.Index(left.size())];
            choice.kind = RewardKind::Item;
            choice.rarity = Rarity::Common;
            choice.item_id = e->id;
            choice.item_count = e->bundle;
            taken_items.insert(e->id);
        } else {
            // Every pool is exhausted: pad with progressively larger, distinct bundles.
            choice.kind = RewardKind::Supplies;
            choice.supplies = SuppliesBundle(eco, rarity, ctx.reward_mult, pad++);
        }
        if (choice.kind == RewardKind::Supplies) {
            for (bool clash = true; clash;) {
                clash = false;
                for (const RewardChoice& c : out) {
                    if (c.kind == RewardKind::Supplies && c.supplies == choice.supplies) {
                        ++choice.supplies;
                        clash = true;
                    }
                }
            }
        }
        out.push_back(std::move(choice));
    }
    return out;
}

}  // namespace rm
