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
    }
    return "damage_pct";
}

std::optional<AffixKind> ParseAffixKind(std::string_view name) {
    for (AffixKind k : {AffixKind::DamagePct, AffixKind::CritChance, AffixKind::CritMultPct,
                        AffixKind::LowHpTargetDamagePct, AffixKind::SuppliesOnKill}) {
        if (AffixKindName(k) == name) return k;
    }
    return std::nullopt;
}

std::optional<AffixKind> ParseUniqueEffect(std::string_view name) {
    for (AffixKind k : {AffixKind::UniqueLowHpTargetBonus, AffixKind::UniqueKillStreakExtend}) {
        if (AffixKindName(k) == name) return k;
    }
    return std::nullopt;
}

bool IsUniqueAffix(AffixKind k) {
    return k == AffixKind::UniqueLowHpTargetBonus || k == AffixKind::UniqueKillStreakExtend;
}

WeaponInstance MakeStartingWeapon(std::string_view base_id, std::string instance_id) {
    WeaponInstance w;
    w.instance_id = std::move(instance_id);
    w.base_id = std::string(base_id);
    return w;
}

WeaponInstance GenerateWeapon(Rng& rng, const Content& content, const Capabilities& caps,
                              std::string_view base_id, Rarity rarity, std::string instance_id) {
    WeaponInstance w;
    w.instance_id = std::move(instance_id);
    w.base_id = std::string(base_id);
    w.rarity = rarity;
    const EconomyConfig::WeaponRarityRule& rule =
        content.economy().weapon_rarity[RarityIndex(rarity)];

    std::vector<const AffixDef*> pool;
    for (const AffixDef& a : content.affixes()) {
        if (caps.HasAll(a.requires_capabilities)) pool.push_back(&a);
    }
    for (int i = 0; i < rule.affix_count && !pool.empty(); ++i) {
        const std::size_t k = rng.Index(pool.size());
        const AffixDef* def = pool[k];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(k));
        Affix affix;
        affix.kind = def->kind;
        affix.threshold = def->threshold;
        const double rolled = rng.UniformReal(def->min, def->max) * rule.magnitude_mult;
        switch (def->kind) {
            case AffixKind::SuppliesOnKill: affix.value = std::max(1.0, std::round(rolled)); break;
            case AffixKind::CritChance: affix.value = RoundTo(rolled, 1000.0); break;
            default: affix.value = RoundTo(rolled, 100.0); break;
        }
        w.affixes.push_back(std::move(affix));
    }

    std::vector<const UniqueAffixDef*> uniques;
    for (const UniqueAffixDef& u : content.unique_affixes()) {
        if (caps.HasAll(u.requires_capabilities)) uniques.push_back(&u);
    }
    for (int i = 0; i < rule.unique_count && !uniques.empty(); ++i) {
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
    return w;
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
        case AffixKind::SuppliesOnKill: return "+" + Num(affix.value, 0) + " supplies per kill";
        case AffixKind::UniqueLowHpTargetBonus:
            return unique_name + ": +" + Num(affix.value * 100.0) + "% damage vs targets below " +
                   Num(affix.threshold * 100.0) + "% HP";
        case AffixKind::UniqueKillStreakExtend:
            return unique_name + ": kills extend kill-streak windows by " + Num(affix.value) + "s";
    }
    return "?";
}

}  // namespace rm
