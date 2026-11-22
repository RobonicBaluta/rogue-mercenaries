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
    bool saveGuard = false;     // the game's save file write (none while a run is on)
    bool appRequest = false;    // the application's request word (developer "quitmatch")
    bool itemBoxGuard = false;  // the Organize screen's panels (no Buy or Upgrade with a run in it)
    bool itemBoxPictures = false;  // the Organize screen's item table (pictures for the run's guns)
    bool actions = false;       // character actions by id (M8: staggers)
    bool dodge = false;         // the player's dodge, started the game's way (M8: Wesker's moves)
    bool turn = false;          // a character turned the game's way (M8: Wesker faces his lunge's target)
    bool motions = false;       // motion lists looked up, bound to a model and played (M8: boss moves)
    bool effects = false;       // effect lists started on a unit the game's way (the elements' looks)
    bool effectStop = false;    // an effect the mod started stopped by its id
    bool flameStream = false;   // the flamethrower's fire with an element's look in place of its flame
    bool partner = false;       // a computer player added the game's way (E22: Josh's backup)
    bool partnerDeath = false;  // a partner's execution does not end the run
    bool equipCell = false;     // the game's own change of the item in the hands
    bool weaponLevels = false;  // a gun's capacity and reload levels (a SHOP upgrade raises them)
    bool weaponScope = false;   // a gun's scope can be turned off (sniper rifles aim like any gun)
    bool twinGuns = false;      // the boss Jill's SMG units on the player (E16 spike, developer commands)
    bool jointPosition = false; // a character's bone in the world (E16: the twin guns' shots leave her hands)
    bool hitAttacker = false;
    bool gameWork = false;   // mode/stage/merc timer layout
    bool mercCountdown = false;
    bool mercIntro = false;      // the match's intro can be skipped (main state 6, kFnMercStartState)
    bool playMenu = false;  // native "Rogue Mercenaries" entry in the Play menu
    bool menuMoveSound = false;  // the menus' cursor-move tick (the mod's lists, mouse or keys)
    bool setupScreens = false;  // Mercenaries setup screens (direct start)
    bool fader = false;         // the game's screen fader (black-out while the autopilot runs)
    bool flowScript = false;    // title-flow script steps (direct start without screens)
    bool splitScreen = false;   // M10: the split-screen setup script (a co-op run's matches)
    bool gameOverCheck = false; // M10: the match's game-over check (a co-op run's fallen player)
    bool mercEndEvent = false;  // M10: the match's end message (a co-op fall must not end it)
    bool appRequestWord = false;  // M10: the application's request words (a co-op fall's input lock)
    bool playerDeathChecks = false;  // M10: uPlayer's dead and dying checks (a co-op fall stays the fallen one's)
    bool cockpitRings = false;  // M10: the cockpit's weapon rings check (a co-op fall keeps the other's)
    bool menuRowGlow = false;  // the Play menu's ninth row gets the selection glow
    bool hudNameTables = false;  // the HUD's text names (the inventory tabs) can be the characters' own
    bool infiniteAmmo = false;  // a gun can be marked the game's own way to never run out (re5_layout.h kSlotInfiniteBit)
    bool em82Tuning = false;    // Uroboros Wesker's second form and rage can be set (maps.json boss tuning)
    bool em84Tuning = false;    // the boss Jill's attack chance and fire cooldown tables can be patched
    bool em84DieBlow = false;   // the boss Jill's blow-away death's game over can be told apart (hooks_combat)
    bool seatMenuInput = false;  // M10: each split-screen player's own menu input (their screens of their own)
    bool playerMayAct = false;  // M10: uPlayer's may-act check (a co-op fall keeps the other's inventory)
    bool pickupPartner = false;  // M10: the may-take-item check's partner (a co-op fall keeps the other's pickups)
    bool playerGameOver = false;  // M10: uPlayer's own game over (a co-op fall does not set it)
    bool wpSlotUiVisible = false;  // M10: the inventory grid's visibility (a co-op fall keeps the other's)
    bool inventory = false;     // uPlayer inventory layout and add helpers
    bool resultsScreen = false; // Mercenaries results screen move (auto-accept between stages)
    bool gameoverScreen = false; // game-over screen move and its decide (a run's death answers "no")
    bool pauseScreen = false;   // pause screen move (host of the in-match screens)
    bool timeBonus = false;     // time-bonus pickups and the countdown add (chests)
    bool fatalHandler = false;  // the engine's fatal error handler (its message is logged)
    bool itemBox = false;       // the campaign Organize/Buy screen can be created (M7, research)
    bool matchSetup = false;    // the Mercenaries match setup and the save's stage field
    bool worldObjects = false;  // the object manager: set spawn, create, set records
    bool stageLayouts = false;  // the area load's layout pass and its register call (a campaign area's ladders)
    bool resources = false;     // the archive loader: list commit and the unit archive requests
    bool mercHud = false;       // the clock (cockpit) and score (Mercenaries HUD) draws, the kill cap
    bool slotClear = false;     // the game's inventory record clear (releases weapon models)
    bool gameShop = false;      // campaign shop screen constructor and unit registration
    bool enemySpawn = false;    // sEnemy::Create, the set spawn, class lookup (director)
    bool areaCameras = false;   // the player camera's zone cameras (a run's campaign areas keep its own)
    bool hitCheckProbe = false; // developer: the enemy hit check, logged (dev "hitprobe")
    bool bossGunfire = false;   // Uroboros Wesker as a run's boss takes gunfire anywhere (his own check: only his open core)
    bool bossNoRefill = false;  // Uroboros Mkono as a run's boss keeps its damage (its update refills its health)
    bool bossBrain = false;     // the Uroboros' own brain can be released from the story's waiting mode
    bool faderState = false;    // developer: the fader's state, logged (dev "screenwatch")
    bool stage308Setup = false; // s308's setup can be kept from its null read (stage_fixes.cpp)
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
/// A character at zero health is refused unless `atZeroHp` (the Wesker bosses fight on there).
bool StartAction(std::uintptr_t character, int id, const char* expectedClass, bool atZeroHp = false);
/// The player's hold (cPlAction_pl_act_test, re5_layout.h kVtablePlActTest) gets a start that
/// clears the "action done" bit after its own, like the game's other starts do. Applied once, in
/// game memory only, when `player`'s action 0xA8 is that class with the vtable and slots expected;
/// logs what it found. True when the start is in place.
bool EnsureHoldStartClearsDone(std::uintptr_t player);
/// The ids in a character's action table whose action is of class `cls`, lowest first.
std::vector<int> ActionIdsOf(std::uintptr_t character, const char* cls);
/// The class of every action in a character's table, by id ("" for an empty slot).
std::vector<std::string> ActionNames(std::uintptr_t character);
/// The id of the character's current action in its table (-1: none or not in the table).
int CurrentActionId(std::uintptr_t character);
/// The player's own forward dodge toward `toward`, started the way the game's dodge picker
/// starts one (threat point, prep, avoid_front). False when refused (another action holds).
bool StartDodge(std::uintptr_t player, const float toward[3]);
/// The class of the character's current action (re5::off::kPlayerAction; "" without one).
std::string CurrentActionName(std::uintptr_t character);
/// Turns a character to face `yaw` (radians; facing (sin, cos) on the ground plane) the game's
/// way (re5::addr::kFnCharTurnTo): at once under 2 frames, else over that many frames.
bool TurnTo(std::uintptr_t character, float yaw, int frames);

/// Motions borrowed from another unit's archive (M8: the boss Jill's twin-SMG sweep on the
/// playable Jill, the boss Wesker's moves on his; re5_layout.h "Motions"). Every player model
/// shares one skeleton, and the boss lists' tracks name its bones (checked offline).
/// The motion list at `path` (e.g. "pawn\em\em84\motion\em84atk") when its archive is loaded,
/// with a reference held for the caller (ReleaseResource); 0 when not: looked up, never loaded.
std::uintptr_t LoadedMotionList(const char* path);
void ReleaseResource(std::uintptr_t resource);
/// An effect list (rEffectList; "effect\efl\wp\wp21\wp2103", no extension): the cached one with
/// a reference held for the caller (ReleaseResource), else 0; with `load`, read from its file now
/// when it is not cached. A missing file is the game's own fatal error: load only files that exist
/// (EffectFileExists).
std::uintptr_t EffectList(const char* path, bool load);
/// Starts the effect list at `path` ("effect\efl\core\cr000c", no extension) on `unit` (it follows
/// it), the game's way (re5::addr::kFnEffectCreate). Only a list already in memory plays (looked up,
/// never loaded): false when it is not, or when effects are off.
bool PlayEffect(const char* path, std::uintptr_t unit);
/// As PlayEffect, at the unit's `joint` (-1 its root), `scale` times its own size (the start's
/// settings +0x10..+0x18, the effect's mScale), and returns the effect's id for StopEffect (0: not started).
std::uint32_t StartEffect(const char* path, std::uintptr_t unit, int joint, float scale = 1.0f);
/// Stops an effect StartEffect (or the game) started; false when it could not.
bool StopEffect(std::uint32_t id);
/// True when the effect list at `path` is in memory (looked up, never loaded).
bool EffectInMemory(const char* path);
/// Every effect list in memory now (the resource manager's table), sorted.
std::vector<std::string> LoadedEffectLists();
/// The camera the game draws the world with: the first active viewport's (sCamera).
struct CameraView {
    int viewport = -1;
    std::uintptr_t camera = 0;
    float eye[3] = {}, target[3] = {}, up[3] = {};
    float fov = 0.0f, aspect = 0.0f, nearPlane = 0.0f, farPlane = 0.0f;
    std::int32_t region[4] = {};  // the viewport's mRegion as it reads (diagnostics)
};
/// False when there is none, or its numbers do not look like a camera's.
bool ReadCameraView(CameraView& out);
/// Every active viewport's camera that looks like one (M10: a split screen has one per player, each
/// in its region of the screen).
std::vector<CameraView> ReadCameraViews();
/// True when nativePC_MT\<path>.efl exists next to the game.
bool EffectFileExists(const std::string& path);
/// Puts `list` into one of the unit's motion list slots through the game's own (reference
/// counted) setter: the slot holding it already, else the highest free one. The slot, or -1.
int BindMotionList(std::uintptr_t unit, std::uintptr_t list);
/// Of motion `index` in the unit's slot: its bone tracks, how many of them reach a joint of the
/// unit's model, the same for the body's bones (ids under 64: spine, limbs, head; the rest are
/// clothing, which a costume may not have), and its length in frames (empty: no such motion).
struct MotionFit {
    int tracks = 0;
    int mapped = 0;
    int bodyTracks = 0;
    int bodyMapped = 0;
    float frames = 0.0f;
};
std::optional<MotionFit> CheckMotion(std::uintptr_t unit, int slot, int index);
/// Plays motion `index` of the unit's slot on its body layer (0), once, the way enemies start
/// theirs (re5::addr::kFnCharSetMotion). False when refused.
bool PlayMotion(std::uintptr_t unit, int slot, int index, float blendFrames, float speed);
/// The body layer's motion: its id (slot << 8 | index), current frame and length.
struct BodyMotion {
    int id = 0xFFFF;
    float frame = 0.0f;
    float frames = 0.0f;
};
std::optional<BodyMotion> ReadBodyMotion(std::uintptr_t unit);

/// A computer-controlled player (ROADMAP E22: Josh's backup), added the way the Port stage adds its
/// Josh (re5::addr::kFnPlayerAdd): `character` (0 Chris, 1 Sheva, 2 Jill, 3 Wesker, 4 Josh ...)
/// in `costume`, in `slot` (1..3, free). The game fills the save's player entry of that slot and
/// game work's slot fields on the way: the caller keeps copies (SavePlayerArea) and puts them
/// back. 0 when refused or faulted. The class's archive must be loaded, else the game ends.
std::uintptr_t AddPlayer(int slot, int character, int costume);
/// The save's player entries, their items and the slots' extra blocks (save + slot data + 0x23300,
/// 0x1600 bytes), the area the add writes; empty without a save.
std::vector<std::uint8_t> SavePlayerArea();
bool RestorePlayerArea(const std::vector<std::uint8_t>& copy);
/// The player class the add creates for (character, costume): the class lookup by id (category 1).
std::string PlayerClassName(int character, int costume);

/// The boss Jill's SMG units on the player (E16 spike; re5::addr::kFnItemUnitCreate): a uWp76 unit
/// from sItem (its archive must be loaded, else the game ends), attached to `parent`'s `joint` with a
/// local offset and rotation as the boss attaches hers (the gun's owner at +0xFDC), fired from
/// `muzzle` along `dir` (a shot unit the gun owns; whether it hurts enemies is what the spike is
/// for), deleted the MT way (+4 &= ~0x2000, +0x30). 0 / false when refused or faulted.
std::uintptr_t CreateTwinGun();
bool AttachTwinGun(std::uintptr_t gun, std::uintptr_t parent, int joint, const float offset[3], const float euler[3]);
bool FireTwinGun(std::uintptr_t gun, const float muzzle[3], const float dir[3]);
/// Where bone `bone` of `unit`'s model is in the world, the game's way (0x00B56650, which the
/// boss Jill's guns use for their muzzles: her bones 11 and 6, the hands); a bone the model lacks
/// gives the origin offset. False without the capability `jointPosition` or on a fault.
bool JointPosition(std::uintptr_t unit, int bone, float out[3]);
bool DeleteUnit(std::uintptr_t unit);

/// Fixes of the game's own code for what Mercenaries mode 2 never met before the mod (in memory,
/// each behind its signature): Excella's finisher on a Majini (re5::addr::kDeadBombModeJump).
void ApplyCodeFixes();
/// Writes `size` code bytes at `address` when the bytes there are `expectedHex` ("75 0B"); false,
/// nothing written, when they differ.
bool WriteCode(std::uintptr_t address, const char* expectedHex, const unsigned char* bytes, std::size_t size);
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
/// The player the mod plays for: the local one (the first human player, control 0), or on this
/// thread the unit an ActingPlayerScope names (M10 local co-op: the second player's turn).
std::uintptr_t LocalPlayer();
/// M10 local co-op: a human player's sPlayer slot (control 0; a split-screen match's player 1 is
/// slot 0, player 2 slot 1), -1 for any other unit; the human player in a slot, 0 when none.
int HumanPlayerSlot(std::uintptr_t unit);
std::uintptr_t HumanPlayerInSlot(int slot);
/// While alive, LocalPlayer() on this thread answers `unit` (0: no change).
class ActingPlayerScope {
public:
    explicit ActingPlayerScope(std::uintptr_t unit);
    ~ActingPlayerScope();
    ActingPlayerScope(const ActingPlayerScope&) = delete;
    ActingPlayerScope& operator=(const ActingPlayerScope&) = delete;

private:
    std::uintptr_t previous_ = 0;
};

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
