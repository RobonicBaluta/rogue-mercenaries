// RunController::Snapshot: flattens run state into plain UI data.

#include <algorithm>
#include <cstdio>

#include "rm/rosters.h"
#include "rm/run_controller.h"

namespace rm {
namespace {

std::string Percent(double fraction) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", fraction * 100.0);
    return buf;
}

// A gun's lines: its elements first, then its legendary effects, then its bonuses.
std::vector<std::string> WeaponLines(const WeaponInstance& w, const Content& content) {
    std::vector<std::string> out;
    for (Element e : w.elements) out.push_back(DescribeElement(e, &content));
    for (const Affix& a : w.affixes) {
        if (IsUniqueAffix(a.kind)) out.push_back(DescribeAffix(a, &content));
    }
    for (const Affix& a : w.affixes) {
        if (!IsUniqueAffix(a.kind)) out.push_back(DescribeAffix(a, &content));
    }
    return out;
}

std::string JoinAffixes(const WeaponInstance& w, const Content& content) {
    std::string out;
    for (const std::string& line : WeaponLines(w, content)) {
        if (!out.empty()) out += "; ";
        out += line;
    }
    return out.empty() ? std::string("no affixes") : out;
}

std::string Capitalised(std::string_view s) {
    std::string out(s);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

// What a refine adds: the new elements, effects and bonuses, and how much the old bonuses grow.
std::string RefineDetail(const WeaponInstance* now, const WeaponInstance& refined, const Content& content) {
    std::string out = "Becomes " + Capitalised(RarityName(refined.rarity)) + ".";
    std::vector<std::string> gains;
    for (Element e : refined.elements) {
        if (now == nullptr || std::find(now->elements.begin(), now->elements.end(), e) == now->elements.end()) {
            gains.push_back(DescribeElement(e, &content));
        }
    }
    for (const Affix& a : refined.affixes) {
        const bool had = now != nullptr && std::any_of(now->affixes.begin(), now->affixes.end(), [&a](const Affix& x) {
            return x.kind == a.kind && x.unique_id == a.unique_id;
        });
        if (!had) gains.push_back(DescribeAffix(a, &content));
    }
    for (const std::string& g : gains) out += " " + g + ".";
    if (now != nullptr) {
        const auto& rules = content.economy().weapon_rarity;
        const double from = rules[RarityIndex(now->rarity)].magnitude_mult;
        const double to = rules[RarityIndex(refined.rarity)].magnitude_mult;
        const bool has_bonus = std::any_of(now->affixes.begin(), now->affixes.end(), [](const Affix& a) { return !IsUniqueAffix(a.kind); });
        if (has_bonus && from > 0.0 && to > from) out += " Its bonuses grow by " + Percent(to / from - 1.0) + ".";
    }
    return out;
}

// The maps.json name of an arena's map key; empty when unknown.
std::string ArenaName(const Content& content, int map_key) {
    const MapDef* map = map_key >= 0 ? content.FindMapByStage(map_key) : nullptr;
    return map != nullptr ? map->name : std::string();
}

// The boss of a boss arena's map key; empty otherwise.
std::string BossName(const Content& content, int map_key) {
    const MapDef* map = map_key >= 0 ? content.FindMapByStage(map_key) : nullptr;
    return map != nullptr && map->boss ? map->boss->name : std::string();
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
    s.free_shop_rerolls = run_.phase == Phase::Shop ? run_.free_shop_rerolls : 0;
    s.restock_price = ShopRestockPrice();
    s.keys_enabled = c.economy().keys.enabled;
    s.keys_needed = s.keys_enabled && !run_.character.build.chest_keys_exempt;
    s.keys = run_.keys;
    s.stats = run_.stats;
    s.invulnerable_left = std::max(0.0, run_.invulnerable_until - now);
    s.damage_buff_left = std::max(0.0, run_.damage_buff_until - now);
    s.damage_buff = s.damage_buff_left > 0.0 ? run_.damage_buff : 0.0;
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
        v.affixes = WeaponLines(w, c);
        v.level_bonus = LevelDamageBonus(c.economy(), w.level);
        if (const WeaponBaseDef* base = c.FindWeapon(w.base_id)) v.base_mult = base->base_damage_mult;
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
            case RewardKind::Refine: {
                v.id = r.target_instance_id;
                const WeaponInstance* held = nullptr;
                for (const WeaponInstance& w : run_.weapons) {
                    if (w.instance_id == r.target_instance_id) held = &w;
                }
                v.title = "Refine " + WeaponName(r.weapon, c);
                v.detail = RefineDetail(held, r.weapon, c);
                break;
            }
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
        v.restricted = o.restricted;
        switch (o.kind) {
            case ShopOfferKind::Heal:
                v.id = "heal";
                v.title = "Heal " + Percent(o.heal_fraction);
                break;
            case ShopOfferKind::RerollToken:
                v.id = "reroll";
                v.title = "Reward reroll";
                v.detail = "A token to draw three other rewards after an arena (not this shop: NEW STOCK does that)";
                break;
            case ShopOfferKind::WeaponUpgrade: {
                v.id = o.target_instance_id;
                std::string name = o.target_instance_id;
                for (const WeaponInstance& w : run_.weapons) {
                    if (w.instance_id == o.target_instance_id) name = WeaponName(w, c);
                }
                v.title = "Upgrade " + name + " to Lv " + std::to_string(o.target_level + 1);
                const double atLevel = LevelDamageBonus(c.economy(), o.target_level);
                const double afterUpgrade = LevelDamageBonus(c.economy(), o.target_level + 1);
                v.detail = "Damage from its level " + Percent(atLevel) + " now, " + Percent(afterUpgrade) + " after";
                break;
            }
            case ShopOfferKind::Weapon:
                v.id = o.weapon.base_id;
                v.title = WeaponName(o.weapon, c) + " (" + v.rarity + ")";
                v.detail = JoinAffixes(o.weapon, c);
                break;
            case ShopOfferKind::Refine: {
                v.id = o.target_instance_id;
                const WeaponInstance* held = nullptr;
                for (const WeaponInstance& w : run_.weapons) {
                    if (w.instance_id == o.target_instance_id) held = &w;
                }
                v.title = "Refine " + WeaponName(o.weapon, c);
                v.detail = RefineDetail(held, o.weapon, c);
                break;
            }
            case ShopOfferKind::Perk:
                v.id = o.perk_id;
                if (const PerkDef* p = c.FindPerk(o.perk_id)) {
                    v.title = p->name;
                    v.detail = p->description;
                }
                break;
            case ShopOfferKind::Item:
                v.id = o.item_id;
                if (const EquipmentDef* e = c.loadouts().FindEquipment(o.item_id)) {
                    v.title = e->name + (e->bundle > 1 ? " x" + std::to_string(e->bundle) : std::string());
                } else {
                    v.title = o.item_id;
                }
                v.detail = "Goes straight into your inventory";
                break;
        }
        s.shop_offers.push_back(std::move(v));
    }
    const auto& layers = run_.run_map.layers;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        std::vector<RunMapNodeView> views;
        for (std::size_t n = 0; n < layers[l].size(); ++n) {
            const RunMapNode& node = layers[l][n];
            RunMapNodeView v;
            v.map_key = node.map_key;
            v.arena_name = ArenaName(c, node.map_key);
            v.boss_name = BossName(c, node.map_key);
            v.route_id = node.route_id;
            if (const RouteDef* r = c.FindRoute(node.route_id)) {
                v.route_name = r->name;
                v.tier = r->difficulty_tier;
            }
            v.next = node.next;
            v.guests = node.guests;
            v.guests_from = GuestHomes(c, node.guests);
            v.current = static_cast<int>(l) == run_.map_layer && static_cast<int>(n) == run_.map_node;
            if (v.current) s.arena_name = v.arena_name;
            views.push_back(std::move(v));
        }
        s.run_map.push_back(std::move(views));
    }
    for (const RouteChoice& rc : run_.route_choices) {
        OfferView v;
        v.kind = "route";
        v.id = rc.route_id;
        const RouteDef* r = c.FindRoute(rc.route_id);
        const std::string arena = ArenaName(c, rc.map_key);
        const std::string boss = BossName(c, rc.map_key);
        v.boss = !boss.empty();
        v.title = v.boss ? boss : !arena.empty() ? arena : (r != nullptr ? r->name : rc.route_id);
        // Two short lines for the game's help box, whose font has no ':' or apostrophe: the route
        // and its rewards (the curse in place of the credits when it carries one), then where the
        // choice leads.
        if (r != nullptr) {
            v.tier = r->difficulty_tier;
            char buf[200];
            if (v.boss) {
                std::snprintf(buf, sizeof(buf), "Boss fight, rewards x%.2f", r->reward_mult);
            } else {
                std::snprintf(buf, sizeof(buf), "%s, %d enemies, rewards x%.2f",
                              arena.empty() ? ("Tier " + std::to_string(r->difficulty_tier)).c_str() : r->name.c_str(),
                              r->enemy_budget, r->reward_mult);
            }
            v.detail = buf;
            if (rc.curse_perk_id.empty()) {
                std::snprintf(buf, sizeof(buf), ", credits x%.2f", r->credit_mult);
                v.detail += buf;
            }
        }
        if (!rc.curse_perk_id.empty()) {
            const PerkDef* curse = c.FindPerk(rc.curse_perk_id);
            v.detail += ", curse " + (curse != nullptr ? curse->name : rc.curse_perk_id);
        }
        // Where the choice leads: the arenas of the nodes after it.
        const std::size_t next_layer = static_cast<std::size_t>(run_.map_layer) + 1;
        if (rc.node >= 0 && next_layer < layers.size() && static_cast<std::size_t>(rc.node) < layers[next_layer].size()) {
            const RunMapNode& node = layers[next_layer][static_cast<std::size_t>(rc.node)];
            std::string then;
            for (std::size_t i = 0; i < node.next.size() && next_layer + 1 < layers.size(); ++i) {
                const RunMapNode& after = layers[next_layer + 1][static_cast<std::size_t>(node.next[i])];
                const std::string after_boss = BossName(c, after.map_key);
                const std::string name = !after_boss.empty() ? after_boss : ArenaName(c, after.map_key);
                const RouteDef* ar = c.FindRoute(after.route_id);
                if (!then.empty()) then += i + 1 == node.next.size() ? " or " : ", ";
                then += !name.empty() ? name : ar != nullptr ? "tier " + std::to_string(ar->difficulty_tier) : after.route_id;
            }
            v.detail += then.empty() ? "\nThe last arena of the run." : "\nThen " + then + ".";
            // The arena's guests (M3.3), on whichever line of the help box has room.
            const std::string from = GuestHomes(c, node.guests);
            if (!from.empty()) {
                constexpr std::size_t kHelpLine = 56;
                const std::string also = " Also enemies from " + from + ".";
                const std::size_t nl = v.detail.find('\n');
                if (v.detail.size() - nl - 1 + also.size() <= kHelpLine) v.detail += also;
                else if (nl + 1 + also.size() <= kHelpLine) v.detail.insert(nl, "." + also);  // the first line has no stop
            }
        }
        s.route_offers.push_back(std::move(v));
    }
    return s;
}

}  // namespace rm
