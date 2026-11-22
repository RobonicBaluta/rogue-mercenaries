#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "native_menu.h"

// Native in-match screens (arena results, rewards, shop, route) drawn with the game's own
// menu text routine while the world is paused the way the pause menu pauses it, plus a
// one-line status drawn with the same font during the fight. Hosted by a hook on the
// pause screen's per-frame update, which runs for the whole match.
//
// M10: on the game's split screen each player has screens of their own (the user, 2026-11-21:
// "both players should have their own menu in split scren"): player 1's in the top half in the
// game's font, player 2's in the bottom half, which the game's text routine cannot reach (it draws
// in the top view only), so the overlay draws it from SplitView. Each answers its player's own pad
// (and the keyboard, for the player the game gives it to).
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
    // M10: two players on the game's split screen. The game's text routine draws in the top
    // view only; the overlay draws the texts on the bottom half (mod.cpp DrawSplitHud).
    bool split = false;
};
void SetHud(const Hud& hud);

/// M10: the players' pads, as the game numbers them (its XInput user; -1 the keyboard, -2 not
/// known). Player 2's own screens need theirs.
void SetPlayerPads(int one, int two);

/// M10: a player's screen for the overlay to draw, in the 1280x720 layout of its half (y from the
/// half's top): player 2's always, player 1's while it shows a grid (the game's text routine draws
/// rows only).
struct ViewRow {
    std::string label;
    std::uint32_t argb = 0;  // 0xAARRGGBB
    bool selected = false;
    bool locked = false;
    bool cell = false;  // a grid cell (a box), not a listed row
    int index = 0;      // the screen's item
    int x = 0, y = 0;   // its centre
    float scale = 0;    // the game font's scale it stands for
};
struct View {
    std::string header;
    std::string help;  // wrapped, lines split by '\n'
    std::vector<ViewRow> rows;  // the visible rows (a grid's cells first)
    int grid = 0;               // the screen's first `grid` items are cells, three to a row
    int headerY = 0, helpY = 0;
    float headerScale = 0, rowScale = 0, helpScale = 0;  // the game font's scales
};
/// False when seat `seat`'s screen is not the overlay's to draw.
bool SplitView(int seat, View& out);

/// Developer (the UI lab): texts drawn with the game's text routine while no screen is open, each
/// a borrowed message slot (3..7, 9) drawn at its place, scale and 0xAARRGGBB colour; one slot's
/// text may be drawn several times. Empty: none.
struct LabText {
    int slot = 3;
    std::string text;
    int x = 640, y = 360;
    float scale = 0.5f;
    std::uint32_t argb = 0xFFFFFFFFu;
};
void SetLabTexts(std::vector<LabText> texts);

/// Developer: the world-pause request bit the pause menu uses.
void SetWorldPause(bool paused);
/// While a screen is up: the pause again if something cleared it (coming back to the game window).
/// True when it had to be asked again.
bool HoldWorldPause(const char* why);

}  // namespace rmre5::matchui
