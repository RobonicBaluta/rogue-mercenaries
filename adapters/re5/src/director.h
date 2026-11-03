#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The enemy director (ROADMAP.md, milestone M1): the mod decides which enemies exist.
//
// Engine pieces (research/re5-engine-map.md, "Enemy spawning"):
// - sEnemy::Create(dti, group, mode, flag) 0x007AF550 (vtable slot 11) creates any uEnemy
//   subclass and registers it in a unit group (enemies use group 9, mode 1, flag 1);
// - the stage's scripted and Mercenaries spawns all go through the set spawn 0x007AF5F0,
//   which builds the unit from a set record and applies its cSetInfoEmXX descriptor.
// Spawn() reproduces that recipe with a descriptor of our own; the gate blocks 0x007AF5F0.
namespace rmre5::director {

/// Queues the hooks (set-spawn gate, creation probe). Requires the spawn signatures.
bool Install();
bool Available();

/// E2: while blocked, the game's own set spawns (Mercenaries waves, scripts) create nothing.
void BlockNativeSpawns(bool blocked);
bool NativeSpawnsBlocked();
/// Native spawn attempts refused since the last call.
int TakeBlockedAttempts();

/// Developer survey: log every native spawn (class, weapon id, set record, position).
void SetSurvey(bool on);

/// What to spawn. The descriptor starts from the values the game's own Mercenaries
/// descriptors hold (the enemy already knows where the player is, mIsFindPL), then takes
/// `fields` (a surveyed profile: "armed", "arm_num", "arm", "plaga", "mask", ...; see
/// kSpawnFields in director.cpp) and `weapon`.
struct SpawnSpec {
    std::string unitClass;                // "uEm10"
    float pos[3]{};
    std::optional<float> facing;          // radians about Y (forward = sin, cos)
    int weapon = -1;                      // descriptor weapon id (-1 = profile or default)
    std::vector<std::pair<std::string, double>> fields;
};
std::uintptr_t Spawn(const SpawnSpec& spec);

/// E1: an enemy of `unitClass` ("uEm10") at `pos` (x, y, z) with descriptor weapon id
/// `weapon` (-1 = the descriptor's default). Returns the unit or 0.
std::uintptr_t Spawn(const char* unitClass, const float pos[3], int weapon);

/// Kills a live enemy through the game's own damage call (its HP in one hit), so the
/// death runs as in play. False when the unit is not a live character.
bool Kill(std::uintptr_t unit);
/// Takes `amount` health from a live enemy through the game's own call (as Kill does); a
/// build's special. False when the unit is dead or the call faults.
bool Damage(std::uintptr_t unit, int amount);

}  // namespace rmre5::director
