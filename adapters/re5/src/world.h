#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// World objects (ROADMAP.md, milestone M4): the stage's map objects (uOm..., children of
// uObjModel) are created by the object manager sObjModel ([0x011B2720], vtable 0x00F65240)
// from the stage's set records: its set spawn (slot 13, 0x007E2480) looks the record up,
// creates the class through Create (slot 11, 0x007E2410: dti, group, mode, flag), applies the
// record's descriptor and links the unit, like the enemy manager does for enemies. Items
// (dropped ammo, herbs, weapons) come from sItem ([0x011B2200]) the same way. The Mercenaries
// time bonus is the breakable statue uOm0100 (a uOmHourGlass action; seconds at +0x1364).
namespace rmre5::world {

/// Queues the hooks on the object set spawn and create. Requires their signatures.
bool Install();
bool Available();

/// Places a time-bonus statue (uOm0100: a chest in a run) at `pos` the way the stage's set
/// spawn places its own: a cSetInfoInsModel descriptor with the game's values (`seconds`, its
/// bonus), sObjModel::Create (group 12, mode 1), the descriptor applied, the unit linked.
/// Returns the object or 0.
std::uintptr_t PlaceChest(const float pos[3], int seconds);

/// While blocked, the stage's own time-bonus statues (uOm0100 set records) are not created:
/// a run places its chests itself.
void BlockNativeChests(bool blocked);

/// What the player gets for the items they pick up from the ground (M8 builds, style perks):
/// `swaps` first (Rebecca's Chemist: a green herb is a mixed herb), then `all` (Safari Chris:
/// ammo, recovery items and grenades are one mine each), else with `heldAmmo` set ammo and
/// recovery items become `heldAmmo()`'s item (Sheva's Scavenger: ammo for the gun in hand; 0
/// leaves the item as it is). Last, with `ownAmmo` set, ammo becomes `ownAmmo(id)`'s item when that
/// is not 0 (Josh's backup: ammo for the partner's guns becomes the player's). A changed item comes
/// in the new item's own pickup amount.
struct DropRules {
    int all = -1;
    std::uint32_t (*heldAmmo)() = nullptr;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> swaps;
    std::uint32_t (*ownAmmo)(std::uint32_t id) = nullptr;
};
void SetDropRules(const DropRules& rules);

/// Developer: logs every set record of the stage (enemies, objects, items: class, descriptor
/// class and position), whether or not it was ever spawned. Returns the records logged.
int DumpSetRecords();

/// Developer survey: logs every object the stage creates (class, set record, position, the
/// fields the spawn sets, the descriptor's class and contents).
void SetSurvey(bool on);

/// Developer: units of class `cls` sItem creates from now on are collected (empty: none);
/// TakeCreated returns them and starts over (the twin guns spike's shots).
void WatchCreated(const std::string& cls);
std::vector<std::uintptr_t> TakeCreated();

}  // namespace rmre5::world
