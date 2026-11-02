#pragma once

#include <cstdint>

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

/// Developer: logs every set record of the stage (enemies, objects, items: class, descriptor
/// class and position), whether or not it was ever spawned. Returns the records logged.
int DumpSetRecords();

/// Developer survey: logs every object the stage creates (class, set record, position, the
/// fields the spawn sets, the descriptor's class and contents).
void SetSurvey(bool on);

}  // namespace rmre5::world
