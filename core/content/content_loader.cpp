#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "rm/content.h"
#include "rm/version.h"
#include "rm/common.h"

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
    CheckKeys(sub, *t, {"common", "uncommon", "rare", "epic", "legendary", "exotic"});
    for (Rarity r : kAllRarities) {
        double v = 0.0;
        if (GetNumber(sub, *t, RarityName(r), v, r != Rarity::Exotic)) {  // exotic: 0 unless given
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
    CheckKeys(sub, *t, {"common", "uncommon", "rare", "epic", "legendary", "exotic"});
    for (Rarity r : kAllRarities) {
        std::int64_t v = 0;
        if (GetInt(sub, *t, RarityName(r), v, r != Rarity::Exotic)) {
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
        case EffectKind::ItemOnKill:
            rules.push_back({"game_item", true, 0.0, 65535.0, false});
            rules.push_back({"amount", true, 1.0, 99.0, false});
            rules.push_back({"every_kills", false, 1.0, 100.0, false});
            rules.push_back({"chance", false, 0.0, 1.0, true});
            break;
        case EffectKind::ItemOnArenaClear:
        case EffectKind::ItemOnRunStart:
            rules.push_back({"game_item", true, 0.0, 65535.0, false});
            rules.push_back({"amount", true, 1.0, 99.0, false});
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
        if (r.name == "chance" && v == 0.0 &&
            (kind == EffectKind::SuppliesOnKill || kind == EffectKind::ItemOnKill)) {
            continue;
        }
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
    if (effect.kind == EffectKind::SuppliesOnKill || effect.kind == EffectKind::ItemOnKill) keys.push_back("melee_only");
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
    if ((effect.kind == EffectKind::SuppliesOnKill || effect.kind == EffectKind::ItemOnKill) &&
        std::find(seen.begin(), seen.end(), "chance") == seen.end()) {
        effect.params.chance = 1.0;
    }
    if (effect.kind == EffectKind::ItemOnKill && std::floor(effect.params.every_kills) != effect.params.every_kills) {
        d.Error("field 'every_kills' must be a whole number");
    }
    if ((effect.kind == EffectKind::ItemOnKill || effect.kind == EffectKind::ItemOnArenaClear ||
         effect.kind == EffectKind::ItemOnRunStart) &&
        std::floor(effect.params.game_item) != effect.params.game_item) {
        d.Error("field 'game_item' must be a whole number");
    }
    if (effect.kind == EffectKind::CheatDeath && std::floor(effect.params.per_arena) != effect.params.per_arena) {
        d.Error("field 'per_arena' must be a whole number");
    }
    if (effect.kind == EffectKind::SuppliesOnArenaClear) {
        GetBool(d, obj, "require_no_damage", effect.require_no_damage, false);
    }
    if (effect.kind == EffectKind::SuppliesOnKill || effect.kind == EffectKind::ItemOnKill) {
        GetBool(d, obj, "melee_only", effect.melee_only, false);
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

void ParseBuild(const Diag& d, const json& b, SkinBuild& out);

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
                         "requires_capabilities", "enabled", "effects", "icon", "build"});
        GetString(d, e, "name", p.name, true);
        if (GetString(d, e, "icon", p.icon, false) && !IsValidId(p.icon)) {
            d.Error("field 'icon' " + Q(p.icon) + " must use only a-z, 0-9 and '_'");
        }
        GetString(d, e, "description", p.description, false);
        ReadEnum(d, e, "category", p.category, ParsePerkCategory,
                 "style, supply, support, skin, curse");
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
        // Build changes (a style perk's rule with its cost): only what adds into a run's build.
        if (const json* b = Get(e, "build")) {
            const Diag bd = d.Sub(": build");
            if (!b->is_object()) {
                bd.Error("must be an object");
            } else {
                CheckKeys(bd, *b, {"category_damage", "melee_damage", "rarity_shift", "max_health_mult", "heal_mult",
                                   "supplies_mult", "ammo_stack_mult", "stack_mult", "never_offered", "drops_as_held_ammo",
                                   "pickup_as", "break_grabs"});
                SkinBuild build;
                ParseBuild(bd, *b, build);
                p.build = std::move(build);
            }
        }

        const json* effects = Get(e, "effects");
        if (effects == nullptr || !effects->is_array() || (effects->empty() && !p.build)) {
            d.Error("field 'effects' must be a non-empty array (or empty with a 'build')");
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
                  std::vector<AffixDef>& affixes, std::vector<UniqueAffixDef>& uniques,
                  std::vector<ElementDef>& elements) {
    CheckKeys(file, doc, {"_comment", "schema_version", "weapons", "affixes", "unique_affixes", "elements"});
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
            CheckKeys(d, e, {"id", "name", "category", "base_damage_mult", "unlock_cost_credits", "enabled", "game_item",
                             "build_only"});
            GetBool(d, e, "build_only", w.build_only, false);
            if (GetInt(d, e, "game_item", w.game_item, false) && w.game_item < 0) d.Error("field 'game_item' must be >= 0");
            GetString(d, e, "name", w.name, true);
            ReadEnum(d, e, "category", w.category, ParseWeaponCategory,
                     "handgun, shotgun, smg, rifle, machine_gun, magnum, bow");
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
            CheckKeys(d, e, {"id", "name", "description", "effect", "value", "threshold", "seconds", "radius",
                             "every", "targets", "requires_capabilities"});
            GetString(d, e, "name", u.name, true);
            GetString(d, e, "description", u.description, false);
            if (!ReadEnum(d, e, "effect", u.kind, ParseUniqueEffect,
                          "low_hp_target_bonus, kill_streak_extend, execute, dragons_breath, chain_lightning, "
                          "bottomless, overkill, last_word, bloodthirst")) {
                continue;
            }
            const bool remake = u.kind != AffixKind::UniqueLowHpTargetBonus && u.kind != AffixKind::UniqueKillStreakExtend;
            if (remake) {
                // The rarity remake's effects: what each reads (the others must be absent).
                const bool wants_value = u.kind == AffixKind::UniqueExecute || u.kind == AffixKind::UniqueChainLightning ||
                                         u.kind == AffixKind::UniqueOverkill || u.kind == AffixKind::UniqueLastWord ||
                                         u.kind == AffixKind::UniqueBloodthirst;
                const bool wants_threshold = u.kind == AffixKind::UniqueExecute;
                const bool wants_radius = u.kind == AffixKind::UniqueDragonsBreath || u.kind == AffixKind::UniqueChainLightning ||
                                          u.kind == AffixKind::UniqueOverkill;
                const bool wants_arcs = u.kind == AffixKind::UniqueChainLightning;
                if (wants_value) {
                    if (GetNumber(d, e, "value", u.value, true)) CheckRange(d, "value", u.value, 0.0, 10.0, true);
                } else if (Get(e, "value") != nullptr) {
                    d.Error("field 'value' does not apply to " + Q(AffixKindName(u.kind)));
                }
                if (wants_threshold) {
                    if (GetNumber(d, e, "threshold", u.threshold, true)) CheckRange(d, "threshold", u.threshold, 0.0, 1.0, true);
                } else if (Get(e, "threshold") != nullptr) {
                    d.Error("field 'threshold' does not apply to " + Q(AffixKindName(u.kind)));
                }
                if (wants_radius) {
                    if (GetNumber(d, e, "radius", u.radius, true)) CheckRange(d, "radius", u.radius, 0.0, 5000.0, true);
                } else if (Get(e, "radius") != nullptr) {
                    d.Error("field 'radius' does not apply to " + Q(AffixKindName(u.kind)));
                }
                if (wants_arcs) {
                    GetIntRange(d, e, "every", u.every, true, 1, 100);
                    GetIntRange(d, e, "targets", u.targets, true, 1, 20);
                } else if (Get(e, "every") != nullptr || Get(e, "targets") != nullptr) {
                    d.Error("fields 'every' and 'targets' only apply to chain_lightning");
                }
                if (Get(e, "seconds") != nullptr) d.Error("field 'seconds' only applies to kill_streak_extend");
            } else if (u.kind == AffixKind::UniqueLowHpTargetBonus) {
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
            if (!remake && (Get(e, "radius") != nullptr || Get(e, "every") != nullptr || Get(e, "targets") != nullptr)) {
                d.Error("fields 'radius', 'every' and 'targets' do not apply to " + Q(AffixKindName(u.kind)));
            }
            ReadCapabilities(d, e, u.requires_capabilities);
            MergeCapabilities(u.requires_capabilities, AffixImpliedCaps(u.kind));
            uniques.push_back(std::move(u));
        }
    }
    // Elements (optional: without them no gun has one).
    if (const json* list = Get(doc, "elements")) {
        if (!list->is_array()) {
            file.Error("field 'elements' must be an array");
            return;
        }
        std::set<Element> seen;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json& e = (*list)[i];
            Diag d = file.Sub(": elements[" + std::to_string(i) + "]");
            ElementDef el;
            if (!ReadEnum(d, e, "element", el.element, ParseElement, "incendiary, shock, cryo, explosive, toxic")) continue;
            d = file.Sub(": element " + Q(ElementName(el.element)));
            if (!seen.insert(el.element).second) d.Error("duplicate element");
            CheckKeys(d, e, {"element", "name", "description", "chance", "seconds", "dps_fraction", "damage_fraction",
                             "damage_cap", "radius", "targets", "taken_mult", "boss_bonus"});
            GetString(d, e, "name", el.name, true);
            GetString(d, e, "description", el.description, false);
            if (GetNumber(d, e, "chance", el.chance, false)) CheckRange(d, "chance", el.chance, 0.0, 1.0, true);
            if (GetNumber(d, e, "seconds", el.seconds, false)) CheckRange(d, "seconds", el.seconds, 0.0, 30.0);
            if (GetNumber(d, e, "dps_fraction", el.dps_fraction, false)) CheckRange(d, "dps_fraction", el.dps_fraction, 0.0, 5.0);
            if (GetNumber(d, e, "damage_fraction", el.damage_fraction, false)) {
                CheckRange(d, "damage_fraction", el.damage_fraction, 0.0, 5.0);
            }
            if (GetNumber(d, e, "damage_cap", el.damage_cap, false)) CheckRange(d, "damage_cap", el.damage_cap, 0.0, 30000.0);
            if (GetNumber(d, e, "radius", el.radius, false)) CheckRange(d, "radius", el.radius, 0.0, 5000.0);
            GetIntRange(d, e, "targets", el.targets, false, 0, 20);
            if (GetNumber(d, e, "taken_mult", el.taken_mult, false)) CheckRange(d, "taken_mult", el.taken_mult, 0.1, 10.0);
            if (GetNumber(d, e, "boss_bonus", el.boss_bonus, false)) CheckRange(d, "boss_bonus", el.boss_bonus, 0.0, 5.0);
            // What each element needs to do anything.
            switch (el.element) {
                case Element::Incendiary:
                case Element::Toxic:
                    if (!(el.seconds > 0.0) || !(el.dps_fraction > 0.0)) d.Error("needs 'seconds' and 'dps_fraction' above 0");
                    break;
                case Element::Shock:
                    if (el.targets <= 0 || !(el.radius > 0.0) || !(el.damage_fraction > 0.0)) {
                        d.Error("needs 'targets', 'radius' and 'damage_fraction' above 0");
                    }
                    break;
                case Element::Cryo:
                    if (!(el.seconds > 0.0)) d.Error("needs 'seconds' above 0");
                    break;
                case Element::Explosive:
                    if (!(el.radius > 0.0) || !(el.damage_fraction > 0.0)) d.Error("needs 'radius' and 'damage_fraction' above 0");
                    break;
            }
            elements.push_back(std::move(el));
        }
    }
}

// ---- characters / routes -------------------------------------------------------------------

void ParseChallenge(const Diag& d, const json& e, Challenge& out) {
    std::string kind;
    GetString(d, e, "unlock_kind", kind, false);
    if (!kind.empty()) {
        if (const auto parsed = ParseChallengeKind(kind)) {
            out.kind = *parsed;
        } else {
            d.Error("unknown unlock_kind " + Q(kind));
        }
    }
    if (GetInt(d, e, "unlock_count", out.count, false) && out.count < 0) d.Error("field 'unlock_count' must be >= 0");
    GetString(d, e, "unlock_character", out.character, false);
    GetString(d, e, "unlock_boss", out.boss, false);
    GetString(d, e, "unlock_text", out.text, false);
    if (out.kind == ChallengeKind::BossBeaten && out.count < 1) out.count = 1;  // a boss is beaten or not
    if (out.kind != ChallengeKind::None && out.count < 1) d.Error("unlock_count must be >= 1 with an unlock_kind");
    if (out.kind == ChallengeKind::RunsCompletedAs && out.character.empty()) {
        d.Error("unlock_kind runs_completed_as needs unlock_character");
    }
    if (out.kind == ChallengeKind::BossBeaten && out.boss.empty()) d.Error("unlock_kind boss_beaten needs unlock_boss");
}

void CheckChallengeTarget(const Diag& d, const Challenge& c, const std::set<std::string>& character_ids) {
    if (c.kind == ChallengeKind::RunsCompletedAs && character_ids.count(c.character) == 0) {
        d.Error("unlock_character " + Q(c.character) + " is not a character");
    }
}

// {"categories": [...], "ids": [...]}; categories are checked here, ids by CrossCheck.
void ParseWeaponFilter(const Diag& d, const json& b, std::string_view key, WeaponFilter& out) {
    const json* f = Get(b, key);
    if (f == nullptr) return;
    if (!f->is_object()) {
        d.Error("field " + Q(key) + " must be an object");
        return;
    }
    const Diag fd = d.Sub(" " + std::string(key));
    CheckKeys(fd, *f, {"categories", "ids"});
    GetStringList(fd, *f, "categories", out.categories, false);
    GetStringList(fd, *f, "ids", out.ids, false);
    for (const std::string& cat : out.categories) {
        if (!ParseWeaponCategory(cat)) fd.Error("unknown weapon category " + Q(cat));
    }
    if (out.Empty()) fd.Error("must list at least one category or id");
}

// A game item id given as an integer or a string ("0x010A"); -1 when it is neither.
std::int64_t ItemIdValue(const json& v) {
    if (v.is_number_integer()) return v.get<std::int64_t>();
    if (!v.is_string()) return -1;
    try {
        std::size_t used = 0;
        const std::string text = v.get<std::string>();
        const std::int64_t id = std::stoll(text, &used, 0);
        return used == text.size() ? id : -1;
    } catch (...) {
        return -1;
    }
}

void ParseSpecial(const Diag& d, const json& sp, SkinSpecial& out) {
    CheckKeys(d, sp, {"kind", "name", "icon", "cooldown_seconds", "every_arenas", "power", "duration_seconds"});
    GetString(d, sp, "kind", out.kind, true);
    if (!out.kind.empty() && out.kind != "dash" && out.kind != "dual_smg" && out.kind != "backup" && out.kind != "burst") {
        d.Error("unknown special kind " + Q(out.kind) + " (dash, dual_smg, backup, burst)");
    }
    GetString(d, sp, "name", out.name, true);
    if (GetString(d, sp, "icon", out.icon, false) && !IsValidId(out.icon)) d.Error("special icon " + Q(out.icon) + " is not a valid name");
    if (GetNumber(d, sp, "cooldown_seconds", out.cooldown_seconds, false)) CheckRange(d, "cooldown_seconds", out.cooldown_seconds, 0.0, 600.0);
    std::int64_t every = 0;
    if (GetInt(d, sp, "every_arenas", every, false)) {
        CheckRange(d, "every_arenas", static_cast<double>(every), 0, 10);
        out.every_arenas = static_cast<int>(every);
    }
    if (GetNumber(d, sp, "power", out.power, false)) CheckRange(d, "power", out.power, 0.0, 100000.0);
    if (GetNumber(d, sp, "duration_seconds", out.duration_seconds, false)) CheckRange(d, "duration_seconds", out.duration_seconds, 0.0, 60.0);
}

void ParseBuild(const Diag& d, const json& b, SkinBuild& out) {
    CheckKeys(d, b, {"name", "icon", "summary", "offered_weapons", "usable_weapons", "category_damage", "supplies_mult",
                     "free_shop_rerolls", "rarity_shift", "uncapped_upgrades", "starting_weapon",
                     "starting_weapon_rarity", "max_health_mult", "stack_max", "ammo_stack_mult", "extra_weapons",
                     "melee_damage", "melee_kill_stats", "melee_kill_caps", "heal_mult", "heal_buff_fraction", "heal_invulnerable_seconds",
                     "heal_damage_bonus", "heal_damage_seconds", "drops_as", "chest_keys_exempt", "extra_chests", "special",
                     "special_2", "stack_mult", "never_offered", "drops_as_held_ammo", "pickup_as", "break_grabs",
                     "shop_weapon", "shop_weapon_price", "gun_elements", "arena_element", "refine_every_shop",
                     "refine_price_mult", "fuel_regen_mult"});
    GetString(d, b, "shop_weapon", out.shop_weapon, false);
    if (GetStringList(d, b, "gun_elements", out.gun_elements, false)) {
        for (const std::string& e : out.gun_elements) {
            if (!ParseElement(e)) d.Error("unknown element " + Q(e) + " in gun_elements");
        }
    }
    GetBool(d, b, "arena_element", out.arena_element, false);
    GetBool(d, b, "refine_every_shop", out.refine_every_shop, false);
    if (GetNumber(d, b, "refine_price_mult", out.refine_price_mult, false)) {
        CheckRange(d, "refine_price_mult", out.refine_price_mult, 0.0, 5.0);
    }
    if (GetNumber(d, b, "fuel_regen_mult", out.fuel_regen_mult, false)) CheckRange(d, "fuel_regen_mult", out.fuel_regen_mult, 0.1, 10.0);
    if (GetInt(d, b, "shop_weapon_price", out.shop_weapon_price, false)) {
        CheckRange(d, "shop_weapon_price", static_cast<double>(out.shop_weapon_price), 0, 10000);
    }
    GetString(d, b, "name", out.name, false);
    if (GetString(d, b, "icon", out.icon, false) && !IsValidId(out.icon)) d.Error("icon " + Q(out.icon) + " is not a valid name");
    if (GetNumber(d, b, "ammo_stack_mult", out.ammo_stack_mult, false)) CheckRange(d, "ammo_stack_mult", out.ammo_stack_mult, 1.0, 20.0);
    GetStringList(d, b, "extra_weapons", out.extra_weapons, false);
    if (GetNumber(d, b, "melee_damage", out.melee_damage, false)) CheckRange(d, "melee_damage", out.melee_damage, -0.9, 20.0);
    // The run stats a melee kill can raise, and their caps.
    const auto knownStat = [](const std::string& stat) {
        return stat == "damage" || stat == "defense" || stat == "max_health" || stat == "crit" || stat == "move_speed" ||
               stat == "heal" || stat == "supplies";
    };
    constexpr const char* kStatNames = "damage, defense, max_health, crit, move_speed, heal, supplies";
    if (const json* ms = Get(b, "melee_kill_stats")) {
        if (!ms->is_object()) {
            d.Error("field 'melee_kill_stats' must be an object");
        } else {
            for (const auto& [stat, v] : ms->items()) {
                if (!knownStat(stat)) {
                    d.Error("melee_kill_stats: unknown stat " + Q(stat) + " (" + kStatNames + ")");
                    continue;
                }
                if (!v.is_number()) {
                    d.Error("melee_kill_stats " + Q(stat) + " must be a number");
                    continue;
                }
                CheckRange(d, "melee_kill_stats " + stat, v.get<double>(), 0.0, 1.0, true);
                out.melee_kill_stats[stat] = v.get<double>();
            }
        }
    }
    if (const json* mc = Get(b, "melee_kill_caps")) {
        if (!mc->is_object()) {
            d.Error("field 'melee_kill_caps' must be an object");
        } else {
            for (const auto& [stat, v] : mc->items()) {
                if (!knownStat(stat)) {
                    d.Error("melee_kill_caps: unknown stat " + Q(stat) + " (" + kStatNames + ")");
                } else if (!v.is_number()) {
                    d.Error("melee_kill_caps " + Q(stat) + " must be a number");
                } else {
                    CheckRange(d, "melee_kill_caps " + stat, v.get<double>(), 0.0, 5.0, true);
                    out.melee_kill_caps[stat] = v.get<double>();
                }
            }
        }
    }
    if (GetNumber(d, b, "heal_mult", out.heal_mult, false)) CheckRange(d, "heal_mult", out.heal_mult, 0.1, 10.0);
    if (GetNumber(d, b, "heal_buff_fraction", out.heal_buff_fraction, false)) CheckRange(d, "heal_buff_fraction", out.heal_buff_fraction, 0.0, 1.0);
    if (GetNumber(d, b, "heal_invulnerable_seconds", out.heal_invulnerable_seconds, false)) {
        CheckRange(d, "heal_invulnerable_seconds", out.heal_invulnerable_seconds, 0.0, 60.0);
    }
    if (GetNumber(d, b, "heal_damage_bonus", out.heal_damage_bonus, false)) CheckRange(d, "heal_damage_bonus", out.heal_damage_bonus, 0.0, 5.0);
    if (GetNumber(d, b, "heal_damage_seconds", out.heal_damage_seconds, false)) {
        CheckRange(d, "heal_damage_seconds", out.heal_damage_seconds, 0.0, 120.0);
    }
    if (const json* da = Get(b, "drops_as")) {
        out.drops_as = ItemIdValue(*da);
        if (out.drops_as < 0 || out.drops_as > 0xFFFF) d.Error("drops_as is not a game item id");
    }
    GetBool(d, b, "chest_keys_exempt", out.chest_keys_exempt, false);
    GetIntRange(d, b, "extra_chests", out.extra_chests, false, 0, 6);
    GetBool(d, b, "drops_as_held_ammo", out.drops_as_held_ammo, false);
    GetBool(d, b, "break_grabs", out.break_grabs, false);
    if (out.drops_as_held_ammo && out.drops_as >= 0) d.Error("drops_as and drops_as_held_ammo exclude each other");
    // Objects keyed by game item id (hex strings allowed).
    const auto itemKey = [](const std::string& key) {
        try {
            std::size_t used = 0;
            const std::int64_t item = std::stoll(key, &used, 0);
            return used == key.size() && item >= 0 && item <= 0xFFFF ? item : std::int64_t{-1};
        } catch (...) {
            return std::int64_t{-1};
        }
    };
    if (const json* sm = Get(b, "stack_mult")) {
        if (!sm->is_object()) {
            d.Error("field 'stack_mult' must be an object");
        } else {
            for (const auto& [key, v] : sm->items()) {
                const std::int64_t item = itemKey(key);
                if (item < 0) {
                    d.Error("stack_mult key " + Q(key) + " is not a game item id");
                } else if (!v.is_number() || v.get<double>() < 1.0 || v.get<double>() > 10.0) {
                    d.Error("stack_mult " + Q(key) + " must be a number in [1, 10]");
                } else {
                    out.stack_mult.emplace_back(item, v.get<double>());
                }
            }
        }
    }
    if (const json* pa = Get(b, "pickup_as")) {
        if (!pa->is_object()) {
            d.Error("field 'pickup_as' must be an object");
        } else {
            for (const auto& [key, v] : pa->items()) {
                const std::int64_t from = itemKey(key);
                const std::int64_t to = ItemIdValue(v);
                if (from < 0) {
                    d.Error("pickup_as key " + Q(key) + " is not a game item id");
                } else if (to < 0 || to > 0xFFFF || to == from) {
                    d.Error("pickup_as " + Q(key) + " must name another game item id");
                } else {
                    out.pickup_as.emplace_back(from, to);
                }
            }
        }
    }
    ParseWeaponFilter(d, b, "never_offered", out.never_offered);
    if (const json* sp = Get(b, "special")) {
        if (sp->is_object()) {
            ParseSpecial(d.Sub(" special"), *sp, out.special);
        } else {
            d.Error("field 'special' must be an object");
        }
    }
    if (const json* sp = Get(b, "special_2")) {
        if (sp->is_object()) {
            ParseSpecial(d.Sub(" special_2"), *sp, out.special_2);
        } else {
            d.Error("field 'special_2' must be an object");
        }
    }
    if (const json* sm = Get(b, "stack_max")) {
        if (!sm->is_object()) {
            d.Error("field 'stack_max' must be an object");
        } else {
            for (const auto& [key, v] : sm->items()) {
                std::int64_t item = -1;
                try {
                    std::size_t used = 0;
                    item = std::stoll(key, &used, 0);
                    if (used != key.size()) item = -1;
                } catch (...) {
                    item = -1;
                }
                if (item < 0 || item > 0xFFFF) {
                    d.Error("stack_max key " + Q(key) + " is not a game item id");
                    continue;
                }
                if (!v.is_number_integer() || v.get<std::int64_t>() < 1 || v.get<std::int64_t>() > 999) {
                    d.Error("stack_max " + Q(key) + " must be an integer in [1, 999]");
                    continue;
                }
                out.stack_max.emplace_back(item, static_cast<int>(v.get<std::int64_t>()));
            }
        }
    }
    GetString(d, b, "summary", out.summary, false);
    ParseWeaponFilter(d, b, "offered_weapons", out.offered_weapons);
    ParseWeaponFilter(d, b, "usable_weapons", out.usable_weapons);
    ParseWeaponFilter(d, b, "uncapped_upgrades", out.uncapped_upgrades);
    if (const json* cd = Get(b, "category_damage")) {
        if (!cd->is_object()) {
            d.Error("field 'category_damage' must be an object");
        } else {
            for (const auto& [cat, v] : cd->items()) {
                if (!ParseWeaponCategory(cat)) d.Error("category_damage: unknown weapon category " + Q(cat));
                if (!v.is_number()) {
                    d.Error("category_damage " + Q(cat) + " must be a number");
                    continue;
                }
                CheckRange(d, "category_damage " + cat, v.get<double>(), -0.9, 5.0);
                out.category_damage[cat] = v.get<double>();
            }
        }
    }
    if (GetNumber(d, b, "supplies_mult", out.supplies_mult, false)) CheckRange(d, "supplies_mult", out.supplies_mult, 0.0, 10.0, true);
    std::int64_t rerolls = 0;
    if (GetInt(d, b, "free_shop_rerolls", rerolls, false)) {
        CheckRange(d, "free_shop_rerolls", static_cast<double>(rerolls), 0, 10);
        out.free_shop_rerolls = static_cast<int>(rerolls);
    }
    if (GetNumber(d, b, "rarity_shift", out.rarity_shift, false)) CheckRange(d, "rarity_shift", out.rarity_shift, -4.0, 4.0);
    GetString(d, b, "starting_weapon", out.starting_weapon, false);
    if (GetString(d, b, "starting_weapon_rarity", out.starting_weapon_rarity, false) &&
        !ParseRarity(out.starting_weapon_rarity)) {
        d.Error("unknown starting_weapon_rarity " + Q(out.starting_weapon_rarity));
    }
    if (!out.starting_weapon_rarity.empty() && out.starting_weapon.empty()) {
        d.Error("starting_weapon_rarity without a starting_weapon");
    }
    if (GetNumber(d, b, "max_health_mult", out.max_health_mult, false)) {
        CheckRange(d, "max_health_mult", out.max_health_mult, 0.1, 5.0);
    }
}

void ParseSkins(const Diag& file, const json& e, CharacterDef& c) {
    const auto it = e.find("skins");
    if (it == e.end()) return;
    if (!it->is_array()) {
        file.Error("field 'skins' must be a list");
        return;
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
        const json& s = (*it)[i];
        Diag d = file.Sub(": skins[" + std::to_string(i) + "]");
        if (!s.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        SkinDef skin;
        if (!ReadId(d, s, skin.id)) continue;
        d = file.Sub(" skin " + Q(skin.id));
        CheckKeys(d, s, {"id", "name", "costume_index", "starting_weapons", "starting_passive", "skill_pool",
                         "unlock_kind", "unlock_count", "unlock_character", "unlock_boss", "unlock_text", "build", "mod"});
        if (const json* m = Get(s, "mod")) {
            const Diag md = d.Sub(": mod");
            if (!m->is_object()) {
                md.Error("must be an object");
            } else {
                SkinMod mod;
                CheckKeys(md, *m, {"name", "author", "url", "files", "portrait", "folders"});
                if (GetStringList(md, *m, "folders", mod.folders, false)) {
                    for (const std::string& f : mod.folders) {
                        if (f.empty() || f.find("..") != std::string::npos || f.find(':') != std::string::npos || f.front() == '\\' ||
                            f.front() == '/') {
                            md.Error("folder " + Q(f) + " must be a path under nativePC_MT (\"sound\\\\bgm\\\\...\")");
                        }
                    }
                }
                if (const json* pt = Get(*m, "portrait")) {
                    SkinModPortrait portrait;
                    if (!pt->is_object()) {
                        md.Error("field 'portrait' must be an object {file, entry} or {file, crop}");
                    } else {
                        CheckKeys(md, *pt, {"file", "entry", "crop"});
                        GetString(md, *pt, "file", portrait.file, true);
                        GetString(md, *pt, "entry", portrait.entry, false);
                        if (const json* crop = Get(*pt, "crop")) {
                            if (!crop->is_array() || crop->size() != 4) {
                                md.Error("field 'crop' must be [x, y, width, height]");
                            } else {
                                for (const json& v : *crop) portrait.crop.push_back(v.is_number_integer() ? v.get<int>() : -1);
                                if (portrait.crop[0] < 0 || portrait.crop[1] < 0 || portrait.crop[2] <= 0 || portrait.crop[3] <= 0) {
                                    md.Error("field 'crop' must hold whole numbers, a positive size");
                                }
                            }
                        }
                        if (portrait.file.empty() || portrait.file.find(':') != std::string::npos ||
                            portrait.file.find("..") != std::string::npos || portrait.file.front() == '\\' || portrait.file.front() == '/') {
                            md.Error("portrait file " + Q(portrait.file) + " must be a file name or the end of a path in the mod");
                        }
                        mod.portrait = std::move(portrait);
                    }
                }
                GetString(md, *m, "name", mod.name, true);
                GetString(md, *m, "author", mod.author, false);
                GetString(md, *m, "url", mod.url, true);
                // Only a mod page: the menu opens it in the browser.
                if (mod.url.rfind("https://www.nexusmods.com/", 0) != 0) md.Error("field 'url' must be a https://www.nexusmods.com/ page");
                const json* files = Get(*m, "files");
                if (files == nullptr || !files->is_array() || files->empty()) {
                    md.Error("field 'files' must list the mod's archives");
                } else {
                    for (const json& f : *files) {
                        SkinModFile entry;
                        if (!f.is_object()) {
                            md.Error("a file must be an object {name, size}");
                            continue;
                        }
                        CheckKeys(md, f, {"name", "size", "sizes"});
                        GetString(md, f, "name", entry.name, true);
                        GetInt(md, f, "size", entry.size, false);
                        if (const json* sizes = Get(f, "sizes")) {
                            if (!sizes->is_array() || sizes->empty()) md.Error("field 'sizes' must list the variants' sizes");
                            else {
                                for (const json& v : *sizes) {
                                    if (v.is_number_integer() && v.get<std::int64_t>() > 0) entry.sizes.push_back(v.get<std::int64_t>());
                                    else md.Error("a size in 'sizes' must be a positive whole number");
                                }
                            }
                        }
                        const bool plain = !entry.name.empty() && entry.name.find_first_of("\\/:") == std::string::npos;
                        if (!plain || entry.name.size() < 5 || entry.name.compare(entry.name.size() - 4, 4, ".arc") != 0) {
                            md.Error("file " + Q(entry.name) + " must be an archive's bare name (\"uPl05ExcellaNormal.arc\")");
                        }
                        mod.files.push_back(std::move(entry));
                    }
                }
                skin.mod = std::move(mod);
            }
        }
        if (const json* b = Get(s, "build")) {
            if (b->is_object()) {
                ParseBuild(d.Sub(": build"), *b, skin.build);
            } else {
                d.Error("field 'build' must be an object");
            }
        }
        GetString(d, s, "name", skin.name, true);
        if (GetInt(d, s, "costume_index", skin.costume_index, false) && skin.costume_index < 0) {
            d.Error("field 'costume_index' must be >= 0");
        }
        GetStringList(d, s, "starting_weapons", skin.starting_weapons, false);
        GetString(d, s, "starting_passive", skin.starting_passive, false);
        GetStringList(d, s, "skill_pool", skin.skill_pool, false);
        ParseChallenge(d, s, skin.unlock);
        if (skin.mod && skin.costume_index < 0) d.Error("a mod skin needs the costume_index it stands in for");
        c.skins.push_back(std::move(skin));
    }
}

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
                         "unlock_character", "unlock_boss", "unlock_text", "costume_index", "skins"});
        GetString(d, e, "name", c.name, true);
        GetBool(d, e, "enabled", c.enabled, false);
        GetStringList(d, e, "starting_weapons", c.starting_weapons, true);
        GetString(d, e, "starting_passive", c.starting_passive, false);
        GetStringList(d, e, "skill_pool", c.skill_pool, false);
        GetString(d, e, "exclusive_mechanic", c.exclusive_mechanic, false);
        if (GetInt(d, e, "unlock_cost_credits", c.unlock_cost_credits, false) && c.unlock_cost_credits < 0) {
            d.Error("field 'unlock_cost_credits' must be >= 0");
        }
        ParseChallenge(d, e, c.unlock);
        if (GetInt(d, e, "costume_index", c.costume_index, false) && c.costume_index < 0) {
            d.Error("field 'costume_index' must be >= 0");
        }
        if (c.starting_weapons.empty()) d.Error("starting_weapons must list at least one weapon");
        ParseSkins(d, e, c);
        if (c.skins.empty()) {
            // Every character has a default skin, so runs and the UI never special-case it.
            SkinDef base;
            base.id = c.id + "_base";
            base.name = "Default";
            base.costume_index = c.costume_index;
            c.skins.push_back(std::move(base));
        }
        out.push_back(std::move(c));
    }
    std::set<std::string> ids;
    for (const CharacterDef& c : out) ids.insert(c.id);
    for (const CharacterDef& c : out) {
        const Diag d = file.Sub(": character " + Q(c.id));
        CheckChallengeTarget(d, c.unlock, ids);
        for (const SkinDef& s : c.skins) {
            CheckChallengeTarget(d.Sub(" skin " + Q(s.id)), s.unlock, ids);
            // Skin ids share the profile's unlocked set with character ids.
            if (!ids.insert(s.id).second) d.Error("skin id " + Q(s.id) + " duplicates a character or skin id");
        }
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

void ParseMaps(const Diag& file, const json& doc, std::vector<MapDef>& out) {
    CheckKeys(file, doc, {"schema_version", "_comment", "maps"});
    const json* list = ListField(file, doc, "maps");
    if (list == nullptr) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& e = (*list)[i];
        Diag d = file.Sub(": maps[" + std::to_string(i) + "]");
        if (!e.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        MapDef map;
        if (!ReadId(d, e, map.id)) continue;
        d = file.Sub(": map " + Q(map.id));
        CheckKeys(d, e, {"id", "name", "stage", "rotation", "start", "points", "chests", "preload", "roster", "boss", "max_alive", "min_player_distance", "max_player_distance", "special_after", "max_specials"});
        if (const auto s = e.find("start"); s != e.end()) {
            if (!s->is_array() || s->size() != 3 || !(*s)[0].is_number() || !(*s)[1].is_number() || !(*s)[2].is_number()) {
                d.Error("field 'start' must be [x, y, z]");
            } else {
                map.start = std::array<double, 3>{(*s)[0].get<double>(), (*s)[1].get<double>(), (*s)[2].get<double>()};
            }
        }
        GetBool(d, e, "rotation", map.rotation, false);
        GetString(d, e, "name", map.name, true);
        std::int64_t stage = -1;
        if (GetInt(d, e, "stage", stage, true) && stage < 0) d.Error("field 'stage' must be >= 0");
        map.stage = static_cast<int>(stage);
        std::int64_t maxAlive = 8;
        if (GetInt(d, e, "max_alive", maxAlive, false) && (maxAlive < 1 || maxAlive > 40)) d.Error("field 'max_alive' must be 1..40");
        map.max_alive = static_cast<int>(maxAlive);
        if (GetNumber(d, e, "min_player_distance", map.min_player_distance, false) && map.min_player_distance < 0) {
            d.Error("field 'min_player_distance' must be >= 0");
        }
        if (GetNumber(d, e, "max_player_distance", map.max_player_distance, false) && map.max_player_distance < 0) {
            d.Error("field 'max_player_distance' must be >= 0");
        }
        if (GetNumber(d, e, "special_after", map.special_after, false) && (map.special_after < 0 || map.special_after > 1)) {
            d.Error("field 'special_after' must be 0..1");
        }
        std::int64_t maxSpecials = 1;
        if (GetInt(d, e, "max_specials", maxSpecials, false) && (maxSpecials < 0 || maxSpecials > 10)) d.Error("field 'max_specials' must be 0..10");
        map.max_specials = static_cast<int>(maxSpecials);
        if (const json* pts = ListField(d, e, "points")) {
            for (const json& p : *pts) {
                if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) {
                    d.Error("every point must be [x, y, z]");
                    continue;
                }
                map.points.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
            }
            if (map.points.empty()) d.Error("needs at least one spawn point");
        }
        if (const auto preload = e.find("preload"); preload != e.end()) {
            if (!preload->is_array()) d.Error("field 'preload' must be a list of archive names");
            for (const json& p : preload->is_array() ? *preload : json::array()) {
                const std::string name = p.is_string() ? p.get<std::string>() : std::string();
                if (name.empty() || name.size() > 48 ||
                    !std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
                    d.Error("every preload entry must be an archive name (letters, digits, _)");
                    continue;
                }
                map.preload.push_back(name);
            }
        }
        if (const auto chests = e.find("chests"); chests != e.end()) {
            if (!chests->is_array()) d.Error("field 'chests' must be a list of [x, y, z]");
            for (const json& p : chests->is_array() ? *chests : json::array()) {
                if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) {
                    d.Error("every chest must be [x, y, z]");
                    continue;
                }
                map.chests.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
            }
        }
        if (const auto boss = e.find("boss"); boss != e.end()) {
            const Diag bd = d.Sub(" boss");
            if (!boss->is_object()) {
                bd.Error("must be an object");
            } else {
                CheckKeys(bd, *boss, {"class", "name", "article", "hp", "spawn", "facing", "trigger", "trigger_box", "trigger_wait"});
                MapBoss b;
                GetString(bd, *boss, "class", b.unit_class, true);
                if (b.unit_class.rfind("uEm", 0) != 0) bd.Error("class must be an enemy class (uEm..)");
                GetString(bd, *boss, "name", b.name, true);
                GetString(bd, *boss, "article", b.article, false);
                std::int64_t hp = 0;
                if (GetInt(bd, *boss, "hp", hp, false) && (hp < 0 || hp > 32767)) bd.Error("hp must be 0..32767");
                b.hp = static_cast<int>(hp);
                // Its story spot (and turn); with "trigger" it appears once the player comes that near.
                if (const auto sp = boss->find("spawn"); sp != boss->end()) {
                    if (!sp->is_array() || sp->size() != 3 || !(*sp)[0].is_number() || !(*sp)[1].is_number() || !(*sp)[2].is_number()) {
                        bd.Error("field 'spawn' must be [x, y, z]");
                    } else {
                        b.spawn = std::array<double, 3>{(*sp)[0].get<double>(), (*sp)[1].get<double>(), (*sp)[2].get<double>()};
                    }
                }
                double facing = 0.0;
                if (GetNumber(bd, *boss, "facing", facing, false)) b.facing = facing;
                if (GetNumber(bd, *boss, "trigger", b.trigger, false) && b.trigger < 0) bd.Error("field 'trigger' must be >= 0");
                if (GetNumber(bd, *boss, "trigger_wait", b.trigger_wait, false) && b.trigger_wait < 0) {
                    bd.Error("field 'trigger_wait' must be >= 0");
                }
                // The story's own trigger volume: [min x, min y, min z, max x, max y, max z].
                if (const auto tb = boss->find("trigger_box"); tb != boss->end()) {
                    const bool numbers = tb->is_array() && tb->size() == 6 &&
                                         std::all_of(tb->begin(), tb->end(), [](const json& v) { return v.is_number(); });
                    if (!numbers || (*tb)[0].get<double>() >= (*tb)[3].get<double>() || (*tb)[1].get<double>() >= (*tb)[4].get<double>() ||
                        (*tb)[2].get<double>() >= (*tb)[5].get<double>()) {
                        bd.Error("field 'trigger_box' must be [min x, min y, min z, max x, max y, max z]");
                    } else {
                        std::array<double, 6> box{};
                        for (std::size_t k = 0; k < 6; ++k) box[k] = (*tb)[k].get<double>();
                        b.trigger_box = box;
                    }
                }
                if (!b.spawn && (b.facing || boss->contains("trigger") || boss->contains("trigger_box") || boss->contains("trigger_wait"))) {
                    bd.Error("'facing', 'trigger', 'trigger_box' and 'trigger_wait' need 'spawn'");
                }
                map.boss = std::move(b);
            }
        }
        // A boss map spawns only its boss: its roster is optional.
        if (const json* roster = map.boss && !e.contains("roster") ? nullptr : ListField(d, e, "roster")) {
            for (const json& r : *roster) {
                const Diag rd = d.Sub(" roster entry");
                if (!r.is_object()) {
                    rd.Error("must be an object");
                    continue;
                }
                CheckKeys(rd, r, {"class", "arms", "profiles", "min_tier", "weight", "special", "max_alive"});
                MapEnemy enemy;
                GetString(rd, r, "class", enemy.unit_class, true);
                if (enemy.unit_class.rfind("uEm", 0) != 0) rd.Error("class must be an enemy class (uEm..)");
                if (const auto arms = r.find("arms"); arms != r.end()) {
                    if (!arms->is_array()) {
                        rd.Error("arms must be an array of weapon ids");
                    } else {
                        for (const json& a : *arms) {
                            if (a.is_number_integer()) enemy.arms.push_back(a.get<std::int64_t>());
                            else rd.Error("arms must contain integers");
                        }
                    }
                }
                if (const auto profiles = r.find("profiles"); profiles != r.end()) {
                    if (!profiles->is_array()) {
                        rd.Error("profiles must be an array");
                    } else {
                        for (const json& p : *profiles) {
                            const Diag pd = rd.Sub(" profile");
                            if (!p.is_object()) {
                                pd.Error("must be an object");
                                continue;
                            }
                            CheckKeys(pd, p, {"weight", "fields"});
                            EnemyProfile profile;
                            if (GetNumber(pd, p, "weight", profile.weight, false) && profile.weight < 0) pd.Error("weight must be >= 0");
                            const auto fields = p.find("fields");
                            if (fields == p.end() || !fields->is_object()) {
                                pd.Error("needs a 'fields' object");
                                continue;
                            }
                            for (const auto& [name, value] : fields->items()) {
                                const bool identifier = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
                                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                                });
                                if (!identifier) pd.Error("field name '" + name + "' must be lower_case");
                                else if (!value.is_number()) pd.Error("field '" + name + "' must be a number");
                                else profile.fields.emplace_back(name, value.get<double>());
                            }
                            enemy.profiles.push_back(std::move(profile));
                        }
                    }
                }
                std::int64_t tier = 1;
                if (GetInt(rd, r, "min_tier", tier, false) && tier < 1) rd.Error("min_tier must be >= 1");
                enemy.min_tier = static_cast<int>(tier);
                if (GetNumber(rd, r, "weight", enemy.weight, false) && enemy.weight < 0) rd.Error("weight must be >= 0");
                GetBool(rd, r, "special", enemy.special, false);
                std::int64_t enemyMax = 0;
                if (GetInt(rd, r, "max_alive", enemyMax, false) && (enemyMax < 0 || enemyMax > 40)) rd.Error("max_alive must be 0..40");
                enemy.max_alive = static_cast<int>(enemyMax);
                map.roster.push_back(std::move(enemy));
            }
            if (map.roster.empty()) d.Error("needs at least one roster entry");
        }
        if (std::any_of(out.begin(), out.end(), [&](const MapDef& o) { return o.id == map.id || o.stage == map.stage; })) {
            d.Error("duplicate map id or stage");
        }
        out.push_back(std::move(map));
    }
}

void ParseLoadouts(const Diag& file, const json& doc, LoadoutConfig& out) {
    CheckKeys(file, doc, {"schema_version", "budget", "default_weapon_cost", "max_items", "weapon_cost", "equipment"});
    if (GetInt(file, doc, "budget", out.budget, false) && out.budget < 0) file.Error("field 'budget' must be >= 0");
    std::int64_t max_items = 0;
    if (GetInt(file, doc, "max_items", max_items, false) && (max_items < 0 || max_items > 24)) {
        file.Error("field 'max_items' must be 0..24");
    }
    out.max_items = static_cast<int>(max_items);
    if (GetInt(file, doc, "default_weapon_cost", out.default_weapon_cost, false) && out.default_weapon_cost < 0) {
        file.Error("field 'default_weapon_cost' must be >= 0");
    }
    if (const auto costs = doc.find("weapon_cost"); costs != doc.end()) {
        if (!costs->is_object()) {
            file.Error("field 'weapon_cost' must be an object of category -> points");
        } else {
            for (const auto& item : costs->items()) {
                if (!ParseWeaponCategory(item.key())) file.Error("weapon_cost: unknown category " + Q(item.key()));
                if (!item.value().is_number_integer() || item.value().get<std::int64_t>() < 0) {
                    file.Error("weapon_cost " + Q(item.key()) + " must be a non-negative integer");
                    continue;
                }
                out.weapon_cost[item.key()] = item.value().get<std::int64_t>();
            }
        }
    }
    const json* list = ListField(file, doc, "equipment");
    if (list == nullptr) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& e = (*list)[i];
        Diag d = file.Sub(": equipment[" + std::to_string(i) + "]");
        if (!e.is_object()) {
            d.Error("entry must be an object");
            continue;
        }
        EquipmentDef item;
        if (!ReadId(d, e, item.id)) continue;
        d = file.Sub(": equipment " + Q(item.id));
        CheckKeys(d, e, {"id", "name", "cost", "max", "game_item", "price", "bundle", "for_weapons"});
        GetString(d, e, "name", item.name, true);
        GetStringList(d, e, "for_weapons", item.for_weapons, false);
        if (GetInt(d, e, "cost", item.cost, true) && item.cost < 0) d.Error("field 'cost' must be >= 0");
        if (GetInt(d, e, "price", item.price, false) && item.price < 0) d.Error("field 'price' must be >= 0");
        std::int64_t bundle = 1;
        if (GetInt(d, e, "bundle", bundle, false) && (bundle < 1 || bundle > 99)) d.Error("field 'bundle' must be 1..99");
        item.bundle = static_cast<int>(bundle);
        if (GetInt(d, e, "game_item", item.game_item, false) && item.game_item < 0) {
            d.Error("field 'game_item' must be >= 0");
        }
        if (item.price > 0 && item.game_item < 0) d.Error("a priced item needs a game_item");
        std::int64_t max = 1;
        if (GetInt(d, e, "max", max, false) && (max < 1 || max > 99)) d.Error("field 'max' must be 1..99");
        item.max_count = static_cast<int>(max);
        if (std::any_of(out.equipment.begin(), out.equipment.end(), [&](const EquipmentDef& o) { return o.id == item.id; })) {
            d.Error("duplicate equipment id");
        }
        out.equipment.push_back(std::move(item));
    }
}

void ParseEconomy(const Diag& file, const json& doc, EconomyConfig& eco) {
    const Diag d = file.Sub("");
    CheckKeys(d, doc, {"schema_version", "run", "rewards", "weapon_rarity", "upgrades", "supplies",
                       "credits", "rating", "damage", "shop", "run_map", "chests", "keys", "enemy_threat",
                       "run_rosters", "ladder", "refine", "_comment"});
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
            CheckKeys(kd, *k, {"perk", "weapon", "supplies", "item"});
            if (GetNumber(kd, *k, "perk", eco.rewards.perk_weight, true)) CheckRange(kd, "perk", eco.rewards.perk_weight, 0.0, 1.0e6);
            if (GetNumber(kd, *k, "weapon", eco.rewards.weapon_weight, true)) CheckRange(kd, "weapon", eco.rewards.weapon_weight, 0.0, 1.0e6);
            if (GetNumber(kd, *k, "supplies", eco.rewards.supplies_weight, true)) CheckRange(kd, "supplies", eco.rewards.supplies_weight, 0.0, 1.0e6);
            if (GetNumber(kd, *k, "item", eco.rewards.item_weight, false)) CheckRange(kd, "item", eco.rewards.item_weight, 0.0, 1.0e6);
            if (!(eco.rewards.perk_weight + eco.rewards.weapon_weight + eco.rewards.supplies_weight + eco.rewards.item_weight > 0.0)) {
                kd.Error("kind weights must have a positive total");
            }
        }
        if (GetInt(sd, *s, "supplies_bundle", eco.rewards.supplies_bundle, true) && eco.rewards.supplies_bundle < 1) {
            sd.Error("field 'supplies_bundle' must be >= 1");
        }
    }
    if (const json* s = Section(d, doc, "weapon_rarity")) {
        const Diag sd = d.Sub(": weapon_rarity");
        CheckKeys(sd, *s, {"common", "uncommon", "rare", "epic", "legendary", "exotic"});
        for (Rarity r : kAllRarities) {
            // Exotic is optional: without it refining stops at Legendary.
            const json* t = r == Rarity::Exotic ? Get(*s, RarityName(r)) : Section(sd, *s, RarityName(r));
            if (t == nullptr) continue;
            const Diag td = sd.Sub("." + std::string(RarityName(r)));
            CheckKeys(td, *t, {"affix_count", "magnitude_mult", "unique_count", "element_count"});
            EconomyConfig::WeaponRarityRule& rule = eco.weapon_rarity[RarityIndex(r)];
            GetIntRange(td, *t, "affix_count", rule.affix_count, true, 0, 5);
            if (GetNumber(td, *t, "magnitude_mult", rule.magnitude_mult, true)) {
                CheckRange(td, "magnitude_mult", rule.magnitude_mult, 0.0, 10.0, true);
            }
            GetIntRange(td, *t, "unique_count", rule.unique_count, false, 0, 3);
            GetIntRange(td, *t, "element_count", rule.element_count, false, 0, 5);
        }
    }
    if (const json* s = Get(doc, "refine")) {  // optional: without it no gun is refined
        const Diag sd = d.Sub(": refine");
        CheckKeys(sd, *s, {"_comment", "reward_chance", "reward_rating", "shop_chance", "chest_chance", "price",
                           "exotic_unlocked_by"});
        eco.refine.enabled = true;
        if (GetNumber(sd, *s, "reward_chance", eco.refine.reward_chance, false)) {
            CheckRange(sd, "reward_chance", eco.refine.reward_chance, 0.0, 1.0);
        }
        if (GetString(sd, *s, "reward_rating", eco.refine.reward_rating, false) && !ParseRating(eco.refine.reward_rating)) {
            sd.Error("unknown reward_rating " + Q(eco.refine.reward_rating) + " (S, A, B or C)");
        }
        if (GetNumber(sd, *s, "shop_chance", eco.refine.shop_chance, false)) CheckRange(sd, "shop_chance", eco.refine.shop_chance, 0.0, 1.0);
        if (GetNumber(sd, *s, "chest_chance", eco.refine.chest_chance, false)) {
            CheckRange(sd, "chest_chance", eco.refine.chest_chance, 0.0, 1.0);
        }
        ReadRarityPrices(sd, *s, "price", eco.refine.price);
        GetString(sd, *s, "exotic_unlocked_by", eco.refine.exotic_unlocked_by, false);
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
        CheckKeys(sd, *s, {"heal_fraction", "heal_price", "reroll_price", "perk_price", "weapon_price", "perk_offers",
                           "item_offers", "restock_price", "restock_price_step"});
        if (GetInt(sd, *s, "restock_price", eco.shop.restock_price, false) && eco.shop.restock_price < 0) {
            sd.Error("restock_price must be >= 0");
        }
        if (GetInt(sd, *s, "restock_price_step", eco.shop.restock_price_step, false) && eco.shop.restock_price_step < 0) {
            sd.Error("restock_price_step must be >= 0");
        }
        GetIntRange(sd, *s, "item_offers", eco.shop.item_offers, false, 0, 6);
        if (GetNumber(sd, *s, "heal_fraction", eco.shop.heal_fraction, true)) {
            CheckRange(sd, "heal_fraction", eco.shop.heal_fraction, 0.0, 1.0, true);
        }
        if (GetInt(sd, *s, "heal_price", eco.shop.heal_price, true) && eco.shop.heal_price < 0) sd.Error("heal_price must be >= 0");
        if (GetInt(sd, *s, "reroll_price", eco.shop.reroll_price, true) && eco.shop.reroll_price < 0) sd.Error("reroll_price must be >= 0");
        ReadRarityPrices(sd, *s, "perk_price", eco.shop.perk_price);
        ReadRarityPrices(sd, *s, "weapon_price", eco.shop.weapon_price);
        GetIntRange(sd, *s, "perk_offers", eco.shop.perk_offers, true, 0, 6);
    }
    if (Get(doc, "keys") != nullptr) {
        if (const json* s = Section(d, doc, "keys")) {
            const Diag kd = d.Sub(": keys");
            CheckKeys(kd, *s, {"enabled", "drop_chance", "classes", "start", "max"});
            GetBool(kd, *s, "enabled", eco.keys.enabled, false);
            if (GetNumber(kd, *s, "drop_chance", eco.keys.drop_chance, false)) CheckRange(kd, "drop_chance", eco.keys.drop_chance, 0.0, 1.0);
            GetStringList(kd, *s, "classes", eco.keys.classes, false);
            std::int64_t start = eco.keys.start, max = eco.keys.max;
            if (GetInt(kd, *s, "start", start, false)) CheckRange(kd, "start", static_cast<double>(start), 0, 99);
            if (GetInt(kd, *s, "max", max, false)) CheckRange(kd, "max", static_cast<double>(max), 1, 99);
            eco.keys.start = static_cast<int>(start);
            eco.keys.max = static_cast<int>(max);
            if (eco.keys.enabled && eco.keys.classes.empty()) kd.Error("enabled keys need at least one enemy class prefix");
        }
    }
    if (Get(doc, "chests") != nullptr) {
        if (const json* s = Section(d, doc, "chests")) {
            const Diag sd = d.Sub(": chests");
            CheckKeys(sd, *s, {"per_arena", "supplies_base", "supplies_per_arena", "item_chance", "weapon_chance"});
            std::int64_t perArena = eco.chests.per_arena;
            if (GetInt(sd, *s, "per_arena", perArena, false) && (perArena < 0 || perArena > 16)) sd.Error("per_arena must be 0..16");
            eco.chests.per_arena = static_cast<int>(perArena);
            if (GetInt(sd, *s, "supplies_base", eco.chests.supplies_base, false) && eco.chests.supplies_base < 0) {
                sd.Error("supplies_base must be >= 0");
            }
            if (GetInt(sd, *s, "supplies_per_arena", eco.chests.supplies_per_arena, false) && eco.chests.supplies_per_arena < 0) {
                sd.Error("supplies_per_arena must be >= 0");
            }
            if (GetNumber(sd, *s, "item_chance", eco.chests.item_chance, false)) CheckRange(sd, "item_chance", eco.chests.item_chance, 0.0, 1.0);
            if (GetNumber(sd, *s, "weapon_chance", eco.chests.weapon_chance, false)) CheckRange(sd, "weapon_chance", eco.chests.weapon_chance, 0.0, 1.0);
            if (eco.chests.item_chance + eco.chests.weapon_chance > 1.0) sd.Error("item_chance + weapon_chance must be <= 1");
        }
    }
    if (Get(doc, "enemy_threat") != nullptr) {
        if (const json* s = Section(d, doc, "enemy_threat")) {
            const Diag td = d.Sub(": enemy_threat");
            CheckKeys(td, *s, {"enabled", "later_arena_bonus", "max_level", "max_alive_by_level", "specials_from_level",
                               "default", "weapons", "classes"});
            EconomyConfig::EnemyThreat& t = eco.enemy_threat;
            GetBool(td, *s, "enabled", t.enabled, false);
            GetIntRange(td, *s, "max_level", t.max_level, false, 1, 9);
            GetIntRange(td, *s, "later_arena_bonus", t.later_arena_bonus, false, 0, 9);
            GetIntRange(td, *s, "specials_from_level", t.specials_from_level, false, 1, 10);
            GetIntRange(td, *s, "default", t.table.fallback, false, 1, t.max_level);
            auto level = [&](const Diag& ld, const json& v, const std::string& what, int& out) {
                if (!v.is_number_integer() || v.get<std::int64_t>() < 1 || v.get<std::int64_t>() > t.max_level) {
                    ld.Error(what + " must be a level 1.." + std::to_string(t.max_level));
                    return false;
                }
                out = v.get<int>();
                return true;
            };
            if (const json* m = Get(*s, "max_alive_by_level")) {
                if (!m->is_array() || m->size() != static_cast<std::size_t>(t.max_level)) {
                    td.Error("max_alive_by_level must hold one count per level (" + std::to_string(t.max_level) + ")");
                } else {
                    for (const json& v : *m) {
                        if (!v.is_number_integer() || v.get<std::int64_t>() < 1 || v.get<std::int64_t>() > 40) {
                            td.Error("max_alive_by_level entries must be 1..40");
                            break;
                        }
                        t.max_alive_by_level.push_back(v.get<int>());
                    }
                }
            }
            if (const json* w = Get(*s, "weapons")) {
                if (!w->is_object()) {
                    td.Error("weapons must be an object");
                } else {
                    for (const auto& [key, value] : w->items()) {
                        int lv = 0;
                        if (!level(td, value, "weapon " + Q(key), lv)) continue;
                        if (key == "none") {
                            t.table.none = lv;
                            continue;
                        }
                        const bool hex = key.size() == 2 && std::all_of(key.begin(), key.end(), [](char c) {
                            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                        });
                        if (!hex) td.Error("weapon " + Q(key) + " must be \"none\" or two lower-case hex digits");
                        else t.table.weapons[std::stoi(key, nullptr, 16)] = lv;
                    }
                }
            }
            if (const json* c = Get(*s, "classes")) {
                if (!c->is_object()) {
                    td.Error("classes must be an object");
                } else {
                    for (const auto& [key, value] : c->items()) {
                        int lv = 0;
                        if (key.rfind("uEm", 0) != 0) td.Error("class " + Q(key) + " must be an enemy class (uEm..)");
                        else if (level(td, value, "class " + Q(key), lv)) t.table.classes[key] = lv;
                    }
                }
            }
            if (t.specials_from_level > t.max_level + 1) td.Error("specials_from_level must be at most max_level + 1");
        }
    }
    if (Get(doc, "run_rosters") != nullptr) {
        if (const json* s = Section(d, doc, "run_rosters")) {
            const Diag rd = d.Sub(": run_rosters");
            CheckKeys(rd, *s, {"enabled", "from_arena", "normal_guests", "special_guests", "max_classes", "normal_share",
                               "special_share", "guests"});
            EconomyConfig::RunRosters& r = eco.run_rosters;
            GetBool(rd, *s, "enabled", r.enabled, false);
            GetIntRange(rd, *s, "from_arena", r.from_arena, false, 1, 9);
            GetIntRange(rd, *s, "normal_guests", r.normal_guests, false, 0, 3);
            GetIntRange(rd, *s, "special_guests", r.special_guests, false, 0, 3);
            GetIntRange(rd, *s, "max_classes", r.max_classes, false, 1, 12);
            // Below 1: a share of 1 would leave the map's own enemies no picks at all.
            if (GetNumber(rd, *s, "normal_share", r.normal_share, false)) CheckRange(rd, "normal_share", r.normal_share, 0.0, 0.9);
            if (GetNumber(rd, *s, "special_share", r.special_share, false)) CheckRange(rd, "special_share", r.special_share, 0.0, 0.9);
            if (const json* g = Get(*s, "guests")) {
                if (!g->is_object()) {
                    rd.Error("guests must be an object (enemy class -> home map key)");
                } else {
                    for (const auto& [key, value] : g->items()) {
                        if (key.rfind("uEm", 0) != 0) rd.Error("guest " + Q(key) + " must be an enemy class (uEm..)");
                        else if (!value.is_number_integer()) rd.Error("guest " + Q(key) + " needs its home map key (a number)");
                        else r.guests[key] = value.get<int>();
                    }
                }
            }
            if (r.enabled && r.guests.empty()) rd.Error("enabled run_rosters need at least one guest class");
        }
    }
    if (Get(doc, "ladder") != nullptr) {
        if (const json* s = Section(d, doc, "ladder")) {
            const Diag ld = d.Sub(": ladder");
            CheckKeys(ld, *s, {"enabled", "lengths", "finale", "growth_from_arena", "incoming_per_arena", "toughness_per_arena",
                               "budget_per_arena"});
            EconomyConfig::Ladder& l = eco.ladder;
            GetBool(ld, *s, "enabled", l.enabled, false);
            GetIntRange(ld, *s, "growth_from_arena", l.growth_from_arena, false, 2, 20);
            if (GetNumber(ld, *s, "incoming_per_arena", l.incoming_per_arena, false)) CheckRange(ld, "incoming_per_arena", l.incoming_per_arena, 0.0, 1.0);
            if (GetNumber(ld, *s, "toughness_per_arena", l.toughness_per_arena, false)) CheckRange(ld, "toughness_per_arena", l.toughness_per_arena, 0.0, 1.0);
            GetIntRange(ld, *s, "budget_per_arena", l.budget_per_arena, false, 0, 20);
            if (const json* lengths = Get(*s, "lengths")) {
                if (!lengths->is_array()) ld.Error("lengths must be an array");
                else {
                    for (const json& e : *lengths) {
                        const Diag ed = ld.Sub(": lengths");
                        CheckKeys(ed, e, {"id", "name", "arenas", "easy_boss", "hard_boss", "unlocked_by", "boss_hp_mult"});
                        EconomyConfig::Ladder::Length len;
                        GetString(ed, e, "id", len.id, true);
                        GetString(ed, e, "name", len.name, true);
                        GetIntRange(ed, e, "arenas", len.arenas, true, 2, 20);
                        GetIntRange(ed, e, "easy_boss", len.easy_boss, true, 0, 9999);
                        // One boss map key or a list of them.
                        if (const auto hb = e.find("hard_boss"); hb != e.end()) {
                            const json list = hb->is_array() ? *hb : json::array({*hb});
                            for (const json& k : list) {
                                if (!k.is_number_integer() || k.get<std::int64_t>() < 0 || k.get<std::int64_t>() > 9999) {
                                    ed.Error("hard_boss must be a map key 0..9999 or a list of them");
                                } else if (!len.IsHard(k.get<int>())) {
                                    len.hard_bosses.push_back(k.get<int>());
                                }
                            }
                        }
                        GetString(ed, e, "unlocked_by", len.unlocked_by, false);
                        if (GetNumber(ed, e, "boss_hp_mult", len.boss_hp_mult, false)) CheckRange(ed, "boss_hp_mult", len.boss_hp_mult, 0.1, 10.0);
                        if (l.Find(len.id) != nullptr) ed.Error("duplicate length " + Q(len.id));
                        l.lengths.push_back(std::move(len));
                    }
                }
            }
            if (const json* f = Get(*s, "finale")) {
                const Diag fd = ld.Sub(": finale");
                CheckKeys(fd, *f, {"boss", "after", "needs_hard", "boss_hp_mult"});
                GetIntRange(fd, *f, "boss", l.finale.boss, false, -1, 9999);
                GetString(fd, *f, "after", l.finale.after, false);
                GetStringList(fd, *f, "needs_hard", l.finale.needs_hard, false);
                if (GetNumber(fd, *f, "boss_hp_mult", l.finale.boss_hp_mult, false)) CheckRange(fd, "boss_hp_mult", l.finale.boss_hp_mult, 0.1, 10.0);
            }
            if (l.enabled && l.lengths.empty()) ld.Error("an enabled ladder needs at least one length");
            if (!l.lengths.empty() && !l.lengths.front().unlocked_by.empty()) ld.Error("the first length must be open from the start (no unlocked_by)");
            for (const auto& len : l.lengths) {
                if (len.unlocked_by.empty()) continue;
                const auto colon = len.unlocked_by.find(':');
                const auto* base = colon == std::string::npos ? nullptr : l.Find(len.unlocked_by.substr(0, colon));
                if (base == nullptr) ld.Error("length " + Q(len.id) + " is unlocked by unknown key " + Q(len.unlocked_by));
            }
            // A run fights a boss at every length's arenas: the lengths must grow.
            for (std::size_t i = 1; i < l.lengths.size(); ++i) {
                if (l.lengths[i].arenas <= l.lengths[i - 1].arenas) {
                    ld.Error("length " + Q(l.lengths[i].id) + " must have more arenas than " + Q(l.lengths[i - 1].id));
                }
            }
            if (l.finale.boss >= 0) {
                const auto* after = l.Find(l.finale.after);
                if (after == nullptr) ld.Error("finale.after names unknown length " + Q(l.finale.after));
                for (const std::string& id : l.finale.needs_hard) {
                    const auto* tier = l.Find(id);
                    if (tier == nullptr || tier->hard_bosses.empty() || (after != nullptr && tier->arenas >= after->arenas)) {
                        ld.Error("finale.needs_hard: " + Q(id) + " must be a shorter length with a hard boss");
                    }
                }
            }
        }
    }
    if (const json* s = Section(d, doc, "run_map")) {
        const Diag sd = d.Sub(": run_map");
        CheckKeys(sd, *s, {"width"});
        GetIntRange(sd, *s, "width", eco.run_map.width, true, 1, 5);
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
            ParseWeapons(file("weapons.json"), *doc, content->weapons_, content->affixes_, content->uniques_,
                         content->elements_);
        }
        if (auto doc = ParseDocument(file("characters.json"), src.characters)) {
            ParseCharacters(file("characters.json"), *doc, content->characters_);
        }
        if (auto doc = ParseDocument(file("routes.json"), src.routes)) ParseRoutes(file("routes.json"), *doc, content->routes_);
        if (auto doc = ParseDocument(file("economy.json"), src.economy)) ParseEconomy(file("economy.json"), *doc, content->economy_);
        if (!src.loadouts.empty()) {
            if (auto doc = ParseDocument(file("loadouts.json"), src.loadouts)) ParseLoadouts(file("loadouts.json"), *doc, content->loadouts_);
        }
        if (!src.maps.empty()) {
            if (auto doc = ParseDocument(file("maps.json"), src.maps)) ParseMaps(file("maps.json"), *doc, content->maps_);
        }

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
            if (p.build) {
                for (const std::string& id : p.build->never_offered.ids) {
                    if (c.FindWeapon(id) == nullptr) d.Error("build never_offered references unknown weapon " + Q(id));
                }
                if (p.category == PerkCategory::Curse || p.category == PerkCategory::Skin) {
                    d.Error("a build is for perks chosen in the menu (style, supply, support)");
                }
            }
        }
        for (const EquipmentDef& e : c.loadouts_.equipment) {
            for (const std::string& id : e.for_weapons) {
                if (c.FindWeapon(id) == nullptr) {
                    Diag{&errors, &warnings, "loadouts.json"}.Sub(": equipment " + Q(e.id)).Error("for_weapons references unknown weapon " + Q(id));
                }
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
        for (const CharacterDef& base : c.characters_) {
          // The character itself and every skin's variant of it must be playable.
          std::vector<std::pair<std::string, CharacterDef>> variants;
          variants.emplace_back(std::string(), base);
          for (const SkinDef& s : base.skins) variants.emplace_back(" skin " + Q(s.id), ApplySkin(base, s));
          for (const auto& [suffix, ch] : variants) {
            const Diag d = chars_d.Sub(": character " + Q(base.id) + suffix);
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
          }
          for (const SkinDef& s : base.skins) {
            const Diag d = chars_d.Sub(": character " + Q(base.id) + " skin " + Q(s.id) + ": build");
            const SkinBuild& b = s.build;
            for (const WeaponFilter* f : {&b.offered_weapons, &b.usable_weapons, &b.uncapped_upgrades, &b.never_offered}) {
                for (const std::string& id : f->ids) {
                    if (c.FindWeapon(id) == nullptr) d.Error("references unknown weapon " + Q(id));
                }
            }
            for (const std::string& id : b.extra_weapons) {
                const WeaponBaseDef* w = c.FindWeapon(id);
                if (w == nullptr) {
                    d.Error("extra_weapons references unknown weapon " + Q(id));
                } else if (!w->build_only) {
                    d.Error("extra_weapons lists " + Q(id) + ", which is not build_only (every build is offered it)");
                }
            }
            if (!b.shop_weapon.empty()) {
                const WeaponBaseDef* w = c.FindWeapon(b.shop_weapon);
                if (w == nullptr) {
                    d.Error("shop_weapon references unknown weapon " + Q(b.shop_weapon));
                } else if (w->build_only && std::find(b.extra_weapons.begin(), b.extra_weapons.end(), w->id) == b.extra_weapons.end()) {
                    d.Error("shop_weapon " + Q(w->id) + " is build_only and not in the build's extra_weapons");
                }
            }
            if (b.drops_as >= 0) {
                const auto& eq = c.loadouts().equipment;
                if (std::none_of(eq.begin(), eq.end(), [&b](const EquipmentDef& e) { return e.game_item == b.drops_as; })) {
                    d.Error("drops_as names a game item no loadouts.json equipment hands out");
                }
            }
            if ((!b.special.kind.empty() && b.special.cooldown_seconds <= 0.0 && b.special.every_arenas <= 0) ||
                (!b.special_2.kind.empty() && b.special_2.cooldown_seconds <= 0.0 && b.special_2.every_arenas <= 0)) {
                d.Error("special has neither a cooldown nor every_arenas (it could be used without limit)");
            }
            if (!b.starting_weapon.empty()) {
                const WeaponBaseDef* w = c.FindWeapon(b.starting_weapon);
                if (w == nullptr) {
                    d.Error("starting_weapon references unknown weapon " + Q(b.starting_weapon));
                } else if (!b.usable_weapons.Allows(w->id, WeaponCategoryName(w->category))) {
                    d.Error("starting_weapon " + Q(w->id) + " is not one of the build's usable weapons");
                }
            }
          }
          if (base.enabled && base.unlock_cost_credits == 0 && base.unlock.kind == ChallengeKind::None) any_free = true;
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
        // A guest travels as its home map's roster entry (ROADMAP M3.3).
        for (const auto& [unit_class, home] : c.economy_.run_rosters.guests) {
            const MapDef* m = c.FindMapByStage(home);
            const std::string what = "run_rosters.guests." + unit_class;
            if (m == nullptr) {
                eco_d.Error(what + " references unknown map " + std::to_string(home));
            } else if (std::none_of(m->roster.begin(), m->roster.end(), [&](const MapEnemy& e) { return e.unit_class == unit_class; })) {
                eco_d.Error(what + ": map " + std::to_string(home) + " (" + m->name + ") has no " + unit_class + " in its roster");
            }
        }
        // The ladder's bosses are boss maps.
        if (c.economy_.ladder.enabled) {
            auto bossMap = [&](int key, const std::string& what) {
                if (key < 0) return;
                const MapDef* m = c.FindMapByStage(key);
                if (m == nullptr) eco_d.Error("ladder " + what + " references unknown map " + std::to_string(key));
                else if (!m->boss) eco_d.Error("ladder " + what + ": map " + std::to_string(key) + " (" + m->name + ") has no boss");
            };
            for (const auto& len : c.economy_.ladder.lengths) {
                bossMap(len.easy_boss, len.id + ".easy_boss");
                for (const int hard : len.hard_bosses) bossMap(hard, len.id + ".hard_boss");
            }
            bossMap(c.economy_.ladder.finale.boss, "finale.boss");
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
    // Optional: defaults apply when the file is absent.
    if (std::filesystem::exists(directory / "loadouts.json")) (void)ReadFile(directory / "loadouts.json", src.loadouts);
    if (std::filesystem::exists(directory / "maps.json")) (void)ReadFile(directory / "maps.json", src.maps);
    for (const auto& [name, target] : files) {
        const std::filesystem::path p = directory / name;
        if (!ReadFile(p, *target)) io_errors.push_back(std::string(name) + ": cannot read " + rm::PathText(p));
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
