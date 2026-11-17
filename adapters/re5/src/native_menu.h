#pragma once

// Native "ROGUE MERCENARIES" row and mod menu inside RE5's Play menu; see
// native_menu.cpp. The screens' content comes from a Model that mod.cpp
// implements on top of the run controller.

#include <cstdint>
#include <string>
#include <vector>

namespace rmre5::nativemenu {

enum class ScreenId {
    Main = 0, Characters, Perks, Credits, Skins, PerkList,
    MatchResults, MatchRewards, MatchShop, MatchRoute, MatchRunOver, MatchOrganize,
    RunResults, RunLength, MatchElement
};

struct Item {
    std::string label;  // one row; rendered in capitals by the game font
    std::string help;   // up to two lines under the list
    bool locked = false;  // drawn in the game's "disabled" red
    std::uint32_t color = 0;  // 0xAARRGGBB rarity colour (0 = the game's own)
    std::string icon;   // icons\<icon>.png drawn left of the label by the overlay (empty = none)
    int portrait = -1;  // costume slot whose bust the overlay shows while the cursor is here
};

struct Screen {
    std::string header;  // small line above the list
    std::vector<Item> items;
    std::string empty_help;  // help line when there are no items
    int portrait = -1;       // bust shown when the cursor row has none (-1 = none)
};

enum class Outcome {
    Stay,      // nothing happened
    Refresh,   // the screen content changed (rebuild it)
    Push,      // open `next`
    Back,      // close this screen
    StartRun,  // leave the mod menu into the game's Mercenaries flow
    Denied,    // play the game's error sound
    Close,     // close every screen of this host
};

struct Decision {
    Outcome outcome = Outcome::Stay;
    ScreenId next = ScreenId::Main;
};

// Implemented by mod.cpp. Both calls arrive on the game's menu thread.
struct Model {
    virtual ~Model() = default;
    virtual Screen Build(ScreenId id) = 0;
    virtual Decision Decide(ScreenId id, int index) = 0;
};
void SetModel(Model* model);

// Queues the Play menu hooks; call before MH_ApplyQueued.
bool Install();
// Game thread: restores the borrowed messages when the menu has closed.
void OnGameTick();
// True while the Play menu is on screen and waiting for input (not while it
// fades out after a choice).
bool MenuAtInput();
/// Developer: log every distinct label/help draw call of other screens (message id, position,
/// scale) to learn a screen's layout before hosting mod screens in it.
void EnableDrawProbe();
/// Developer tests: the game believes it owns no Extra Content, and/or its Mercenaries row is closed.
void SetDevSimulation(bool noExtraContent, bool mercenariesLocked);

// Shared with the in-match host (match_ui): the borrowed message slots, the game's text
// draw, menu input and list navigation. Slots 0..7 are rows, 8 the help, 9 the header.
constexpr int kHelpSlot = 8;
constexpr int kHeaderSlot = 9;
bool TextsReady();
/// While held, the borrowed messages stay applied outside the Play menu (in-match screens).
void HoldTexts(bool hold);
void SetSlotText(int slot, const std::string& text);
/// `self` is the screen object the text is drawn for (the routine reads its layout context).
void DrawSlot(std::uintptr_t self, int slot, int x, int y, float scale, std::uint32_t argb);
std::uint32_t InputBits();
std::uint32_t PadBits(std::size_t offset);
int Navigate(std::uintptr_t self, int cursor, int count);
enum class SoundKind { Decide, Cancel, Deny };
void Sound(SoundKind kind, std::uintptr_t self);
std::string WrapText(const std::string& text, std::size_t width);
/// A visible mod-menu row with an icon, as its label was drawn last frame: the label's centre in
/// the game's 1280x720 layout, its scale (1 on the cursor row, 0.8 on the others), the sum of its
/// glyph widths (font units, unscaled), its glyph count and colour.
struct RowIcon {
    std::string icon;
    int x = 0, y = 0;
    float scale = 1.0f;
    int textWidth = 0;
    int glyphs = 0;
    std::uint32_t argb = 0;
};
/// Empty unless the mod menu is up and waiting for input (no icons over its fades).
std::vector<RowIcon> VisibleRowIcons();
/// The costume slot whose bust goes with the cursor row (its own, else the screen's); -1 when
/// there is none or the mod menu is not up and waiting for input.
int CursorPortrait();
/// The game's Organize/Buy screen labels its two grids with mes_system 2366 and 2367 ("Chris",
/// "Sheva"); these texts stand in for them until RestoreItemBoxLabels. False (labels kept) when
/// the messages are not loaded or are not those names.
bool SetItemBoxLabels(const std::string& first, const std::string& second);
void RestoreItemBoxLabels();
/// The next time the Play menu waits for input, the mod menu opens by itself on `screen`
/// (above its main screen), as if ROGUE MERCENARIES had been chosen: a finished run's results.
void OpenOnPlayMenu(ScreenId screen);
/// Developer mode (M10 F0): at the next Play menu waiting for input, the game's own decide on its
/// Mercenaries row, with no run (the game's Mercenaries menu, played as the game plays it).
void DecideMercenariesOnPlayMenu();
void CancelOpenOnPlayMenu();
// True once (then cleared) after the player chose START RUN.
bool TakeChosen();
bool Chosen();

}  // namespace rmre5::nativemenu
