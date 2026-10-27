// rm_sim: headless Rogue Mercenaries run simulator.
//
// Drives rm::RunController with a simple stochastic player model (no engine) to check
// invariants and report balance:
//   rm_sim --runs 10000 --seed 1 --config configs --out build/sim
// Options: --capabilities all|none|<comma list>  --policy mix|random|greedy|safe
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
        } else if (a == "--log-runs" && next(v)) {
            o.log_runs = std::stoi(v);
        } else if (a == "--date" && next(v)) {
            o.date = v;
        } else {
            return false;
        }
    }
    return o.runs > 0 && (o.policy == "mix" || o.policy == "random" || o.policy == "greedy" || o.policy == "safe");
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

struct Stats {
    std::int64_t runs = 0, completed = 0, failed = 0;
    std::map<std::string, std::int64_t> runs_by_policy, completed_by_policy;
    std::map<std::string, std::int64_t> runs_by_character;
    std::array<std::int64_t, 4> arenas_cleared_hist{};
    std::map<int, std::int64_t> death_arena_hist;
    std::int64_t supplies_earned = 0, supplies_spent = 0;
    std::int64_t credits_banked = 0, credits_banked_completed = 0, credits_banked_failed = 0;
    std::int64_t arenas_played = 0;
    std::map<std::string, std::int64_t> ratings;
    std::map<std::string, std::int64_t> perk_offers, perk_picks, perk_shop_offers, perk_shop_buys;
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
    int WriteReports(const SimSink& sink, double seconds);

    Options opts_;
    std::shared_ptr<const Content> content_;
    Capabilities caps_;
    Stats s_;
    std::vector<std::string> character_ids_;
};

void Simulator::CheckBalances(const RunController& rc) {
    if (rc.state().supplies.balance() < 0) ++s_.negative_balance;
    if (rc.profile().credits.balance() < 0) ++s_.negative_balance;
}

void Simulator::ApplyHeals(RunController& rc, double& hp) {
    for (const HealRequest& h : rc.ConsumeHealRequests()) {
        hp = std::min(kHpMax, hp + h.fraction_of_max * kHpMax);
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
    const double hit_rate = kIncomingHitsPerSecond * kTierHitScale[static_cast<std::size_t>(tier)] / skill;
    double power = 1.0;
    double power_sum = 0.0;
    std::int64_t power_n = 0;
    int enemy_serial = 0;
    const int max_events = route->enemy_budget * 4 + 20;
    const bool melee_known = caps_.Has(cap::kMeleeDetect);

    for (int ev = 0; ev < max_events && !rc.IsArenaComplete(); ++ev) {
        // Time until the next kill, scaled by current damage output and player skill.
        const double mean = kBaseKillSeconds / std::max(0.25, power * skill);
        const double dt = std::max(0.5, -std::log(1.0 - player.NextDouble()) * mean);

        // Incoming hits during dt.
        const double expected_hits = hit_rate * dt;
        int hits = 0;
        for (double l = std::exp(-expected_hits), p = player.NextDouble(); p > l; p *= player.NextDouble()) ++hits;
        for (int h = 0; h < hits; ++h) {
            const double now = t + dt * (h + 1) / (hits + 1);
            const double amount = player.UniformReal(kHitDamageMin, kHitDamageMax);
            const IncomingResult in = rc.ComputeIncoming(IncomingContext{now, amount, hp, kHpMax});
            const double before = hp;
            const double dealt = amount * in.multiplier;
            if (in.prevent_death) {
                hp = in.leave_hp;
                ++s_.cheat_deaths;
            } else {
                hp = std::max(0.0, hp - dealt);
            }
            rc.OnPlayerDamaged(DamageEvent{now, before - hp > 0 ? before - hp : 0.0, before, hp, kHpMax});
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
            hit.player_hp_max = kHpMax;
            const OutgoingResult r = rc.ComputeOutgoingMultiplier(hit);
            s_.raw_mult.push_back(static_cast<float>(r.breakdown.raw));
            s_.clamped_mult.push_back(static_cast<float>(r.multiplier));
            ++s_.hits;
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
        rc.OnEnemyKilled(kill);
        if (kill.by_player && player.Chance(kDuplicateReportChance)) rc.OnEnemyKilled(kill);  // duplicate hook report
        ApplyHeals(rc, hp);
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
            distinct.insert(std::string(RewardKindName(c.kind)) + c.perk_id + c.weapon.base_id + std::to_string(c.supplies));
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
        double score = 0.0;
        switch (policy) {
            case Policy::Random: score = player.Chance(0.5) ? player.NextDouble() + 1.0 : 0.0; break;
            case Policy::Greedy:
                if (o.kind == ShopOfferKind::WeaponUpgrade) score = 9.0;
                if (o.kind == ShopOfferKind::Perk && Traits(*content_->FindPerk(o.perk_id)).offense) score = 6.0 + r;
                if (o.kind == ShopOfferKind::Weapon && o.rarity >= Rarity::Rare) score = 5.0 + r;
                break;
            case Policy::Safe:
                if (o.kind == ShopOfferKind::Heal && hp < 0.7 * kHpMax) score = 10.0;
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
    const auto& choices = rc.RouteChoices();
    const RouteDef* current = content_->FindRoute(rc.state().route_id);
    const int safe = SafeTier(*content_, current != nullptr ? current->difficulty_tier : 1);
    bool has_safe = false;
    bool has_hard = false;
    for (const RouteChoice& c : choices) {
        const RouteDef* r = content_->FindRoute(c.route_id);
        if (r == nullptr || r->enemy_budget <= 0) {
            ++s_.invalid_route;
            continue;
        }
        ++s_.route_offers[c.route_id];
        if (!c.curse_perk_id.empty()) ++s_.curse_offers[c.curse_perk_id];
        if (r->difficulty_tier <= safe) has_safe = true;
        if (r->difficulty_tier > safe) has_hard = true;
    }
    if (choices.size() < 2 || choices.size() > 3 || !has_safe || !has_hard) ++s_.invalid_route_choice;
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
            if (c.kind == UnlockKind::Character) character_ids_.push_back(c.id);
        }
    }
    CheckBalances(rc);
}

void Simulator::PlayRun(RunController& rc, int index, Policy policy) {
    std::uint64_t mix = opts_.seed + static_cast<std::uint64_t>(index);
    const std::uint64_t run_seed = SplitMix64(mix);
    Rng player = Rng::Derive(run_seed, "sim_player");
    const std::string character = character_ids_[static_cast<std::size_t>(index) % character_ids_.size()];
    if (!rc.StartRun(character, run_seed).ok()) {
        std::cerr << "StartRun failed for run " << index << "\n";
        return;
    }
    ++s_.runs;
    ++s_.runs_by_policy[PolicyName(policy)];
    ++s_.runs_by_character[character];
    const double skill = player.UniformReal(kSkillMin, kSkillMax);
    double hp = kHpMax;
    double t = 0.0;
    while (rc.HasActiveRun()) {
        const Phase phase = rc.phase();
        if (phase == Phase::RunComplete || phase == Phase::RunFailed) break;
        if (!PlayArena(rc, player, t, hp, skill, index)) break;
        rc.ContinueToRewards();
        ChooseReward(rc, player, policy);
        if (rc.phase() == Phase::Shop) {
            Shop(rc, player, policy, hp);
            ChooseRoute(rc, player, policy);
            hp = std::min(kHpMax, hp + kRegenBetweenArenas);
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
    } else {
        ++s_.failed;
        ++s_.death_arena_hist[sum.arenas_cleared + 1];
        s_.credits_banked_failed += sum.credits_banked;
    }
    s_.arenas_cleared_hist[static_cast<std::size_t>(std::clamp(sum.arenas_cleared, 0, 3))]++;
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
    for (const CharacterDef& c : content_->characters()) {
        if (CheckCharacter(&c, {}) == CharacterGate::Selectable) character_ids_.push_back(c.id);
    }
    if (character_ids_.empty()) {
        std::cerr << "no selectable character\n";
        return 2;
    }
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
        }
        if (is_passive) continue;  // granted at run start, never offered by design
        if (!s_.perk_offers.count(p.id) && !s_.perk_shop_offers.count(p.id)) unreachable.push_back("perk:" + p.id);
    }
    for (const WeaponBaseDef& w : content_->weapons()) {
        if (!w.enabled) {
            disabled.push_back("weapon:" + w.id);
        } else if (!s_.weapon_offers.count(w.id)) {
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
                   " --policy " + opts_.policy;
    j["capabilities"] = caps_.ToString();
    j["player_model"] = {{"hp_max", kHpMax}, {"base_kill_seconds", kBaseKillSeconds},
                         {"incoming_hits_per_second", kIncomingHitsPerSecond},
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
       << kIncomingHitsPerSecond << "/s x tier scale 1.0/1.3/1.6 / skill), " << kHitDamageMin << "-" << kHitDamageMax
       << " damage before modifiers; " << Fmt(kPartnerKillChance * 100, 0) << "% partner kills (not counted), "
       << Fmt(kDuplicateReportChance * 100, 0) << "% duplicate kill reports; +" << kRegenBetweenArenas
       << " HP between arenas; heal requests applied immediately.\n";

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
                         " [--policy mix|random|greedy|safe] [--log-runs N] [--date YYYY-MM-DD]\n";
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
