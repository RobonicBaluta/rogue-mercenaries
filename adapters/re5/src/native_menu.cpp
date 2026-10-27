// Native "ROGUE MERCENARIES" entry and mod menu inside RE5's Play menu
// (uIdStartUp01).
//
// Vanilla Play menu: the item count is raised from 8 to 9, so the game draws,
// highlights and navigates a ninth row itself. Choosing it opens the mod menu
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
#include <string>
#include <vector>

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

constexpr int kRogueItem = 8;
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
constexpr int kRogueRowY = re5::addr::kPlayMenuFirstRowY + kRogueItem * re5::addr::kPlayMenuRowPitch;
constexpr int kShiftedHelpY = re5::addr::kPlayMenuHelpY + re5::addr::kPlayMenuRowPitch;

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
// Play menu inside its draw on this thread. The engine runs menu move() on
// job threads, and the label/help helpers are shared by every menu.
thread_local std::uintptr_t t_drawingMenu = 0;
thread_local std::uint32_t t_lockedRows = 0;  // bit per visible row drawn red

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
};

Messages FindMessages() {
    const auto sMessage = ReadOr<std::uintptr_t>(re5::addr::kSMessageInstance, 0);
    const auto rMessage = sMessage ? ReadOr<std::uintptr_t>(sMessage + re5::off::kMessageSystemSlot, 0) : 0;
    if (!rMessage || ReadOr<std::uint32_t>(rMessage + re5::off::kRMessageTypeCrc, 0) != kRMessageCrc) return {};
    const std::string path = ReadCString(rMessage + re5::off::kRMessagePath, 64);
    if (path.rfind("etc\\message\\mes_system", 0) != 0) return {};
    const auto data = ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageData, 0);
    if (!data || ReadOr<std::uint32_t>(data, 0) != kMsg2Magic) return {};
    return {ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageTable, 0),
            ReadOr<std::uint16_t>(data + re5::off::kMsg2Count, 0)};
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

// Writes `text` into a slot: (code, width) pairs, (0, 0x0403) line breaks,
// (0, 0x0401) terminators filling the rest. Unknown characters become spaces.
void Encode(int slot, const std::string& text) {
    auto& buf = g_buffers[static_cast<std::size_t>(slot)];
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
    if (!m.table || m.count <= kPool[kSlots - 1]) return false;  // messages not loaded yet; retry next frame
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
    RM_LOGI("native menu: texts set (%d messages borrowed of %d)", kSlots, m.count);
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
    return g_applied.load() && ReadOr<std::int32_t>(menu + re5::off::kMenuItemCount, 0) == 9;
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

void EnterModMenu(std::uintptr_t self) {
    g_stack.assign(1, Level{});
    Rebuild();
    g_modMenu = true;
    SafeStore<std::uint8_t>(self + re5::off::kMenuBlink, 0);
    SetBallIndex(self, 0);
    PlaySound(re5::addr::kFnMenuSoundDecide, self);
    RM_LOGI("native menu: mod menu opened");
}

void LeaveModMenu(std::uintptr_t self, int cursor) {
    g_stack.clear();
    g_modMenu = false;
    SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, 9);
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
    for (int r = 0; r < visible; ++r) {
        if (g_screen.items[static_cast<std::size_t>(level.offset + r)].locked) t_lockedRows |= 1u << r;
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
        LeaveModMenu(self, kRogueItem);
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
            d.outcome = Outcome::StartRun;  // no model: behave like the plain row
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
                        LeaveModMenu(self, kRogueItem);
                        return false;
                    }
                    break;
                case Outcome::StartRun:
                    // The game's own decide on "Los Mercenarios" (this frame's
                    // decide press is still visible to the original move).
                    LeaveModMenu(self, kMercenariesItem);
                    g_chosen = true;
                    RM_LOGI("native menu: START RUN chosen");
                    g_origMove(self);
                    return true;
                case Outcome::Denied: PlaySound(re5::addr::kFnMenuSoundDeny, self); break;
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
                LeaveModMenu(self, kRogueItem);
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
    if (g_resetRequested.exchange(false) && g_modMenu.load()) LeaveModMenu(self, kRogueItem);
    if (g_modMenu.load()) {
        if (ModMenuFrame(self, true)) {
            g_atInput = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) == kStateInput;
            return;
        }
    }
    std::int32_t count = 0;
    // Only extend the full Gold Edition menu (8 rows; 9 once extended).
    // Trial/benchmark variants use other counts and keep the vanilla menu.
    if (SafeRead(self + re5::off::kMenuItemCount, count) && (count == 8 || count == 9)) {
        SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, ApplyTexts() ? 9 : 8);
    }
    const auto cursor = ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1);
    const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
    // move() handles decide before navigation, so the cursor here is the
    // row drawn highlighted last frame.
    if (state == kStateInput && (MenuInputBits() & PadMask(re5::off::kPadDecideMask)) != 0) {
        RM_LOGI("native menu: decide on row %d of %d", cursor, ReadOr<std::int32_t>(self + re5::off::kMenuItemCount, 0));
        if (cursor == kRogueItem && Extended(self)) {
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
        SafeStore<std::uint32_t>(self + re5::off::kMenuEnabledMask, mask | (1u << kRogueItem));
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
    } else if (extended && ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1) == kRogueItem) {
        // The game draws no help line for row 8.
        g_origDrawHelp(self, kPool[kSlotHelp], kShiftedHelpY);
    }
}

void __fastcall DrawLabelDetour(void* ecx, void*, int message, int x, int y, const std::uint32_t* argb,
                                float scale) {
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
                }
            }
        } else if (message == re5::addr::kMessageItem8Label && y == kRogueRowY) {
            message = kPool[0];
        }
    }
    g_origDrawLabel(ecx, message, x, y, argb, scale);
}

void __fastcall DrawHelpDetour(std::uintptr_t self, void*, int message, int y) {
    if (self && self == t_drawingMenu) {
        if (g_modMenu.load()) return;  // replaced after the draw
        if (y == re5::addr::kPlayMenuHelpY) y = kShiftedHelpY;
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
    if (g_applied.load() && GetTickCount() - g_lastMenuFrame.load() > 500) {
        RestoreTexts();
        if (g_modMenu.load()) g_resetRequested = true;
    }
}

bool MenuAtInput() { return g_atInput.load() && GetTickCount() - g_lastMenuFrame.load() < 500; }
bool TakeChosen() { return g_chosen.exchange(false); }
bool Chosen() { return g_chosen.load(); }

}  // namespace rmre5::nativemenu
