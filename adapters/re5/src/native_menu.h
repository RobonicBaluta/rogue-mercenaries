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
    RunResults, RunLength, MatchElement, WhichPlayer, MapEditor,
    MatchWait,  // M10: a player waits for the other (split-screen screens of their own)
    MatchTestElement  // the tester's special (element_pick): the gun in hand's element
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
    int grid = 0;            // M10: the first `grid` items are cells, three to a row (split screen)
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
    // M10: split-screen screens of each player's own (match_ui). How many players have them (1:
    // one screen for both, in turns); the screen `seat` belongs on now, given the first one of its
    // stack; and Build and Decide for that player.
    virtual int SplitSeats() { return 1; }
    virtual ScreenId SeatScreen(int seat, ScreenId current) { (void)seat; return current; }
    virtual Screen BuildFor(ScreenId id, int seat) { (void)seat; return Build(id); }
    virtual Decision DecideFor(ScreenId id, int index, int seat) { (void)seat; return Decide(id, index); }
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
/// Move: the cursor-move tick (silent when its signature is missing).
enum class SoundKind { Decide, Cancel, Deny, Move };
void Sound(SoundKind kind, std::uintptr_t self);
std::string WrapText(const std::string& text, std::size_t width);
/// A label's width in canvas units is scale x (kGlyphUnit x its glyph widths + kGlyphSpacing per
/// glyph): fitted to the perk lists at 1920x1080 (cursor rows exact, the others about 4 % wide).
constexpr float kGlyphUnit = 0.1907f;
constexpr float kGlyphSpacing = 5.64f;
/// Half the drawn width of a slot's first line at `scale` (canvas units); labels are centred on x.
float SlotHalfWidth(int slot, float scale);
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
/// Developer: the layout element `name` of the uId screen `screen` (its layer: screen +0x18), 0 when
/// none; for the UI lab's layout tests.
std::uintptr_t LayoutElementOf(std::uintptr_t screen, const char* name);
/// Developer: the layout element `name` on layout layer `layer` (0..0x5B), 0 when none.
std::uintptr_t LayoutElementOnLayer(int layer, const char* name);
/// Developer: logs every loaded message (any language, any file) whose text is `text`.
void LogMessagesWith(const std::string& text);
/// Developer: mes_system message `id` shows `text` (empty: the game's again).
void SetMessageText(int id, const std::string& text);
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
