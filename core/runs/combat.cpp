// Damage modifier evaluation for RunController (outgoing/incoming hooks, kill streaks).

#include <algorithm>
#include <cmath>

#include "rm/run_controller.h"

namespace rm {

const WeaponInstance* RunController::ApplicableWeapon(const std::optional<std::string>& weapon_id,
                                                      const std::optional<bool>& melee) const {
    if (melee.value_or(false)) return nullptr;  // weapon modifiers never apply to melee
    if (WeaponIdMode()) {
        if (!weapon_id) return nullptr;
        for (const WeaponInstance& w : run_.weapons) {
            if (w.base_id == *weapon_id) return &w;
        }
        return nullptr;
    }
    return run_.weapons.empty() ? nullptr : &run_.weapons.front();  // sidearm slot
}

int RunController::ActiveKills(double now, double window) const {
    int n = 0;
    for (const KillRecord& r : run_.kill_records) {
        if (r.time <= now && now < r.time + window + r.extra_seconds) ++n;
    }
    return n;
}

double RunController::KillStreakBonus(double now, int* kills_in_longest_window) const {
    double bonus = 0.0;
    double longest = -1.0;
    int kills = 0;
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def == nullptr) continue;
        for (const EffectDef& ef : def->effects) {
            if (ef.kind != EffectKind::OutgoingDamageMult || ef.condition != ConditionKind::KillStreak) continue;
            const EffectParams p = ef.AtStacks(stacks);
            const int n = ActiveKills(now, p.window_seconds);
            bonus += std::min(p.max_bonus, p.per_kill * static_cast<double>(n));
            if (p.window_seconds > longest) {
                longest = p.window_seconds;
                kills = n;
            }
        }
    }
    if (kills_in_longest_window != nullptr) *kills_in_longest_window = kills;
    return bonus;
}

int RunController::CheatDeathCharges() const {
    int charges = 0;
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def == nullptr) continue;
        for (const EffectDef& ef : def->effects) {
            if (ef.kind == EffectKind::CheatDeath) {
                charges += static_cast<int>(std::floor(ef.AtStacks(stacks).per_arena));
            }
        }
    }
    return charges;
}

OutgoingResult RunController::ComputeOutgoingMultiplier(const HitContext& hit) {
    OutgoingResult out;
    if (hit.player_hp && hit.player_hp_max && *hit.player_hp_max > 0.0) {
        run_.player_hp = *hit.player_hp;
        run_.player_hp_max = *hit.player_hp_max;
    }
    if (!run_.active || run_.phase != Phase::ArenaActive || !streams_) return out;

    const EconomyConfig& eco = content_->economy();
    const RouteDef* route = Route();
    OutgoingBreakdown& b = out.breakdown;
    b.route_mult = route != nullptr ? route->modifiers.outgoing_damage_mult : 1.0;

    std::optional<double> target_fraction;
    if (hit.target_hp && hit.target_hp_max && *hit.target_hp_max > 0.0) {
        target_fraction = std::clamp(*hit.target_hp / *hit.target_hp_max, 0.0, 1.0);
    }
    std::optional<double> player_fraction;
    if (run_.player_hp && run_.player_hp_max && *run_.player_hp_max > 0.0) {
        player_fraction = std::clamp(*run_.player_hp / *run_.player_hp_max, 0.0, 1.0);
    }

    double crit_chance = 0.0;
    double best_crit_mult = 0.0;
    double crit_extra = 0.0;
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def == nullptr) continue;
        for (const EffectDef& ef : def->effects) {
            const EffectParams p = ef.AtStacks(stacks);
            if (ef.kind == EffectKind::Crit) {
                crit_chance += p.chance;
                best_crit_mult = std::max(best_crit_mult, p.multiplier);
                continue;
            }
            if (ef.kind != EffectKind::OutgoingDamageMult) continue;
            switch (ef.condition) {
                case ConditionKind::Always: b.perk_bonus += p.value; break;
                case ConditionKind::TargetHpBelow:
                    if (target_fraction && *target_fraction < p.fraction) b.perk_bonus += p.value;
                    break;
                case ConditionKind::KillStreak:
                    b.kill_streak_bonus +=
                        std::min(p.max_bonus, p.per_kill * static_cast<double>(ActiveKills(hit.now, p.window_seconds)));
                    break;
                case ConditionKind::PlayerHpBelow:
                    if (player_fraction && *player_fraction < p.fraction) b.perk_bonus += p.value;
                    break;
                case ConditionKind::AfterMelee:
                    if (run_.last_melee_time && hit.now >= *run_.last_melee_time &&
                        hit.now - *run_.last_melee_time <= p.window_seconds) {
                        b.perk_bonus += p.value;
                    }
                    break;
            }
        }
    }

    // The skin's build: melee damage, the stats a run gained, a heal's damage buff.
    const SkinBuild& build = run_.character.build;
    if (hit.is_melee.value_or(false)) b.build_bonus += build.melee_damage;
    b.build_bonus += run_.stats.damage;
    if (hit.now < run_.damage_buff_until) b.build_bonus += run_.damage_buff;
    crit_chance += run_.stats.crit;
    // Damage by the category of the gun that fired (any gun of that kind, instance or not;
    // never melee).
    if (hit.weapon_id && !hit.is_melee.value_or(false) && !run_.character.build.category_damage.empty()) {
        if (const WeaponBaseDef* base = content_->FindWeapon(*hit.weapon_id)) {
            const auto it = run_.character.build.category_damage.find(std::string(WeaponCategoryName(base->category)));
            if (it != run_.character.build.category_damage.end()) b.build_bonus += it->second;
        }
    }

    if (const WeaponInstance* weapon = ApplicableWeapon(hit.weapon_id, hit.is_melee)) {
        const WeaponBaseDef* base = content_->FindWeapon(weapon->base_id);
        b.weapon_base_mult = base != nullptr ? base->base_damage_mult : 1.0;
        b.weapon_bonus += LevelDamageBonus(eco, weapon->level);
        b.weapon_instance_id = weapon->instance_id;
        for (const Affix& af : weapon->affixes) {
            switch (af.kind) {
                case AffixKind::DamagePct: b.weapon_bonus += af.value / 100.0; break;
                case AffixKind::CritChance: crit_chance += af.value; break;
                case AffixKind::CritMultPct: crit_extra += af.value / 100.0; break;
                case AffixKind::LowHpTargetDamagePct:
                    if (target_fraction && *target_fraction < af.threshold) b.weapon_bonus += af.value / 100.0;
                    break;
                case AffixKind::UniqueLowHpTargetBonus:
                    if (target_fraction && *target_fraction < af.threshold) b.weapon_bonus += af.value;
                    break;
                case AffixKind::SuppliesOnKill:
                case AffixKind::UniqueKillStreakExtend: break;
            }
        }
    }

    b.crit_chance = std::clamp(crit_chance, 0.0, eco.damage.max_crit_chance);
    // Roll only when a crit is possible so the combat stream advances only for crit builds.
    if (b.crit_chance > 0.0) b.crit = streams_->combat.NextDouble() < b.crit_chance;
    b.crit_multiplier = (best_crit_mult > 0.0 ? best_crit_mult : eco.damage.default_crit_multiplier) + crit_extra;

    b.raw = b.route_mult * std::max(0.0, 1.0 + b.perk_bonus + b.kill_streak_bonus + b.build_bonus) * b.weapon_base_mult *
            std::max(0.0, 1.0 + b.weapon_bonus) * (b.crit ? b.crit_multiplier : 1.0);
    b.clamped = std::clamp(b.raw, eco.damage.outgoing_min, eco.damage.outgoing_max);
    out.multiplier = b.clamped;
    out.crit = b.crit;

    if (hit.is_melee.value_or(false)) run_.last_melee_time = hit.now;
    ++run_.hits_computed;

    const bool clamped = b.clamped != b.raw;
    const bool sample = damage_sample_every_ > 0 && run_.hits_computed % damage_sample_every_ == 0;
    if (sample || (clamped && !run_.clamp_logged)) {
        if (clamped) run_.clamp_logged = true;
        Event e = MakeEvent(EventType::DamageModified);
        e.Add("base_damage", hit.base_damage).Add("raw", b.raw).Add("clamped", b.clamped).Add("crit", b.crit)
            .Add("perk_bonus", b.perk_bonus).Add("kill_streak_bonus", b.kill_streak_bonus)
            .Add("weapon_bonus", b.weapon_bonus).Add("crit_chance", b.crit_chance)
            .Add("sample_every", damage_sample_every_).Add("hit_index", run_.hits_computed);
        if (!b.weapon_instance_id.empty()) e.Add("weapon_instance", b.weapon_instance_id);
        if (hit.weapon_id) e.Add("weapon_id", *hit.weapon_id);
        Emit(std::move(e));
    }
    return out;
}

IncomingResult RunController::ComputeIncoming(const IncomingContext& incoming) {
    IncomingResult out;
    if (incoming.hp_max > 0.0) {
        run_.player_hp = incoming.hp;
        run_.player_hp_max = incoming.hp_max;
    }
    if (!run_.active || run_.phase != Phase::ArenaActive) return out;
    // A build's heal or dash: no damage at all for a while.
    if (incoming.now < run_.invulnerable_until) {
        out.multiplier = 0.0;
        out.raw_multiplier = 0.0;
        return out;
    }

    const EconomyConfig& eco = content_->economy();
    const RouteDef* route = Route();
    double bonus = -run_.stats.defense;  // a build's gained defense
    double leave_fraction = 0.0;
    std::string cheat_source;
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def == nullptr) continue;
        for (const EffectDef& ef : def->effects) {
            const EffectParams p = ef.AtStacks(stacks);
            if (ef.kind == EffectKind::IncomingDamageMult) {
                const bool active = ef.condition == ConditionKind::Always ||
                                    (ef.condition == ConditionKind::PlayerHpBelow && incoming.hp_max > 0.0 &&
                                     incoming.hp / incoming.hp_max < p.fraction);
                if (active) bonus += p.value;
            } else if (ef.kind == EffectKind::CheatDeath && p.leave_hp_fraction > leave_fraction) {
                leave_fraction = p.leave_hp_fraction;
                cheat_source = id;
            }
        }
    }
    out.raw_multiplier =
        (route != nullptr ? route->modifiers.incoming_damage_mult : 1.0) * std::max(0.0, 1.0 + bonus);
    out.multiplier = std::clamp(out.raw_multiplier, eco.damage.incoming_min, eco.damage.incoming_max);

    const double final_damage = incoming.amount * out.multiplier;
    if (incoming.hp > 0.0 && incoming.hp - final_damage <= 0.0) {
        ArenaMetrics& a = *CurrentArena();
        if (a.cheat_death_used < CheatDeathCharges()) {
            ++a.cheat_death_used;
            ++run_.cheat_death_used_total;
            out.prevent_death = true;
            out.leave_hp = std::max(1.0, std::ceil(leave_fraction * incoming.hp_max));
            Event e = MakeEvent(EventType::PerkTriggered);
            e.Add("source", cheat_source).Add("trigger", "cheat_death").Add("leave_hp", out.leave_hp)
                .Add("used_this_arena", a.cheat_death_used);
            Emit(std::move(e));
        }
    }
    return out;
}

}  // namespace rm
