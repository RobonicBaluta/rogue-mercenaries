#pragma once

// Engine hooks. Detours translate raw engine calls into the plain structs
// below and forward them to a single HookSink owned by the mod layer.

#include <vector>
#include <cstdint>

namespace rmre5 {

struct OutgoingHit {
    std::uintptr_t enemy = 0;
    std::uintptr_t attacker = 0;
    bool attackerIsPlayer = false;
    bool attackerIsLocal = false;
    int damage = 0;  // value the game is about to subtract
    int targetHp = 0;
    int targetMaxHp = 0;
};

struct EnemyHitResult {
    std::uintptr_t enemy = 0;
    std::uintptr_t attacker = 0;
    bool attackerIsPlayer = false;
    bool attackerIsLocal = false;
    int hpBefore = 0;
    int hpAfter = 0;
    int maxHp = 0;
    int rawDamage = 0;     // damage before our multiplier (0 if SubHp not reached)
    int appliedDamage = 0; // damage passed to the game
    bool killed = false;
};

struct IncomingHit {
    std::uintptr_t player = 0;
    int damage = 0;
    int hp = 0;
    int maxHp = 0;
};

struct IncomingDecision {
    float multiplier = 1.0f;
    bool preventDeath = false;
    int leaveHp = 1;
};

struct PlayerHitResult {
    std::uintptr_t player = 0;
    int rawDamage = 0;
    int appliedDamage = 0;
    int hpBefore = 0;
    int hpAfter = 0;
    int maxHp = 0;
};

class HookSink {
public:
    virtual ~HookSink() = default;
    // Game thread. Return the multiplier for a pending enemy HP subtraction.
    virtual float OutgoingMultiplier(const OutgoingHit& hit) = 0;
    virtual void EnemyHitDone(const EnemyHitResult& result) = 0;
    virtual IncomingDecision Incoming(const IncomingHit& hit) = 0;
    virtual void PlayerHitDone(const PlayerHitResult& result) = 0;
    // Game thread, once per input poll (about once per frame).
    virtual void GameTick() = 0;
    // True while the overlay owns input; the game sees neutral input.
    virtual bool BlockGameInput() = 0;
    // Game thread: the local player's health is about to rise by `amount` (a healing item);
    // returns the amount to apply.
    virtual int HealAmount(std::uintptr_t player, int amount) { (void)player; return amount; }
};

// The last raw input the game polled (before the overlay hides anything): DirectInput keyboard
// scancodes, DirectInput joystick buttons and the XInput gamepad's button bits.
/// Inputs the mod owns during a run (M8 specials): hidden from the game while held, so the
/// game's own action on them (the knife) does not happen too. A binding of several buttons held
/// together holds its first ones back for a moment: completed in time it is the mod's, else they
/// reach the game late. `joy` holds DirectInput joystick buttons, one bit per button number.
struct OwnedInput {
    int key = -1;                 // DirectInput keyboard scancode, -1 none
    unsigned short xinput = 0;    // XInput buttons held together
    unsigned joy = 0;             // DirectInput joystick buttons held together
};
/// The bindings owned now (none: an empty list); set from the game thread.
void SetOwnedInputs(const std::vector<OwnedInput>& inputs);
bool RawKeyDown(int scancode);
bool RawJoyButton(int index);
unsigned short RawXInputButtons();

void SetHookSink(HookSink* sink);

struct HookStatus {
    bool combat = false;
    bool input = false;
    bool xinput = false;
    bool d3d9 = false;
    unsigned long gameThreadId = 0;
    unsigned long long ticks = 0;
    unsigned long long enemyHits = 0;
    unsigned long long playerHits = 0;
    unsigned long long presents = 0;
};
const HookStatus& Status();

// Mercenaries countdown (applied inside the game's countdown update).
struct MercTimerView {
    std::uintptr_t object = 0;
    bool recent = false;  // updated within the last 500 ms (a match is running)
    float countdown = 0.0f;
    int state = -1;       // 0 running, 1 finished
};
void SetMercTimerFloor(float seconds);  // <= 0 disables the floor; > 0 also holds off the kill cap
void SetMercHudHidden(bool hidden);     // clock and score panels off screen
void SetMercHudCounter(int value);      // hidden HUD: 0..99 in the clock's big digits, < 0 none
void RequestMercMatchEnd();
void ClearMercMatchEnd();  // drop a pending request (a new match must not inherit it)
void SuppressMercAddTime(bool suppress);  // countdown bonuses ignored (a run holds the countdown)
int TakeTimeBonusPickups();               // time-bonus pickups collected since the last call
bool TakeTimeBonusPosition(float pos[3]);  // where an opened statue stood, oldest first (false: none left)
void SimulateTimeBonusPickup();           // developer: count one pickup
MercTimerView ReadMercTimer();
bool InstallMercHooks();

// Drives HookSink::GameTick from the game's main thread.
void TickGame(const char* source);

// Install* only create and queue hooks; the caller enables them with one
// MH_ApplyQueued() and calls MarkHooksInactive() if that fails.
bool InstallCombatHooks();
/// Developer: every call of the enemy hit check for `unit` (0: off) with a hit pending is logged
/// with what the check reads (block, flags, cooldowns) and its answer.
void SetHitProbe(std::uintptr_t unit);
/// The run's boss (0: none): when its class's own "may this hit hurt" says no (Uroboros Wesker:
/// anything but his open core), the hit hurts anyway, the game's own damage then (2026-11-08).
void SetBossTakesGunfire(std::uintptr_t unit);
/// The computer partner of this arena (0: none): an execution of that player does not start the
/// death camera that ends the match (2026-11-09: a chainsaw killed Josh's Sheva, game over).
void SetPartnerUnit(std::uintptr_t unit);
void MarkHooksInactive();
bool InstallInputHooks(void* realDirectInput8Create);
bool InstallD3d9Hooks();
void RecordDirectInputCreate(const void* riid);

}  // namespace rmre5
