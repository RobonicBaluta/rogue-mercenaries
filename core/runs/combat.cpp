// Damage modifier evaluation for RunController (outgoing/incoming hooks, kill streaks).

#include <algorithm>
#include <cmath>

#include "rm/run_controller.h"

namespace rm {

std::string_view ProcKindName(ProcKind k) {
    switch (k) {
        case ProcKind::Burn: return "burn";
        case ProcKind::Poison: return "poison";
        case ProcKind::Freeze: return "freeze";
        case ProcKind::Shock: return "shock";
        case ProcKind::IgniteAround: return "ignite_around";
        case ProcKind::Arc: return "arc";
        case ProcKind::Execute: return "execute";
        case ProcKind::Explode: return "explode";
        case ProcKind::Shatter: return "shatter";
        case ProcKind::Overkill: return "overkill";
        case ProcKind::Refill: return "refill";
        case ProcKind::PoisonCloud: return "poison_cloud";
        case ProcKind::FreezeAround: return "freeze_around";
    }
    return "burn";
}

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
    // A long run's later arenas: tougher enemies (EconomyConfig::Ladder).
    b.route_mult /= 1.0 + eco.ladder.toughness_per_arena * eco.ladder.Steps(run_.arena_index);

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
    const RunStats charm = CharmStats();  // a carried charm's kill stacks (the Uroboros Virus)
    b.build_bonus += run_.stats.damage + charm.damage;
    if (hit.now < run_.damage_buff_until) b.build_bonus += run_.damage_buff;
    crit_chance += run_.stats.crit + charm.crit;
    // Damage by the category of the gun that fired (any gun of that kind, instance or not;
    // never melee).
    if (hit.weapon_id && !hit.is_melee.value_or(false) && !run_.character.build.category_damage.empty()) {
        if (const WeaponBaseDef* base = content_->FindWeapon(*hit.weapon_id)) {
            const auto it = run_.character.build.category_damage.find(std::string(WeaponCategoryName(base->category)));
            if (it != run_.character.build.category_damage.end()) b.build_bonus += it->second;
        }
    }

    const WeaponInstance* weapon = ApplicableWeapon(hit.weapon_id, hit.is_melee);
    // The magazine's last round (Last Word), and a hit that kills outright (Execute, Last Word).
    const bool last_round = hit.magazine_left.has_value() && *hit.magazine_left <= 0;
    bool execute = false;
    if (weapon != nullptr) {
        const WeaponBaseDef* base = content_->FindWeapon(weapon->base_id);
        b.weapon_base_mult = base != nullptr ? base->base_damage_mult : 1.0;
        b.weapon_bonus += LevelDamageBonus(eco, weapon->level);
        // A gun the game gives no levels: its crit chance by level (weapons.json per_level crit_pct).
        if (base != nullptr) crit_chance += base->per_level.crit_pct / 100.0 * std::max(0, weapon->level - 1);
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
                case AffixKind::UniqueExecute:
                    if (target_fraction && *target_fraction < af.threshold) {
                        if (hit.target_is_boss) {
                            b.weapon_bonus += af.value;
                        } else {
                            execute = true;
                        }
                    }
                    break;
                case AffixKind::UniqueLastWord:
                    if (last_round) {
                        if (hit.target_is_boss) {
                            b.weapon_bonus += af.value;
                        } else {
                            execute = true;
                        }
                    }
                    break;
                default: break;  // kill and area effects: below and in OnEnemyKilled
            }
        }
        // Against a boss an element is more damage instead of a status; Incendiary and Toxic burn and poison
        // a boss as anyone (the user, 2026-11-22: "add dot on bosses"), in place of their bonus.
        if (hit.target_is_boss) {
            for (const auto& [el, evolved] : GunElements(*weapon)) {
                if (el == Element::Incendiary || el == Element::Toxic) continue;
                if (const ElementDef* d = content_->FindElement(el, evolved)) b.weapon_bonus += d->boss_bonus;
            }
        }
    }
    // The target's statuses, whatever hits it (melee too).
    if (hit.target_frozen) {
        if (const ElementDef* d = content_->FindElement(Element::Cryo)) b.status_mult *= d->taken_mult;
    }
    if (hit.target_poisoned) {
        if (const ElementDef* d = content_->FindElement(Element::Toxic)) b.status_mult *= d->taken_mult;
    }

    b.crit_chance = std::clamp(crit_chance, 0.0, eco.damage.max_crit_chance);
    // Roll only when a crit is possible so the combat stream advances only for crit builds.
    if (b.crit_chance > 0.0) b.crit = streams_->combat.NextDouble() < b.crit_chance;
    b.crit_multiplier = (best_crit_mult > 0.0 ? best_crit_mult : eco.damage.default_crit_multiplier) + crit_extra;

    b.raw = b.route_mult * std::max(0.0, 1.0 + b.perk_bonus + b.kill_streak_bonus + b.build_bonus) * b.weapon_base_mult *
            std::max(0.0, 1.0 + b.weapon_bonus) * (b.crit ? b.crit_multiplier : 1.0) * b.status_mult;
    b.clamped = std::clamp(b.raw, eco.damage.outgoing_min, eco.damage.outgoing_max);
    out.multiplier = b.clamped;
    out.crit = b.crit;
    if (weapon != nullptr && !hit.target_is_boss) AddHitProcs(*weapon, hit.base_damage * out.multiplier, execute, out.procs);
    if (weapon != nullptr && hit.target_is_boss) {
        // A boss: only the burn and the poison of its elements (no arcs, freezes or legendary effects).
        std::vector<WeaponProc> procs;
        AddHitProcs(*weapon, hit.base_damage * out.multiplier, false, procs);
        for (WeaponProc& p : procs) {
            if ((p.kind == ProcKind::Burn || p.kind == ProcKind::IgniteAround || p.kind == ProcKind::Poison) && !p.element.empty()) {
                if (p.kind == ProcKind::IgniteAround) p.kind = ProcKind::Burn;  // Inferno's spread is for those around a Majini
                out.procs.push_back(std::move(p));
            }
        }
    }

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

std::vector<std::pair<Element, bool>> RunController::GunElements(const WeaponInstance& weapon) const {
    std::vector<std::pair<Element, bool>> out;
    const auto add = [&out](Element e, bool evolved) {
        if (std::none_of(out.begin(), out.end(), [e](const auto& x) { return x.first == e; })) out.emplace_back(e, evolved);
    };
    for (Element e : weapon.elements) add(e, weapon.element_evolved);
    for (Element e : BuildElements()) add(e, false);
    if (run_.arena_element >= 0) add(static_cast<Element>(run_.arena_element), false);
    return out;
}

std::vector<Element> RunController::BuildElements() const { return ParseElements(run_.character.build.gun_elements); }

std::vector<std::pair<Element, bool>> RunController::ElementsOf(std::string_view base_id) const {
    for (const WeaponInstance& w : run_.weapons) {
        if (w.base_id == base_id) return GunElements(w);
    }
    return {};
}

void RunController::AddHitProcs(const WeaponInstance& weapon, double dealt, bool execute, std::vector<WeaponProc>& procs) {
    const auto roll = [this](double chance) {
        return chance >= 1.0 || (chance > 0.0 && streams_->combat.NextDouble() < chance);
    };
    const WeaponBaseDef* base = content_->FindWeapon(weapon.base_id);
    const double procMult = base != nullptr ? base->proc_chance_mult : 1.0;  // a slow gun's few hits proc more often
    for (const auto& [el, evolved] : GunElements(weapon)) {
        const ElementDef* d = content_->FindElement(el, evolved);
        if (d == nullptr) continue;
        WeaponProc p;
        p.source = d->name;
        p.element = std::string(ElementName(el));
        p.evolved = evolved;
        switch (el) {
            case Element::Incendiary:
            case Element::Toxic:
                if (!roll(d->chance)) continue;
                p.kind = el == Element::Incendiary ? ProcKind::Burn : ProcKind::Poison;
                p.dps = dealt * d->dps_fraction;
                p.seconds = d->seconds;
                // Inferno (evolved): the burn spreads to those around the target.
                if (el == Element::Incendiary && d->spread_radius > 0.0) {
                    p.kind = ProcKind::IgniteAround;
                    p.radius = d->spread_radius;
                }
                // Plague (evolved): the poisoned enemy's death, however it comes, poisons those around.
                if (el == Element::Toxic && d->cloud_dps_fraction > 0.0) {
                    p.cloud_dps_fraction = d->cloud_dps_fraction;
                    p.cloud_radius = d->radius;
                }
                break;
            case Element::Shock:
                if (!roll(d->chance * procMult)) continue;
                p.kind = ProcKind::Shock;
                p.damage = dealt * d->damage_fraction;
                p.radius = d->radius;
                p.targets = d->targets;
                break;
            case Element::Cryo:
                if (!roll(d->chance * procMult)) continue;
                p.kind = ProcKind::Freeze;
                p.seconds = d->seconds;
                break;
            case Element::Explosive: continue;  // on a kill
        }
        procs.push_back(std::move(p));
    }
    for (const Affix& af : weapon.affixes) {
        const UniqueAffixDef* u = af.unique_id.empty() ? nullptr : content_->FindUnique(af.unique_id);
        if (u == nullptr) continue;
        if (af.kind == AffixKind::UniqueDragonsBreath) {
            const ElementDef* fire = content_->FindElement(Element::Incendiary);
            WeaponProc p;
            p.kind = ProcKind::IgniteAround;
            p.source = u->id;
            p.dps = dealt * (fire != nullptr ? fire->dps_fraction : 0.35);
            p.seconds = fire != nullptr ? fire->seconds : 3.0;
            p.radius = u->radius;
            procs.push_back(std::move(p));
        } else if (af.kind == AffixKind::UniqueChainLightning && u->every > 0) {
            if (++run_.weapon_hits[weapon.instance_id] % u->every != 0) continue;
            WeaponProc p;
            p.kind = ProcKind::Arc;
            p.source = u->id;
            p.damage = dealt * u->value;
            p.radius = u->radius;
            p.targets = u->targets;
            procs.push_back(std::move(p));
        }
    }
    if (execute) {
        WeaponProc p;
        p.kind = ProcKind::Execute;
        p.source = "execute";
        procs.push_back(std::move(p));
    }
}

void RunController::AddKillProcs(const KillEvent& kill, const WeaponInstance* weapon, KillOutcome& out) {
    if (!kill.by_player || kill.target_is_boss) return;
    const auto blast = [&](ProcKind kind, const ElementDef& d, bool evolved) {
        WeaponProc p;
        p.kind = kind;
        p.source = d.name;
        p.element = std::string(ElementName(d.element));
        p.evolved = evolved;
        p.damage = d.damage_fraction * kill.target_hp_max;
        if (d.damage_cap > 0.0) p.damage = std::min(p.damage, d.damage_cap);
        p.radius = d.radius;
        p.chain = kind == ProcKind::Explode ? d.chain : 0;
        p.freeze_seconds = kind == ProcKind::Shatter ? d.shatter_freeze_seconds : 0.0;
        if (p.damage > 0.0 && p.radius > 0.0) out.procs.push_back(std::move(p));
    };
    std::vector<std::pair<Element, bool>> elements;
    if (weapon != nullptr) elements = GunElements(*weapon);
    const auto evolved_of = [&elements](Element e) {
        return std::any_of(elements.begin(), elements.end(), [e](const auto& x) { return x.first == e && x.second; });
    };
    // A frozen enemy shatters whatever killed it (the player's); Absolute Zero's (an evolved Cryo
    // gun's kill) freezes those it hits.
    if (kill.target_frozen) {
        const bool evolved = evolved_of(Element::Cryo);
        if (const ElementDef* d = content_->FindElement(Element::Cryo, evolved)) blast(ProcKind::Shatter, *d, evolved);
    }
    if (weapon == nullptr) return;
    for (const auto& [el, evolved] : elements) {
        const ElementDef* d = content_->FindElement(el, evolved);
        if (d == nullptr) continue;
        if (el == Element::Explosive) blast(ProcKind::Explode, *d, evolved);
        // A Cryo gun's kill: each enemy around may freeze (its own roll, the adapter's).
        if (el == Element::Cryo && d->kill_freeze_chance > 0.0 && d->kill_freeze_radius > 0.0) {
            WeaponProc p;
            p.kind = ProcKind::FreezeAround;
            p.source = d->name;
            p.element = std::string(ElementName(el));
            p.evolved = evolved;
            p.radius = d->kill_freeze_radius;
            p.seconds = d->seconds;
            p.chance = d->kill_freeze_chance;
            out.procs.push_back(std::move(p));
        }
    }
    for (const Affix& af : weapon->affixes) {
        const UniqueAffixDef* u = af.unique_id.empty() ? nullptr : content_->FindUnique(af.unique_id);
        if (u == nullptr) continue;
        if (af.kind == AffixKind::UniqueOverkill && kill.overkill > 0.0) {
            WeaponProc p;
            p.kind = ProcKind::Overkill;
            p.source = u->id;
            p.damage = kill.overkill * u->value;
            p.radius = u->radius;
            out.procs.push_back(std::move(p));
            out.triggered.push_back(u->id);
        } else if (af.kind == AffixKind::UniqueBottomless) {
            WeaponProc p;
            p.kind = ProcKind::Refill;
            p.source = u->id;
            out.procs.push_back(std::move(p));
            out.triggered.push_back(u->id);
        } else if (af.kind == AffixKind::UniqueBloodthirst && caps_.Has(cap::kHeal) && u->value > 0.0) {
            run_.heal_requests.push_back(HealRequest{u->value, u->id});
            out.heal_fraction_queued += u->value;
            out.triggered.push_back(u->id);
        }
    }
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
        out.no_reaction = Transformed(incoming.now);  // a transformation: not even a flinch
        return out;
    }

    const EconomyConfig& eco = content_->economy();
    const RouteDef* route = Route();
    double bonus = -(run_.stats.defense + CharmStats().defense);  // a build's gained defense, a charm's stacks
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
        (route != nullptr ? route->modifiers.incoming_damage_mult : 1.0) * std::max(0.0, 1.0 + bonus) *
        (1.0 + eco.ladder.incoming_per_arena * eco.ladder.Steps(run_.arena_index));  // a long run's later arenas
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
