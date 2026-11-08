#pragma once

// Area cameras (2026-11-08, the user's playtest: on the Volcano "the boulder scene camera
// movement ... doesn't let go until you go off a ledge, which is hard to see when you can't see
// your character"). A campaign area's camera zones (rAHCamera) take the player camera over while
// the player stands in one, and its scripts force one for their scenes. A run's arena is not the
// story: while a run plays a campaign area the zone check answers "no zone" (the game's own path
// for it), and a script's or an object's forced zone camera is refused. Mercenaries stages keep
// theirs. research/re5-engine-map.md "Area cameras".
namespace rmre5::areacams {

enum class Mode {
    Auto,  // off while a run plays a campaign area (SetRunWantsOff)
    Game,  // the game's own, always (developer)
    Off,   // off, always (developer)
};

/// Queues the two hooks (needs engine::Caps().areaCameras).
bool Install();
/// Game thread: true while a run plays a campaign area.
void SetRunWantsOff(bool off);
void SetMode(Mode mode);
Mode GetMode();
/// True while the zone cameras are off.
bool Off();
const char* ModeName(Mode mode);

}  // namespace rmre5::areacams
