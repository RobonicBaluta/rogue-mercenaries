#pragma once

// Engine hooks. Detours translate raw engine calls into the plain structs
// below and forward them to a single HookSink owned by the mod layer.

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
};

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

// Drives HookSink::GameTick from the game's main thread.
void TickGame(const char* source);

// Install* only create and queue hooks; the caller enables them with one
// MH_ApplyQueued() and calls MarkHooksInactive() if that fails.
bool InstallCombatHooks();
void MarkHooksInactive();
bool InstallInputHooks(void* realDirectInput8Create);
bool InstallD3d9Hooks();
void RecordDirectInputCreate(const void* riid);

}  // namespace rmre5
