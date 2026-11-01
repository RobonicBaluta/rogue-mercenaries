#pragma once

#include <cstdint>

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

/// E1: an enemy of `unitClass` ("uEm10") at `pos` (x, y, z) with descriptor weapon id
/// `weapon` (-1 = the descriptor's default). Returns the unit or 0.
std::uintptr_t Spawn(const char* unitClass, const float pos[3], int weapon);

}  // namespace rmre5::director
