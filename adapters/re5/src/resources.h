#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Resources (ROADMAP.md, milestone M3): the models a unit needs come from its class archive,
// Image\Archive\<class>.arc (uEm11.arc, uWp1a.arc). The game's loader ([0x011B26C4]) keeps
// lists of archive requests that every area load rebuilds: list 3 the players and their
// items (a weapon item 0x01NN loads class 0x0701NN00, e.g. uWp1a), list 4 the classes of
// the stage's set records and their weapons, list 5 its objects. Committing a list
// (0x007D4A10) releases what was not asked for again and loads what is new, behind the
// loading screen. Extras are queued into lists 3 and 4 right before they are committed, so
// they load and stay with the stage's own.
namespace rmre5::resources {

struct Extras {
    std::vector<std::string> enemyClasses;     // with the stage's enemies: "uEm11"
    std::vector<std::uint32_t> enemyWeapons;   // weapons enemies hold, category<<16 | id: 0x70160
    std::vector<std::uint16_t> playerWeapons;  // weapon item ids, with the player's items: 0x011A
};

/// Queues the hook on the list commit. Requires the loader signatures.
bool Install();
bool Available();

/// Extras for every following Mercenaries area load: the run's (the mod keeps them current
/// for the stage it arms) and a developer set; both are queued.
void SetRunExtras(Extras extras);
void SetDevExtras(Extras extras);

/// True when "Image\Archive\<name>" is in one of the loader's lists and its resource exists.
bool ArchiveLoaded(const std::string& name);
/// The class a class id names (0x07011A00 -> "uWp1a"); empty when the game has none.
std::string ClassNameById(std::uint32_t classId);
/// True when the model of weapon item 0x01NN is loaded (its class archive is).
bool WeaponLoaded(std::uint16_t item);

/// Developer: logs every request of every list (path, state, resource).
void LogLists();

}  // namespace rmre5::resources
