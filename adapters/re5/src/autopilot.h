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
// Game thread: times out an autopilot that could not finish.
void OnGameTick();

}  // namespace rmre5::autopilot
