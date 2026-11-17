// Native "ROGUE MERCENARIES" entry and mod menu inside RE5's Play menu
// (uIdStartUp01).
//
// Vanilla Play menu: the item count is raised by one (8 to 9; 7 to 8 for a player without the
// Extra Content DLC, whose menu has no eighth row), so the game draws, highlights and navigates one
// more row itself. Choosing it opens the mod menu
// *inside the same screen object*: from then on this module does the screen's
// per-frame work (input through the game's own menu helpers, then the game's
// own row/help draw), so the list shows the mod's screens with the game's
// font, highlight bar, sounds, planet node and help line. START RUN hands the
// frame back to the game as a decide on its "Los Mercenarios" row; Esc on the
// mod's main screen returns to the vanilla list.
//
// Text: every row, the help line and the header are borrowed mes_system
// messages (PS3/Xbox-only texts a PC build never shows). Their pointers are
// redirected to fixed buffers encoded in the game's glyph format while the
// Play menu is open and restored as soon as it closes.

#include "native_menu.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "autopilot.h"
#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::nativemenu {
namespace {

using MoveFn = void(__thiscall*)(std::uintptr_t self);
using MenuInputFn = std::uint32_t(__stdcall*)(int pad, int mode);
using NavigateFn = int(__thiscall*)(std::uintptr_t self, int cursor, int count);
using SoundFn = void(__thiscall*)(std::uintptr_t self);
using BallIndexFn = void(__thiscall*)(std::uintptr_t topMenu, int item);
using RowColorFn = void(__thiscall*)(std::uintptr_t self, std::uint32_t* argb, bool selected, std::uint8_t alpha,
                                     int fade);
using DrawHelpFn = void(__thiscall*)(std::uintptr_t self, int message, int y);
using DrawLabelFn = void(__thiscall*)(void* ecx, int message, int x, int y, const std::uint32_t* argb, float scale);

constexpr int kMercenariesItem = 6;
constexpr std::uint8_t kStateInput = 2;
constexpr std::uint32_t kRMessageCrc = 0x10C460E6;
constexpr std::uint32_t kMsg2Magic = 0x3247534D;  // "MSG2"
constexpr int kMaxRows = 8;                       // the highlight bar exists for rows 0..7
constexpr int kRowChars = 34;
constexpr int kHelpChars = 54;
constexpr int kHeaderY = 100;
constexpr float kHeaderScale = 0.55f;
constexpr char kLabel[] = "ROGUE MERCENARIES";
constexpr char kHelp[] = "Chain Mercenaries stages into one run\nwith perks and rewards in between.";

// The rows the game itself gives the menu: its init (0x00A39560) makes 8 when Extra Content is owned,
// and says so in kMenuExtraContentRow, and 7 otherwise (a friend's PC, 2026-11-08: no row, "the mod
// doesn't load"). Ours is the row after them.
int GameRows(std::uintptr_t menu) { return ReadOr<std::uint8_t>(menu + re5::off::kMenuExtraContentRow, 0) != 0 ? 8 : 7; }
int RogueItem(std::uintptr_t menu) { return GameRows(menu); }
int RogueRowY(std::uintptr_t menu) { return re5::addr::kPlayMenuFirstRowY + RogueItem(menu) * re5::addr::kPlayMenuRowPitch; }
// A ninth row takes the help line's place, which moves down; eight rows leave it where it is.
int HelpLineY(std::uintptr_t menu) {
    return re5::addr::kPlayMenuHelpY + (GameRows(menu) == 8 ? re5::addr::kPlayMenuRowPitch : 0);
}

// Borrowed message ids (mes_system): PS3 / Xbox LIVE texts. Slots 0..7 are
// the rows, 8 the help line, 9 the header.
constexpr int kPool[] = {626, 631, 632, 643, 653, 656, 956, 964, 1005, 1007};
constexpr int kSlotHelp = 8;
constexpr int kSlotHeader = 9;
constexpr int kSlots = static_cast<int>(sizeof(kPool) / sizeof(kPool[0]));
constexpr std::size_t kSlotWords = 2 * 128;  // (glyph, width) pairs per slot

MoveFn g_origMove = nullptr;
MoveFn g_origDraw = nullptr;
DrawHelpFn g_origDrawHelp = nullptr;
DrawLabelFn g_origDrawLabel = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_chosen{false};
std::atomic<DWORD> g_lastMenuFrame{0};
std::atomic<bool> g_atInput{false};  // menu state was "input" after the last move()
std::atomic<Model*> g_model{nullptr};
std::atomic<bool> g_modMenu{false};      // the mod's screens are showing
std::atomic<bool> g_resetRequested{false};
std::atomic<int> g_openOnMenu{-1};       // screen to open by itself at the next Play menu (-1 = none)
std::atomic<bool> g_devDecideMercenaries{false};  // DecideMercenariesOnPlayMenu
// Play menu inside its draw on this thread. The engine runs menu move() on
// job threads, and the label/help helpers are shared by every menu.
thread_local std::uintptr_t t_drawingMenu = 0;
thread_local std::uint32_t t_lockedRows = 0;  // bit per visible row drawn red
thread_local std::uint32_t t_rowColors[8] = {};  // rarity colour per visible row (0 = default)

// Where each visible row's label was drawn, for the icons the overlay puts next to it.
struct RowDraw {
    std::string icon;
    int x = 0, y = 0;
    float scale = 1.0f;
    int textWidth = 0;  // the label's glyph widths, unscaled
    int glyphs = 0;
    std::uint32_t argb = 0;
    DWORD drawnAt = 0;
};
std::mutex g_rowMutex;
std::array<RowDraw, 8> g_rowDraws{};
std::atomic<int> g_cursorPortrait{-1};

// ---- borrowed messages -------------------------------------------------------

// Texts are lent from menu job threads and returned from the game tick.
std::mutex g_textMutex;
std::array<std::array<std::uint16_t, kSlotWords>, kSlots> g_buffers{};
std::array<std::uintptr_t, kSlots> g_slotAddress{};   // message pointer cells
std::array<std::uintptr_t, kSlots> g_slotOriginal{};  // their original values
std::array<std::uint16_t, 0x200> g_width{};           // glyph widths from the loaded messages
bool g_widthsReady = false;
bool g_textFailed = false;
std::atomic<bool> g_applied{false};

struct Messages {
    std::uintptr_t table = 0;
    int count = 0;
    std::string path;
};

// The loaded mes_system of the game's language, or any loaded one. This used to be the Spanish
// table's fixed address: in every other language that slot is empty, and the row silently never
// appeared (2026-11-08, the game in English).
std::uintptr_t SystemMessages(std::uintptr_t sMessage) {
    const auto table = [sMessage](int language) {
        return ReadOr<std::uintptr_t>(sMessage + re5::off::kMessageFirstTable + language * re5::off::kMessageLanguageStride, 0);
    };
    const auto save = ReadOr<std::uintptr_t>(re5::addr::kSaveData, 0);
    const int language =
        save ? ReadOr<std::int32_t>(save + ReadOr<std::int32_t>(save + re5::off::kSaveSlotIndex, 0) * re5::off::kSaveLanguageStride + re5::off::kSaveLanguage, -1)
             : -1;
    if (language >= 0 && language < re5::off::kMessageLanguages && table(language)) return table(language);
    for (int other = 0; other < re5::off::kMessageLanguages; ++other) {
        if (table(other)) return table(other);
    }
    return 0;
}

Messages FindMessages() {
    const auto sMessage = ReadOr<std::uintptr_t>(re5::addr::kSMessageInstance, 0);
    const auto rMessage = sMessage ? SystemMessages(sMessage) : 0;
    if (!rMessage || ReadOr<std::uint32_t>(rMessage + re5::off::kRMessageTypeCrc, 0) != kRMessageCrc) return {};
    const std::string path = ReadCString(rMessage + re5::off::kRMessagePath, 64);
    if (path.rfind("etc\\message\\mes_system", 0) != 0) return {};
    const auto data = ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageData, 0);
    if (!data || ReadOr<std::uint32_t>(data, 0) != kMsg2Magic) return {};
    return {ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageTable, 0),
            ReadOr<std::uint16_t>(data + re5::off::kMsg2Count, 0), path};
}

// Glyph code for a character in the game's message encoding (Latin fonts:
// A=0x38, a=0x6C, two codes apart; digits 1-9 at 2, 4, ... 18 and 0 at 0).
bool GlyphFor(char c, std::uint16_t& code) {
    if (c >= 'A' && c <= 'Z') code = static_cast<std::uint16_t>(0x38 + 2 * (c - 'A'));
    else if (c >= 'a' && c <= 'z') code = static_cast<std::uint16_t>(0x6C + 2 * (c - 'a'));
    else if (c >= '0' && c <= '9') code = static_cast<std::uint16_t>(2 * (c - '0'));
    else if (c == ' ') code = 0x1C;
    else if (c == ',') code = 0x24;
    else if (c == '.') code = 0x26;
    else if (c == '-') code = 0x2E;
    else if (c == '(') code = 0x18;
    else if (c == ')') code = 0x1A;
    else if (c == '?') code = 0x16;
    else return false;
    return code < g_width.size() && g_width[code] != 0;
}

// Writes `text` into a message buffer: (code, width) pairs, (0, 0x0403) line breaks,
// (0, 0x0401) terminators filling the rest. Unknown characters become spaces.
template <std::size_t Words>
void EncodeInto(std::array<std::uint16_t, Words>& buf, const std::string& text) {
    std::size_t n = 0;
    for (char c : text) {
        if (n + 4 > buf.size()) break;
        if (c == '\n') {
            buf[n++] = 0;
            buf[n++] = 0x0403;
            continue;
        }
        std::uint16_t code = 0x1C;
        if (!GlyphFor(c, code)) code = 0x1C;
        buf[n++] = code;
        buf[n++] = g_width[code];
    }
    for (; n + 1 < buf.size(); n += 2) {
        buf[n] = 0;
        buf[n + 1] = 0x0401;
    }
}

void Encode(int slot, const std::string& text) { EncodeInto(g_buffers[static_cast<std::size_t>(slot)], text); }

// Breaks text into at most two lines of about `width` characters.
std::string Wrap(const std::string& text, std::size_t width) {
    std::string out;
    std::string line;
    int lines = 0;
    auto flush = [&] {
        if (lines > 0) out += '\n';
        out += line;
        line.clear();
        ++lines;
    };
    std::size_t i = 0;
    while (i < text.size() && lines < 2) {
        std::size_t end = text.find_first_of(" \n", i);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(i, end - i);
        if (!line.empty() && line.size() + 1 + word.size() > width) flush();
        if (lines >= 2) break;
        if (!line.empty()) line += ' ';
        line += word;
        if (end < text.size() && text[end] == '\n') flush();
        i = end + 1;
    }
    if (!line.empty() && lines < 2) flush();
    return out;
}

bool BuildWidths(const Messages& m) {
    // The game's own Mercenaries label must be Latin capitals in this encoding,
    // otherwise (Japanese, Chinese, Russian fonts) nothing can be encoded.
    const auto mercs = ReadOr<std::uintptr_t>(m.table + 943 * 4, 0);
    std::uint16_t first = 0;
    if (!mercs || !SafeRead(mercs, first) || first < 0x38 || first > 0x6A) {
        RM_LOGW("native menu: message font is not Latin; Rogue Mercenaries entry disabled");
        return false;
    }
    g_width.fill(0);
    for (int id = 0; id < m.count; ++id) {
        const auto text = ReadOr<std::uintptr_t>(m.table + id * 4, 0);
        if (!text) continue;
        for (int i = 0; i < 512; ++i) {
            std::uint16_t glyph[2] = {};
            if (!SafeCopy(glyph, text + i * 4, sizeof(glyph))) break;
            if (glyph[0] == 0 && glyph[1] == 0x0401) break;  // end of message
            // Control codes carry parameters >= 0x100 in the second field.
            if (glyph[0] < g_width.size() && glyph[1] > 0 && glyph[1] < 0x100) g_width[glyph[0]] = glyph[1];
        }
    }
    std::uint16_t code = 0;
    for (const char* p = kLabel; *p; ++p) {
        if (!GlyphFor(*p, code)) {
            RM_LOGW("native menu: no glyph for '%c'; entry disabled", *p);
            return false;
        }
    }
    return true;
}

void EncodeVanillaTexts() {
    Encode(0, kLabel);
    Encode(kSlotHelp, kHelp);
}

bool ApplyTexts() {
    if (g_applied.load()) return true;
    std::lock_guard lock(g_textMutex);
    if (g_applied.load()) return true;
    if (g_textFailed) return false;
    const Messages m = FindMessages();
    if (!m.table || m.count <= kPool[kSlots - 1]) {  // messages not loaded yet; retry next frame
        static int waits = 0;
        if (++waits == 300) RM_LOGW("native menu: the game's text table was not found (%d messages); no Rogue Mercenaries row yet", m.count);
        return false;
    }
    if (!g_widthsReady) {
        if (!BuildWidths(m)) {
            g_textFailed = true;
            return false;
        }
        g_widthsReady = true;
    }
    EncodeVanillaTexts();
    for (int i = 0; i < kSlots; ++i) {
        g_slotAddress[i] = m.table + kPool[i] * 4;
        g_slotOriginal[i] = ReadOr<std::uintptr_t>(g_slotAddress[i], 0);
        SafeStore(g_slotAddress[i], reinterpret_cast<std::uintptr_t>(g_buffers[i].data()));
    }
    g_applied = true;
    RM_LOGI("native menu: texts set (%d messages borrowed of %d, %s)", kSlots, m.count, m.path.c_str());
    return true;
}

void RestoreTexts() {
    std::lock_guard lock(g_textMutex);
    if (!g_applied.load()) return;
    g_applied = false;
    for (int i = 0; i < kSlots; ++i) {
        // Only put the original back if the cell still holds our pointer (the
        // message file may have been reloaded meanwhile).
        if (ReadOr<std::uintptr_t>(g_slotAddress[i], 0) == reinterpret_cast<std::uintptr_t>(g_buffers[i].data())) {
            SafeStore(g_slotAddress[i], g_slotOriginal[i]);
        }
        g_slotAddress[i] = 0;
    }
    RM_LOGI("native menu: texts restored");
}

// ---- the Organize/Buy screen's grid labels ---------------------------------------------

// mes_system 2366 + the panel's character (0 Chris, 1 Sheva; 2368 Jill, 2369 Wesker follow): the
// labels over the screen's two grids (drawn at 0x00A06041).
constexpr int kItemBoxLabels[] = {2366, 2367};
constexpr const char* kItemBoxLabelTexts[] = {"Chris", "Sheva"};
std::array<std::array<std::uint16_t, 64>, 2> g_labelBuffers{};
std::array<std::uintptr_t, 2> g_labelAddress{};
std::array<std::uintptr_t, 2> g_labelOriginal{};

// The message's text in the Latin encoding (letters only), to check what a cell holds.
std::string Letters(std::uintptr_t text) {
    std::string out;
    for (int i = 0; text && i < 32; ++i) {
        std::uint16_t glyph[2] = {};
        if (!SafeCopy(glyph, text + i * 4, sizeof(glyph)) || (glyph[0] == 0 && glyph[1] == 0x0401)) break;
        if (glyph[0] >= 0x38 && glyph[0] <= 0x6A && glyph[0] % 2 == 0) out += static_cast<char>('A' + (glyph[0] - 0x38) / 2);
        else if (glyph[0] >= 0x6C && glyph[0] <= 0x9E && glyph[0] % 2 == 0) out += static_cast<char>('a' + (glyph[0] - 0x6C) / 2);
        else out += '?';
    }
    return out;
}

// ---- game helpers ----------------------------------------------------------------

std::uint32_t MenuInputBits() {
    __try {
        return reinterpret_cast<MenuInputFn>(re5::addr::kFnMenuInput)(-1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint32_t PadMask(std::size_t offset) {
    const auto pad = ReadOr<std::uintptr_t>(re5::addr::kPadInstance, 0);
    return pad ? ReadOr<std::uint32_t>(pad + offset, 0) : 0;
}

void PlaySound(std::uintptr_t fn, std::uintptr_t self) { reinterpret_cast<SoundFn>(fn)(self); }

void SetBallIndex(std::uintptr_t self, int item) {
    const auto topMenu = ReadOr<std::uintptr_t>(self + re5::off::kMenuTopMenu, 0);
    if (topMenu) reinterpret_cast<BallIndexFn>(re5::addr::kFnMenuSetBallIndex)(topMenu, item);
}

bool Extended(std::uintptr_t menu) {
    return g_applied.load() && ReadOr<std::int32_t>(menu + re5::off::kMenuItemCount, 0) == GameRows(menu) + 1;
}

std::atomic<bool> g_devMercenariesLocked{false};

// START RUN is the game's own decide on its Mercenaries row, which the game refuses on a save that has
// not opened that mode yet (the row reads "???"). The run does not need it: the mod sets up the match
// itself. The row counts as open for that one call; the save is not touched.
void MoveWithMercenariesOpen(std::uintptr_t self) {
    const auto work = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    const auto address = work + re5::off::kGameWorkPlayMenuMask;
    const auto mask = work ? ReadOr<std::uint32_t>(address, 0) : 0;
    const std::uint32_t row = 1u << kMercenariesItem;
    const bool closed = work != 0 && (mask & row) == 0;
    if (closed) {
        SafeStore<std::uint32_t>(address, mask | row);
        RM_LOGI("native menu: this save has not opened The Mercenaries; a run does not need it");
    }
    g_origMove(self);
    if (closed) SafeStore<std::uint32_t>(address, mask);
}

// ---- mod menu ------------------------------------------------------------------------

struct Level {
    ScreenId id = ScreenId::Main;
    int cursor = 0;
    int offset = 0;  // first visible item
};

// Touched only from the Play menu's move() (one call per frame).
std::mutex g_menuMutex;
std::vector<Level> g_stack;
Screen g_screen;
bool g_dirty = false;
bool g_buttonLatch = false;  // decide/cancel acted on; wait for a release

void Rebuild() {
    Model* model = g_model.load();
    g_screen = model ? model->Build(g_stack.back().id) : Screen{};
    Level& level = g_stack.back();
    const int n = static_cast<int>(g_screen.items.size());
    level.cursor = std::clamp(level.cursor, 0, std::max(n - 1, 0));
    g_dirty = true;
}

void EnterModMenu(std::uintptr_t self, bool sound = true) {
    g_stack.assign(1, Level{});
    Rebuild();
    g_modMenu = true;
    SafeStore<std::uint8_t>(self + re5::off::kMenuBlink, 0);
    SetBallIndex(self, 0);
    if (sound) PlaySound(re5::addr::kFnMenuSoundDecide, self);
    RM_LOGI("native menu: mod menu opened");
}

void LeaveModMenu(std::uintptr_t self, int cursor) {
    g_stack.clear();
    g_modMenu = false;
    SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, GameRows(self) + 1);
    SafeStore<std::int32_t>(self + re5::off::kMenuCursor, cursor);
    SafeStore<std::uint8_t>(self + re5::off::kMenuBlink, 0);
    SetBallIndex(self, cursor);
    EncodeVanillaTexts();
    RM_LOGI("native menu: mod menu closed (row %d)", cursor);
}

// Writes the visible window into the screen object and the texts into the
// borrowed messages.
void SyncRows(std::uintptr_t self) {
    Level& level = g_stack.back();
    const int n = static_cast<int>(g_screen.items.size());
    const int visible = std::min(n, kMaxRows);
    if (level.cursor < level.offset) level.offset = level.cursor;
    if (level.cursor >= level.offset + visible) level.offset = level.cursor - visible + 1;
    level.offset = std::clamp(level.offset, 0, std::max(n - visible, 0));
    SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, visible);
    SafeStore<std::int32_t>(self + re5::off::kMenuCursor, level.cursor - level.offset);
    SafeStore<std::uint32_t>(self + re5::off::kMenuEnabledMask, 0xFFFF);
    SafeStore<std::uint8_t>(self + re5::off::kMenuBlink, 0);
    t_lockedRows = 0;
    for (int r = 0; r < kMaxRows; ++r) t_rowColors[r] = 0;
    for (int r = 0; r < visible; ++r) {
        const Item& item = g_screen.items[static_cast<std::size_t>(level.offset + r)];
        if (item.locked) t_lockedRows |= 1u << r;
        t_rowColors[r] = item.color;
    }
    const int portrait = n > 0 ? g_screen.items[static_cast<std::size_t>(level.cursor)].portrait : -1;
    g_cursorPortrait = portrait >= 0 ? portrait : g_screen.portrait;
    {
        std::lock_guard lock(g_rowMutex);
        for (int r = 0; r < kMaxRows; ++r) {
            g_rowDraws[r].icon = r < visible ? g_screen.items[static_cast<std::size_t>(level.offset + r)].icon : std::string();
        }
    }
    if (!g_dirty) return;
    g_dirty = false;
    for (int r = 0; r < kMaxRows; ++r) {
        const int index = level.offset + r;
        std::string label = index < n ? g_screen.items[static_cast<std::size_t>(index)].label : std::string();
        if (label.size() > kRowChars) label.resize(kRowChars);
        Encode(r, label);
    }
    std::string help = n > 0 ? g_screen.items[static_cast<std::size_t>(level.cursor)].help : g_screen.empty_help;
    if (n > visible) {
        help += level.offset + visible < n ? "\nMore below." : "";
    }
    Encode(kSlotHelp, Wrap(help, kHelpChars));
    Encode(kSlotHeader, g_screen.header);
}

void __fastcall PlayMenuDrawDetour(std::uintptr_t self, void*);

// Returns true when the frame was fully handled (including the draw).
bool ModMenuFrame(std::uintptr_t self, bool readInput) {
    if (ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) != kStateInput) {
        // Something else moved the screen on; fall back to the vanilla list.
        LeaveModMenu(self, RogueItem(self));
        return false;
    }
    Level* level = &g_stack.back();
    const int n = static_cast<int>(g_screen.items.size());
    if (readInput) {
        const std::uint32_t input = MenuInputBits();
        const bool decide = (input & PadMask(re5::off::kPadDecideMask)) != 0;
        const bool cancel = (input & PadMask(re5::off::kPadCancelMask)) != 0;
        if (!decide && !cancel) g_buttonLatch = false;
        if (decide && !g_buttonLatch) {
            g_buttonLatch = true;
            Model* model = g_model.load();
            Decision d;
            d.outcome = model ? Outcome::Stay : Outcome::StartRun;  // no model: behave like the plain row
            if (model && n > 0) d = model->Decide(level->id, level->cursor);
            switch (d.outcome) {
                case Outcome::Stay: break;
                case Outcome::Refresh:
                    PlaySound(re5::addr::kFnMenuSoundDecide, self);
                    Rebuild();
                    break;
                case Outcome::Push:
                    PlaySound(re5::addr::kFnMenuSoundDecide, self);
                    g_stack.push_back(Level{d.next});
                    Rebuild();
                    SetBallIndex(self, 0);
                    break;
                case Outcome::Back:
                    PlaySound(re5::addr::kFnMenuSoundCancel, self);
                    if (g_stack.size() > 1) {
                        g_stack.pop_back();
                        Rebuild();
                    } else {
                        LeaveModMenu(self, RogueItem(self));
                        return false;
                    }
                    break;
                case Outcome::StartRun:
                    // The game's own decide on "Los Mercenarios" (this frame's
                    // decide press is still visible to the original move).
                    LeaveModMenu(self, kMercenariesItem);
                    g_chosen = true;
                    RM_LOGI("native menu: START RUN chosen");
                    MoveWithMercenariesOpen(self);
                    return true;
                case Outcome::Denied: PlaySound(re5::addr::kFnMenuSoundDeny, self); break;
                case Outcome::Close:
                    PlaySound(re5::addr::kFnMenuSoundCancel, self);
                    LeaveModMenu(self, RogueItem(self));
                    return false;
            }
            level = &g_stack.back();
        } else if (cancel && !g_buttonLatch) {
            g_buttonLatch = true;
            PlaySound(re5::addr::kFnMenuSoundCancel, self);
            if (g_stack.size() > 1) {
                g_stack.pop_back();
                Rebuild();
                level = &g_stack.back();
                SetBallIndex(self, std::min(level->cursor, kMaxRows - 1));
            } else {
                LeaveModMenu(self, RogueItem(self));
                return false;
            }
        } else if (n > 0) {
            const int before = level->cursor;
            const int after = reinterpret_cast<NavigateFn>(re5::addr::kFnMenuNavigate)(self, before, n);
            if (after != before && after >= 0 && after < n) {
                level->cursor = after;
                g_dirty = true;
                SetBallIndex(self, std::min(after, kMaxRows - 1));
            }
        }
    }
    SyncRows(self);
    PlayMenuDrawDetour(self, nullptr);
    return true;
}

// ---- hooks ---------------------------------------------------------------------------

void __fastcall PlayMenuMoveDetour(std::uintptr_t self, void*) {
    if (!g_enabled.load()) {
        g_origMove(self);
        return;
    }
    g_lastMenuFrame = GetTickCount();
    std::lock_guard lock(g_menuMutex);
    if (g_resetRequested.exchange(false) && g_modMenu.load()) LeaveModMenu(self, RogueItem(self));
    if (g_modMenu.load()) {
        if (ModMenuFrame(self, true)) {
            g_atInput = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) == kStateInput;
            return;
        }
    }
    if (g_devMercenariesLocked.load()) {
        const auto work = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
        if (work) {
            SafeStore<std::uint32_t>(work + re5::off::kGameWorkPlayMenuMask,
                                     ReadOr<std::uint32_t>(work + re5::off::kGameWorkPlayMenuMask, 0) & ~(1u << kMercenariesItem));
        }
    }
    std::int32_t count = 0;
    // Add our row to the game's 7 or 8 (one more once extended).
    // Trial/benchmark variants use other counts and keep the vanilla menu.
    const int gameRows = GameRows(self);
    if (SafeRead(self + re5::off::kMenuItemCount, count) && (count == gameRows || count == gameRows + 1)) {
        SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, ApplyTexts() ? gameRows + 1 : gameRows);
    }
    static std::uintptr_t loggedMenu = 0;
    if (loggedMenu != self) {
        loggedMenu = self;
        RM_LOGI("native menu: the Play menu has %d rows of its own (item count %d); ROGUE MERCENARIES is row %d", gameRows, count, RogueItem(self));
    }
    const auto cursor = ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1);
    static int lastCursor = -2;
    static int cursorLines = 0;
    if (cursor != lastCursor && cursorLines < 64) {
        lastCursor = cursor;
        ++cursorLines;
        RM_LOGI("native menu: cursor on row %d of %d", cursor, ReadOr<std::int32_t>(self + re5::off::kMenuItemCount, 0));
    }
    const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
    // Back from a finished run: the mod menu opens by itself on the run's results.
    if (state == kStateInput && Extended(self) && g_openOnMenu.load() >= 0) {
        const auto screen = static_cast<ScreenId>(g_openOnMenu.exchange(-1));
        EnterModMenu(self, false);
        if (screen != ScreenId::Main) {
            g_stack.push_back(Level{screen});
            Rebuild();
        }
        g_buttonLatch = true;  // a button still held from the match acts only once released
        ModMenuFrame(self, false);
        g_atInput = true;
        RM_LOGI("native menu: opened on screen %d by itself", static_cast<int>(screen));
        return;
    }
    if (state == kStateInput && g_devDecideMercenaries.exchange(false)) {
        SafeStore<std::int32_t>(self + re5::off::kMenuCursor, kMercenariesItem);
        SetBallIndex(self, kMercenariesItem);
        RM_LOGW("native menu: dev: the game's decide on The Mercenaries (no run)");
        autopilot::InjectMenuBits(autopilot::DecideBits());
        MoveWithMercenariesOpen(self);
        autopilot::InjectMenuBits(0);
        return;
    }
    // move() handles decide before navigation, so the cursor here is the
    // row drawn highlighted last frame.
    if (state == kStateInput && (MenuInputBits() & PadMask(re5::off::kPadDecideMask)) != 0) {
        RM_LOGI("native menu: decide on row %d of %d", cursor, ReadOr<std::int32_t>(self + re5::off::kMenuItemCount, 0));
        if (cursor == RogueItem(self) && Extended(self)) {
            EnterModMenu(self);
            g_buttonLatch = true;  // this press opened the menu
            ModMenuFrame(self, false);
            g_atInput = true;
            return;
        }
    }
    g_origMove(self);
    // A decide moves the state off "input" within this same call.
    g_atInput = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) == kStateInput;
}

void __fastcall PlayMenuDrawDetour(std::uintptr_t self, void*) {
    const bool mod = g_enabled.load() && g_modMenu.load();
    const bool extended = !mod && g_enabled.load() && Extended(self);
    if (extended) {
        // move() refreshes the enabled mask from game work every frame and
        // then calls this draw.
        const auto mask = ReadOr<std::uint32_t>(self + re5::off::kMenuEnabledMask, 0);
        SafeStore<std::uint32_t>(self + re5::off::kMenuEnabledMask, mask | (1u << RogueItem(self)));
    }
    if (mod || extended) t_drawingMenu = self;
    g_origDraw(self);
    t_drawingMenu = 0;
    if (!g_applied.load()) return;
    if (mod) {
        // The game's help was suppressed in DrawHelpDetour; draw the mod's.
        g_origDrawHelp(self, kPool[kSlotHelp], re5::addr::kPlayMenuHelpY);
        std::uint32_t argb = 0;
        reinterpret_cast<RowColorFn>(re5::addr::kFnMenuRowColor)(self, &argb, false, 0xFF, 0);
        g_origDrawLabel(reinterpret_cast<void*>(self), kPool[kSlotHeader], 640, kHeaderY, &argb, kHeaderScale);
    } else if (extended && ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1) == RogueItem(self)) {
        // Ours: the game draws none for row 8, and for row 7 its Extra Content text (kept out below).
        g_origDrawHelp(self, kPool[kSlotHelp], HelpLineY(self));
    }
}

std::atomic<bool> g_drawProbe{false};

void ProbeDraw(const char* kind, std::uintptr_t self, int message, int x, int y, float scale) {
    static std::mutex mutex;
    static std::set<std::tuple<int, int, int, int>> seen;
    std::lock_guard lock(mutex);
    if (seen.size() >= 160 || !seen.insert({message, x, y, static_cast<int>(scale * 100)}).second) return;
    RM_LOGI("draw probe: %s self=%08X message %d at (%d, %d) scale %.2f", kind, static_cast<unsigned>(self), message, x,
            y, scale);
}

void RecordRowDraw(int row, int x, int y, float scale, std::uint32_t argb) {
    int width = 0;
    int glyphs = 0;
    const auto& buf = g_buffers[static_cast<std::size_t>(row)];
    for (std::size_t i = 0; i + 1 < buf.size() && !(buf[i] == 0 && (buf[i + 1] == 0x0401 || buf[i + 1] == 0x0403)); i += 2) {
        width += buf[i + 1];
        ++glyphs;
    }
    std::lock_guard lock(g_rowMutex);
    RowDraw& d = g_rowDraws[static_cast<std::size_t>(row)];
    static std::set<std::tuple<int, int, int>> logged;
    if (!d.icon.empty() && logged.size() < 64 && logged.insert({row, width, static_cast<int>(scale * 1000)}).second) {
        RM_LOGI("native menu: row %d label at (%d, %d) scale %.3f, glyph widths %d", row, x, y, scale, width);
    }
    d.x = x;
    d.y = y;
    d.scale = scale;
    d.textWidth = width;
    d.glyphs = glyphs;
    d.argb = argb;
    d.drawnAt = GetTickCount();
}

void __fastcall DrawLabelDetour(void* ecx, void*, int message, int x, int y, const std::uint32_t* argb,
                                float scale) {
    if (g_drawProbe.load() && !t_drawingMenu) ProbeDraw("label", reinterpret_cast<std::uintptr_t>(ecx), message, x, y, scale);
    if (t_drawingMenu) {
        if (g_modMenu.load()) {
            const int row = (y - re5::addr::kPlayMenuFirstRowY) / re5::addr::kPlayMenuRowPitch;
            if (row >= 0 && row < kMaxRows && y == re5::addr::kPlayMenuFirstRowY + row * re5::addr::kPlayMenuRowPitch) {
                message = kPool[row];
                if (t_lockedRows & (1u << row)) {
                    // The game's own "disabled" tint: keep alpha and red.
                    thread_local std::uint32_t red = 0;
                    red = (argb ? *argb : 0xFFFFFFFFu) & 0xFFFF0000u;
                    argb = &red;
                } else if (t_rowColors[row] != 0) {
                    // Rarity colour, with the game's alpha (its highlight fades the others).
                    thread_local std::uint32_t tint = 0;
                    tint = ((argb ? *argb : 0xFFFFFFFFu) & 0xFF000000u) | (t_rowColors[row] & 0x00FFFFFFu);
                    argb = &tint;
                }
                RecordRowDraw(row, x, y, scale, argb ? *argb : 0xFFFFFFFFu);
            }
        } else if (y == RogueRowY(t_drawingMenu)) {
            message = kPool[0];
        }
    }
    g_origDrawLabel(ecx, message, x, y, argb, scale);
}

void __fastcall DrawHelpDetour(std::uintptr_t self, void*, int message, int y) {
    if (g_drawProbe.load() && self != t_drawingMenu) ProbeDraw("help", self, message, 0, y, 1.0f);
    if (self && self == t_drawingMenu) {
        if (g_modMenu.load()) return;  // replaced after the draw
        if (ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1) == RogueItem(self)) return;  // ours, after the draw
        if (y == re5::addr::kPlayMenuHelpY) y = HelpLineY(self);
    }
    g_origDrawHelp(self, message, y);
}

bool QueueHook(std::uintptr_t address, void* detour, void** original) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("native menu: hook at 0x%08X failed: %s", static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

void SetModel(Model* model) { g_model = model; }

void EnableDrawProbe() { g_drawProbe = true; }

void SetDevSimulation(bool noExtraContent, bool mercenariesLocked) {
    g_devMercenariesLocked = mercenariesLocked;
    if (!noExtraContent) return;
    RM_LOGW("dev: the game is told it owns no Extra Content");
    // What the game sets from Steam at start-up; kept clear so its menus take the path a player
    // without the DLC gets. A test aid: the thread lives as long as the process.
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (;;) {
                if (const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0)) {
                    SafeStore<std::uint8_t>(app + re5::off::kAppOwnsExtraContent, 0);
                    SafeStore<std::uint8_t>(app + re5::off::kAppOwnsExtraContent2, 0);
                }
                if (const auto message = ReadOr<std::uintptr_t>(re5::addr::kSMessageInstance, 0)) {
                    SafeStore<std::int32_t>(message + re5::off::kMessageExtraContent, 0);
                }
                Sleep(20);
            }
        }, nullptr, 0, nullptr)) {
        CloseHandle(t);
    }
}

std::atomic<bool> g_holdTexts{false};
bool TextsReady() { return ApplyTexts(); }
void HoldTexts(bool hold) { g_holdTexts = hold; }
void SetSlotText(int slot, const std::string& text) {
    if (slot >= 0 && slot < kSlots) Encode(slot, text);
}
void DrawSlot(std::uintptr_t self, int slot, int x, int y, float scale, std::uint32_t argb) {
    if (slot < 0 || slot >= kSlots || !g_origDrawLabel || !self) return;
    g_origDrawLabel(reinterpret_cast<void*>(self), kPool[slot], x, y, &argb, scale);
}
std::uint32_t InputBits() { return MenuInputBits(); }
std::uint32_t PadBits(std::size_t offset) { return PadMask(offset); }
int Navigate(std::uintptr_t self, int cursor, int count) {
    return reinterpret_cast<NavigateFn>(re5::addr::kFnMenuNavigate)(self, cursor, count);
}
void Sound(SoundKind kind, std::uintptr_t self) {
    PlaySound(kind == SoundKind::Decide ? re5::addr::kFnMenuSoundDecide
              : kind == SoundKind::Cancel ? re5::addr::kFnMenuSoundCancel : re5::addr::kFnMenuSoundDeny, self);
}
std::string WrapText(const std::string& text, std::size_t width) { return Wrap(text, width); }

std::vector<RowIcon> VisibleRowIcons() {
    std::vector<RowIcon> out;
    if (!g_modMenu.load() || !MenuAtInput()) return out;
    const DWORD now = GetTickCount();
    std::lock_guard lock(g_rowMutex);
    for (const RowDraw& d : g_rowDraws) {
        if (d.icon.empty() || now - d.drawnAt > 150) continue;
        out.push_back({d.icon, d.x, d.y, d.scale, d.textWidth, d.glyphs, d.argb});
    }
    return out;
}

int CursorPortrait() { return g_modMenu.load() && MenuAtInput() ? g_cursorPortrait.load() : -1; }

bool SetItemBoxLabels(const std::string& first, const std::string& second) {
    std::lock_guard lock(g_textMutex);
    const Messages m = FindMessages();
    if (!g_widthsReady || !m.table || m.count <= kItemBoxLabels[1]) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto cell = m.table + static_cast<std::uintptr_t>(kItemBoxLabels[i]) * 4;
        const auto text = ReadOr<std::uintptr_t>(cell, 0);
        if (text == reinterpret_cast<std::uintptr_t>(g_labelBuffers[i].data())) continue;  // already ours
        // Only over the game's own names: another language's table may number them differently.
        if (Letters(text) != kItemBoxLabelTexts[i]) {
            RM_LOGW("native menu: message %d is not \"%s\"; the item box keeps its labels", kItemBoxLabels[i], kItemBoxLabelTexts[i]);
            return false;
        }
        g_labelAddress[i] = cell;
        g_labelOriginal[i] = text;
    }
    EncodeInto(g_labelBuffers[0], first);
    EncodeInto(g_labelBuffers[1], second);
    for (std::size_t i = 0; i < 2; ++i) SafeStore(g_labelAddress[i], reinterpret_cast<std::uintptr_t>(g_labelBuffers[i].data()));
    return true;
}

void RestoreItemBoxLabels() {
    std::lock_guard lock(g_textMutex);
    for (std::size_t i = 0; i < 2; ++i) {
        if (g_labelAddress[i] && ReadOr<std::uintptr_t>(g_labelAddress[i], 0) == reinterpret_cast<std::uintptr_t>(g_labelBuffers[i].data())) {
            SafeStore(g_labelAddress[i], g_labelOriginal[i]);
        }
        g_labelAddress[i] = 0;
    }
}

bool Install() {
    if (!engine::Caps().playMenu) {
        RM_LOGW("native menu: signatures missing; Play menu entry disabled");
        return false;
    }
    // The move hook goes last: without it the other detours never activate.
    if (!QueueHook(re5::addr::kFnMenuDrawHelp, reinterpret_cast<void*>(&DrawHelpDetour),
                   reinterpret_cast<void**>(&g_origDrawHelp)) ||
        !QueueHook(re5::addr::kFnMenuDrawLabel, reinterpret_cast<void*>(&DrawLabelDetour),
                   reinterpret_cast<void**>(&g_origDrawLabel)) ||
        !QueueHook(re5::addr::kFnPlayMenuDraw, reinterpret_cast<void*>(&PlayMenuDrawDetour),
                   reinterpret_cast<void**>(&g_origDraw)) ||
        !QueueHook(re5::addr::kFnPlayMenuMove, reinterpret_cast<void*>(&PlayMenuMoveDetour),
                   reinterpret_cast<void**>(&g_origMove))) {
        return false;
    }
    g_enabled = true;
    RM_LOGI("native menu: hook uIdStartUp01::move at 0x%08X queued", static_cast<unsigned>(re5::addr::kFnPlayMenuMove));
    return true;
}

void OnGameTick() {
    // The menu object stops calling move() when it closes; restore the texts.
    if (g_applied.load() && !g_holdTexts.load() && GetTickCount() - g_lastMenuFrame.load() > 500) {
        RestoreTexts();
        if (g_modMenu.load()) g_resetRequested = true;
    }
}

bool MenuAtInput() { return g_atInput.load() && GetTickCount() - g_lastMenuFrame.load() < 500; }
void OpenOnPlayMenu(ScreenId screen) { g_openOnMenu = static_cast<int>(screen); }
void CancelOpenOnPlayMenu() { g_openOnMenu = -1; }
void DecideMercenariesOnPlayMenu() { g_devDecideMercenaries = true; }
bool TakeChosen() { return g_chosen.exchange(false); }
bool Chosen() { return g_chosen.load(); }

}  // namespace rmre5::nativemenu
