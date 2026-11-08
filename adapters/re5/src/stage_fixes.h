#pragma once

// Campaign stages whose own setup breaks when a run's match loads them (2026-11-08).
//
// s308 (aS308, the flamethrower Uroboros' room): its setup (0x0074CFB0) looks up the player's
// flamethrower (a uBioModel of kind 0, id 1: uWp21), gives it fuel and readies the room's fuel
// tanks; with none it still reads the flamethrower's fuel through a null pointer and the game
// crashes (0x0074D12A, seen 2026-11-02 when the host loaded s308). In a run, or whenever there is
// no flamethrower, only the stage's base setup runs (0x0072D240, what the setup itself calls
// first); the stage's update checks the flamethrower before it uses it, so its fuel tanks and
// their scenes stay off. research/re5-engine-map.md "Stage s308".
namespace rmre5::stagefix {

/// Queues the hooks whose signatures match.
bool Install();
/// Game thread: true while a run is active (its stages skip their story setup).
void SetInRun(bool inRun);

}  // namespace rmre5::stagefix
