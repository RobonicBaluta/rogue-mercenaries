#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "rm/content.h"
#include "rm/version.h"

namespace rm {
namespace {

using nlohmann::json;

std::string Num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string Q(std::string_view s) { return "'" + std::string(s) + "'"; }

/// Error/warning collector with a location prefix ("perks.json: perk 'x': effect[0]").
struct Diag {
    std::vector<std::string>* errors;
    std::vector<std::string>* warnings;
    std::string prefix;

    void Error(const std::string& msg) const { errors->push_back(prefix + ": " + msg); }
    void Warn(const std::string& msg) const { warnings->push_back(prefix + ": " + msg); }
    [[nodiscard]] Diag Sub(const std::string& suffix) const {
        return Diag{errors, warnings, prefix + suffix};
    }
};

const json* Get(const json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(std::string(key));
    return it == obj.end() ? nullptr : &*it;
}

void CheckKeys(const Diag& d, const json& obj, std::initializer_list<std::string_view> allowed) {
    if (!obj.is_object()) return;
    for (const auto& item : obj.items()) {
        const std::string& key = item.key();
        if (!key.empty() && key[0] == '_') continue;  // "_comment" style annotations
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            d.Error("unknown field " + Q(key));
        }
    }
}

bool GetString(const Diag& d, const json& obj, std::string_view key, std::string& out,
               bool required) {
    const json* v = Get(obj, key);
    if (v == nullptr) {
        if (required) d.Error("missing required field " + Q(key));
        return false;
    }
    if (!v->is_string()) {
        d.Error("field " + Q(key) + " must be a string");
        return false;
    }
    out = v->get<std::string>();
    return true;
}

bool GetNumber(const Diag& d, const json& obj, std::string_view key, double& out, bool required) {
    const json* v = Get(obj, key);
    if (v == nullptr) {
        if (required) d.Error("missing required field " + Q(key));
        return false;
    }
    if (!v->is_number()) {
        d.Error("field " + Q(key) + " must be a number");
        return false;
    }
    const double value = v->get<double>();
    if (!std::isfinite(value)) {
        d.Error("field " + Q(key) + " must be finite");
        return false;
    }
    out = value;
    return true;
}

bool GetInt(const Diag& d, const json& obj, std::string_view key, std::int64_t& out, bool required) {
    const json* v = Get(obj, key);
    if (v == nullptr) {
        if (required) d.Error("missing required field " + Q(key));
        return false;
    }
    if (v->is_number_integer()) {
        out = v->get<std::int64_t>();
        return true;
    }
    if (v->is_number_float()) {
        const double f = v->get<double>();
        if (std::isfinite(f) && std::floor(f) == f && std::fabs(f) < 9.0e15) {
            out = static_cast<std::int64_t>(f);
            return true;
        }
    }
    d.Error("field " + Q(key) + " must be an integer");
    return false;
}

bool GetIntRange(const Diag& d, const json& obj, std::string_view key, int& out, bool required,
                 int lo, int hi) {
    std::int64_t v = out;
    if (!GetInt(d, obj, key, v, required)) return false;
    if (v < lo || v > hi) {
        d.Error("field " + Q(key) + " = " + std::to_string(v) + " must be in [" +
                std::to_string(lo) + ", " + std::to_string(hi) + "]");
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool GetBool(const Diag& d, const json& obj, std::string_view key, bool& out, bool required) {
    const json* v = Get(obj, key);
    if (v == nullptr) {
        if (required) d.Error("missing required field " + Q(key));
        return false;
    }
    if (!v->is_boolean()) {
        d.Error("field " + Q(key) + " must be true or false");
        return false;
    }
    out = v->get<bool>();
    return true;
}

bool GetStringList(const Diag& d, const json& obj, std::string_view key,
                   std::vector<std::string>& out, bool required) {
    const json* v = Get(obj, key);
    if (v == nullptr) {
        if (required) d.Error("missing required field " + Q(key));
        return false;
    }
    if (!v->is_array()) {
        d.Error("field " + Q(key) + " must be an array of strings");
        return false;
    }
    out.clear();
    for (const json& e : *v) {
        if (!e.is_string()) {
            d.Error("field " + Q(key) + " must contain only strings");
            return false;
        }
        out.push_back(e.get<std::string>());
    }
    return true;
}

/// Inclusive range check; lo_open makes the lower bound exclusive.
void CheckRange(const Diag& d, std::string_view field, double v, double lo, double hi,
                bool lo_open = false) {
    const bool bad_lo = lo_open ? !(v > lo) : !(v >= lo);
    if (bad_lo || !(v <= hi)) {
        d.Error("field " + Q(field) + " = " + Num(v) + " must be in " + (lo_open ? "(" : "[") +
                Num(lo) + ", " + Num(hi) + "]");
    }
}

bool IsValidId(std::string_view id) {
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool ReadId(const Diag& d, const json& obj, std::string& id) {
    if (!GetString(d, obj, "id", id, true)) return false;
    if (!IsValidId(id)) {
        d.Error("id " + Q(id) + " must be non-empty and use only a-z, 0-9 and '_'");
        return false;
    }
    return true;
}

void ReadCapabilities(const Diag& d, const json& obj, std::vector<std::string>& out) {
    if (!GetStringList(d, obj, "requires_capabilities", out, false)) return;
    for (const std::string& c : out) {
        if (!IsKnownCapability(c)) d.Error("unknown capability " + Q(c));
    }
}

void MergeCapabilities(std::vector<std::string>& declared, const std::vector<std::string>& implied) {
    for (const std::string& c : implied) {
        if (std::find(declared.begin(), declared.end(), c) == declared.end()) declared.push_back(c);
    }
    std::sort(declared.begin(), declared.end());
}

template <class T, class Parse>
bool ReadEnum(const Diag& d, const json& obj, std::string_view key, T& out, Parse parse,
              std::string_view allowed) {
    std::string name;
    if (!GetString(d, obj, key, name, true)) return false;
    const auto parsed = parse(name);
    if (!parsed) {
        d.Error("unsupported " + std::string(key) + " " + Q(name) + " (expected one of: " +
                std::string(allowed) + ")");
        return false;
    }
    out = *parsed;
    return true;
}

bool ReadRarityTable(const Diag& d, const json& obj, std::string_view key, RarityWeights& out) {
    const json* t = Get(obj, key);
    if (t == nullptr || !t->is_object()) {
        d.Error("field " + Q(key) + " must be an object keyed by rarity");
        return false;
    }
    const Diag sub = d.Sub("." + std::string(key));
    CheckKeys(sub, *t, {"common", "uncommon", "rare", "epic", "legendary"});
    for (Rarity r : kAllRarities) {
        double v = 0.0;
        if (GetNumber(sub, *t, RarityName(r), v, true)) {
            CheckRange(sub, RarityName(r), v, 0.0, 1.0e9);
            out[RarityIndex(r)] = v;
        }
    }
    return true;
}

bool ReadRarityPrices(const Diag& d, const json& obj, std::string_view key,
                      std::array<std::int64_t, kRarityCount>& out) {
    const json* t = Get(obj, key);
    if (t == nullptr || !t->is_object()) {
        d.Error("field " + Q(key) + " must be an object keyed by rarity");
        return false;
    }
    const Diag sub = d.Sub("." + std::string(key));
    CheckKeys(sub, *t, {"common", "uncommon", "rare", "epic", "legendary"});
    for (Rarity r : kAllRarities) {
        std::int64_t v = 0;
        if (GetInt(sub, *t, RarityName(r), v, true)) {
            if (v < 0) sub.Error("price for " + Q(RarityName(r)) + " must be >= 0");
            out[RarityIndex(r)] = v;
        }
    }
    return true;
}

bool ReadRatingTable(const Diag& d, const json& obj, std::string_view key,
                     std::array<double, kRatingCount>& out, double lo, double hi) {
    const json* t = Get(obj, key);
    if (t == nullptr || !t->is_object()) {
        d.Error("field " + Q(key) + " must be an object keyed by rating (S, A, B, C)");
        return false;
    }
    const Diag sub = d.Sub("." + std::string(key));
    CheckKeys(sub, *t, {"S", "A", "B", "C"});
    for (Rating r : {Rating::S, Rating::A, Rating::B, Rating::C}) {
        double v = 0.0;
        if (GetNumber(sub, *t, RatingName(r), v, true)) {
            CheckRange(sub, RatingName(r), v, lo, hi);
            out[RatingIndex(r)] = v;
        }
    }
    return true;
}

const json* Section(const Diag& d, const json& obj, std::string_view key) {
    const json* s = Get(obj, key);
    if (s == nullptr || !s->is_object()) {
        d.Error("missing required object " + Q(key));
        return nullptr;
    }
    return s;
}

const json* ListField(const Diag& d, const json& doc, std::string_view key) {
    const json* list = Get(doc, key);
    if (list == nullptr || !list->is_array()) {
        d.Error("missing required array " + Q(key));
        return nullptr;
    }
    return list;
}

std::optional<json> ParseDocument(const Diag& d, const std::string& text) {
    if (text.empty()) {
        d.Error("file is empty or missing");
        return std::nullopt;
    }
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception& e) {
        d.Error(std::string("invalid JSON: ") + e.what());
        return std::nullopt;
    }
    if (!doc.is_object()) {
        d.Error("top level must be a JSON object");
        return std::nullopt;
    }
    std::int64_t version = 0;
    if (GetInt(d, doc, "schema_version", version, true) && version != kContentSchemaVersion) {
        d.Error("schema_version " + std::to_string(version) + " is not supported (expected " +
                std::to_string(kContentSchemaVersion) + ")");
    }
    return doc;
}

// ---- perks -------------------------------------------------------------------------------

struct ParamRule {
    std::string_view name;
    bool required;
    double lo;
    double hi;
    bool lo_open;
};

std::vector<ParamRule> EffectRules(EffectKind kind, ConditionKind cond) {
    std::vector<ParamRule> rules;
    switch (kind) {
        case EffectKind::OutgoingDamageMult:
            rules.push_back({"value", cond != ConditionKind::KillStreak, -0.95, 10.0, false});
            break;
        case EffectKind::IncomingDamageMult: rules.push_back({"value", true, -0.95, 10.0, false}); break;
        case EffectKind::Crit:
            rules.push_back({"chance", true, 0.0, 1.0, true});
            rules.push_back({"multiplier", false, 1.0, 10.0, false});
            break;
        case EffectKind::HealOnKill:
            rules.push_back({"amount_fraction_of_max", true, 0.0, 1.0, true});
            break;
        case EffectKind::SuppliesOnKill:
            rules.push_back({"amount", true, 1.0, 1000.0, false});
            rules.push_back({"chance", false, 0.0, 1.0, true});
            break;
        case EffectKind::SuppliesOnArenaClear: rules.push_back({"amount", true, 0.0, 10000.0, false}); break;
        case EffectKind::CheatDeath:
            rules.push_back({"per_arena", true, 1.0, 10.0, false});
            rules.push_back({"leave_hp_fraction", true, 0.0, 1.0, true});
            break;
    }
    switch (cond) {
        case ConditionKind::TargetHpBelow:
        case ConditionKind::PlayerHpBelow: rules.push_back({"fraction", true, 0.0, 1.0, true}); break;
        case ConditionKind::KillStreak:
            rules.push_back({"window_seconds", true, 0.0, 600.0, true});
            rules.push_back({"per_kill", true, 0.0, 5.0, true});
            rules.push_back({"max_bonus", true, 0.0, 10.0, true});
            break;
        case ConditionKind::AfterMelee: rules.push_back({"window_seconds", true, 0.0, 600.0, true}); break;
        case ConditionKind::Always: break;
    }
    return rules;
}

void ReadParams(const Diag& d, const json& obj, const std::vector<ParamRule>& rules,
                EffectParams& params, std::vector<std::string>& seen) {
    for (const ParamRule& r : rules) {
        double v = 0.0;
        if (GetNumber(d, obj, r.name, v, false)) {
            params.*(*EffectParamField(r.name)) = v;
            seen.emplace_back(r.name);
        }
    }
}

void ValidateParams(const Diag& d, const EffectParams& p, const std::vector<ParamRule>& rules,
                    EffectKind kind) {
    for (const ParamRule& r : rules) {
        const double v = p.*(*EffectParamField(r.name));
        if (r.name == "multiplier" && v == 0.0) continue;  // 0 = economy default crit multiplier
        if (r.name == "chance" && v == 0.0 && kind == EffectKind::SuppliesOnKill) continue;
        if (!r.required && v == 0.0 && r.name != "chance") continue;
        CheckRange(d, r.name, v, r.lo, r.hi, r.lo_open);
    }
}

bool ParseEffect(const Diag& d, const json& obj, EffectDef& effect) {
    if (!obj.is_object()) {
        d.Error("effect must be an object");
        return false;
    }
    if (!ReadEnum(d, obj, "kind", effect.kind, ParseEffectKind,
                  "outgoing_damage_mult, incoming_damage_mult, crit, heal_on_kill, supplies_on_kill, "
                  "supplies_on_arena_clear, cheat_death")) {
        return false;
    }

    const json* cond = Get(obj, "condition");
    const json* cond_obj = nullptr;
    if (cond != nullptr) {
        std::string type;
        if (cond->is_string()) {
            type = cond->get<std::string>();
        } else if (cond->is_object()) {
            cond_obj = cond;
            if (!GetString(d.Sub(".condition"), *cond, "type", type, true)) return false;
        } else {
            d.Error("field 'condition' must be a string or an object with 'type'");
            return false;
        }
        const auto parsed = ParseConditionKind(type);
        if (!parsed) {
            d.Error("unsupported condition " + Q(type) +
                    " (expected one of: always, target_hp_below, kill_streak, player_hp_below, after_melee)");
            return false;
        }
        effect.condition = *parsed;
        const bool allowed =
            effect.condition == ConditionKind::Always ||
            effect.kind == EffectKind::OutgoingDamageMult ||
            (effect.kind == EffectKind::IncomingDamageMult && effect.condition == ConditionKind::PlayerHpBelow);
        if (!allowed) {
            d.Error("condition " + Q(type) + " is not supported for effect kind " +
                    Q(EffectKindName(effect.kind)));
            return false;
        }
    }

    const std::vector<ParamRule> effect_rules = EffectRules(effect.kind, ConditionKind::Always);
    const std::vector<ParamRule> all_rules = EffectRules(effect.kind, effect.condition);
    std::vector<ParamRule> cond_rules;
    for (const ParamRule& r : all_rules) {
        if (std::none_of(effect_rules.begin(), effect_rules.end(),
                         [&r](const ParamRule& e) { return e.name == r.name; })) {
            cond_rules.push_back(r);
        }
    }

    // Allowed keys at the effect level: kind, condition, per_stack, the kind's own params.
    std::vector<std::string_view> keys = {"kind", "condition", "per_stack"};
    if (effect.kind == EffectKind::SuppliesOnArenaClear) keys.push_back("require_no_damage");
    for (const ParamRule& r : effect_rules) keys.push_back(r.name);
    for (const auto& item : obj.items()) {
        const std::string& key = item.key();
        if (!key.empty() && key[0] == '_') continue;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            d.Error("field " + Q(key) + " is not a parameter of " + Q(EffectKindName(effect.kind)) +
                    (cond_rules.empty() ? std::string() : " (condition parameters go inside 'condition')"));
        }
    }
    if (cond_obj != nullptr) {
        std::vector<std::string_view> ckeys = {"type"};
        for (const ParamRule& r : cond_rules) ckeys.push_back(r.name);
        for (const auto& item : cond_obj->items()) {
            const std::string& key = item.key();
            if (!key.empty() && key[0] == '_') continue;
            if (std::find(ckeys.begin(), ckeys.end(), key) == ckeys.end()) {
                d.Error("field " + Q(key) + " is not a parameter of condition " +
                        Q(ConditionKindName(effect.condition)));
            }
        }
    }

    std::vector<std::string> seen;
    ReadParams(d, obj, effect_rules, effect.params, seen);
    if (cond_obj != nullptr) ReadParams(d.Sub(".condition"), *cond_obj, cond_rules, effect.params, seen);
    for (const ParamRule& r : all_rules) {
        if (r.required && std::find(seen.begin(), seen.end(), r.name) == seen.end()) {
            d.Error("missing required parameter " + Q(r.name) + " for " +
                    Q(EffectKindName(effect.kind)) +
                    (effect.condition != ConditionKind::Always
                         ? " with condition " + Q(ConditionKindName(effect.condition))
                         : std::string()));
        }
    }
    if (effect.kind == EffectKind::SuppliesOnKill &&
        std::find(seen.begin(), seen.end(), "chance") == seen.end()) {
        effect.params.chance = 1.0;
    }
    if (effect.kind == EffectKind::CheatDeath && std::floor(effect.params.per_arena) != effect.params.per_arena) {
        d.Error("field 'per_arena' must be a whole number");
    }
    if (effect.kind == EffectKind::SuppliesOnArenaClear) {
        GetBool(d, obj, "require_no_damage", effect.require_no_damage, false);
    }

    if (const json* ps = Get(obj, "per_stack")) {
        if (!ps->is_object()) {
            d.Error("field 'per_stack' must be an object of parameter increments");
        } else {
            for (const auto& item : ps->items()) {
                const std::string& key = item.key();
                const bool known = std::any_of(all_rules.begin(), all_rules.end(),
                                               [&key](const ParamRule& r) { return r.name == key; });
                if (!known) {
                    d.Error("per_stack key " + Q(key) + " is not a parameter of this effect");
                    continue;
                }
                if (!item.value().is_number()) {
                    d.Error("per_stack." + key + " must be a number");
                    continue;
                }
                effect.per_stack.*(*EffectParamField(key)) = item.value().get<double>();
            }
        }
    }
    ValidateParams(d, effect.params, all_rules, effect.kind);
    return true;
}

void ParsePerks(const Diag& file, const json& doc, std::vector<PerkDef>& out) {
    CheckKeys(file, doc, {"schema_version", "perks"});
    const json* list = ListField(file, doc, "perks");
    if (list == nullptr) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& e = (*list)[i];
        Diag d = file.Sub(": perks[" + std::to_string(i) + "]");
        if (!e.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        PerkDef p;
        if (!ReadId(d, e, p.id)) continue;
        d = file.Sub(": perk " + Q(p.id));
        CheckKeys(d, e, {"id", "name", "description", "category", "rarity", "tags", "max_stacks",
                         "exclusive_with", "characters", "unlock_cost_credits",
                         "requires_capabilities", "enabled", "effects"});
        GetString(d, e, "name", p.name, true);
        GetString(d, e, "description", p.description, false);
        ReadEnum(d, e, "category", p.category, ParsePerkCategory,
                 "offensive, defensive, precision, economy, utility, cooperative, curse");
        ReadEnum(d, e, "rarity", p.rarity, ParseRarity, "common, uncommon, rare, epic, legendary");
        GetStringList(d, e, "tags", p.tags, false);
        GetIntRange(d, e, "max_stacks", p.max_stacks, false, 1, 99);
        GetStringList(d, e, "exclusive_with", p.exclusive_with, false);
        GetStringList(d, e, "characters", p.characters, false);
        if (GetInt(d, e, "unlock_cost_credits", p.unlock_cost_credits, false) && p.unlock_cost_credits < 0) {
            d.Error("field 'unlock_cost_credits' must be >= 0");
        }
        ReadCapabilities(d, e, p.requires_capabilities);
        GetBool(d, e, "enabled", p.enabled, false);

        const json* effects = Get(e, "effects");
        if (effects == nullptr || !effects->is_array() || effects->empty()) {
            d.Error("field 'effects' must be a non-empty array");
        } else {
            for (std::size_t j = 0; j < effects->size(); ++j) {
                const Diag ed = d.Sub(": effect[" + std::to_string(j) + "]");
                EffectDef effect;
                if (!ParseEffect(ed, (*effects)[j], effect)) continue;
                if (p.max_stacks > 1) {
                    // Scaled parameters at max stacks must stay within the same ranges.
                    const EffectParams at_max = effect.AtStacks(p.max_stacks);
                    ValidateParams(ed.Sub(" at max_stacks=" + std::to_string(p.max_stacks)), at_max,
                                   EffectRules(effect.kind, effect.condition), effect.kind);
                }
                MergeCapabilities(p.requires_capabilities, ImpliedCapabilities(effect));
                p.effects.push_back(std::move(effect));
            }
        }
        if (p.category == PerkCategory::Cooperative && p.enabled) {
            d.Warn("cooperative perks are reserved; consider \"enabled\": false until co-op exists");
        }
        std::sort(p.requires_capabilities.begin(), p.requires_capabilities.end());
        p.requires_capabilities.erase(
            std::unique(p.requires_capabilities.begin(), p.requires_capabilities.end()),
            p.requires_capabilities.end());
        out.push_back(std::move(p));
    }
}

// ---- weapons -----------------------------------------------------------------------------

std::vector<std::string> AffixImpliedCaps(AffixKind kind) {
    switch (kind) {
        case AffixKind::SuppliesOnKill: return {std::string(cap::kKillEvents)};
        case AffixKind::LowHpTargetDamagePct:
        case AffixKind::UniqueLowHpTargetBonus:
            return {std::string(cap::kOutgoingDamage), std::string(cap::kEnemyHp)};
        case AffixKind::UniqueKillStreakExtend:
            return {std::string(cap::kOutgoingDamage), std::string(cap::kKillEvents)};
        default: return {std::string(cap::kOutgoingDamage)};
    }
}

void ParseWeapons(const Diag& file, const json& doc, std::vector<WeaponBaseDef>& weapons,
                  std::vector<AffixDef>& affixes, std::vector<UniqueAffixDef>& uniques) {
    CheckKeys(file, doc, {"schema_version", "weapons", "affixes", "unique_affixes"});
    if (const json* list = ListField(file, doc, "weapons")) {
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json& e = (*list)[i];
            Diag d = file.Sub(": weapons[" + std::to_string(i) + "]");
            if (!e.is_object()) {
                d.Error("entry must be an object");
                continue;
            }
            WeaponBaseDef w;
            if (!ReadId(d, e, w.id)) continue;
            d = file.Sub(": weapon " + Q(w.id));
            CheckKeys(d, e, {"id", "name", "category", "base_damage_mult", "unlock_cost_credits", "enabled"});
            GetString(d, e, "name", w.name, true);
            ReadEnum(d, e, "category", w.category, ParseWeaponCategory,
                     "handgun, shotgun, smg, rifle, machine_gun, magnum");
            if (GetNumber(d, e, "base_damage_mult", w.base_damage_mult, false)) {
                CheckRange(d, "base_damage_mult", w.base_damage_mult, 0.1, 10.0);
            }
            if (GetInt(d, e, "unlock_cost_credits", w.unlock_cost_credits, false) && w.unlock_cost_credits < 0) {
                d.Error("field 'unlock_cost_credits' must be >= 0");
            }
            GetBool(d, e, "enabled", w.enabled, false);
            weapons.push_back(std::move(w));
        }
    }
    if (const json* list = ListField(file, doc, "affixes")) {
        std::set<AffixKind> kinds;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json& e = (*list)[i];
            const Diag d = file.Sub(": affixes[" + std::to_string(i) + "]");
            AffixDef a;
            if (!ReadEnum(d, e, "kind", a.kind, ParseAffixKind,
                          "damage_pct, crit_chance, crit_mult_pct, low_hp_target_damage_pct, supplies_on_kill")) {
                continue;
            }
            CheckKeys(d, e, {"kind", "min", "max", "threshold", "requires_capabilities"});
            if (!kinds.insert(a.kind).second) d.Error("duplicate affix kind " + Q(AffixKindName(a.kind)));
            GetNumber(d, e, "min", a.min, true);
            GetNumber(d, e, "max", a.max, true);
            if (a.min < 0.0 || a.max < a.min) d.Error("affix range must satisfy 0 <= min <= max");
            if (a.kind == AffixKind::CritChance) CheckRange(d, "max", a.max, 0.0, 1.0);
            if (a.kind == AffixKind::LowHpTargetDamagePct) {
                if (GetNumber(d, e, "threshold", a.threshold, true)) {
                    CheckRange(d, "threshold", a.threshold, 0.0, 1.0, true);
                }
            } else if (Get(e, "threshold") != nullptr) {
                d.Error("field 'threshold' only applies to low_hp_target_damage_pct");
            }
            ReadCapabilities(d, e, a.requires_capabilities);
            MergeCapabilities(a.requires_capabilities, AffixImpliedCaps(a.kind));
            affixes.push_back(std::move(a));
        }
    }
    if (const json* list = ListField(file, doc, "unique_affixes")) {
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json& e = (*list)[i];
            Diag d = file.Sub(": unique_affixes[" + std::to_string(i) + "]");
            UniqueAffixDef u;
            if (!ReadId(d, e, u.id)) continue;
            d = file.Sub(": unique affix " + Q(u.id));
            CheckKeys(d, e, {"id", "name", "description", "effect", "value", "threshold", "seconds",
                             "requires_capabilities"});
            GetString(d, e, "name", u.name, true);
            GetString(d, e, "description", u.description, false);
            if (!ReadEnum(d, e, "effect", u.kind, ParseUniqueEffect, "low_hp_target_bonus, kill_streak_extend")) {
                continue;
            }
            if (u.kind == AffixKind::UniqueLowHpTargetBonus) {
                if (GetNumber(d, e, "value", u.value, true)) CheckRange(d, "value", u.value, 0.0, 10.0, true);
                if (GetNumber(d, e, "threshold", u.threshold, true)) {
                    CheckRange(d, "threshold", u.threshold, 0.0, 1.0, true);
                }
                if (Get(e, "seconds") != nullptr) d.Error("field 'seconds' only applies to kill_streak_extend");
            } else {
                if (GetNumber(d, e, "seconds", u.value, true)) CheckRange(d, "seconds", u.value, 0.0, 60.0, true);
                if (Get(e, "value") != nullptr || Get(e, "threshold") != nullptr) {
                    d.Error("kill_streak_extend takes only 'seconds'");
                }
            }
            ReadCapabilities(d, e, u.requires_capabilities);
            MergeCapabilities(u.requires_capabilities, AffixImpliedCaps(u.kind));
            uniques.push_back(std::move(u));
        }
    }
}

// ---- characters / routes -------------------------------------------------------------------

void ParseCharacters(const Diag& file, const json& doc, std::vector<CharacterDef>& out) {
    CheckKeys(file, doc, {"schema_version", "characters"});
    const json* list = ListField(file, doc, "characters");
    if (list == nullptr) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& e = (*list)[i];
        Diag d = file.Sub(": characters[" + std::to_string(i) + "]");
        if (!e.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        CharacterDef c;
        if (!ReadId(d, e, c.id)) continue;
        d = file.Sub(": character " + Q(c.id));
        CheckKeys(d, e, {"id", "name", "enabled", "starting_weapons", "starting_passive", "skill_pool",
                         "exclusive_mechanic", "unlock_cost_credits", "unlock_kind", "unlock_count",
                         "unlock_character", "unlock_text"});
        GetString(d, e, "name", c.name, true);
        GetBool(d, e, "enabled", c.enabled, false);
        GetStringList(d, e, "starting_weapons", c.starting_weapons, true);
        GetString(d, e, "starting_passive", c.starting_passive, false);
        GetStringList(d, e, "skill_pool", c.skill_pool, false);
        GetString(d, e, "exclusive_mechanic", c.exclusive_mechanic, false);
        if (GetInt(d, e, "unlock_cost_credits", c.unlock_cost_credits, false) && c.unlock_cost_credits < 0) {
            d.Error("field 'unlock_cost_credits' must be >= 0");
        }
        std::string kind;
        GetString(d, e, "unlock_kind", kind, false);
        if (!kind.empty()) {
            const auto parsed = ParseChallengeKind(kind);
            if (parsed) {
                c.unlock_kind = *parsed;
            } else {
                d.Error("unknown unlock_kind " + Q(kind));
            }
        }
        if (GetInt(d, e, "unlock_count", c.unlock_count, false) && c.unlock_count < 0) {
            d.Error("field 'unlock_count' must be >= 0");
        }
        GetString(d, e, "unlock_character", c.unlock_character, false);
        GetString(d, e, "unlock_text", c.unlock_text, false);
        if (c.unlock_kind != ChallengeKind::None && c.unlock_count < 1) {
            d.Error("unlock_count must be >= 1 with an unlock_kind");
        }
        if (c.unlock_kind == ChallengeKind::RunsCompletedAs && c.unlock_character.empty()) {
            d.Error("unlock_kind runs_completed_as needs unlock_character");
        }
        if (c.starting_weapons.empty()) d.Error("starting_weapons must list at least one weapon");
        out.push_back(std::move(c));
    }
    for (const CharacterDef& c : out) {
        if (c.unlock_kind != ChallengeKind::RunsCompletedAs) continue;
        const bool known = std::any_of(out.begin(), out.end(),
                                       [&](const CharacterDef& other) { return other.id == c.unlock_character; });
        if (!known) file.Sub(": character " + Q(c.id)).Error("unlock_character " + Q(c.unlock_character) + " is not a character");
    }
}

void ParseRoutes(const Diag& file, const json& doc, std::vector<RouteDef>& out) {
    CheckKeys(file, doc, {"schema_version", "routes"});
    const json* list = ListField(file, doc, "routes");
    if (list == nullptr) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& e = (*list)[i];
        Diag d = file.Sub(": routes[" + std::to_string(i) + "]");
        if (!e.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        RouteDef r;
        if (!ReadId(d, e, r.id)) continue;
        d = file.Sub(": route " + Q(r.id));
        CheckKeys(d, e, {"id", "name", "description", "enabled", "difficulty_tier", "enemy_budget",
                         "reward_mult", "rarity_bonus", "elite_chance", "modifiers", "credit_mult",
                         "curse_chance"});
        GetString(d, e, "name", r.name, true);
        GetString(d, e, "description", r.description, false);
        GetBool(d, e, "enabled", r.enabled, false);
        GetIntRange(d, e, "difficulty_tier", r.difficulty_tier, true, 1, 3);
        GetIntRange(d, e, "enemy_budget", r.enemy_budget, true, 1, 1000);
        if (GetNumber(d, e, "reward_mult", r.reward_mult, true)) CheckRange(d, "reward_mult", r.reward_mult, 0.0, 10.0, true);
        if (GetNumber(d, e, "rarity_bonus", r.rarity_bonus, false)) CheckRange(d, "rarity_bonus", r.rarity_bonus, -0.9, 3.0);
        if (GetNumber(d, e, "elite_chance", r.elite_chance, false) && r.elite_chance != 0.0) {
            d.Error("field 'elite_chance' is reserved and must be 0 until an adapter supports elites");
        }
        if (GetNumber(d, e, "credit_mult", r.credit_mult, true)) CheckRange(d, "credit_mult", r.credit_mult, 0.0, 10.0, true);
        if (GetNumber(d, e, "curse_chance", r.curse_chance, false)) CheckRange(d, "curse_chance", r.curse_chance, 0.0, 1.0);
        if (const json* m = Section(d, e, "modifiers")) {
            const Diag md = d.Sub(".modifiers");
            CheckKeys(md, *m, {"incoming_damage_mult", "outgoing_damage_mult", "par_time_seconds"});
            if (GetNumber(md, *m, "incoming_damage_mult", r.modifiers.incoming_damage_mult, true)) {
                CheckRange(md, "incoming_damage_mult", r.modifiers.incoming_damage_mult, 0.0, 10.0, true);
            }
            if (GetNumber(md, *m, "outgoing_damage_mult", r.modifiers.outgoing_damage_mult, true)) {
                CheckRange(md, "outgoing_damage_mult", r.modifiers.outgoing_damage_mult, 0.0, 10.0, true);
            }
            if (GetNumber(md, *m, "par_time_seconds", r.modifiers.par_time_seconds, true)) {
                CheckRange(md, "par_time_seconds", r.modifiers.par_time_seconds, 0.0, 3600.0, true);
            }
        }
        out.push_back(std::move(r));
    }
}

// ---- economy -------------------------------------------------------------------------------

void ParseEconomy(const Diag& file, const json& doc, EconomyConfig& eco) {
    const Diag d = file.Sub("");
    CheckKeys(d, doc, {"schema_version", "run", "rewards", "weapon_rarity", "upgrades", "supplies",
                       "credits", "rating", "damage", "shop", "routes"});
    if (const json* s = Section(d, doc, "run")) {
        const Diag sd = d.Sub(": run");
        CheckKeys(sd, *s, {"arena_count", "starting_route", "starting_supplies",
                           "non_player_kills_count_toward_budget", "max_reroll_tokens"});
        GetIntRange(sd, *s, "arena_count", eco.run.arena_count, true, 1, 20);
        GetString(sd, *s, "starting_route", eco.run.starting_route, true);
        if (GetInt(sd, *s, "starting_supplies", eco.run.starting_supplies, true) && eco.run.starting_supplies < 0) {
            sd.Error("field 'starting_supplies' must be >= 0");
        }
        GetBool(sd, *s, "non_player_kills_count_toward_budget", eco.run.non_player_kills_count_toward_budget, false);
        GetIntRange(sd, *s, "max_reroll_tokens", eco.run.max_reroll_tokens, true, 0, 10);
    }
    if (const json* s = Section(d, doc, "rewards")) {
        const Diag sd = d.Sub(": rewards");
        CheckKeys(sd, *s, {"choices", "rarity_weights", "rating_rarity_bonus", "kind_weights", "supplies_bundle"});
        GetIntRange(sd, *s, "choices", eco.rewards.choices, true, 2, 5);
        ReadRarityTable(sd, *s, "rarity_weights", eco.rewards.rarity_weights);
        double total = 0.0;
        for (double w : eco.rewards.rarity_weights) total += w;
        if (!(total > 0.0)) sd.Error("rarity_weights must have a positive total");
        ReadRatingTable(sd, *s, "rating_rarity_bonus", eco.rewards.rating_rarity_bonus, -0.9, 3.0);
        if (const json* k = Section(sd, *s, "kind_weights")) {
            const Diag kd = sd.Sub(".kind_weights");
            CheckKeys(kd, *k, {"perk", "weapon", "supplies"});
            if (GetNumber(kd, *k, "perk", eco.rewards.perk_weight, true)) CheckRange(kd, "perk", eco.rewards.perk_weight, 0.0, 1.0e6);
            if (GetNumber(kd, *k, "weapon", eco.rewards.weapon_weight, true)) CheckRange(kd, "weapon", eco.rewards.weapon_weight, 0.0, 1.0e6);
            if (GetNumber(kd, *k, "supplies", eco.rewards.supplies_weight, true)) CheckRange(kd, "supplies", eco.rewards.supplies_weight, 0.0, 1.0e6);
            if (!(eco.rewards.perk_weight + eco.rewards.weapon_weight + eco.rewards.supplies_weight > 0.0)) {
                kd.Error("kind weights must have a positive total");
            }
        }
        if (GetInt(sd, *s, "supplies_bundle", eco.rewards.supplies_bundle, true) && eco.rewards.supplies_bundle < 1) {
            sd.Error("field 'supplies_bundle' must be >= 1");
        }
    }
    if (const json* s = Section(d, doc, "weapon_rarity")) {
        const Diag sd = d.Sub(": weapon_rarity");
        CheckKeys(sd, *s, {"common", "uncommon", "rare", "epic", "legendary"});
        for (Rarity r : kAllRarities) {
            const json* t = Section(sd, *s, RarityName(r));
            if (t == nullptr) continue;
            const Diag td = sd.Sub("." + std::string(RarityName(r)));
            CheckKeys(td, *t, {"affix_count", "magnitude_mult", "unique_count"});
            EconomyConfig::WeaponRarityRule& rule = eco.weapon_rarity[RarityIndex(r)];
            GetIntRange(td, *t, "affix_count", rule.affix_count, true, 0, 5);
            if (GetNumber(td, *t, "magnitude_mult", rule.magnitude_mult, true)) {
                CheckRange(td, "magnitude_mult", rule.magnitude_mult, 0.0, 10.0, true);
            }
            GetIntRange(td, *t, "unique_count", rule.unique_count, false, 0, 3);
        }
    }
    if (const json* s = Section(d, doc, "upgrades")) {
        const Diag sd = d.Sub(": upgrades");
        CheckKeys(sd, *s, {"max_level", "cost_to_next", "damage_pct_per_level"});
        GetIntRange(sd, *s, "max_level", eco.upgrades.max_level, true, 1, 10);
        const json* costs = Get(*s, "cost_to_next");
        if (costs == nullptr || !costs->is_array()) {
            sd.Error("field 'cost_to_next' must be an array of integers");
        } else {
            eco.upgrades.cost_to_next.clear();
            for (const json& c : *costs) {
                if (!c.is_number_integer() || c.get<std::int64_t>() < 1) {
                    sd.Error("cost_to_next entries must be integers >= 1");
                    break;
                }
                eco.upgrades.cost_to_next.push_back(c.get<std::int64_t>());
            }
            if (eco.upgrades.cost_to_next.size() != static_cast<std::size_t>(eco.upgrades.max_level - 1)) {
                sd.Error("cost_to_next must have max_level - 1 = " + std::to_string(eco.upgrades.max_level - 1) +
                         " entries (has " + std::to_string(eco.upgrades.cost_to_next.size()) + ")");
            }
        }
        if (GetNumber(sd, *s, "damage_pct_per_level", eco.upgrades.damage_pct_per_level, true)) {
            CheckRange(sd, "damage_pct_per_level", eco.upgrades.damage_pct_per_level, 0.0, 100.0);
        }
    }
    if (const json* s = Section(d, doc, "supplies")) {
        const Diag sd = d.Sub(": supplies");
        CheckKeys(sd, *s, {"per_kill", "per_arena_clear"});
        if (GetInt(sd, *s, "per_kill", eco.supplies.per_kill, true) && eco.supplies.per_kill < 0) sd.Error("per_kill must be >= 0");
        if (GetInt(sd, *s, "per_arena_clear", eco.supplies.per_arena_clear, true) && eco.supplies.per_arena_clear < 0) {
            sd.Error("per_arena_clear must be >= 0");
        }
    }
    if (const json* s = Section(d, doc, "credits")) {
        const Diag sd = d.Sub(": credits");
        CheckKeys(sd, *s, {"per_kill", "per_arena_clear", "run_complete_bonus", "death_rate", "rating_mult"});
        if (GetInt(sd, *s, "per_kill", eco.credits.per_kill, true) && eco.credits.per_kill < 0) sd.Error("per_kill must be >= 0");
        if (GetInt(sd, *s, "per_arena_clear", eco.credits.per_arena_clear, true) && eco.credits.per_arena_clear < 0) {
            sd.Error("per_arena_clear must be >= 0");
        }
        if (GetInt(sd, *s, "run_complete_bonus", eco.credits.run_complete_bonus, true) && eco.credits.run_complete_bonus < 0) {
            sd.Error("run_complete_bonus must be >= 0");
        }
        if (GetNumber(sd, *s, "death_rate", eco.credits.death_rate, true)) CheckRange(sd, "death_rate", eco.credits.death_rate, 0.0, 1.0);
        ReadRatingTable(sd, *s, "rating_mult", eco.credits.rating_mult, 0.0, 10.0);
    }
    if (const json* s = Section(d, doc, "rating")) {
        const Diag sd = d.Sub(": rating");
        CheckKeys(sd, *s, {"s_time_fraction", "a_time_fraction", "low_damage_fraction"});
        if (GetNumber(sd, *s, "s_time_fraction", eco.rating.s_time_fraction, true)) {
            CheckRange(sd, "s_time_fraction", eco.rating.s_time_fraction, 0.0, 10.0, true);
        }
        if (GetNumber(sd, *s, "a_time_fraction", eco.rating.a_time_fraction, true)) {
            CheckRange(sd, "a_time_fraction", eco.rating.a_time_fraction, eco.rating.s_time_fraction, 10.0);
        }
        if (GetNumber(sd, *s, "low_damage_fraction", eco.rating.low_damage_fraction, true)) {
            CheckRange(sd, "low_damage_fraction", eco.rating.low_damage_fraction, 0.0, 10.0, true);
        }
    }
    if (const json* s = Section(d, doc, "damage")) {
        const Diag sd = d.Sub(": damage");
        auto& dm = eco.damage;
        CheckKeys(sd, *s, {"outgoing_min", "outgoing_max", "incoming_min", "incoming_max",
                           "default_crit_multiplier", "max_crit_chance"});
        GetNumber(sd, *s, "outgoing_min", dm.outgoing_min, true);
        GetNumber(sd, *s, "outgoing_max", dm.outgoing_max, true);
        GetNumber(sd, *s, "incoming_min", dm.incoming_min, true);
        GetNumber(sd, *s, "incoming_max", dm.incoming_max, true);
        if (!(dm.outgoing_min > 0.0 && dm.outgoing_min < dm.outgoing_max && dm.outgoing_max <= 100.0)) {
            sd.Error("outgoing clamp must satisfy 0 < outgoing_min < outgoing_max <= 100");
        }
        if (!(dm.incoming_min > 0.0 && dm.incoming_min < dm.incoming_max && dm.incoming_max <= 100.0)) {
            sd.Error("incoming clamp must satisfy 0 < incoming_min < incoming_max <= 100");
        }
        if (GetNumber(sd, *s, "default_crit_multiplier", dm.default_crit_multiplier, true)) {
            CheckRange(sd, "default_crit_multiplier", dm.default_crit_multiplier, 1.0, 10.0);
        }
        if (GetNumber(sd, *s, "max_crit_chance", dm.max_crit_chance, true)) {
            CheckRange(sd, "max_crit_chance", dm.max_crit_chance, 0.0, 1.0);
        }
    }
    if (const json* s = Section(d, doc, "shop")) {
        const Diag sd = d.Sub(": shop");
        CheckKeys(sd, *s, {"heal_fraction", "heal_price", "reroll_price", "perk_price", "weapon_price", "perk_offers"});
        if (GetNumber(sd, *s, "heal_fraction", eco.shop.heal_fraction, true)) {
            CheckRange(sd, "heal_fraction", eco.shop.heal_fraction, 0.0, 1.0, true);
        }
        if (GetInt(sd, *s, "heal_price", eco.shop.heal_price, true) && eco.shop.heal_price < 0) sd.Error("heal_price must be >= 0");
        if (GetInt(sd, *s, "reroll_price", eco.shop.reroll_price, true) && eco.shop.reroll_price < 0) sd.Error("reroll_price must be >= 0");
        ReadRarityPrices(sd, *s, "perk_price", eco.shop.perk_price);
        ReadRarityPrices(sd, *s, "weapon_price", eco.shop.weapon_price);
        GetIntRange(sd, *s, "perk_offers", eco.shop.perk_offers, true, 0, 6);
    }
    if (const json* s = Section(d, doc, "routes")) {
        const Diag sd = d.Sub(": routes");
        CheckKeys(sd, *s, {"third_choice_chance"});
        if (GetNumber(sd, *s, "third_choice_chance", eco.routes.third_choice_chance, true)) {
            CheckRange(sd, "third_choice_chance", eco.routes.third_choice_chance, 0.0, 1.0);
        }
    }
}

template <class T>
void IndexById(const Diag& d, const std::vector<T>& items, StringMap<std::size_t>& index,
               std::string_view what) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!index.emplace(items[i].id, i).second) d.Error("duplicate " + std::string(what) + " id " + Q(items[i].id));
    }
}

bool ReadFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

/// Builds a Content from parsed definitions and runs the cross-file checks.
class ContentBuilder {
public:
    static ContentLoadResult Build(const ContentSources& src) {
        ContentLoadResult result;
        auto content = std::make_shared<Content>();
        std::vector<std::string>& errors = result.errors;
        std::vector<std::string>& warnings = result.warnings;
        auto file = [&](const char* name) { return Diag{&errors, &warnings, name}; };

        if (auto doc = ParseDocument(file("perks.json"), src.perks)) ParsePerks(file("perks.json"), *doc, content->perks_);
        if (auto doc = ParseDocument(file("weapons.json"), src.weapons)) {
            ParseWeapons(file("weapons.json"), *doc, content->weapons_, content->affixes_, content->uniques_);
        }
        if (auto doc = ParseDocument(file("characters.json"), src.characters)) {
            ParseCharacters(file("characters.json"), *doc, content->characters_);
        }
        if (auto doc = ParseDocument(file("routes.json"), src.routes)) ParseRoutes(file("routes.json"), *doc, content->routes_);
        if (auto doc = ParseDocument(file("economy.json"), src.economy)) ParseEconomy(file("economy.json"), *doc, content->economy_);

        IndexById(file("perks.json"), content->perks_, content->perk_index_, "perk");
        IndexById(file("weapons.json"), content->weapons_, content->weapon_index_, "weapon");
        IndexById(file("weapons.json"), content->uniques_, content->unique_index_, "unique affix");
        IndexById(file("characters.json"), content->characters_, content->character_index_, "character");
        IndexById(file("routes.json"), content->routes_, content->route_index_, "route");

        CrossCheck(*content, errors, warnings);

        if (errors.empty()) result.content = std::move(content);
        return result;
    }

private:
    static void CrossCheck(Content& c, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
        const Diag perks_d{&errors, &warnings, "perks.json"};
        const Diag chars_d{&errors, &warnings, "characters.json"};
        const Diag routes_d{&errors, &warnings, "routes.json"};
        const Diag eco_d{&errors, &warnings, "economy.json"};
        const Diag weapons_d{&errors, &warnings, "weapons.json"};

        // Perk references; normalise exclusions to be symmetric.
        for (PerkDef& p : c.perks_) {
            const Diag d = perks_d.Sub(": perk " + Q(p.id));
            for (const std::string& ex : p.exclusive_with) {
                if (ex == p.id) {
                    d.Error("excludes itself, so it can never be picked");
                } else if (c.FindPerk(ex) == nullptr) {
                    d.Error("exclusive_with references unknown perk " + Q(ex));
                }
            }
            for (const std::string& ch : p.characters) {
                if (c.FindCharacter(ch) == nullptr) d.Error("characters references unknown character " + Q(ch));
            }
        }
        for (PerkDef& p : c.perks_) {
            for (const std::string& ex : p.exclusive_with) {
                if (ex == p.id) continue;
                const auto it = c.perk_index_.find(ex);
                if (it == c.perk_index_.end()) continue;
                PerkDef& other = c.perks_[it->second];
                if (std::find(other.exclusive_with.begin(), other.exclusive_with.end(), p.id) ==
                    other.exclusive_with.end()) {
                    other.exclusive_with.push_back(p.id);
                }
            }
        }

        // Characters.
        bool any_free = false;
        for (const CharacterDef& ch : c.characters_) {
            const Diag d = chars_d.Sub(": character " + Q(ch.id));
            for (const std::string& w : ch.starting_weapons) {
                const WeaponBaseDef* def = c.FindWeapon(w);
                if (def == nullptr) {
                    d.Error("starting_weapons references unknown weapon " + Q(w));
                } else if (!def->enabled && ch.enabled) {
                    d.Error("starting weapon " + Q(w) + " is disabled");
                }
            }
            const PerkDef* passive = nullptr;
            if (!ch.starting_passive.empty()) {
                passive = c.FindPerk(ch.starting_passive);
                if (passive == nullptr) {
                    d.Error("starting_passive references unknown perk " + Q(ch.starting_passive));
                } else if (!CharacterMayUsePerk(*passive, ch)) {
                    d.Error("starting_passive " + Q(ch.starting_passive) + " is restricted to other characters");
                } else if (passive->IsCurse()) {
                    d.Error("starting_passive " + Q(ch.starting_passive) + " is a curse");
                }
            }
            for (const std::string& s : ch.skill_pool) {
                const PerkDef* p = c.FindPerk(s);
                if (p == nullptr) {
                    d.Error("skill_pool references unknown perk " + Q(s));
                } else if (passive != nullptr &&
                           std::find(passive->exclusive_with.begin(), passive->exclusive_with.end(), s) !=
                               passive->exclusive_with.end()) {
                    d.Error("skill_pool perk " + Q(s) + " is excluded by the starting passive " +
                            Q(passive->id) + ", so it can never be picked");
                }
            }
            if (ch.enabled && ch.unlock_cost_credits == 0) any_free = true;
        }
        if (!c.characters_.empty() && !any_free) chars_d.Error("no enabled character is unlocked by default");
        if (c.characters_.empty()) chars_d.Error("at least one character is required");

        // Character-restricted perks that every allowed character's passive excludes are unpickable.
        for (const PerkDef& p : c.perks_) {
            if (!p.enabled || p.IsCurse() || p.characters.empty()) continue;
            bool pickable = false;
            bool any_character = false;
            for (const CharacterDef& ch : c.characters_) {
                if (!ch.enabled || !CharacterMayUsePerk(p, ch)) continue;
                any_character = true;
                const PerkDef* passive = c.FindPerk(ch.starting_passive);
                const bool excluded = passive != nullptr && passive->id != p.id &&
                                      std::find(passive->exclusive_with.begin(), passive->exclusive_with.end(),
                                                p.id) != passive->exclusive_with.end();
                if (!excluded) pickable = true;
            }
            if (any_character && !pickable) {
                perks_d.Sub(": perk " + Q(p.id))
                    .Error("every character allowed to take it starts with an excluding passive, so it can never be picked");
            } else if (!any_character) {
                perks_d.Sub(": perk " + Q(p.id)).Warn("restricted to characters that are all disabled");
            }
        }

        // Routes and economy references.
        std::set<int> tiers;
        for (const RouteDef& r : c.routes_) {
            if (r.enabled) tiers.insert(r.difficulty_tier);
        }
        if (tiers.size() < 2) {
            routes_d.Error("enabled routes must cover at least two difficulty tiers (route choice needs a safe and a harder option)");
        }
        if (!c.economy_.run.starting_route.empty()) {
            const RouteDef* start = c.FindRoute(c.economy_.run.starting_route);
            if (start == nullptr) {
                eco_d.Error("run.starting_route references unknown route " + Q(c.economy_.run.starting_route));
            } else if (!start->enabled) {
                eco_d.Error("run.starting_route " + Q(start->id) + " is disabled");
            }
        }
        if (c.weapons_.empty()) weapons_d.Error("at least one weapon is required");
        if (c.affixes_.empty()) weapons_d.Warn("no regular affixes defined; weapons above common rarity will be plain");
        const int affix_kinds = static_cast<int>(c.affixes_.size());
        for (Rarity r : kAllRarities) {
            const auto& rule = c.economy_.weapon_rarity[RarityIndex(r)];
            if (rule.affix_count > affix_kinds) {
                eco_d.Warn("weapon_rarity." + std::string(RarityName(r)) + ".affix_count exceeds the " +
                           std::to_string(affix_kinds) + " affix kinds defined; extra slots stay empty");
            }
            if (rule.unique_count > static_cast<int>(c.uniques_.size())) {
                eco_d.Warn("weapon_rarity." + std::string(RarityName(r)) + ".unique_count exceeds defined unique affixes");
            }
        }
        if (std::none_of(c.perks_.begin(), c.perks_.end(), [](const PerkDef& p) { return p.enabled && !p.IsCurse(); })) {
            perks_d.Error("at least one enabled, non-curse perk is required");
        }
    }
};

ContentLoadResult Content::LoadFromStrings(const ContentSources& sources) {
    return ContentBuilder::Build(sources);
}

ContentLoadResult Content::LoadFromDirectory(const std::filesystem::path& directory) {
    ContentSources src;
    std::vector<std::string> io_errors;
    const std::pair<const char*, std::string*> files[] = {
        {"perks.json", &src.perks},           {"weapons.json", &src.weapons},
        {"characters.json", &src.characters}, {"routes.json", &src.routes},
        {"economy.json", &src.economy},
    };
    for (const auto& [name, target] : files) {
        const std::filesystem::path p = directory / name;
        if (!ReadFile(p, *target)) io_errors.push_back(std::string(name) + ": cannot read " + p.string());
    }
    ContentLoadResult result = ContentBuilder::Build(src);
    if (!io_errors.empty()) {
        result.errors.insert(result.errors.begin(), io_errors.begin(), io_errors.end());
        result.content.reset();
    }
    return result;
}

const PerkDef* Content::FindPerk(std::string_view id) const {
    const auto it = perk_index_.find(id);
    return it == perk_index_.end() ? nullptr : &perks_[it->second];
}

const WeaponBaseDef* Content::FindWeapon(std::string_view id) const {
    const auto it = weapon_index_.find(id);
    return it == weapon_index_.end() ? nullptr : &weapons_[it->second];
}

const CharacterDef* Content::FindCharacter(std::string_view id) const {
    const auto it = character_index_.find(id);
    return it == character_index_.end() ? nullptr : &characters_[it->second];
}

const RouteDef* Content::FindRoute(std::string_view id) const {
    const auto it = route_index_.find(id);
    return it == route_index_.end() ? nullptr : &routes_[it->second];
}

const UniqueAffixDef* Content::FindUnique(std::string_view id) const {
    const auto it = unique_index_.find(id);
    return it == unique_index_.end() ? nullptr : &uniques_[it->second];
}

int Content::MaxRouteTier() const {
    int tier = 0;
    for (const RouteDef& r : routes_) {
        if (r.enabled) tier = std::max(tier, r.difficulty_tier);
    }
    return tier;
}

}  // namespace rm
