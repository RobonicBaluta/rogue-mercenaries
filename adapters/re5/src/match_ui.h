#pragma once

#include <cstdint>
#include <string>

#include "native_menu.h"

// Native in-match screens (arena results, rewards, shop, route) drawn with the game's own
// menu text routine while the world is paused the way the pause menu pauses it, plus a
// one-line status drawn with the same font during the fight. Hosted by a hook on the
// pause screen's per-frame update, which runs for the whole match.
namespace rmre5::matchui {

bool Install();
void SetModel(nativemenu::Model* model);

/// Opens the screen stack at `first` and pauses the world; no-op when already open.
void Open(nativemenu::ScreenId first);
/// Closes every screen and resumes the world.
void Close();
bool IsOpen();

/// Status line drawn at the top during a match (empty = nothing).
void SetStatus(const std::string& line);

/// Developer: the world-pause request bit the pause menu uses.
void SetWorldPause(bool paused);

}  // namespace rmre5::matchui
