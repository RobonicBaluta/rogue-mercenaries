// RunController::Snapshot: flattens run state into plain UI data.

#include <algorithm>
#include <cstdio>

#include "rm/run_controller.h"

namespace rm {
namespace {

std::string Percent(double fraction) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", fraction * 100.0);
    return buf;
}

std::string JoinAffixes(const WeaponInstance& w, const Content& content) {
    std::string out;
    for (const Affix& a : w.affixes) {
        if (!out.empty()) out += "; ";
        out += DescribeAffix(a, &content);
    }
    return out.empty() ? std::string("no affixes") : out;
}

std::string WeaponName(const WeaponInstance& w, const Content& content) {
    const WeaponBaseDef* def = content.FindWeapon(w.base_id);
    return def != nullptr ? def->name : w.base_id;
}

}  // namespace

RunSnapshot RunController::Snapshot(double now) const {
    RunSnapshot s;
    const Content& c = *content_;
    s.run_active = run_.active;
    s.phase = run_.phase;
    s.phase_name = std::string(PhaseName(run_.phase));
    s.run_id = run_.run_id;
    s.character_id = run_.character_id;
    s.skin_id = run_.skin_id;
    if (run_.active) s.loadout = FormatLoadout(run_.loadout, c);
    s.route_id = run_.route_id;
    if (const RouteDef* r = Route()) s.route_name = r->name;
    s.arena = run_.arena_index;
    s.arena_count = run_.arena_count;
    if (const ArenaMetrics* a = CurrentArena()) {
        s.kills = a->player_kills;
        s.budget = a->budget;
    }
    s.remaining = RemainingEnemies();
    s.supplies = run_.supplies.balance();
    s.credits = profile_.credits.balance();
    s.credits_pending = run_.credits_pending;
    s.reroll_tokens = run_.reroll_tokens;
    const int charges = CheatDeathCharges();
    const ArenaMetrics* current = CurrentArena();
    const int used = (run_.phase == Phase::ArenaActive && current != nullptr) ? current->cheat_death_used : 0;
    s.cheat_death_remaining = std::max(0, charges - used);
    s.kill_streak_bonus = run_.phase == Phase::ArenaActive ? KillStreakBonus(now, &s.kill_streak_kills) : 0.0;
    s.pending_heal = PendingHeal();
    if (!run_.results.empty()) s.last_result = run_.results.back();
    s.reward_chosen = run_.reward_chosen;

    for (const auto& [id, stacks] : run_.perks) {
        PerkView v;
        v.id = id;
        v.stacks = stacks;
        if (const PerkDef* p = c.FindPerk(id)) {
            v.name = p->name;
            v.category = std::string(PerkCategoryName(p->category));
            v.rarity = std::string(RarityName(p->rarity));
        }
        s.perks.push_back(std::move(v));
    }
    for (const WeaponInstance& w : run_.weapons) {
        WeaponView v;
        v.instance_id = w.instance_id;
        v.base_id = w.base_id;
        v.name = WeaponName(w, c);
        v.rarity = std::string(RarityName(w.rarity));
        v.level = w.level;
        for (const Affix& a : w.affixes) v.affixes.push_back(DescribeAffix(a, &c));
        s.weapons.push_back(std::move(v));
    }
    for (const RewardChoice& r : run_.reward_offer) {
        OfferView v;
        v.kind = std::string(RewardKindName(r.kind));
        v.rarity = std::string(RarityName(r.rarity));
        switch (r.kind) {
            case RewardKind::Perk:
                v.id = r.perk_id;
                if (const PerkDef* p = c.FindPerk(r.perk_id)) {
                    v.title = p->name;
                    v.detail = p->description;
                }
                break;
            case RewardKind::Weapon:
                v.id = r.weapon.base_id;
                v.title = WeaponName(r.weapon, c) + " (" + v.rarity + ")";
                v.detail = JoinAffixes(r.weapon, c);
                break;
            case RewardKind::Supplies:
                v.id = "supplies";
                v.title = "+" + std::to_string(r.supplies) + " supplies";
                break;
            case RewardKind::Item:
                v.id = r.item_id;
                if (const EquipmentDef* e = c.loadouts().FindEquipment(r.item_id)) {
                    v.title = e->name + (r.item_count > 1 ? " x" + std::to_string(r.item_count) : std::string());
                } else {
                    v.title = r.item_id;
                }
                v.detail = "Goes straight into your inventory";
                break;
        }
        s.reward_offers.push_back(std::move(v));
    }
    for (const ShopOffer& o : run_.shop_offers) {
        OfferView v;
        v.kind = std::string(ShopOfferKindName(o.kind));
        v.rarity = std::string(RarityName(o.rarity));
        v.price = o.price;
        v.purchased = o.purchased;
        switch (o.kind) {
            case ShopOfferKind::Heal:
                v.id = "heal";
                v.title = "Heal " + Percent(o.heal_fraction);
                break;
            case ShopOfferKind::RerollToken:
                v.id = "reroll";
                v.title = "Reroll token";
                v.detail = "Reroll the next reward offer once";
                break;
            case ShopOfferKind::WeaponUpgrade: {
                v.id = o.target_instance_id;
                std::string name = o.target_instance_id;
                for (const WeaponInstance& w : run_.weapons) {
                    if (w.instance_id == o.target_instance_id) name = WeaponName(w, c);
                }
                v.title = "Upgrade " + name + " to Lv " + std::to_string(o.target_level + 1);
                break;
            }
            case ShopOfferKind::Weapon:
                v.id = o.weapon.base_id;
                v.title = WeaponName(o.weapon, c) + " (" + v.rarity + ")";
                v.detail = JoinAffixes(o.weapon, c);
                break;
            case ShopOfferKind::Perk:
                v.id = o.perk_id;
                if (const PerkDef* p = c.FindPerk(o.perk_id)) {
                    v.title = p->name;
                    v.detail = p->description;
                }
                break;
            case ShopOfferKind::Item:
                v.id = o.item_id;
                if (const EquipmentDef* e = c.loadouts().FindEquipment(o.item_id)) v.title = e->name;
                else v.title = o.item_id;
                v.detail = "Goes straight into your inventory";
                break;
        }
        s.shop_offers.push_back(std::move(v));
    }
    for (const RouteChoice& rc : run_.route_choices) {
        OfferView v;
        v.kind = "route";
        v.id = rc.route_id;
        if (const RouteDef* r = c.FindRoute(rc.route_id)) {
            v.title = r->name;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Tier %d, %d enemies, rewards x%.2f, credits x%.2f", r->difficulty_tier,
                          r->enemy_budget, r->reward_mult, r->credit_mult);
            v.detail = buf;
        }
        if (!rc.curse_perk_id.empty()) {
            const PerkDef* curse = c.FindPerk(rc.curse_perk_id);
            v.detail += "; curse: " + (curse != nullptr ? curse->name : rc.curse_perk_id);
        }
        s.route_offers.push_back(std::move(v));
    }
    return s;
}

}  // namespace rm
