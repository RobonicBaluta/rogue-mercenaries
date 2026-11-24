#pragma once

#include <cstdint>

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
/// The player camera (uCameraCtrl) the zone check ran on last (0: none yet).
std::uintptr_t LastCamera();

/// The scene camera (a transformation's, 2026-11-24: "with a camera while everyone else is not moving so you get to
/// see the transformation"): while it is set, the view of the camera `camera` (a uCamera, engine::ReadCameraView's)
/// is built from `eye` and `target`, written to its +0x30 and +0x50 just before its view function (0x00835C20, the
/// uCameraBase +0x34 slot) builds the view, whatever its controller wrote; ClearSceneCamera gives it back. Any
/// thread. Needs engine::Caps().sceneCamera (InstallSceneCamera).
bool InstallSceneCamera();
void SetSceneCamera(std::uintptr_t camera, const float eye[3], const float target[3]);
void ClearSceneCamera();
/// The scene camera's views built since it was set (0 while none: the hook is not reached).
int SceneCameraViews();

}  // namespace rmre5::areacams
