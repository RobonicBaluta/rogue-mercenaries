#pragma once

// Native "ROGUE MERCENARIES" row in RE5's Play menu; see native_menu.cpp.

namespace rmre5::nativemenu {

// Queues the Play menu hook; call before MH_ApplyQueued.
bool Install();
// Game thread: restores the borrowed message when the menu has closed.
void OnGameTick();
// True while the Play menu is on screen and waiting for input (not while it
// fades out after a choice).
bool MenuAtInput();
// True once (then cleared) after the player picked ROGUE MERCENARIES.
bool TakeChosen();
bool Chosen();

}  // namespace rmre5::nativemenu
