// rm_sim: headless Rogue Mercenaries run simulator.
//
// Drives rm::RunController with a simple stochastic player model (no engine) to check
// invariants and report balance:
//   rm_sim --runs 10000 --seed 1 --config configs --out build/sim
// Options: --capabilities all|none|<comma list>  --policy mix|random|greedy|safe
//          --hit-scale X (enemies hit X times as often: calibrates the model's difficulty)
//          --log-runs N (write JSONL telemetry for the first N runs)  --date YYYY-MM-DD
// Exit code: 0 = all invariants hold, 1 = an invariant failed, 2 = usage/config error.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "rm/content.h"
#include "rm/rewards.h"
#include "rm/routes.h"
#include "rm/run_controller.h"
#include "rm/telemetry.h"
#include "rm/version.h"

namespace {

using namespace rm;
using nlohmann::ordered_json;

// ---- player model constants (documented in the summary) -------------------------------------
constexpr double kHpMax = 100.0;
constexpr double kBaseKillSeconds = 7.0;     // mean seconds per kill at multiplier 1.0
constexpr double kIncomingHitsPerSecond = 0.03;
constexpr std::array<double, 4> kTierHitScale = {1.0, 1.0, 1.3, 1.6};  // index = tier
constexpr double kHitDamageMin = 5.0;
constexpr double kHitDamageMax = 11.0;
constexpr double kPartnerKillChance = 0.10;
constexpr double kDuplicateReportChance = 0.03;
constexpr double kMeleeHitChance = 0.10;
constexpr double kRegenBetweenArenas = 30.0;
constexpr double kSkillMin = 0.8;
constexpr double kSkillMax = 1.3;
// Healing items (loadouts.json game items the rewards, the shop and perks hand out), used when
// health is under kHealBelow: the share of maximum health each heals before the build's own
// multiplier (a red herb only with a green one, as the mixed herb).
constexpr double kHealBelow = 0.35;
constexpr std::int64_t kItemSpray = 0x0304, kItemGreenHerb = 0x0305, kItemRedHerb = 0x0306, kItemMixedHerb = 0x0309;
constexpr double kSprayHeal = 1.0, kGreenHerbHeal = 0.4, kMixedHerbHeal = 1.0;
// The builds' specials, used whenever ready (estimates, the adapter performs them in game): kill
// credits (enemies finished at once) per use, and for the backup partner a share of the arena's
// work for the rest of it (seen in game 2026-11-03: Sheva followed, shot, killed and revived).
// Against Majini of 600 to 3000 health: the twin SMGs deal about 900 to each enemy in their cone,
// the burst 350 to each one near, a dash 200 to those on its line and a lunge 300 to 650 to one.
constexpr double kDashKills = 0.35, kBurstKills = 0.6, kDualSmgKills = 1.2;
constexpr double kPartnerKillSpeed = 1.5, kPartnerIncoming = 0.8;
// The arena's chests (economy chests.per_arena, placed by the adapter) are reached after a
// quarter, half and three quarters of its kills (a key each, unless the build is exempt); the
// Majini killed are of the key classes. A kill drops a green herb this often (the game's own
// drops; a build's drops_as turns them into its item).
constexpr double kHerbDropChance = 0.04;
constexpr double kAnyDropChance = 0.2;  // any item drop (mostly ammo), for a build's drops_as
constexpr const char* kEnemyClass = "uEm10";
// Thrown and placed items (perk grants, rewards, the shop, a build's drops): used as they come, one
// every kThrowSeconds, each finishing this many enemies (estimates).
constexpr std::int64_t kItemHandGrenade = 0x0106, kItemIncendiary = 0x0107, kItemFlash = 0x0108, kItemMine = 0x010A;
constexpr double kGrenadeKills = 0.8, kIncendiaryKills = 0.6, kFlashKills = 0.3, kMineKills = 1.0;
constexpr double kThrowSeconds = 6.0;
// The backup partner revives a player bleeding out once per arena (seen in game 2026-11-03), at
// this share of maximum health.
constexpr double kPartnerReviveHp = 0.25;

enum class Policy { Random, Greedy, Safe };
const char* PolicyName(Policy p) {
    switch (p) {
        case Policy::Random: return "random";
        case Policy::Greedy: return "greedy_damage";
        case Policy::Safe: return "safe";
    }
    return "?";
}

struct Options {
    int runs = 10000;
    std::uint64_t seed = 1;
    std::string config = "configs";
    std::string out = "build/sim";
    std::string capabilities = "all";
    std::string policy = "mix";
    double hit_scale = 1.0;  // enemies hit this much more often (calibration)
    int log_runs = 0;
    std::string date;
};

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--runs" && next(v)) {
            o.runs = std::stoi(v);
        } else if (a == "--seed" && next(v)) {
            o.seed = std::stoull(v);
        } else if (a == "--config" && next(v)) {
            o.config = v;
        } else if (a == "--out" && next(v)) {
            o.out = v;
        } else if (a == "--capabilities" && next(v)) {
            o.capabilities = v;
        } else if (a == "--policy" && next(v)) {
            o.policy = v;
        } else if (a == "--hit-scale" && next(v)) {
            o.hit_scale = std::stod(v);
        } else if (a == "--log-runs" && next(v)) {
            o.log_runs = std::stoi(v);
        } else if (a == "--date" && next(v)) {
            o.date = v;
        } else {
            return false;
        }
    }
    return o.runs > 0 && o.hit_scale > 0.0 && (o.policy == "mix" || o.policy == "random" || o.policy == "greedy" || o.policy == "safe");
}

Capabilities ParseCaps(const std::string& spec) {
    if (spec == "all") return Capabilities::All();
    Capabilities caps;
    if (spec == "none") return caps;
    std::stringstream ss(spec);
    for (std::string item; std::getline(ss, item, ',');) {
        if (!item.empty()) caps.Add(item);
    }
    return caps;
}

/// Counts events and watches invariants visible only through telemetry.
class SimSink final : public IEventSink {
public:
    void OnEvent(const Event& e) override {
        ++by_type[std::string(EventTypeName(e.type))];
        if (e.type == EventType::CurrencyChanged) {
            if (const FieldValue* b = e.Find("balance"); b != nullptr && std::get<std::int64_t>(*b) < 0) {
                ++negative_balance_events;
            }
        }
        if (e.type == EventType::Error) {
            std::string op;
            std::string code;
            if (const FieldValue* f = e.Find("operation")) op = std::get<std::string>(*f);
            if (const FieldValue* f = e.Find("code")) code = std::get<std::string>(*f);
            ++errors[op + ":" + code];
        }
        if (e.type == EventType::DamageModified) ++damage_samples;
        if (forward != nullptr) forward->OnEvent(e);
    }

    std::map<std::string, std::int64_t> by_type;
    std::map<std::string, std::int64_t> errors;
    std::int64_t negative_balance_events = 0;
    std::int64_t damage_samples = 0;
    IEventSink* forward = nullptr;
};

struct PerkTraits {
    bool offense = false;
    bool defense = false;
    bool economy = false;
};

PerkTraits Traits(const PerkDef& p) {
    PerkTraits t;
    for (const EffectDef& e : p.effects) {
        switch (e.kind) {
            case EffectKind::OutgoingDamageMult:
                if (e.params.value > 0.0 || e.condition == ConditionKind::KillStreak) t.offense = true;
                break;
            case EffectKind::Crit: t.offense = true; break;
            case EffectKind::IncomingDamageMult:
                if (e.params.value < 0.0) t.defense = true;
                break;
            case EffectKind::CheatDeath:
            case EffectKind::HealOnKill: t.defense = true; break;
            case EffectKind::SuppliesOnKill:
            case EffectKind::SuppliesOnArenaClear: t.economy = true; break;
        }
    }
    return t;
}

double ScoreReward(const RewardChoice& c, Policy policy, const Content& content) {
    const double r = static_cast<double>(RarityIndex(c.rarity));
    switch (c.kind) {
        case RewardKind::Perk: {
            const PerkTraits t = Traits(*content.FindPerk(c.perk_id));
            if (policy == Policy::Greedy) return t.offense ? 10.0 + 2.0 * r : 3.0 + r;
            return t.defense ? 10.0 + 2.0 * r : (t.economy ? 6.0 + r : 3.0 + r);
        }
        case RewardKind::Weapon:
            return (policy == Policy::Greedy ? 6.0 : 4.0) + 2.0 * r + static_cast<double>(c.weapon.affixes.size());
        case RewardKind::Supplies: return policy == Policy::Greedy ? 1.0 : 3.0;
        case RewardKind::Item: {
            // Healing first when playing safe, grenades and ammo otherwise.
            const EquipmentDef* e = content.loadouts().FindEquipment(c.item_id);
            const bool heals = e != nullptr && (e->game_item >> 8) == 3;
            if (policy == Policy::Safe) return heals ? 9.0 : 4.0;
            return heals ? 5.0 : 6.0;
        }
    }
    return 0.0;
}

struct Percentiles {
    double max = 0.0, p50 = 0.0, p99 = 0.0, p999 = 0.0, mean = 0.0;
};

Percentiles Summarise(std::vector<float>& v) {
    Percentiles p;
    if (v.empty()) return p;
    double sum = 0.0;
    for (float x : v) sum += x;
    p.mean = sum / static_cast<double>(v.size());
    auto at = [&v](double q) {
        const auto k = static_cast<std::size_t>(std::min<double>(static_cast<double>(v.size() - 1),
                                                                 std::floor(q * static_cast<double>(v.size() - 1))));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return static_cast<double>(v[k]);
    };
    p.p50 = at(0.5);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = static_cast<double>(*std::max_element(v.begin(), v.end()));
    return p;
}

std::string Fmt(double v, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string Pct(double num, double den) { return den > 0 ? Fmt(100.0 * num / den, 1) + "%" : "n/a"; }

/// Forwards to the JSONL sink only while enabled (first --log-runs runs).
class GateSink final : public IEventSink {
public:
    explicit GateSink(IEventSink* target) : target_(target) {}
    void OnEvent(const Event& e) override {
        if (enabled && target_ != nullptr) target_->OnEvent(e);
    }
    bool enabled = false;

private:
    IEventSink* target_;
};

/// One build (a character's skin) and how its runs went.
struct BuildStats {
    std::string character, skin, name;
    std::int64_t runs = 0, completed = 0, arenas_cleared = 0, reached_last = 0;
    std::int64_t credits = 0, supplies_earned = 0, specials = 0, heals = 0, chests_opened = 0, chests_locked = 0;
    std::int64_t throws = 0, revives = 0;
    double incoming_raw = 0.0, incoming_dealt = 0.0;  // damage before and after the run's modifiers
    double outgoing_mult = 0.0;                       // summed clamped multipliers of its hits
    std::int64_t hits = 0;
};

struct Stats {
    std::int64_t runs = 0, completed = 0, failed = 0;
    std::map<std::string, std::int64_t> runs_by_policy, completed_by_policy;
    std::map<std::string, std::int64_t> runs_by_character;
    std::vector<std::int64_t> arenas_cleared_hist;  // index = arenas cleared, 0..arena_count
    std::map<std::string, BuildStats> builds;       // by skin id
    std::map<int, std::int64_t> death_arena_hist;
    std::int64_t supplies_earned = 0, supplies_spent = 0;
    std::int64_t credits_banked = 0, credits_banked_completed = 0, credits_banked_failed = 0;
    std::int64_t arenas_played = 0;
    std::map<std::string, std::int64_t> ratings;
    std::map<std::string, std::int64_t> perk_offers, perk_picks, perk_shop_offers, perk_shop_buys, perk_equips;
    std::map<std::string, std::int64_t> curse_offers, curse_taken;
    std::map<std::string, std::int64_t> weapon_offers, unique_rolls;
    std::map<std::string, std::int64_t> route_offers, route_picks;
    std::array<std::int64_t, kRarityCount> perk_offer_rarity{}, weapon_offer_rarity{}, weapon_acquired_rarity{};
    std::map<std::string, std::int64_t> reward_kind_offers, reward_kind_picks, shop_kind_buys;
    std::int64_t reward_offers_total = 0;
    std::int64_t invalid_budget = 0, invalid_route = 0, invalid_offer = 0, invalid_route_choice = 0,
                 incomplete_arena = 0;
    std::int64_t negative_balance = 0;
    std::int64_t duplicate_attempts = 0, duplicate_grants = 0;
    std::int64_t rerolls = 0, shop_rollbacks = 0;
    std::int64_t hits = 0, clamped_hits = 0, crits = 0;
    std::int64_t cheat_deaths = 0, heals_applied = 0;
    std::vector<float> raw_mult, clamped_mult;
    std::map<std::string, std::int64_t> unlock_run;  // "kind:id" -> run index of purchase
};

class Simulator {
public:
    Simulator(Options opts, std::shared_ptr<const Content> content)
        : opts_(std::move(opts)), content_(std::move(content)), caps_(ParseCaps(opts_.capabilities)) {}

    int Run();

private:
    void PlayRun(RunController& rc, int index, Policy policy);
    bool PlayArena(RunController& rc, Rng& player, double& t, double& hp, double skill, int index);
    void ChooseReward(RunController& rc, Rng& player, Policy policy);
    void Shop(RunController& rc, Rng& player, Policy policy, double& hp);
    void ChooseRoute(RunController& rc, Rng& player, Policy policy);
    void BuyUnlocks(RunController& rc, int index);
    void CheckBalances(const RunController& rc);
    void ApplyHeals(RunController& rc, double& hp);
    void CollectItems(RunController& rc);
    void UseHealingItem(RunController& rc, double now, double& hp);
    double ThrowItem();
    double UseSpecials(RunController& rc, double now, double& partner);
    [[nodiscard]] double HpMax(const RunController& rc) const;
    int WriteReports(const SimSink& sink, double seconds);

    struct BuildRef {
        std::string character, skin;
    };
    Options opts_;
    std::shared_ptr<const Content> content_;
    Capabilities caps_;
    Stats s_;
    std::vector<BuildRef> builds_;
    BuildStats* build_ = nullptr;   // the run being played
    std::map<std::int64_t, int> items_;  // healing items held this run (game item -> count)
};

double Simulator::HpMax(const RunController& rc) const {
    // The build's maximum health and what the run's melee-kill stats added (the adapter scales it in game).
    return kHpMax * rc.state().character.build.max_health_mult * (1.0 + rc.state().stats.max_health);
}

void Simulator::CollectItems(RunController& rc) {
    for (const ItemGrant& g : rc.ConsumeItemGrants()) {
        switch (g.game_item) {
            case kItemSpray: case kItemGreenHerb: case kItemRedHerb: case kItemMixedHerb:
            case kItemHandGrenade: case kItemIncendiary: case kItemFlash: case kItemMine:
                items_[g.game_item] += g.count;
                break;
            default: break;
        }
    }
}

double Simulator::ThrowItem() {
    for (const auto& [item, kills] : {std::pair{kItemMine, kMineKills}, std::pair{kItemHandGrenade, kGrenadeKills},
                                      std::pair{kItemIncendiary, kIncendiaryKills}, std::pair{kItemFlash, kFlashKills}}) {
        if (items_[item] <= 0) continue;
        --items_[item];
        if (build_ != nullptr) ++build_->throws;
        return kills;
    }
    return 0.0;
}

void Simulator::UseHealingItem(RunController& rc, double now, double& hp) {
    const double max = HpMax(rc);
    if (hp >= kHealBelow * max) return;
    double share = 0.0;
    auto take = [&](std::int64_t item, double heal) {
        if (share > 0.0 || items_[item] <= 0) return;
        --items_[item];
        share = heal;
    };
    take(kItemMixedHerb, kMixedHerbHeal);
    if (share == 0.0 && items_[kItemRedHerb] > 0 && items_[kItemGreenHerb] > 0) {
        --items_[kItemRedHerb];
        --items_[kItemGreenHerb];
        share = kMixedHerbHeal;
    }
    take(kItemSpray, kSprayHeal);
    take(kItemGreenHerb, kGreenHerbHeal);
    if (share == 0.0) return;
    // The core applies the build's heal multiplier and a big heal's buffs (invulnerability, damage).
    const HealOutcome out = rc.OnPlayerHealed(HealContext{now, share * max, hp, max});
    hp = std::min(max, hp + out.amount);
    if (build_ != nullptr) ++build_->heals;
}

double Simulator::UseSpecials(RunController& rc, double now, double& partner) {
    double kills = 0.0;
    for (int slot = 0; slot < 2; ++slot) {
        const SpecialState st = rc.Special(now, slot);
        if (st.kind.empty() || !st.ready || !rc.UseSpecial(now, 1.0, slot).ok()) continue;
        if (build_ != nullptr) ++build_->specials;
        if (st.kind == "dash") kills += kDashKills;
        else if (st.kind == "burst") kills += kBurstKills;
        else if (st.kind == "dual_smg") kills += kDualSmgKills;
        else if (st.kind == "backup") partner = 1.0;
    }
    return kills;
}

void Simulator::CheckBalances(const RunController& rc) {
    if (rc.state().supplies.balance() < 0) ++s_.negative_balance;
    if (rc.profile().credits.balance() < 0) ++s_.negative_balance;
}

void Simulator::ApplyHeals(RunController& rc, double& hp) {
    const double max = HpMax(rc);
    for (const HealRequest& h : rc.ConsumeHealRequests()) {
        hp = std::min(max, hp + h.fraction_of_max * max);
        ++s_.heals_applied;
    }
}

bool Simulator::PlayArena(RunController& rc, Rng& player, double& t, double& hp, double skill, int index) {
    const Status begin = rc.BeginArena(t);
    if (!begin.ok()) {
        if (begin.code == ErrorCode::InvalidEncounter) ++s_.invalid_route;
        return false;
    }
    const RouteDef* route = content_->FindRoute(rc.state().route_id);
    if (route == nullptr) {
        ++s_.invalid_route;
        return false;
    }
    if (route->enemy_budget <= 0) ++s_.invalid_budget;
    ++s_.arenas_played;
    const int tier = std::clamp(route->difficulty_tier, 1, 3);
    const double hit_rate = kIncomingHitsPerSecond * opts_.hit_scale * kTierHitScale[static_cast<std::size_t>(tier)] / skill;
    double power = 1.0;
    double power_sum = 0.0;
    std::int64_t power_n = 0;
    int enemy_serial = 0;
    const int max_events = route->enemy_budget * 4 + 20;
    const bool melee_known = caps_.Has(cap::kMeleeDetect);
    double partner = 0.0;       // the backup partner fights the rest of this arena (1) or not (0)
    double kill_credits = 0.0;  // enemies the specials finish at once
    const int chests = content_->economy().chests.per_arena + rc.state().character.build.extra_chests;
    int chests_tried = 0, kills = 0;
    double next_throw = t;
    bool revived = false;

    for (int ev = 0; ev < max_events && !rc.IsArenaComplete(); ++ev) {
        kill_credits += UseSpecials(rc, t, partner);
        if (t >= next_throw) {
            const double k = ThrowItem();
            if (k > 0.0) {
                kill_credits += k;
                next_throw = t + kThrowSeconds;
            }
        }
        // Time until the next kill, scaled by current damage output, player skill and the partner;
        // a special's kill comes at once.
        const double speed = power * skill * (partner > 0.0 ? kPartnerKillSpeed : 1.0);
        const double mean = kBaseKillSeconds / std::max(0.25, speed);
        double dt = std::max(0.5, -std::log(1.0 - player.NextDouble()) * mean);
        if (kill_credits >= 1.0) {
            kill_credits -= 1.0;
            dt = 0.5;
        }

        // Incoming hits during dt (fewer with a partner drawing enemies).
        const double expected_hits = hit_rate * dt * (partner > 0.0 ? kPartnerIncoming : 1.0);
        int hits = 0;
        for (double l = std::exp(-expected_hits), p = player.NextDouble(); p > l; p *= player.NextDouble()) ++hits;
        for (int h = 0; h < hits; ++h) {
            const double now = t + dt * (h + 1) / (hits + 1);
            const double amount = player.UniformReal(kHitDamageMin, kHitDamageMax);
            const double max = HpMax(rc);
            const IncomingResult in = rc.ComputeIncoming(IncomingContext{now, amount, hp, max});
            const double before = hp;
            const double dealt = amount * in.multiplier;
            if (in.prevent_death) {
                hp = in.leave_hp;
                ++s_.cheat_deaths;
            } else {
                hp = std::max(0.0, hp - dealt);
            }
            if (build_ != nullptr) {
                build_->incoming_raw += amount;
                build_->incoming_dealt += before - hp > 0 ? before - hp : 0.0;
            }
            rc.OnPlayerDamaged(DamageEvent{now, before - hp > 0 ? before - hp : 0.0, before, hp, max});
            CollectItems(rc);
            UseHealingItem(rc, now, hp);
            if (hp <= 0.0 && partner > 0.0 && !revived) {
                // Bleeding out with the partner here: she revives once.
                revived = true;
                hp = kPartnerReviveHp * HpMax(rc);
                if (build_ != nullptr) ++build_->revives;
            }
            if (hp <= 0.0) {
                rc.OnPlayerDied(now);
                t += dt;
                return false;
            }
        }

        // Player hits on the next enemy, then the kill.
        const std::string enemy = "e" + std::to_string(++enemy_serial);
        const auto& weapons = rc.state().weapons;
        std::optional<std::string> weapon_id;
        if (caps_.Has(cap::kWeaponId) && !weapons.empty()) weapon_id = weapons[player.Index(weapons.size())].base_id;
        const int n_hits = static_cast<int>(player.UniformInt(3, 6));
        bool last_melee = false;
        for (int k = 0; k < n_hits; ++k) {
            HitContext hit;
            hit.now = t + dt * (k + 1) / (n_hits + 1);
            hit.base_damage = 100.0;
            hit.target_hp = 100.0 * (1.0 - static_cast<double>(k) / n_hits);
            hit.target_hp_max = 100.0;
            hit.weapon_id = weapon_id;
            last_melee = melee_known && player.Chance(kMeleeHitChance);
            if (melee_known) hit.is_melee = last_melee;
            hit.player_hp = hp;
            hit.player_hp_max = HpMax(rc);
            const OutgoingResult r = rc.ComputeOutgoingMultiplier(hit);
            s_.raw_mult.push_back(static_cast<float>(r.breakdown.raw));
            s_.clamped_mult.push_back(static_cast<float>(r.multiplier));
            ++s_.hits;
            if (build_ != nullptr) {
                build_->outgoing_mult += r.multiplier;
                ++build_->hits;
            }
            if (r.breakdown.raw != r.breakdown.clamped) ++s_.clamped_hits;
            if (r.crit) ++s_.crits;
            power_sum += r.multiplier;
            ++power_n;
        }
        power = power_sum / static_cast<double>(power_n);
        t += dt;

        KillEvent kill;
        kill.now = t;
        kill.by_player = !player.Chance(kPartnerKillChance);
        kill.weapon_id = weapon_id;
        if (melee_known) kill.was_melee = last_melee;
        kill.enemy_id = enemy;
        kill.enemy_class = kEnemyClass;
        rc.OnEnemyKilled(kill);
        if (kill.by_player && player.Chance(kDuplicateReportChance)) rc.OnEnemyKilled(kill);  // duplicate hook report
        // The game's own drops: a green herb now and then; a build that turns every drop (ammo,
        // herbs, grenades) into its item gets that item from any of them (Safari: mines).
        const std::int64_t drops_as = rc.state().character.build.drops_as;
        if (drops_as >= 0) {
            if (player.Chance(kAnyDropChance)) ++items_[drops_as];
        } else if (player.Chance(kHerbDropChance)) {
            ++items_[kItemGreenHerb];
        }
        // A chest each quarter of the arena (a key each, or the build's exemption).
        ++kills;
        while (chests_tried < chests && kills * (chests + 1) >= route->enemy_budget * (chests_tried + 1)) {
            ++chests_tried;
            const Result<ChestReward> chest = rc.OpenChest(t);
            if (build_ != nullptr && chest.ok()) ++(chest->locked ? build_->chests_locked : build_->chests_opened);
        }
        ApplyHeals(rc, hp);
        CollectItems(rc);
        hp = std::min(hp, HpMax(rc));
        CheckBalances(rc);
    }
    if (!rc.IsArenaComplete()) {
        ++s_.incomplete_arena;
        rc.AbandonRun(t);
        return false;
    }
    const Result<ArenaResult> result = rc.CompleteArena(t + 1.0);
    if (!result.ok()) {
        ++s_.incomplete_arena;
        return false;
    }
    ++s_.ratings[std::string(RatingName(result->rating))];
    t += 2.0;
    (void)index;
    return true;
}

void Simulator::ChooseReward(RunController& rc, Rng& player, Policy policy) {
    auto record = [this, &rc]() {
        const auto& offer = rc.RewardOffer();
        ++s_.reward_offers_total;
        std::set<std::string> distinct;
        for (const RewardChoice& c : offer) {
            distinct.insert(std::string(RewardKindName(c.kind)) + c.perk_id + c.weapon.base_id + std::to_string(c.supplies) +
                            c.item_id + "x" + std::to_string(c.item_count));
            ++s_.reward_kind_offers[std::string(RewardKindName(c.kind))];
            if (c.kind == RewardKind::Perk) {
                ++s_.perk_offers[c.perk_id];
                ++s_.perk_offer_rarity[RarityIndex(c.rarity)];
            } else if (c.kind == RewardKind::Weapon) {
                ++s_.weapon_offers[c.weapon.base_id];
                ++s_.weapon_offer_rarity[RarityIndex(c.rarity)];
                for (const Affix& a : c.weapon.affixes) {
                    if (!a.unique_id.empty()) ++s_.unique_rolls[a.unique_id];
                }
            }
        }
        if (offer.empty() || distinct.size() != offer.size() ||
            offer.size() != static_cast<std::size_t>(content_->economy().rewards.choices)) {
            ++s_.invalid_offer;
        }
    };
    record();
    auto best = [&]() {
        const auto& offer = rc.RewardOffer();
        std::size_t idx = 0;
        double top = -1.0;
        for (std::size_t i = 0; i < offer.size(); ++i) {
            const double sc = ScoreReward(offer[i], policy, *content_);
            if (sc > top) {
                top = sc;
                idx = i;
            }
        }
        return std::make_pair(idx, top);
    };
    if (rc.state().reroll_tokens > 0) {
        const bool reroll = policy == Policy::Random ? player.Chance(0.3) : best().second < 8.0;
        if (reroll && rc.RerollReward().ok()) {
            ++s_.rerolls;
            record();
        }
    }
    const auto& offer = rc.RewardOffer();
    if (offer.empty()) return;
    const std::size_t pick = policy == Policy::Random ? player.Index(offer.size()) : best().first;
    const RewardChoice choice = offer[pick];
    if (!rc.ChooseReward(pick).ok()) return;
    ++s_.reward_kind_picks[std::string(RewardKindName(choice.kind))];
    if (choice.kind == RewardKind::Perk) ++s_.perk_picks[choice.perk_id];
    if (choice.kind == RewardKind::Weapon) ++s_.weapon_acquired_rarity[RarityIndex(choice.rarity)];
    // Adversarial duplicate: the same arena's reward must never be granted twice.
    ++s_.duplicate_attempts;
    if (rc.ChooseReward(pick).ok()) ++s_.duplicate_grants;
    CheckBalances(rc);
}

void Simulator::Shop(RunController& rc, Rng& player, Policy policy, double& hp) {
    const auto& offers = rc.ShopOffers();
    for (const ShopOffer& o : offers) {
        if (o.kind == ShopOfferKind::Perk) {
            ++s_.perk_shop_offers[o.perk_id];
            ++s_.perk_offer_rarity[RarityIndex(o.rarity)];
        } else if (o.kind == ShopOfferKind::Weapon) {
            ++s_.weapon_offers[o.weapon.base_id];
            ++s_.weapon_offer_rarity[RarityIndex(o.rarity)];
            for (const Affix& a : o.weapon.affixes) {
                if (!a.unique_id.empty()) ++s_.unique_rolls[a.unique_id];
            }
        }
    }
    std::vector<std::pair<double, std::size_t>> wanted;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        const ShopOffer& o = offers[i];
        const double r = static_cast<double>(RarityIndex(o.rarity));
        const EquipmentDef* item = o.kind == ShopOfferKind::Item ? content_->loadouts().FindEquipment(o.item_id) : nullptr;
        const bool heals = item != nullptr && (item->game_item >> 8) == 3;
        double score = 0.0;
        switch (policy) {
            case Policy::Random: score = player.Chance(0.5) ? player.NextDouble() + 1.0 : 0.0; break;
            case Policy::Greedy:
                if (o.kind == ShopOfferKind::WeaponUpgrade) score = 9.0;
                if (o.kind == ShopOfferKind::Perk && Traits(*content_->FindPerk(o.perk_id)).offense) score = 6.0 + r;
                if (o.kind == ShopOfferKind::Weapon && o.rarity >= Rarity::Rare) score = 5.0 + r;
                if (item != nullptr && !heals) score = 3.0;
                break;
            case Policy::Safe:
                if (o.kind == ShopOfferKind::Heal && hp < 0.7 * HpMax(rc)) score = 10.0;
                if (heals) score = 8.0;
                if (o.kind == ShopOfferKind::Perk && Traits(*content_->FindPerk(o.perk_id)).defense) score = 7.0 + r;
                if (o.kind == ShopOfferKind::WeaponUpgrade) score = 4.0;
                break;
        }
        if (score > 0.0) wanted.emplace_back(score, i);
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    int bought = 0;
    for (const auto& [score, i] : wanted) {
        if (bought >= 3) break;
        const ShopOffer o = rc.ShopOffers()[i];
        if (o.price > rc.state().supplies.balance()) continue;
        const Status st = rc.Purchase(i);
        if (st.ok()) {
            ++bought;
            ++s_.shop_kind_buys[std::string(ShopOfferKindName(o.kind))];
            if (o.kind == ShopOfferKind::Perk) ++s_.perk_shop_buys[o.perk_id];
            if (o.kind == ShopOfferKind::Weapon) ++s_.weapon_acquired_rarity[RarityIndex(o.rarity)];
        } else if (st.code == ErrorCode::GrantFailed) {
            ++s_.shop_rollbacks;
        }
        CheckBalances(rc);
    }
    ApplyHeals(rc, hp);
    rc.LeaveShop();
}

void Simulator::ChooseRoute(RunController& rc, Rng& player, Policy policy) {
    // The run map's next nodes: two or three, a safer and a harder tier among them.
    const auto& choices = rc.RouteChoices();
    std::set<int> tiers;
    for (const RouteChoice& c : choices) {
        const RouteDef* r = content_->FindRoute(c.route_id);
        if (r == nullptr || r->enemy_budget <= 0) {
            ++s_.invalid_route;
            continue;
        }
        ++s_.route_offers[c.route_id];
        if (!c.curse_perk_id.empty()) ++s_.curse_offers[c.curse_perk_id];
        tiers.insert(r->difficulty_tier);
    }
    // A safer and a harder tier, except once the run map's tiers reach the highest (its last
    // layers repeat it: rm::GenerateRunMap).
    const bool at_top = !tiers.empty() && *tiers.begin() == content_->MaxRouteTier();
    if (choices.size() < 2 || choices.size() > 3 || (tiers.size() < 2 && !at_top)) ++s_.invalid_route_choice;
    if (choices.empty()) return;
    std::size_t pick = 0;
    if (policy == Policy::Random) {
        pick = player.Index(choices.size());
    } else {
        double best = -1e9;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const RouteDef* r = content_->FindRoute(choices[i].route_id);
            const double tier = static_cast<double>(r->difficulty_tier);
            const double curse = choices[i].curse_perk_id.empty() ? 0.0 : 1.0;
            const double score = policy == Policy::Greedy ? tier * 10.0 + r->reward_mult - curse * 0.5
                                                          : -tier * 10.0 - curse * 5.0 + r->reward_mult;
            if (score > best) {
                best = score;
                pick = i;
            }
        }
    }
    const RouteChoice choice = choices[pick];
    if (rc.ChooseRoute(pick).ok()) {
        ++s_.route_picks[choice.route_id];
        if (!choice.curse_perk_id.empty()) ++s_.curse_taken[choice.curse_perk_id];
    }
}

void Simulator::BuyUnlocks(RunController& rc, int index) {
    struct Candidate {
        std::int64_t cost;
        UnlockKind kind;
        std::string id;
    };
    std::vector<Candidate> candidates;
    const Profile& p = rc.profile();
    for (const CharacterDef& c : content_->characters()) {
        if (c.enabled && c.unlock_cost_credits > 0 && !p.unlocked_characters.count(c.id)) {
            candidates.push_back({c.unlock_cost_credits, UnlockKind::Character, c.id});
        }
    }
    for (const PerkDef& d : content_->perks()) {
        if (d.enabled && d.unlock_cost_credits > 0 && !p.unlocked_perks.count(d.id)) {
            candidates.push_back({d.unlock_cost_credits, UnlockKind::Perk, d.id});
        }
    }
    for (const WeaponBaseDef& w : content_->weapons()) {
        if (w.enabled && w.unlock_cost_credits > 0 && !p.unlocked_weapons.count(w.id)) {
            candidates.push_back({w.unlock_cost_credits, UnlockKind::Weapon, w.id});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.cost != b.cost ? a.cost < b.cost : a.id < b.id; });
    if (!candidates.empty() && candidates.front().cost <= p.credits.balance()) {
        const Candidate& c = candidates.front();
        if (rc.PurchaseUnlock(c.kind, c.id).ok()) {
            s_.unlock_run[std::string(UnlockKindName(c.kind)) + ":" + c.id] = index + 1;
            if (c.kind == UnlockKind::Character) {
                if (const CharacterDef* ch = content_->FindCharacter(c.id)) {
                    for (const SkinDef& s : ch->skins) builds_.push_back({ch->id, s.id});
                }
            }
        }
    }
    CheckBalances(rc);
}

void Simulator::PlayRun(RunController& rc, int index, Policy policy) {
    std::uint64_t mix = opts_.seed + static_cast<std::uint64_t>(index);
    const std::uint64_t run_seed = SplitMix64(mix);
    Rng player = Rng::Derive(run_seed, "sim_player");
    // Every build in turn (a character's skin), with a perk of each kind the profile has, as the
    // PERKS menu would equip them.
    const BuildRef& b = builds_[static_cast<std::size_t>(index) % builds_.size()];
    const std::string& character = b.character;
    Rng loadout = Rng::Derive(run_seed, "sim_loadout");
    for (const PerkCategory category : {PerkCategory::Style, PerkCategory::Supply, PerkCategory::Support}) {
        std::vector<const PerkDef*> pool;
        for (const PerkDef& p : content_->perks()) {
            if (p.category == category && p.enabled) pool.push_back(&p);
        }
        loadout.Shuffle(pool);
        for (const PerkDef* p : pool) {
            if (rc.EquipPerk(character, p->id).ok()) break;  // locked, or another character's: the next
        }
    }
    for (const std::string& id : rc.EquippedPerks(character)) ++s_.perk_equips[id];
    if (!rc.StartRun(character, run_seed, b.skin).ok()) {
        std::cerr << "StartRun failed for run " << index << " (" << character << " " << b.skin << ")\n";
        return;
    }
    ++s_.runs;
    ++s_.runs_by_policy[PolicyName(policy)];
    ++s_.runs_by_character[character];
    build_ = &s_.builds[b.skin];
    build_->character = character;
    build_->skin = b.skin;
    build_->name = rc.state().character.build.name;
    ++build_->runs;
    items_.clear();
    CollectItems(rc);  // supply perks' starting items
    const double skill = player.UniformReal(kSkillMin, kSkillMax);
    double hp = HpMax(rc);
    double t = 0.0;
    while (rc.HasActiveRun()) {
        const Phase phase = rc.phase();
        if (phase == Phase::RunComplete || phase == Phase::RunFailed) break;
        if (rc.state().arena_index + 1 == rc.state().arena_count) ++build_->reached_last;
        if (!PlayArena(rc, player, t, hp, skill, index)) break;
        rc.ContinueToRewards();
        if (rc.phase() == Phase::RewardSelection) ChooseReward(rc, player, policy);
        CollectItems(rc);
        if (rc.phase() == Phase::Shop) {
            Shop(rc, player, policy, hp);
            CollectItems(rc);
            ChooseRoute(rc, player, policy);
            hp = std::min(HpMax(rc), hp + kRegenBetweenArenas * HpMax(rc) / kHpMax);
            t += 30.0;
        }
        CheckBalances(rc);
    }
    if (rc.HasActiveRun() && rc.phase() != Phase::RunComplete && rc.phase() != Phase::RunFailed) rc.AbandonRun(t);
    const Result<RunSummary> summary = rc.EndRun();
    if (!summary.ok()) {
        std::cerr << "EndRun failed for run " << index << ": " << summary.status().message << "\n";
        return;
    }
    const RunSummary& sum = summary.value();
    if (sum.completed) {
        ++s_.completed;
        ++s_.completed_by_policy[PolicyName(policy)];
        s_.credits_banked_completed += sum.credits_banked;
        ++build_->completed;
    } else {
        ++s_.failed;
        ++s_.death_arena_hist[sum.arenas_cleared + 1];
        s_.credits_banked_failed += sum.credits_banked;
    }
    build_->arenas_cleared += sum.arenas_cleared;
    build_->credits += sum.credits_banked;
    build_->supplies_earned += sum.supplies_earned;
    const std::size_t cleared = static_cast<std::size_t>(std::max(0, sum.arenas_cleared));
    if (s_.arenas_cleared_hist.size() <= cleared) s_.arenas_cleared_hist.resize(cleared + 1, 0);
    s_.arenas_cleared_hist[cleared]++;
    s_.supplies_earned += sum.supplies_earned;
    s_.supplies_spent += sum.supplies_spent;
    s_.credits_banked += sum.credits_banked;
    // A second EndRun must not bank again.
    const Result<RunSummary> again = rc.EndRun();
    ++s_.duplicate_attempts;
    if (again.ok() && again->credits_banked != 0) ++s_.duplicate_grants;
    BuyUnlocks(rc, index);
}

int Simulator::Run() {
    // Every selectable character's every selectable skin: each skin is its own build.
    for (const CharacterDef& c : content_->characters()) {
        if (CheckCharacter(&c, {}) != CharacterGate::Selectable) continue;
        for (const SkinDef& s : c.skins) {
            if (CheckSkin(s, {}) == CharacterGate::Selectable) builds_.push_back({c.id, s.id});
        }
    }
    if (builds_.empty()) {
        std::cerr << "no selectable character\n";
        return 2;
    }
    s_.arenas_cleared_hist.assign(static_cast<std::size_t>(std::max(1, content_->economy().run.arena_count)) + 1, 0);
    std::filesystem::create_directories(opts_.out);
    std::unique_ptr<JsonlSink> jsonl;
    if (opts_.log_runs > 0) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(opts_.out) / "sim_events.jsonl", ec);  // fresh log per invocation
        JsonlOptions jo;
        jo.directory = opts_.out;
        jo.base_name = "sim_events";
        jo.max_level = EventLevel::Debug;
        jo.flush = FlushPolicy::Manual;
        jo.max_file_bytes = 64u * 1024u * 1024u;
        jo.clock = [] { return std::int64_t{0}; };  // reproducible output
        jsonl = std::make_unique<JsonlSink>(jo);
    }
    GateSink gate(jsonl.get());
    SimSink sink;
    sink.forward = &gate;
    RunControllerOptions ro;
    ro.capabilities = caps_;
    ro.sink = &sink;
    ro.damage_sample_every = 50;
    RunController rc(content_, Profile{}, ro);

    const auto start = std::chrono::steady_clock::now();
    const std::array<Policy, 3> mix = {Policy::Random, Policy::Greedy, Policy::Safe};
    for (int i = 0; i < opts_.runs; ++i) {
        Policy policy = mix[static_cast<std::size_t>(i) % mix.size()];
        if (opts_.policy == "random") policy = Policy::Random;
        if (opts_.policy == "greedy") policy = Policy::Greedy;
        if (opts_.policy == "safe") policy = Policy::Safe;
        gate.enabled = i < opts_.log_runs;
        PlayRun(rc, i, policy);
    }
    if (jsonl) jsonl->Flush();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    s_.negative_balance += sink.negative_balance_events;
    return WriteReports(sink, seconds);
}

int Simulator::WriteReports(const SimSink& sink, double seconds) {
    const double runs = static_cast<double>(s_.runs);
    const Percentiles raw = Summarise(s_.raw_mult);
    const Percentiles clamped = Summarise(s_.clamped_mult);
    const double avg_credits = runs > 0 ? static_cast<double>(s_.credits_banked) / runs : 0.0;

    std::int64_t unexpected_errors = 0;
    ordered_json errors_json = ordered_json::object();
    for (const auto& [key, n] : sink.errors) {
        errors_json[key] = n;
        if (key != "choose_reward:already_chosen" && key != "purchase:grant_failed") unexpected_errors += n;
    }

    // Content never offered across all runs.
    std::vector<std::string> unreachable;
    std::vector<std::string> disabled;
    for (const PerkDef& p : content_->perks()) {
        if (!p.enabled) {
            disabled.push_back("perk:" + p.id);
            continue;
        }
        if (p.IsCurse()) {
            if (!s_.curse_offers.count(p.id)) unreachable.push_back("curse:" + p.id);
            continue;
        }
        bool is_passive = false;
        for (const CharacterDef& c : content_->characters()) {
            if (c.starting_passive == p.id && p.max_stacks <= 1) is_passive = true;
            for (const SkinDef& s : c.skins) is_passive = is_passive || s.starting_passive == p.id;
        }
        if (is_passive) continue;  // granted at run start (a character's or a skin's trait), never offered by design
        // Perks come from the PERKS menu (equipped) since rewards became items; offers still count.
        if (!s_.perk_offers.count(p.id) && !s_.perk_shop_offers.count(p.id) && !s_.perk_equips.count(p.id)) {
            unreachable.push_back("perk:" + p.id);
        }
    }
    std::set<std::string> starting;  // a build's own starting weapon is handed over, never offered
    for (const CharacterDef& c : content_->characters()) {
        for (const SkinDef& sk : c.skins) {
            if (!sk.build.starting_weapon.empty()) starting.insert(sk.build.starting_weapon);
        }
    }
    for (const WeaponBaseDef& w : content_->weapons()) {
        if (!w.enabled) {
            disabled.push_back("weapon:" + w.id);
        } else if (!s_.weapon_offers.count(w.id) && !starting.count(w.id)) {
            unreachable.push_back("weapon:" + w.id);
        }
    }
    for (const UniqueAffixDef& u : content_->unique_affixes()) {
        if (!s_.unique_rolls.count(u.id)) unreachable.push_back("unique_affix:" + u.id);
    }
    for (const RouteDef& r : content_->routes()) {
        if (!r.enabled) {
            disabled.push_back("route:" + r.id);
        } else if (!s_.route_offers.count(r.id) && r.id != content_->economy().run.starting_route) {
            unreachable.push_back("route:" + r.id);
        }
    }
    for (const CharacterDef& c : content_->characters()) {
        if (!c.enabled) {
            disabled.push_back("character:" + c.id);
        } else if (!s_.runs_by_character.count(c.id)) {
            unreachable.push_back("character:" + c.id);
        }
    }

    const std::int64_t invalid_encounters = s_.invalid_budget + s_.invalid_route + s_.invalid_offer +
                                            s_.invalid_route_choice + s_.incomplete_arena;
    const bool all_caps = caps_ == Capabilities::All();
    struct Check {
        std::string name;
        std::int64_t value;
        bool pass;
    };
    const std::vector<Check> checks = {
        {"invalid generated encounters (budget<=0, missing route, empty/short offer, bad route choice, unfinishable arena)",
         invalid_encounters, invalid_encounters == 0},
        {"negative balance occurrences", s_.negative_balance, s_.negative_balance == 0},
        {"duplicate reward grants (of " + std::to_string(s_.duplicate_attempts) + " deliberate repeat attempts)",
         s_.duplicate_grants, s_.duplicate_grants == 0},
        {"unexpected error events", unexpected_errors, unexpected_errors == 0},
        {"enabled content never offered" + std::string(all_caps ? "" : " (informational: capabilities restricted)"),
         static_cast<std::int64_t>(unreachable.size()), !all_caps || unreachable.empty()},
    };
    bool ok = true;
    for (const Check& c : checks) ok = ok && c.pass;

    // Progression reachability at the average credit rate.
    struct Unlockable {
        std::string key;
        std::int64_t cost;
        bool enabled;
    };
    std::vector<Unlockable> unlockables;
    for (const CharacterDef& c : content_->characters()) {
        if (c.unlock_cost_credits > 0) unlockables.push_back({"character:" + c.id, c.unlock_cost_credits, c.enabled});
    }
    for (const PerkDef& p : content_->perks()) {
        if (p.unlock_cost_credits > 0) unlockables.push_back({"perk:" + p.id, p.unlock_cost_credits, p.enabled});
    }
    for (const WeaponBaseDef& w : content_->weapons()) {
        if (w.unlock_cost_credits > 0) unlockables.push_back({"weapon:" + w.id, w.unlock_cost_credits, w.enabled});
    }
    std::sort(unlockables.begin(), unlockables.end(),
              [](const Unlockable& a, const Unlockable& b) { return a.cost < b.cost; });
    std::int64_t cumulative = 0;

    // ---- JSON ----
    ordered_json j;
    j["generator"] = "rm_sim";
    j["mod_version"] = kModVersion;
    j["rng_version"] = kRngVersion;
    j["date"] = opts_.date;
    j["command"] = "rm_sim --runs " + std::to_string(opts_.runs) + " --seed " + std::to_string(opts_.seed) +
                   " --config " + opts_.config + " --out " + opts_.out + " --capabilities " + opts_.capabilities +
                   " --policy " + opts_.policy + (opts_.hit_scale != 1.0 ? " --hit-scale " + Fmt(opts_.hit_scale, 2) : std::string());
    j["capabilities"] = caps_.ToString();
    j["player_model"] = {{"hp_max", kHpMax}, {"base_kill_seconds", kBaseKillSeconds},
                         {"incoming_hits_per_second", kIncomingHitsPerSecond * opts_.hit_scale},
                         {"tier_hit_scale", {kTierHitScale[1], kTierHitScale[2], kTierHitScale[3]}},
                         {"hit_damage", {kHitDamageMin, kHitDamageMax}}, {"partner_kill_chance", kPartnerKillChance},
                         {"duplicate_report_chance", kDuplicateReportChance}, {"melee_hit_chance", kMeleeHitChance},
                         {"regen_between_arenas", kRegenBetweenArenas}, {"skill_range", {kSkillMin, kSkillMax}}};
    j["runs"] = s_.runs;
    j["completed"] = s_.completed;
    j["failed"] = s_.failed;
    j["completion_rate"] = runs > 0 ? static_cast<double>(s_.completed) / runs : 0.0;
    j["runs_by_policy"] = s_.runs_by_policy;
    j["completed_by_policy"] = s_.completed_by_policy;
    j["runs_by_character"] = s_.runs_by_character;
    j["arenas_cleared_histogram"] = s_.arenas_cleared_hist;
    j["arena_ratings"] = s_.ratings;
    j["avg_supplies_earned_per_run"] = runs > 0 ? static_cast<double>(s_.supplies_earned) / runs : 0.0;
    j["avg_supplies_spent_per_run"] = runs > 0 ? static_cast<double>(s_.supplies_spent) / runs : 0.0;
    j["avg_credits_per_run"] = avg_credits;
    j["avg_credits_completed_run"] =
        s_.completed > 0 ? static_cast<double>(s_.credits_banked_completed) / static_cast<double>(s_.completed) : 0.0;
    j["avg_credits_failed_run"] =
        s_.failed > 0 ? static_cast<double>(s_.credits_banked_failed) / static_cast<double>(s_.failed) : 0.0;
    j["reward_offers"] = s_.reward_offers_total;
    j["reward_kind_offers"] = s_.reward_kind_offers;
    j["reward_kind_picks"] = s_.reward_kind_picks;
    j["shop_purchases"] = s_.shop_kind_buys;
    j["shop_rollbacks"] = s_.shop_rollbacks;
    j["rerolls"] = s_.rerolls;
    ordered_json perks = ordered_json::object();
    for (const PerkDef& p : content_->perks()) {
        const std::int64_t offered = (s_.perk_offers.count(p.id) ? s_.perk_offers.at(p.id) : 0) +
                                     (s_.perk_shop_offers.count(p.id) ? s_.perk_shop_offers.at(p.id) : 0);
        const std::int64_t picked = (s_.perk_picks.count(p.id) ? s_.perk_picks.at(p.id) : 0) +
                                    (s_.perk_shop_buys.count(p.id) ? s_.perk_shop_buys.at(p.id) : 0);
        perks[p.id] = {{"rarity", RarityName(p.rarity)}, {"category", PerkCategoryName(p.category)},
                       {"offered", offered}, {"picked", picked},
                       {"offer_rate_per_run", runs > 0 ? static_cast<double>(offered) / runs : 0.0},
                       {"pick_rate", offered > 0 ? static_cast<double>(picked) / static_cast<double>(offered) : 0.0}};
        if (p.IsCurse()) {
            perks[p.id]["route_offers"] = s_.curse_offers.count(p.id) ? s_.curse_offers.at(p.id) : 0;
            perks[p.id]["taken"] = s_.curse_taken.count(p.id) ? s_.curse_taken.at(p.id) : 0;
        }
    }
    j["perks"] = perks;
    auto rarity_json = [](const std::array<std::int64_t, kRarityCount>& a) {
        ordered_json o;
        for (Rarity r : kAllRarities) o[std::string(RarityName(r))] = a[RarityIndex(r)];
        return o;
    };
    j["perk_offer_rarity"] = rarity_json(s_.perk_offer_rarity);
    j["weapon_offer_rarity"] = rarity_json(s_.weapon_offer_rarity);
    j["weapon_acquired_rarity"] = rarity_json(s_.weapon_acquired_rarity);
    j["weapon_offers"] = s_.weapon_offers;
    j["unique_affix_rolls"] = s_.unique_rolls;
    j["route_offers"] = s_.route_offers;
    j["route_picks"] = s_.route_picks;
    auto pct_json = [](const Percentiles& p) {
        return ordered_json{{"mean", p.mean}, {"p50", p.p50}, {"p99", p.p99}, {"p99_9", p.p999}, {"max", p.max}};
    };
    j["damage_multiplier"] = {{"hits", s_.hits}, {"raw", pct_json(raw)}, {"clamped", pct_json(clamped)},
                              {"clamped_hits", s_.clamped_hits}, {"crits", s_.crits},
                              {"clamp_range", {content_->economy().damage.outgoing_min, content_->economy().damage.outgoing_max}}};
    j["cheat_deaths"] = s_.cheat_deaths;
    j["heal_requests_applied"] = s_.heals_applied;
    j["invariants"] = ordered_json::array();
    for (const Check& c : checks) j["invariants"].push_back({{"name", c.name}, {"value", c.value}, {"pass", c.pass}});
    j["invalid_encounters_detail"] = {{"budget", s_.invalid_budget}, {"missing_route", s_.invalid_route},
                                      {"bad_offer", s_.invalid_offer}, {"bad_route_choice", s_.invalid_route_choice},
                                      {"unfinishable_arena", s_.incomplete_arena}};
    j["error_events"] = errors_json;
    j["events"] = sink.by_type;
    ordered_json reach = ordered_json::array();
    for (const Unlockable& u : unlockables) {
        if (u.enabled) cumulative += u.cost;
        const double runs_needed = avg_credits > 0 ? std::ceil(static_cast<double>(u.cost) / avg_credits) : -1.0;
        reach.push_back({{"unlock", u.key}, {"cost", u.cost}, {"enabled", u.enabled},
                         {"runs_to_afford_alone", runs_needed},
                         {"bought_in_sim_after_run", s_.unlock_run.count(u.key) ? s_.unlock_run.at(u.key) : 0}});
    }
    j["progression"] = {{"avg_credits_per_run", avg_credits}, {"unlocks", reach},
                        {"runs_to_unlock_all_enabled",
                         avg_credits > 0 ? std::ceil(static_cast<double>(cumulative) / avg_credits) : -1.0}};
    // Per build (a character's skin), weakest first.
    std::vector<const BuildStats*> builds;
    for (const auto& [id, b] : s_.builds) builds.push_back(&b);
    auto rate = [](std::int64_t n, std::int64_t d) { return d > 0 ? static_cast<double>(n) / static_cast<double>(d) : 0.0; };
    std::sort(builds.begin(), builds.end(), [&](const BuildStats* a, const BuildStats* b) {
        return rate(a->completed, a->runs) < rate(b->completed, b->runs);
    });
    ordered_json builds_json = ordered_json::array();
    for (const BuildStats* b : builds) {
        builds_json.push_back({{"skin", b->skin}, {"character", b->character}, {"build", b->name}, {"runs", b->runs},
                               {"completion_rate", rate(b->completed, b->runs)},
                               {"reached_last_arena_rate", rate(b->reached_last, b->runs)},
                               {"avg_arenas_cleared", b->runs > 0 ? static_cast<double>(b->arenas_cleared) / static_cast<double>(b->runs) : 0.0},
                               {"avg_credits", b->runs > 0 ? static_cast<double>(b->credits) / static_cast<double>(b->runs) : 0.0},
                               {"avg_supplies_earned", b->runs > 0 ? static_cast<double>(b->supplies_earned) / static_cast<double>(b->runs) : 0.0},
                               {"specials_per_run", b->runs > 0 ? static_cast<double>(b->specials) / static_cast<double>(b->runs) : 0.0},
                               {"heals_per_run", b->runs > 0 ? static_cast<double>(b->heals) / static_cast<double>(b->runs) : 0.0},
                               {"throws_per_run", b->runs > 0 ? static_cast<double>(b->throws) / static_cast<double>(b->runs) : 0.0},
                               {"revives_per_run", b->runs > 0 ? static_cast<double>(b->revives) / static_cast<double>(b->runs) : 0.0},
                               {"chests_opened_per_run", b->runs > 0 ? static_cast<double>(b->chests_opened) / static_cast<double>(b->runs) : 0.0},
                               {"chests_locked_per_run", b->runs > 0 ? static_cast<double>(b->chests_locked) / static_cast<double>(b->runs) : 0.0},
                               {"mean_outgoing_mult", b->hits > 0 ? b->outgoing_mult / static_cast<double>(b->hits) : 0.0},
                               {"incoming_taken_per_raw", b->incoming_raw > 0.0 ? b->incoming_dealt / b->incoming_raw : 0.0}});
    }
    j["builds"] = builds_json;
    j["unreachable"] = unreachable;
    j["disabled_content"] = disabled;
    j["all_invariants_pass"] = ok;

    const std::filesystem::path out(opts_.out);
    {
        std::ofstream f(out / "summary.json", std::ios::binary | std::ios::trunc);
        f << j.dump(2) << "\n";
    }

    // ---- Markdown ----
    std::ostringstream md;
    md << "# Rogue Mercenaries simulation summary" << (opts_.date.empty() ? "" : " (" + opts_.date + ")") << "\n\n";
    md << "Generated by `" << j["command"].get<std::string>() << "` (mod " << kModVersion << ", RNG v" << kRngVersion
       << "). Output is deterministic for the same seed, configs and build.\n\n";
    md << "Capabilities assumed: `" << caps_.ToString() << "`. Policies rotate random / greedy_damage / safe per run. "
       << "This is a balance model, not evidence of in-game behaviour.\n\n";
    md << "## Headline\n\n| Metric | Value |\n|---|---|\n";
    md << "| Runs | " << s_.runs << " |\n";
    md << "| Completed / failed | " << s_.completed << " / " << s_.failed << " (" << Pct(static_cast<double>(s_.completed), runs)
       << " complete) |\n";
    md << "| Avg supplies earned / spent per run | " << Fmt(j["avg_supplies_earned_per_run"].get<double>(), 1) << " / "
       << Fmt(j["avg_supplies_spent_per_run"].get<double>(), 1) << " |\n";
    md << "| Avg credits banked per run (completed / failed) | " << Fmt(avg_credits, 1) << " ("
       << Fmt(j["avg_credits_completed_run"].get<double>(), 1) << " / " << Fmt(j["avg_credits_failed_run"].get<double>(), 1)
       << ") |\n";
    md << "| Damage multiplier raw: p50 / p99 / p99.9 / max | " << Fmt(raw.p50, 3) << " / " << Fmt(raw.p99, 3) << " / "
       << Fmt(raw.p999, 3) << " / " << Fmt(raw.max, 3) << " |\n";
    md << "| Damage multiplier clamped: p50 / p99 / p99.9 / max | " << Fmt(clamped.p50, 3) << " / " << Fmt(clamped.p99, 3)
       << " / " << Fmt(clamped.p999, 3) << " / " << Fmt(clamped.max, 3) << " |\n";
    md << "| Hits simulated (clamped, crits) | " << s_.hits << " (" << s_.clamped_hits << ", " << s_.crits << ") |\n";
    md << "| Cheat-death saves / heal requests applied | " << s_.cheat_deaths << " / " << s_.heals_applied << " |\n\n";

    md << "## Invariants\n\n| Check | Value | Result |\n|---|---|---|\n";
    for (const Check& c : checks) md << "| " << c.name << " | " << c.value << " | " << (c.pass ? "PASS" : "FAIL") << " |\n";
    md << "\nInvalid encounter detail: budget " << s_.invalid_budget << ", missing route " << s_.invalid_route
       << ", bad offer " << s_.invalid_offer << ", bad route choice " << s_.invalid_route_choice << ", unfinishable arena "
       << s_.incomplete_arena << ". Shop purchases rolled back after a failed grant: " << s_.shop_rollbacks << ".\n\n";

    md << "## Outcomes\n\n| Policy | Runs | Completed |\n|---|---|---|\n";
    for (const auto& [policy, n] : s_.runs_by_policy) {
        const std::int64_t c = s_.completed_by_policy.count(policy) ? s_.completed_by_policy.at(policy) : 0;
        md << "| " << policy << " | " << n << " | " << c << " (" << Pct(static_cast<double>(c), static_cast<double>(n)) << ") |\n";
    }
    md << "\nArenas cleared per run: ";
    for (std::size_t i = 0; i < s_.arenas_cleared_hist.size(); ++i) {
        md << (i ? ", " : "") << i << ": " << s_.arenas_cleared_hist[i];
    }
    md << ". Arena ratings: ";
    bool first = true;
    for (const auto& [r, n] : s_.ratings) {
        md << (first ? "" : ", ") << r << " " << n;
        first = false;
    }
    md << ".\n\n";

    md << "## Builds\n\nEvery skin is its own build (characters.json); runs rotate through all of them, weakest "
          "first here. Damage taken is per point of raw incoming damage (defence, perks, invulnerability; a "
          "partner's share of the enemies' attention is in the hit rate instead). Specials and the partner are "
          "estimates (see the player model).\n\n";
    md << "| Build | Skin | Runs | Complete | Reached last | Arenas | Credits | Supplies | Specials | Heals | Throws | Chests (shut) | Out mult | Damage taken |\n"
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const ordered_json& b : builds_json) {
        md << "| " << b["build"].get<std::string>() << " | " << b["skin"].get<std::string>() << " | " << b["runs"].get<std::int64_t>()
           << " | " << Fmt(100.0 * b["completion_rate"].get<double>(), 1) << "% | " << Fmt(100.0 * b["reached_last_arena_rate"].get<double>(), 1)
           << "% | " << Fmt(b["avg_arenas_cleared"].get<double>(), 2) << " | " << Fmt(b["avg_credits"].get<double>(), 0) << " | "
           << Fmt(b["avg_supplies_earned"].get<double>(), 0) << " | " << Fmt(b["specials_per_run"].get<double>(), 1) << " | "
           << Fmt(b["heals_per_run"].get<double>(), 1) << " | " << Fmt(b["throws_per_run"].get<double>(), 1) << " | "
           << Fmt(b["chests_opened_per_run"].get<double>(), 1) << " ("
           << Fmt(b["chests_locked_per_run"].get<double>(), 1) << ") | " << Fmt(b["mean_outgoing_mult"].get<double>(), 3) << " | "
           << Fmt(b["incoming_taken_per_raw"].get<double>(), 3) << " |\n";
    }
    md << "\n";

    md << "## Rarity distribution\n\n| Rarity | Perk offers | Weapon offers | Weapons acquired |\n|---|---|---|---|\n";
    std::int64_t po = 0, wo = 0, wa = 0;
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        po += s_.perk_offer_rarity[i];
        wo += s_.weapon_offer_rarity[i];
        wa += s_.weapon_acquired_rarity[i];
    }
    for (Rarity r : kAllRarities) {
        const std::size_t i = RarityIndex(r);
        md << "| " << RarityName(r) << " | " << s_.perk_offer_rarity[i] << " (" << Pct(static_cast<double>(s_.perk_offer_rarity[i]), static_cast<double>(po))
           << ") | " << s_.weapon_offer_rarity[i] << " (" << Pct(static_cast<double>(s_.weapon_offer_rarity[i]), static_cast<double>(wo))
           << ") | " << s_.weapon_acquired_rarity[i] << " (" << Pct(static_cast<double>(s_.weapon_acquired_rarity[i]), static_cast<double>(wa)) << ") |\n";
    }
    md << "\nReward choice kinds offered: ";
    first = true;
    for (const auto& [k, n] : s_.reward_kind_offers) {
        md << (first ? "" : ", ") << k << " " << n;
        first = false;
    }
    md << "; picked: ";
    first = true;
    for (const auto& [k, n] : s_.reward_kind_picks) {
        md << (first ? "" : ", ") << k << " " << n;
        first = false;
    }
    md << ". Rerolls used: " << s_.rerolls << ".\n\n";

    md << "## Perk offer and pick rates\n\nOffers count reward and shop appearances; picks count reward choices and shop "
          "purchases. Curses are offered through route choices.\n\n";
    md << "| Perk | Category | Rarity | Offered | Offers/run | Picked | Pick rate |\n|---|---|---|---|---|---|---|\n";
    for (const PerkDef& p : content_->perks()) {
        const auto& pj = perks[p.id];
        const std::int64_t offered = p.IsCurse() ? pj["route_offers"].get<std::int64_t>() : pj["offered"].get<std::int64_t>();
        const std::int64_t picked = p.IsCurse() ? pj["taken"].get<std::int64_t>() : pj["picked"].get<std::int64_t>();
        const bool passive = std::any_of(content_->characters().begin(), content_->characters().end(),
                                         [&p](const CharacterDef& c) { return c.starting_passive == p.id; });
        md << "| " << p.id << (p.enabled ? "" : " (disabled)") << (passive ? " (starting passive)" : "") << " | " << PerkCategoryName(p.category) << " | "
           << RarityName(p.rarity) << " | " << offered << " | " << Fmt(runs > 0 ? static_cast<double>(offered) / runs : 0.0, 3)
           << " | " << picked << " | " << Pct(static_cast<double>(picked), static_cast<double>(offered)) << " |\n";
    }

    md << "\n## Progression reachability\n\nAverage credits banked per run: " << Fmt(avg_credits, 1)
       << ". The simulated profile buys the cheapest affordable enabled unlock after each run.\n\n";
    md << "| Unlock | Cost | Runs to afford alone | Bought in sim after run |\n|---|---|---|---|\n";
    for (const ordered_json& u : reach) {
        const bool enabled = u["enabled"].get<bool>();
        const std::int64_t bought = u["bought_in_sim_after_run"].get<std::int64_t>();
        md << "| " << u["unlock"].get<std::string>() << (enabled ? "" : " (disabled)") << " | " << u["cost"].get<std::int64_t>()
           << " | " << Fmt(u["runs_to_afford_alone"].get<double>(), 0) << " | "
           << (enabled ? (bought > 0 ? std::to_string(bought) : std::string("never")) : std::string("n/a (not purchasable)"))
           << " |\n";
    }
    md << "\nRuns to afford every enabled unlock at the average rate: "
       << Fmt(j["progression"]["runs_to_unlock_all_enabled"].get<double>(), 0) << ".\n\n";

    md << "## Unreachable and disabled content\n\n";
    md << "Enabled content never offered across all runs: " << (unreachable.empty() ? "none" : "") ;
    for (std::size_t i = 0; i < unreachable.size(); ++i) md << (i ? ", " : "") << "`" << unreachable[i] << "`";
    md << ".\n\nDisabled (reserved, expected never offered): ";
    for (std::size_t i = 0; i < disabled.size(); ++i) md << (i ? ", " : "") << "`" << disabled[i] << "`";
    md << (disabled.empty() ? "none" : "") << ".\n\n";

    md << "## Player model\n\nPer arena: kill intervals ~ Exp(" << kBaseKillSeconds
       << " s / (mean clamped multiplier x skill)), skill ~ U(" << kSkillMin << ", " << kSkillMax << ") per run; 3-6 hits per "
       << "enemy at decreasing target HP; " << Fmt(kMeleeHitChance * 100, 0) << "% melee hits; incoming hits Poisson("
       << kIncomingHitsPerSecond * opts_.hit_scale << "/s x tier scale 1.0/1.3/1.6 / skill), " << kHitDamageMin << "-" << kHitDamageMax
       << " damage before modifiers; " << Fmt(kPartnerKillChance * 100, 0) << "% partner kills (not counted), "
       << Fmt(kDuplicateReportChance * 100, 0) << "% duplicate kill reports; +" << kRegenBetweenArenas
       << " HP between arenas (per 100 of maximum); heal requests applied immediately. Maximum health is " << kHpMax
       << " times the build's multiplier and its melee-kill health. Healing items from rewards, the shop and perks "
          "are used under " << Fmt(kHealBelow * 100, 0) << "% health (spray and mixed herb " << Fmt(kSprayHeal * 100, 0)
       << "%, green herb " << Fmt(kGreenHerbHeal * 100, 0) << "%, red only with a green), through the core's heal "
          "(the build's heal multiplier and buffs). Specials are used whenever ready: a dash finishes "
       << kDashKills << " enemies, the burst " << kBurstKills << ", the twin SMGs " << kDualSmgKills
       << " (estimates); the backup partner kills " << kPartnerKillSpeed << "x as fast with you and leaves "
       << kPartnerIncoming << "x the hits for the rest of its arena. The arena's " << content_->economy().chests.per_arena
       << " chests are reached after equal shares of its kills (each needs a key unless the build is exempt; the "
          "kills are of a key class); a kill drops a green herb " << Fmt(kHerbDropChance * 100, 0)
       << "% of the time, or for a build that turns drops into its own item that item " << Fmt(kAnyDropChance * 100, 0)
       << "% of the time (any drop). Grenades and mines are thrown as they come, one every " << kThrowSeconds
       << " s (a mine finishes " << kMineKills << " enemies, a hand grenade " << kGrenadeKills << ", an incendiary "
       << kIncendiaryKills << ", a flash " << kFlashKills << "); the backup partner revives a player bleeding out "
          "once per arena, at " << Fmt(kPartnerReviveHp * 100, 0) << "% health. Each run equips a random unlocked perk per "
          "category (the PERKS menu); the profile buys the cheapest affordable unlock after each run.\n";

    {
        std::ofstream f(out / "summary.md", std::ios::binary | std::ios::trunc);
        f << md.str();
    }

    std::cout << "rm_sim: " << s_.runs << " runs, " << s_.completed << " completed, " << s_.failed << " failed ("
              << Pct(static_cast<double>(s_.completed), runs) << ")\n"
              << "  avg supplies earned/spent per run: " << Fmt(j["avg_supplies_earned_per_run"].get<double>(), 1) << " / "
              << Fmt(j["avg_supplies_spent_per_run"].get<double>(), 1) << "\n"
              << "  avg credits per run: " << Fmt(avg_credits, 1) << "\n"
              << "  damage multiplier raw p50/p99/p99.9/max: " << Fmt(raw.p50, 3) << " / " << Fmt(raw.p99, 3) << " / "
              << Fmt(raw.p999, 3) << " / " << Fmt(raw.max, 3) << "\n"
              << "  damage multiplier clamped p50/p99/p99.9/max: " << Fmt(clamped.p50, 3) << " / " << Fmt(clamped.p99, 3)
              << " / " << Fmt(clamped.p999, 3) << " / " << Fmt(clamped.max, 3) << "\n"
              << "  invalid encounters: " << invalid_encounters << ", negative balances: " << s_.negative_balance
              << ", duplicate reward grants: " << s_.duplicate_grants << " (attempts " << s_.duplicate_attempts << ")\n"
              << "  unexpected errors: " << unexpected_errors << ", unreachable: " << unreachable.size() << "\n"
              << "  invariants: " << (ok ? "PASS" : "FAIL") << "  (" << Fmt(seconds, 1) << " s)\n"
              << "  wrote " << (out / "summary.json").string() << " and " << (out / "summary.md").string() << "\n";
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    try {
        if (!ParseArgs(argc, argv, opts)) {
            std::cerr << "usage: rm_sim [--runs N] [--seed S] [--config DIR] [--out DIR] [--capabilities all|none|a,b]"
                         " [--policy mix|random|greedy|safe] [--hit-scale X] [--log-runs N] [--date YYYY-MM-DD]\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "bad argument: " << e.what() << "\n";
        return 2;
    }
    ContentLoadResult loaded = Content::LoadFromDirectory(opts.config);
    for (const std::string& w : loaded.warnings) std::cerr << "warning: " << w << "\n";
    if (!loaded.ok()) {
        for (const std::string& e : loaded.errors) std::cerr << "error: " << e << "\n";
        return 2;
    }
    Simulator sim(opts, loaded.content);
    return sim.Run();
}
