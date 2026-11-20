#pragma once

#include <filesystem>

namespace rm {
class RunController;
}

// The map editor (2026-11-20, the user: "a tool so I can add spawners on maps ... give me the power
// to choose"; a spawner is one more spawn point the director's waves use). In an arena, F8 opens a
// window over the game: add a spawn point where the player stands, take out the map's point nearest
// to him (one behind a door that never opens), put one back, set the arena's start, step through the
// points (the player goes to each), spawn a test enemy at one, and see every point in the world.
// The changes go to <data>\map_edits.json at once and the director uses them at once
// (rm::RunController::SetMapEdits). On by adapter.json "map_editor": true; the file is read on any
// install, so the edits play wherever it is.
namespace rmre5::mapeditor {

struct Frame {
    rm::RunController* run = nullptr;
    int mapKey = -1;       // the arena's map (CurrentMapKey)
    bool inMatch = false;  // a match is being played
};

/// Reads <dataDir>\map_edits.json into the run (and remembers where to write it).
void Load(const std::filesystem::path& dataDir, rm::RunController& run);
void SetEnabled(bool on);
[[nodiscard]] bool Enabled();
/// Counts the editor's changes (the loads' settings are worked out again after one).
[[nodiscard]] int Serial();
/// The editor window is up (it then stands in for the overlay's own window).
[[nodiscard]] bool Open();
/// The pad combo that opens it (XInput L3 + R3; 0 while off): the mod owns it in a match.
[[nodiscard]] unsigned short PadCombo();
/// A map the window asked to go to now (-1: none); the game tick takes the arena there.
[[nodiscard]] int TakeTravelRequest();
/// The mod menu's MAP EDITOR is starting a run: the player is invulnerable in it (the window's
/// checkbox; off again once the run is over).
void EditorRunStarting();
/// The window's switches, for the run in play: no damage to the player, and no new enemies from
/// the director (the arena's waves held while points are tested).
[[nodiscard]] bool Invulnerable();
[[nodiscard]] bool HoldWaves();
/// Render thread, the mod's lock held: F8, the points in the world, and the window while the
/// overlay has the focus.
void Draw(const Frame& frame);
/// Game tick, the mod's lock held: what the window asked for (a warp, a test spawn).
void Tick(const Frame& frame);

}  // namespace rmre5::mapeditor
