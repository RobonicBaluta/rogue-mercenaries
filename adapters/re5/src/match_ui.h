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
/// The game's own pause menu is showing (uIdPause00 out of its closed state).
bool PauseMenuOpen();

/// Opens the screen stack at `first` and pauses the world; no-op when already open.
void Open(nativemenu::ScreenId first);
/// Closes every screen and resumes the world.
void Close();
bool IsOpen();

/// The fight's HUD texts in the game's font, laid out like the Mercenaries HUD: the arena on
/// the left (where the score was), a label under the counter in the clock's place, supplies
/// on the right (where the combo was) and a notice line above. Empty texts are not drawn.
struct Hud {
    std::string notice;
    std::string left;
    std::string center;
    std::string right;
};
void SetHud(const Hud& hud);

/// Developer: the world-pause request bit the pause menu uses.
void SetWorldPause(bool paused);
/// While a screen is up: the pause again if something cleared it (coming back to the game window).
/// True when it had to be asked again.
bool HoldWorldPause(const char* why);

}  // namespace rmre5::matchui
