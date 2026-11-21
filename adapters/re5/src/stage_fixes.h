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
//
// s100 (Civilian Checkpoint, chapter 1-1's walk into town) holds back the player's weapon and
// inventory with checks of the stage number itself (2026-11-21, the user: "civilian checkpoint you
// don't spawn with weapons tho and can't access your inventory"): the player's walk without a
// weapon (0x008661B6), the weapon model's and holster's handling (0x00B7013E, 0x00B65F7A,
// 0x00B668F3), the weapon's buttons (0x00B8B478), the cockpit's grid state (0x009F11B1), the
// inventory grid on screen (0x00A70C61) and its opening (0x00C6C43D). From a run's start (the
// stage's player setup runs while it loads, before the run counts as active) until the run is over
// these eight branches take the way of every other stage; they are written only while no match is
// loaded, and put back after the run, so the campaign's chapter 1-1 stays as it is.
namespace rmre5::stagefix {

/// Queues the hooks whose signatures match.
bool Install();
/// Game thread: `inRun` while a run is active (its stages skip their story setup); `runWanted` from
/// a run's start until it is over; `inMatch` false when no stage is loaded (stage 100's code is
/// switched only then).
void SetInRun(bool inRun, bool runWanted, bool inMatch);

}  // namespace rmre5::stagefix
