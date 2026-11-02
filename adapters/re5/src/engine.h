#pragma once

// Read-mostly view of RE5 engine state built on re5_layout.h. Every function
// tolerates missing singletons (menus, loading) and returns empty results.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

namespace rmre5::engine {

// Capability flags filled by VerifySignatures(); hooks and queries check them.
struct Verified {
    bool buildIdentity = false;
    bool players = false;    // sPlayer list + HP layout
    bool enemies = false;    // sEnemy list
    bool enemyOnHit = false;
    bool characterSubHp = false;
    bool playerSubHp = false;
    bool hitAttacker = false;
    bool gameWork = false;   // mode/stage/merc timer layout
    bool mercCountdown = false;
    bool playMenu = false;  // native "Rogue Mercenaries" entry in the Play menu
    bool setupScreens = false;  // Mercenaries setup screens (direct start)
    bool fader = false;         // the game's screen fader (black-out while the autopilot runs)
    bool flowScript = false;    // title-flow script steps (direct start without screens)
    bool inventory = false;     // uPlayer inventory layout and add helpers
    bool resultsScreen = false; // Mercenaries results screen move (auto-accept between stages)
    bool pauseScreen = false;   // pause screen move (host of the in-match screens)
    bool timeBonus = false;     // time-bonus pickups and the countdown add (chests)
    bool fatalHandler = false;  // the engine's fatal error handler (its message is logged)
    bool matchSetup = false;    // the Mercenaries match setup and the save's stage field
    bool worldObjects = false;  // the object manager: set spawn, create, set records
    bool resources = false;     // the archive loader: list commit and the unit archive requests
    bool mercHud = false;       // the clock (cockpit) and score (Mercenaries HUD) draws, the kill cap
    bool slotClear = false;     // the game's inventory record clear (releases weapon models)
    bool gameShop = false;      // campaign shop screen constructor and unit registration
    bool enemySpawn = false;    // sEnemy::Create, the set spawn, class lookup (director)
};
const Verified& Caps();

// Waits for the packed executable to finish unpacking, then checks the PE
// header identity and every signature. Returns false if the build differs.
bool VerifySignatures(unsigned timeoutMs);

std::uintptr_t DtiOf(std::uintptr_t object);
bool IsKindOf(std::uintptr_t object, std::uintptr_t dti);
std::string DtiName(std::uintptr_t dti);
std::string ClassName(std::uintptr_t object);

struct CharacterHp {
    int hp = 0;
    int max = 0;
};
std::optional<CharacterHp> ReadHp(std::uintptr_t character);
bool WriteHp(std::uintptr_t character, int hp);

struct PlayerInfo {
    std::uintptr_t object = 0;
    int slot = -1;
    int number = -1;   // uPlayer+0x2D9C
    int control = -1;  // uPlayer+0x2DA8
    CharacterHp hp;
    std::string className;
};
struct PlayersSnapshot {
    int count = 0;
    std::array<PlayerInfo, 4> players{};
    int localSlot = -1;  // best guess, see LocalPlayerSlot()
};
PlayersSnapshot ReadPlayers();
std::uintptr_t LocalPlayer();

struct EnemiesSnapshot {
    int listed = 0;
    int alive = 0;
};
EnemiesSnapshot ReadEnemies();
struct EnemyState {
    std::uintptr_t object = 0;
    int hp = 0;
    int max = 0;
};
/// Every listed uEnemy with its health (empty without the enemy list).
std::vector<EnemyState> ReadEnemyStates();

struct GameWork {
    std::uintptr_t object = 0;
    int mode = -1;
    int stage = -1;
    float mercTimer = 0.0f;
    int mercScore = 0;
    int mercKills = 0;
};
std::optional<GameWork> ReadGameWork();
bool WriteMercTimer(float seconds);

// The game's own cHit::GetAttacker (thiscall). Only call on the game thread
// from inside a hit callback where the cHit is known to be live.
std::uintptr_t HitAttacker(std::uintptr_t hit);

// Character id used by the core for a uPlayer subclass ("chris", "sheva", ...).
std::string CharacterIdForClass(const std::string& className);

}  // namespace rmre5::engine
