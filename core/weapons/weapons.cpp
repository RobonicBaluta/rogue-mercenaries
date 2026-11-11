#include "rm/weapons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rm/content.h"

namespace rm {
namespace {

double RoundTo(double v, double scale) { return std::round(v * scale) / scale; }

/// Compact number: up to `decimals` places, trailing zeros removed.
std::string Num(double v, int decimals = 1) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

}  // namespace

std::string_view WeaponCategoryName(WeaponCategory c) {
    switch (c) {
        case WeaponCategory::Handgun: return "handgun";
        case WeaponCategory::Shotgun: return "shotgun";
        case WeaponCategory::Smg: return "smg";
        case WeaponCategory::Rifle: return "rifle";
        case WeaponCategory::MachineGun: return "machine_gun";
        case WeaponCategory::Magnum: return "magnum";
        case WeaponCategory::Bow: return "bow";
        case WeaponCategory::Launcher: return "launcher";
        case WeaponCategory::Melee: return "melee";
        case WeaponCategory::Flamethrower: return "flamethrower";
    }
    return "handgun";
}

std::optional<WeaponCategory> ParseWeaponCategory(std::string_view name) {
    for (WeaponCategory c : {WeaponCategory::Handgun, WeaponCategory::Shotgun, WeaponCategory::Smg,
                             WeaponCategory::Rifle, WeaponCategory::MachineGun,
                             WeaponCategory::Magnum, WeaponCategory::Bow, WeaponCategory::Launcher,
                             WeaponCategory::Melee, WeaponCategory::Flamethrower}) {
        if (WeaponCategoryName(c) == name) return c;
    }
    return std::nullopt;
}

std::string_view AffixKindName(AffixKind k) {
    switch (k) {
        case AffixKind::DamagePct: return "damage_pct";
        case AffixKind::CritChance: return "crit_chance";
        case AffixKind::CritMultPct: return "crit_mult_pct";
        case AffixKind::LowHpTargetDamagePct: return "low_hp_target_damage_pct";
        case AffixKind::SuppliesOnKill: return "supplies_on_kill";
        case AffixKind::UniqueLowHpTargetBonus: return "low_hp_target_bonus";
        case AffixKind::UniqueKillStreakExtend: return "kill_streak_extend";
        case AffixKind::UniqueExecute: return "execute";
        case AffixKind::UniqueDragonsBreath: return "dragons_breath";
        case AffixKind::UniqueChainLightning: return "chain_lightning";
        case AffixKind::UniqueBottomless: return "bottomless";
        case AffixKind::UniqueOverkill: return "overkill";
        case AffixKind::UniqueLastWord: return "last_word";
        case AffixKind::UniqueBloodthirst: return "bloodthirst";
    }
    return "damage_pct";
}

std::string_view ElementName(Element e) {
    switch (e) {
        case Element::Incendiary: return "incendiary";
        case Element::Shock: return "shock";
        case Element::Cryo: return "cryo";
        case Element::Explosive: return "explosive";
        case Element::Toxic: return "toxic";
    }
    return "incendiary";
}

std::optional<Element> ParseElement(std::string_view name) {
    for (Element e : kAllElements) {
        if (ElementName(e) == name) return e;
    }
    return std::nullopt;
}

std::optional<AffixKind> ParseAffixKind(std::string_view name) {
    for (AffixKind k : {AffixKind::DamagePct, AffixKind::CritChance, AffixKind::CritMultPct,
                        AffixKind::LowHpTargetDamagePct, AffixKind::SuppliesOnKill}) {
        if (AffixKindName(k) == name) return k;
    }
    return std::nullopt;
}

std::optional<AffixKind> ParseUniqueEffect(std::string_view name) {
    for (AffixKind k : {AffixKind::UniqueLowHpTargetBonus, AffixKind::UniqueKillStreakExtend, AffixKind::UniqueExecute,
                        AffixKind::UniqueDragonsBreath, AffixKind::UniqueChainLightning, AffixKind::UniqueBottomless,
                        AffixKind::UniqueOverkill, AffixKind::UniqueLastWord, AffixKind::UniqueBloodthirst}) {
        if (AffixKindName(k) == name) return k;
    }
    return std::nullopt;
}

bool IsUniqueAffix(AffixKind k) {
    switch (k) {
        case AffixKind::DamagePct:
        case AffixKind::CritChance:
        case AffixKind::CritMultPct:
        case AffixKind::LowHpTargetDamagePct:
        case AffixKind::SuppliesOnKill: return false;
        default: return true;
    }
}

WeaponInstance MakeStartingWeapon(std::string_view base_id, std::string instance_id) {
    WeaponInstance w;
    w.instance_id = std::move(instance_id);
    w.base_id = std::string(base_id);
    return w;
}

namespace {

double RoundAffix(AffixKind kind, double v) {
    switch (kind) {
        case AffixKind::SuppliesOnKill: return std::max(0.05, RoundTo(v, 20.0));  // kills pay it exactly (KillShare)
        case AffixKind::CritChance: return RoundTo(v, 1000.0);
        default: return RoundTo(v, 100.0);
    }
}

// Adds what `rarity` has and `w` lacks: regular affixes up to its count (kinds not yet on the
// gun), legendary effects (ids not yet on it), then elements. The elements are drawn last, so a
// gun of a tier without any draws as it did before the rarity remake.
void FillToRarity(Rng& rng, const Content& content, const Capabilities& caps, WeaponInstance& w, Rarity rarity) {
    const EconomyConfig::WeaponRarityRule& rule = content.economy().weapon_rarity[RarityIndex(rarity)];
    int regular = 0;
    int uniques_on = 0;
    for (const Affix& a : w.affixes) {
        if (IsUniqueAffix(a.kind)) {
            ++uniques_on;
        } else {
            ++regular;
        }
    }

    std::vector<const AffixDef*> pool;
    for (const AffixDef& a : content.affixes()) {
        const bool held = std::any_of(w.affixes.begin(), w.affixes.end(), [&a](const Affix& x) { return x.kind == a.kind; });
        if (!held && caps.HasAll(a.requires_capabilities)) pool.push_back(&a);
    }
    for (int i = regular; i < rule.affix_count && !pool.empty(); ++i) {
        const std::size_t k = rng.Index(pool.size());
        const AffixDef* def = pool[k];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(k));
        Affix affix;
        affix.kind = def->kind;
        affix.threshold = def->threshold;
        affix.value = RoundAffix(def->kind, rng.UniformReal(def->min, def->max) * rule.magnitude_mult);
        w.affixes.push_back(std::move(affix));
    }

    std::vector<const UniqueAffixDef*> uniques;
    for (const UniqueAffixDef& u : content.unique_affixes()) {
        const bool held = std::any_of(w.affixes.begin(), w.affixes.end(), [&u](const Affix& x) { return x.unique_id == u.id; });
        if (!held && caps.HasAll(u.requires_capabilities)) uniques.push_back(&u);
    }
    for (int i = uniques_on; i < rule.unique_count && !uniques.empty(); ++i) {
        const std::size_t k = rng.Index(uniques.size());
        const UniqueAffixDef* def = uniques[k];
        uniques.erase(uniques.begin() + static_cast<std::ptrdiff_t>(k));
        Affix affix;
        affix.kind = def->kind;
        affix.value = def->value;
        affix.threshold = def->threshold;
        affix.unique_id = def->id;
        w.affixes.push_back(std::move(affix));
    }

    std::vector<Element> elements;
    for (const ElementDef& e : content.elements()) {
        if (std::find(w.elements.begin(), w.elements.end(), e.element) == w.elements.end()) elements.push_back(e.element);
    }
    for (int i = static_cast<int>(w.elements.size()); i < rule.element_count && !elements.empty(); ++i) {
        const std::size_t k = rng.Index(elements.size());
        w.elements.push_back(elements[k]);
        elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

}  // namespace

WeaponInstance GenerateWeapon(Rng& rng, const Content& content, const Capabilities& caps,
                              std::string_view base_id, Rarity rarity, std::string instance_id) {
    WeaponInstance w;
    w.instance_id = std::move(instance_id);
    w.base_id = std::string(base_id);
    w.rarity = rarity;
    FillToRarity(rng, content, caps, w, rarity);
    return w;
}

WeaponInstance RefineWeapon(Rng& rng, const Content& content, const Capabilities& caps, const WeaponInstance& w) {
    if (w.rarity == Rarity::Exotic) return w;
    WeaponInstance out = w;
    const Rarity to = kAllRarities[RarityIndex(w.rarity) + 1];
    const auto& rules = content.economy().weapon_rarity;
    const double from_mult = rules[RarityIndex(w.rarity)].magnitude_mult;
    const double scale = from_mult > 0.0 ? rules[RarityIndex(to)].magnitude_mult / from_mult : 1.0;
    for (Affix& a : out.affixes) {
        if (!IsUniqueAffix(a.kind)) a.value = RoundAffix(a.kind, a.value * scale);
    }
    out.rarity = to;
    FillToRarity(rng, content, caps, out, to);
    return out;
}

std::string DescribeAffix(const Affix& affix, const Content* content) {
    std::string unique_name = affix.unique_id;
    if (content != nullptr && !affix.unique_id.empty()) {
        if (const UniqueAffixDef* u = content->FindUnique(affix.unique_id)) unique_name = u->name;
    }
    switch (affix.kind) {
        case AffixKind::DamagePct: return "+" + Num(affix.value) + "% damage";
        case AffixKind::CritChance: return "+" + Num(affix.value * 100.0) + "% crit chance";
        case AffixKind::CritMultPct: return "+" + Num(affix.value) + "% crit damage";
        case AffixKind::LowHpTargetDamagePct:
            return "+" + Num(affix.value) + "% damage vs targets below " +
                   Num(affix.threshold * 100.0) + "% HP";
        case AffixKind::SuppliesOnKill: return "+" + Num(affix.value, 2) + (affix.value == 1.0 ? " supply per kill" : " supplies per kill");
        case AffixKind::UniqueLowHpTargetBonus:
            return unique_name + ": +" + Num(affix.value * 100.0) + "% damage vs targets below " +
                   Num(affix.threshold * 100.0) + "% HP";
        case AffixKind::UniqueKillStreakExtend:
            return unique_name + ": kills extend kill-streak windows by " + Num(affix.value) + "s";
        case AffixKind::UniqueExecute:
        case AffixKind::UniqueDragonsBreath:
        case AffixKind::UniqueChainLightning:
        case AffixKind::UniqueBottomless:
        case AffixKind::UniqueOverkill:
        case AffixKind::UniqueLastWord:
        case AffixKind::UniqueBloodthirst: {
            const UniqueAffixDef* u = content != nullptr ? content->FindUnique(affix.unique_id) : nullptr;
            return u != nullptr && !u->description.empty() ? unique_name + ": " + u->description : unique_name;
        }
    }
    return "?";
}

std::string DescribeElement(Element e, const Content* content) {
    const ElementDef* d = content != nullptr ? content->FindElement(e) : nullptr;
    if (d == nullptr) return std::string(ElementName(e));
    return d->description.empty() ? d->name : d->name + ": " + d->description;
}

}  // namespace rm
