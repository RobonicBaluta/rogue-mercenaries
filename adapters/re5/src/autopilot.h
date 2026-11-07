#pragma once

// Drives the game's own Mercenaries setup screens after START RUN; see
// autopilot.cpp.

namespace rmre5::autopilot {

struct Plan {
    int stage = 0;        // uIdStartUp21 row (0..stage_count-1)
    int costume = 0;      // uIdStartUp22 slot (the game's character/costume index)
    int stage_count = 8;  // rows to cycle through when the game refuses a locked stage
};

// Queues the hooks; call before MH_ApplyQueued.
bool Install();
bool Available();
// Starts driving the next screens (uIdStartUp20 -> 21 -> 22) with this plan.
/// True when a match can be restarted on another stage without any screen: the flow
/// script hooks and the results screen hook are in place.
bool CanChangeStage();
void Arm(const Plan& plan);
void Disarm(const char* reason);
bool Armed();
/// After a run's last match: its Mercenaries results screen is skipped like a stage change's
/// (no stats page, no record written to the game's save) and the Mercenaries menu script's
/// first step answers "back", so the game returns to its title Play menu. Cleared by Arm and
/// once the script has taken it. Needs CanChangeStage().
void ReturnToTitle(bool on);
bool ReturningToTitle();
/// A run's match is on (the mod sets it every tick): if it is quit (the pause menu, the game
/// over), the match's quit state finds the Mercenaries phase cleared and writes no save.
void SetRunMatch(bool on);
/// While on, the game's save file write finishes without writing (re5::addr::kFnSaveFileWrite):
/// the mod sets it while a run is on and for a while after its match, so no way of leaving a
/// run's match (the pause menu's quit among them) changes savedata.bin.
void SetSaveGuard(bool on);
// Game thread: times out an autopilot that could not finish.
void OnGameTick();

}  // namespace rmre5::autopilot
