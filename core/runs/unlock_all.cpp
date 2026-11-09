#include "rm/unlock_all.h"

#include <string>

#include "rm/content.h"
#include "rm/profile.h"

namespace rm {

std::string_view ProfileFileName(bool everything_unlocked) { return everything_unlocked ? "profile.unlocked.json" : "profile.json"; }

void UnlockEverything(Profile& profile, const Content& content) {
    for (const CharacterDef& character : content.characters()) {
        profile.unlocked_characters.insert(character.id);
        for (const SkinDef& skin : character.skins) profile.unlocked_characters.insert(skin.id);  // skins share the set
    }
    for (const PerkDef& perk : content.perks()) profile.unlocked_perks.insert(perk.id);
    for (const WeaponBaseDef& weapon : content.weapons()) profile.unlocked_weapons.insert(weapon.id);
    const EconomyConfig& economy = content.economy();
    for (const auto& length : economy.ladder.lengths) {
        profile.stats.bosses_beaten.insert(length.id + ":easy");
        profile.stats.bosses_beaten.insert(length.id + ":hard");
    }
    profile.stats.bosses_beaten.insert("finale");
    if (!economy.refine.exotic_unlocked_by.empty()) profile.stats.bosses_beaten.insert(economy.refine.exotic_unlocked_by);
    (void)profile.credits.Credit("test:everything_unlocked", 99999);  // once: the ledger keeps the id
}

}  // namespace rm
