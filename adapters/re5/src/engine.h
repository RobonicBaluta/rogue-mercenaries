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
    bool playerAddHp = false;   // healing items (M8: a build's heals)
    bool pickupItem = false;    // ground item pickups (M8: a build's drops)
    bool collision = false;     // segment casts against the world (M8: Wesker's moves)
    bool matchQuit = false;     // the match's quit state (a run's quit writes no save)
    bool itemBoxGuard = false;  // the Organize screen's panels (no Buy or Upgrade with a run in it)
    bool itemBoxPictures = false;  // the Organize screen's item table (pictures for the run's guns)
    bool actions = false;       // character actions by id (M8: staggers)
    bool dodge = false;         // the player's dodge, started the game's way (M8: Wesker's moves)
    bool turn = false;          // a character turned the game's way (M8: Wesker faces his lunge's target)
    bool hitAttacker = false;
    bool gameWork = false;   // mode/stage/merc timer layout
    bool mercCountdown = false;
    bool playMenu = false;  // native "Rogue Mercenaries" entry in the Play menu
    bool setupScreens = false;  // Mercenaries setup screens (direct start)
    bool fader = false;         // the game's screen fader (black-out while the autopilot runs)
    bool flowScript = false;    // title-flow script steps (direct start without screens)
    bool inventory = false;     // uPlayer inventory layout and add helpers
    bool resultsScreen = false; // Mercenaries results screen move (auto-accept between stages)
    bool gameoverScreen = false; // game-over screen move and its decide (a run's death answers "no")
    bool pauseScreen = false;   // pause screen move (host of the in-match screens)
    bool timeBonus = false;     // time-bonus pickups and the countdown add (chests)
    bool fatalHandler = false;  // the engine's fatal error handler (its message is logged)
    bool itemBox = false;       // the campaign Organize/Buy screen can be created (M7, research)
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
/// True while the player's current action (re5::off::kPlayerAction) is a hand-to-hand move
/// (cPlAction_pl_act_attack_*) or the knife attack: a hit then is a melee hit.
bool InMeleeAction(std::uintptr_t player);

struct CharacterHp {
    int hp = 0;
    int max = 0;
};
std::optional<CharacterHp> ReadHp(std::uintptr_t character);
bool WriteHp(std::uintptr_t character, int hp);
/// Sets a character's health and maximum to `hp` (bosses: the game's own suits story weapons).
bool SetHealth(std::uintptr_t character, int hp);
/// A segment cast against the world with the game's own query (re5::addr::kFnSegmentHit, the
/// mask its dodge picker uses). `flags` is its raw result: non-zero hit something,
/// re5::off::kSegmentBlocked set is what the dodge picker treats as a wall; `hit` is where.
struct SegmentResult {
    unsigned flags = 0;
    float hit[3] = {};
    [[nodiscard]] bool Blocked() const;
};
std::optional<SegmentResult> CastSegment(const float from[3], const float to[3]);
/// Starts a character's action by id through its own ChangeAction (vtable 0xDC, one of the
/// known implementations), with the arguments the game's dodges and co-op sync use. Only when
/// the action instance at that id is of class `expectedClass`. False when refused or unsafe.
bool StartAction(std::uintptr_t character, int id, const char* expectedClass);
/// The player's own forward dodge toward `toward`, started the way the game's dodge picker
/// starts one (threat point, prep, avoid_front). False when refused (another action holds).
bool StartDodge(std::uintptr_t player, const float toward[3]);
/// The class of the character's current action (re5::off::kPlayerAction; "" without one).
std::string CurrentActionName(std::uintptr_t character);
/// Turns a character to face `yaw` (radians; facing (sin, cos) on the ground plane) the game's
/// way (re5::addr::kFnCharTurnTo): at once under 2 frames, else over that many frames.
bool TurnTo(std::uintptr_t character, float yaw, int frames);
/// Moves a unit: every copy of its position in the object (the first 16 KB: transform, previous
/// and movement positions) gets `pos`; writing +0x30 alone is undone by its movement. Returns
/// the copies written (0: not moved). Verified on the player (s304: stays, walks on the floor).
int WarpUnit(std::uintptr_t unit, const float pos[3]);

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
