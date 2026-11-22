// In-match native screens. See match_ui.h.
//
// The pause screen (uIdPause00) exists for the whole match and its move() runs every frame,
// paused or not; the detour calls the original and then draws the mod's screen with the
// game's menu text routine (the same one the Play menu rows use, through the message slots
// borrowed by native_menu). The world is frozen with the request bit the pause menu itself
// sets on the application object; input comes from the game's menu input query and its
// list navigation helper, exactly like the title menus.
//
// M10: in a split-screen run each player has a host of their own (a stack of screens, its cursor
// and its input): host 0 is player 1's (the game's text in the top half), host 1 player 2's (the
// overlay draws it in the bottom half from the View published here). Each follows the run through
// Model::SeatScreen: a player who chose their reward waits for the other, and so on. Their input is
// their own pad's bits in the game's pad object (the menu input query reads only its main pad and
// the keyboard), moved through the game's own list step.

#include "match_ui.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "menu_mouse.h"
#include "re5_layout.h"

namespace rmre5::matchui {
namespace {

using namespace nativemenu;
using MoveFn = void(__fastcall*)(std::uintptr_t self, void*);
using LockFn = void(__stdcall*)(void* cs);
using KeyboardBitsFn = std::uint32_t(__thiscall*)(std::uintptr_t pads, int mode);
using ListStepFn = int(__thiscall*)(std::uintptr_t self, std::uint32_t trigger, std::uint32_t repeat, int cursor, int count);

constexpr int kMaxRows = 8;
constexpr int kRowChars = 34;
constexpr int kHelpChars = 56;
constexpr int kHeaderY = 150;
constexpr int kFirstRowY = 230;
constexpr int kRowPitch = 42;
constexpr int kHelpY = 600;
// HUD (1280x720 canvas, x = text centre): the Mercenaries HUD put the score at 100..347 and
// the combo at 923..1127 (y 117..160) and the clock's big digits at 613..667 (y 110..163).
constexpr int kNoticeY = 36;
constexpr int kSideY = 128;
constexpr int kLeftX = 224;
constexpr int kRightX = 1056;
constexpr int kCenterY = 176;
// M10: in a split screen the game draws its counter small at the top of each view (about 28..57 on
// the canvas, dist/media-0.2.1/24-two-players-split-screen.jpg), so the label goes just under it.
constexpr int kSplitCenterY = 70;
constexpr float kHeaderScale = 0.55f;
constexpr float kRowScale = 0.5f;
constexpr float kHelpScale = 0.42f;
constexpr float kNoticeScale = 0.38f;
constexpr float kSideScale = 0.46f;
constexpr float kCenterScale = 0.36f;
// M10: on the game's split screen its text routine draws in the top view only (clipped at the
// middle), so the screens are laid out in the top half: closer rows, fewer at once, the help above
// the middle (1280x720 canvas; the top view is 0..360). Player 2's, drawn by the overlay, use the
// same layout in the bottom half.
struct Layout {
    int headerY, firstRowY, rowPitch, helpY, maxRows;
    float headerScale, rowScale, helpScale;
};
constexpr Layout kFullLayout{kHeaderY, kFirstRowY, kRowPitch, kHelpY, kMaxRows, kHeaderScale, kRowScale, kHelpScale};
constexpr Layout kSplitLayout{22, 64, 32, 290, 7, 0.46f, 0.40f, 0.34f};
// M10: a grid screen (the item grid, Screen::grid) on a split half: three cells to a row, the rest
// of the screen's rows listed under them, a few at once.
constexpr int kCellPitchX = 400;
constexpr int kCellPitchY = 40;
constexpr int kCellHalfWidth = 190;
constexpr int kGridListGap = 10;
constexpr int kGridListPitch = 28;
constexpr int kGridListRows = 3;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
constexpr std::uint32_t kSelected = 0xFFFFD27Fu;
constexpr std::uint32_t kLocked = 0xFFFF5050u;
constexpr std::uint32_t kDim = 0xB0FFFFFFu;

MoveFn g_origMove = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<Model*> g_model{nullptr};
std::atomic<bool> g_open{false};
std::atomic<bool> g_pausedByUs{false};

struct Level {
    ScreenId id = ScreenId::Main;
    int cursor = 0;
    int offset = 0;
};

// One player's screens: their stack, the top screen's content, and their one-action-per-press latch.
struct Host {
    std::vector<Level> stack;
    Screen screen;
    bool dirty = false;
    bool latch = false;
};

std::mutex g_mutex;  // the game thread (move) and the mod tick
Host g_hosts[2];     // [0] the game's text (solo; player 1 in split screen), [1] player 2's (split screen)
bool g_dual = false;  // M10: a host per player
int g_pads[2] = {-2, -2};
Hud g_hud;
std::vector<LabText> g_lab;  // developer: the UI lab's texts

// M10: what the overlay draws (its own lock: the overlay draws under the mod's mutex, which the
// screens' builds take under g_mutex).
std::mutex g_viewMutex;
View g_views[2];
bool g_viewShown[2] = {false, false};

std::uintptr_t App() { return ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0); }

// The pause menu's own way of asking the application to pause: the request bit under the
// application's lock when the game runs threaded.
void WriteAppFlags(std::uint32_t set, std::uint32_t clear) {
    const auto app = App();
    if (!app) return;
    const bool threaded = ReadOr<std::uint8_t>(app + re5::off::kAppThreadedFlag, 0) != 0 ||
                          ReadOr<std::uint8_t>(re5::addr::kAppForceLock, 0) != 0;
    const auto enter = ReadOr<std::uintptr_t>(re5::addr::kImportEnterCriticalSection, 0);
    const auto leave = ReadOr<std::uintptr_t>(re5::addr::kImportLeaveCriticalSection, 0);
    void* cs = reinterpret_cast<void*>(app + re5::off::kAppLock);
    if (threaded && enter && leave) reinterpret_cast<LockFn>(enter)(cs);
    std::uint32_t flags = ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0);
    flags = (flags | set) & ~clear;
    SafeStore<std::uint32_t>(app + re5::off::kAppFlags, flags);
    if (threaded && enter && leave) reinterpret_cast<LockFn>(leave)(cs);
}

void HideView(int h) {
    std::lock_guard lock(g_viewMutex);
    g_viewShown[h] = false;
}

void Rebuild(int h) {
    Host& host = g_hosts[h];
    if (host.stack.empty()) return;
    Model* model = g_model.load();
    const ScreenId id = host.stack.back().id;
    host.screen = !model ? Screen{} : g_dual ? model->BuildFor(id, h) : model->Build(id);
    Level& level = host.stack.back();
    const int n = static_cast<int>(host.screen.items.size());
    level.cursor = std::clamp(level.cursor, 0, std::max(n - 1, 0));
    host.dirty = true;
}

void CloseLocked() {
    for (Host& host : g_hosts) host = Host{};
    g_dual = false;
    HideView(0);
    HideView(1);
    g_open = false;
    if (g_pausedByUs.exchange(false)) {
        WriteAppFlags(0, re5::off::kAppPauseRequest | re5::off::kAppPaused);
    }
    RM_LOGI("match ui: closed");
}

void Draw(std::uintptr_t self, int slot, int x, int y, float scale, std::uint32_t argb) {
    nativemenu::DrawSlot(self, slot, x, y, scale, argb);
}

// A row's hit box (canvas units): its label at the cursor row's scale with some room either side,
// at least kMinRowHalfWidth wide each way, and the row pitch high.
constexpr float kRowHitMargin = 24.0f;
constexpr float kMinRowHalfWidth = 120.0f;

// The visible row under the pointer (0..visible-1), or -1. The row slots hold last frame's labels,
// the ones on screen.
int RowUnderPointer(const menumouse::Input& mouse, const Layout& layout, int visible) {
    const float scale = layout.rowScale * 1.12f;
    for (int r = 0; r < visible; ++r) {
        const float half = std::max(nativemenu::SlotHalfWidth(r, scale) + kRowHitMargin, kMinRowHalfWidth);
        if (mouse.Over(640.0f, static_cast<float>(layout.firstRowY + r * layout.rowPitch), half,
                       static_cast<float>(layout.rowPitch) * 0.5f)) {
            return r;
        }
    }
    return -1;
}

// The grid screen's item under the pointer (an index into the screen's items), from the rows the
// overlay drew last frame; -1 for none.
int ItemUnderPointer(const menumouse::Input& mouse, int h) {
    std::lock_guard lock(g_viewMutex);
    if (!g_viewShown[h]) return -1;
    for (const ViewRow& row : g_views[h].rows) {
        const float half = row.cell ? static_cast<float>(kCellHalfWidth) : kMinRowHalfWidth * 2.0f;
        const float tall = row.cell ? kCellPitchY * 0.5f : kGridListPitch * 0.5f;
        if (mouse.Over(static_cast<float>(row.x), static_cast<float>(row.y), half, tall)) return row.index;
    }
    return -1;
}

void MoveCursorTo(std::uintptr_t self, Host& host, Level& level, int index) {
    if (index == level.cursor) return;
    level.cursor = index;
    host.dirty = true;
    nativemenu::Sound(nativemenu::SoundKind::Move, self);
}

// ---- M10: each player's own buttons ----------------------------------------------------------

struct Buttons {
    std::uint32_t trigger = 0;  // pressed this frame
    std::uint32_t repeat = 0;   // held long enough to repeat
};

std::uint32_t KeyboardBits(std::uintptr_t fn, std::uintptr_t pads) {
    __try {
        return reinterpret_cast<KeyboardBitsFn>(fn)(pads, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Player `seat`'s menu buttons: their pad's bits in the game's pad object (what its menu input
// query reads for the main pad), and the keyboard's for the player the game gives the keyboard to
// (player 1 always when they have no pad).
Buttons SeatButtons(int seat) {
    Buttons b;
    const auto app = App();
    if (app && ReadOr<std::uint8_t>(app + re5::off::kAppMenuInputOff, 0) != 0) return b;
    const auto pads = ReadOr<std::uintptr_t>(re5::addr::kPadInstance, 0);
    if (!pads || ReadOr<std::uint8_t>(pads + re5::off::kPadsInputOff, 1) != 0) return b;
    const int pad = g_pads[seat];
    if (pad >= 0 && pad < 4) {
        const auto at = pads + static_cast<std::uintptr_t>(pad) * re5::off::kPadStride;
        b.trigger = ReadOr<std::uint32_t>(at + re5::off::kPadTrigger, 0);
        b.repeat = ReadOr<std::uint32_t>(at + re5::off::kPadRepeat, 0);
    }
    const int keyboard = ReadOr<std::int32_t>(pads + re5::off::kPadsKeyboardPlayer, 0);
    const bool keys = seat == 0 ? (g_pads[0] < 0 || keyboard == 0) : (keyboard == 1 && g_pads[0] >= 0);
    if (keys) {
        b.trigger |= KeyboardBits(re5::addr::kFnKeyboardTrigger, pads);
        b.repeat |= KeyboardBits(re5::addr::kFnKeyboardRepeat, pads);
    }
    return b;
}

int ListStep(std::uintptr_t self, const Buttons& b, int cursor, int count) {
    __try {
        return reinterpret_cast<ListStepFn>(re5::addr::kFnMenuListStep)(self, b.trigger, b.repeat, cursor, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return cursor;
    }
}

// A grid screen's cursor: the cells three to a row (left and right along a row, up and down a row),
// the listed rows under them one by one.
int GridStep(int cursor, int count, int grid, std::uint32_t bits) {
    const auto mask = [](std::uintptr_t at) { return ReadOr<std::uint32_t>(at, 0); };
    const bool up = (bits & mask(re5::addr::kMenuMaskUp)) != 0;
    const bool down = (bits & mask(re5::addr::kMenuMaskDown)) != 0;
    const bool left = (bits & mask(re5::addr::kMenuMaskLeft)) != 0;
    const bool right = (bits & mask(re5::addr::kMenuMaskRight)) != 0;
    const int rows = (grid + 2) / 3;
    if (cursor < grid) {
        const int column = cursor % 3, row = cursor / 3;
        if (left && column > 0) return cursor - 1;
        if (right && column < 2 && cursor + 1 < grid) return cursor + 1;
        if (up && row > 0) return cursor - 3;
        if (down) return row + 1 < rows ? std::min(cursor + 3, grid - 1) : (grid < count ? grid : cursor);
        return cursor;
    }
    if (up) return cursor == grid ? (rows - 1) * 3 : cursor - 1;
    if (down && cursor + 1 < count) return cursor + 1;
    return cursor;
}

// ---- texts -----------------------------------------------------------------------------------

std::string HelpOf(const Host& host, const Level& level, bool more) {
    const int n = static_cast<int>(host.screen.items.size());
    std::string help = n > 0 ? host.screen.items[static_cast<std::size_t>(level.cursor)].help : host.screen.empty_help;
    if (more) help += "\nMore below.";
    return help;
}

std::uint32_t RowColour(const Item& item, bool selected) {
    std::uint32_t colour = item.locked ? kLocked : (item.color != 0 ? item.color : (selected ? kSelected : kWhite));
    // The cursor row is full strength; the others are dimmed.
    return (colour & 0x00FFFFFFu) | (selected ? 0xFF000000u : 0x98000000u);
}

// Host 0's rows in the game's font.
void DrawGameTexts(std::uintptr_t self, Host& host, const Layout& layout) {
    Level& level = host.stack.back();
    const int n = static_cast<int>(host.screen.items.size());
    const int visible = std::min(n, layout.maxRows);
    if (level.cursor < level.offset) level.offset = level.cursor;
    if (level.cursor >= level.offset + visible) level.offset = level.cursor - visible + 1;
    level.offset = std::clamp(level.offset, 0, std::max(n - visible, 0));
    if (host.dirty) {
        host.dirty = false;
        for (int r = 0; r < kMaxRows; ++r) {
            const int index = level.offset + r;
            std::string label = index < n ? host.screen.items[static_cast<std::size_t>(index)].label : std::string();
            if (label.size() > kRowChars) label.resize(kRowChars);
            nativemenu::SetSlotText(r, label);
        }
        const bool more = n > visible && level.offset + visible < n;
        nativemenu::SetSlotText(nativemenu::kHelpSlot, nativemenu::WrapText(HelpOf(host, level, more), kHelpChars));
        nativemenu::SetSlotText(nativemenu::kHeaderSlot, host.screen.header);
    }
    Draw(self, nativemenu::kHeaderSlot, 640, layout.headerY, layout.headerScale, kWhite);
    for (int r = 0; r < visible; ++r) {
        const int index = level.offset + r;
        const bool selected = index == level.cursor;
        Draw(self, r, 640, layout.firstRowY + r * layout.rowPitch, selected ? layout.rowScale * 1.12f : layout.rowScale,
             RowColour(host.screen.items[static_cast<std::size_t>(index)], selected));
    }
    Draw(self, nativemenu::kHelpSlot, 640, layout.helpY, layout.helpScale, kWhite);
}

// A host's screen for the overlay (player 2's, or a grid screen), in its half's layout.
void PublishView(int h, Host& host, const Layout& layout) {
    Level& level = host.stack.back();
    const int n = static_cast<int>(host.screen.items.size());
    const int grid = std::clamp(host.screen.grid, 0, n);
    View v;
    v.header = host.screen.header;
    v.grid = grid;
    v.headerY = layout.headerY;
    v.helpY = layout.helpY;
    v.headerScale = layout.headerScale;
    v.rowScale = layout.rowScale;
    v.helpScale = layout.helpScale;
    const auto add = [&](int index, int x, int y, bool cell) {
        const Item& item = host.screen.items[static_cast<std::size_t>(index)];
        const bool selected = index == level.cursor;
        ViewRow row;
        row.label = item.label;
        row.argb = RowColour(item, selected);
        row.selected = selected;
        row.locked = item.locked;
        row.cell = cell;
        row.index = index;
        row.x = x;
        row.y = y;
        row.scale = selected && !cell ? layout.rowScale * 1.12f : layout.rowScale;
        v.rows.push_back(std::move(row));
    };
    bool more = false;
    if (grid > 0) {
        const int rows = (grid + 2) / 3;
        for (int i = 0; i < grid; ++i) add(i, 640 + (i % 3 - 1) * kCellPitchX, layout.firstRowY + (i / 3) * kCellPitchY, true);
        // The listed rows: `offset` is the first one shown (an item index past the cells).
        const int listed = n - grid;
        const int shown = std::min(listed, kGridListRows);
        if (level.offset < grid) level.offset = grid;
        if (level.cursor >= grid) {
            if (level.cursor < level.offset) level.offset = level.cursor;
            if (level.cursor >= level.offset + shown) level.offset = level.cursor - shown + 1;
        }
        level.offset = std::clamp(level.offset, grid, std::max(grid, n - shown));
        const int listY = layout.firstRowY + rows * kCellPitchY + kGridListGap;
        for (int r = 0; r < shown; ++r) add(level.offset + r, 640, listY + r * kGridListPitch, false);
        more = level.offset + shown < n;
    } else {
        const int visible = std::min(n, layout.maxRows);
        if (level.cursor < level.offset) level.offset = level.cursor;
        if (level.cursor >= level.offset + visible) level.offset = level.cursor - visible + 1;
        level.offset = std::clamp(level.offset, 0, std::max(n - visible, 0));
        for (int r = 0; r < visible; ++r) add(level.offset + r, 640, layout.firstRowY + r * layout.rowPitch, false);
        more = n > visible && level.offset + visible < n;
    }
    v.help = nativemenu::WrapText(HelpOf(host, level, more), kHelpChars);
    host.dirty = false;
    std::lock_guard lock(g_viewMutex);
    g_views[h] = std::move(v);
    g_viewShown[h] = true;
}

// ---- one frame -------------------------------------------------------------------------------

// M10: a host per player while the run splits the screen, each player's pad known.
void UpdateDual() {
    Model* model = g_model.load();
    const bool dual = g_hud.split && g_pads[1] >= 0 && model && model->SplitSeats() >= 2;
    if (dual == g_dual) return;
    g_dual = dual;
    if (dual) {
        g_hosts[1] = Host{};
        g_hosts[1].stack.assign(1, Level{g_hosts[0].stack.front().id});
        g_hosts[1].latch = true;  // the press that led here must be released first
        RM_LOGI("match ui: a screen for each player (player 1 on %d, player 2 on pad %d)", g_pads[0], g_pads[1]);
    } else {
        g_hosts[1] = Host{};
        HideView(1);
        RM_LOGI("match ui: one screen for both players");
    }
    // Both follow the run from here (SeatScreen), host 0 built for its player.
    for (int h = 0; h < 2; ++h) {
        if (!g_hosts[h].stack.empty()) Rebuild(h);
    }
}

// One frame of host `h`: where it belongs, input, then its texts. False when the screens closed.
bool FrameHost(std::uintptr_t self, int h) {
    Host& host = g_hosts[h];
    if (host.stack.empty()) return true;
    Model* model = g_model.load();
    if (g_dual && model) {
        // M10: a player's screens follow the run (a reward chosen, the shop left or a vote cast: a
        // wait for the other; everyone done: the next screen).
        const ScreenId want = model->SeatScreen(h, host.stack.front().id);
        if (want != host.stack.front().id) {
            host.stack.assign(1, Level{want});
            Rebuild(h);
        }
    }
    Level* level = &host.stack.back();
    int n = static_cast<int>(host.screen.items.size());
    const Layout& layout = g_hud.split ? kSplitLayout : kFullLayout;
    const bool grid = g_hud.split && host.screen.grid > 0;
    // The mouse (player 1's): the row under a moving pointer takes the cursor, the wheel steps
    // through the list, a click on a row chooses it and the right button goes back.
    menumouse::Input mouse{};
    int over = -1;
    if (h == 0) {
        mouse = menumouse::Take(menumouse::Host::Match, true);
        if (grid) {
            over = ItemUnderPointer(mouse, h);
        } else {
            const int row = RowUnderPointer(mouse, layout, std::min(n, layout.maxRows));
            over = row >= 0 ? level->offset + row : -1;
        }
        if (over >= n) over = -1;
        if (mouse.moved && over >= 0) MoveCursorTo(self, host, *level, over);
        if (mouse.wheelSteps != 0 && n > 0) MoveCursorTo(self, host, *level, std::clamp(level->cursor - mouse.wheelSteps, 0, n - 1));
    }
    const bool click = mouse.leftPresses > 0 && over >= 0;
    if (click) {
        MoveCursorTo(self, host, *level, over);
        static int logs = 0;
        if (logs < 60) {
            ++logs;
            RM_LOGI("match ui: mouse click on row %d of screen %d", level->cursor, static_cast<int>(level->id));
        }
    }
    Buttons buttons;
    if (g_dual) {
        buttons = SeatButtons(h);
    } else {
        buttons.trigger = nativemenu::InputBits();
    }
    const bool keyDecide = (buttons.trigger & nativemenu::PadBits(re5::off::kPadDecideMask)) != 0;
    const bool keyCancel = (buttons.trigger & nativemenu::PadBits(re5::off::kPadCancelMask)) != 0;
    const bool decide = keyDecide || click;
    const bool cancel = keyCancel || mouse.rightPresses > 0;
    // One action per press, whichever device: the latch opens once nothing is held.
    if (!decide && !cancel && !mouse.leftHeld && !mouse.rightHeld) host.latch = false;
    if ((decide || cancel) && !host.latch && model) {
        host.latch = true;
        const int index = decide ? level->cursor : -1;
        const Decision d = g_dual ? model->DecideFor(level->id, index, h) : model->Decide(level->id, index);
        switch (d.outcome) {
            case Outcome::Stay: break;
            case Outcome::Denied: nativemenu::Sound(nativemenu::SoundKind::Deny, self); break;
            case Outcome::Refresh:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                Rebuild(h);
                break;
            case Outcome::Push:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                host.stack.push_back(Level{d.next});
                Rebuild(h);
                break;
            case Outcome::Back:
                nativemenu::Sound(nativemenu::SoundKind::Cancel, self);
                if (host.stack.size() > 1) {
                    host.stack.pop_back();
                    Rebuild(h);
                } else if (g_dual) {
                    // M10: a player's first screen is where the run has them; it stays.
                    host.stack.assign(1, Level{model->SeatScreen(h, level->id)});
                    Rebuild(h);
                } else {
                    CloseLocked();
                    return false;
                }
                break;
            case Outcome::Close:
            case Outcome::StartRun:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                CloseLocked();
                return false;
        }
        // The other player's screen shows what this one changed (a vote, the storage).
        if (g_dual && d.outcome != Outcome::Stay && d.outcome != Outcome::Denied) Rebuild(1 - h);
        // A push or pop may have moved the stack's storage.
        level = &host.stack.back();
        n = static_cast<int>(host.screen.items.size());
    } else if (n > 1) {
        const int before = level->cursor;
        int after = before;
        if (grid) {
            after = GridStep(before, n, host.screen.grid, buttons.trigger | buttons.repeat);
        } else if (g_dual) {
            after = ListStep(self, buttons, before, n);
        } else {
            after = nativemenu::Navigate(self, before, n);
        }
        if (after != before && after >= 0 && after < n) {
            MoveCursorTo(self, host, *level, after);
            if (h == 0) menumouse::KeyNavigated();
        }
    }
    const bool grid2 = g_hud.split && host.screen.grid > 0;  // the screen may have changed above
    if (h == 0 && !grid2) {
        HideView(0);
        DrawGameTexts(self, host, layout);
    } else {
        PublishView(h, host, layout);
    }
    return true;
}

// One frame of the open screens.
void Frame(std::uintptr_t self) {
    if (g_pausedByUs.load()) HoldWorldPause("the mod's screen");  // the world stays still under it
    UpdateDual();
    if (!FrameHost(self, 0)) return;
    if (g_dual) FrameHost(self, 1);
}

std::atomic<bool> g_pauseMenuOpen{false};

void __fastcall PauseMoveDetour(std::uintptr_t self, void*) {
    g_pauseMenuOpen = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) != 0;
    // While a mod screen is open the game's own pause menu stays shut (its closed state only
    // watches for the pause button); otherwise the screen runs as usual.
    const bool blockPause = g_open.load() && ReadOr<std::uint8_t>(self + re5::off::kMenuState, 1) == 0;
    if (!blockPause) g_origMove(self, nullptr);
    if (!g_enabled.load()) return;
    std::lock_guard lock(g_mutex);
    const bool hud = !g_hud.notice.empty() || !g_hud.left.empty() || !g_hud.center.empty() || !g_hud.right.empty();
    const bool open = g_open.load() && !g_hosts[0].stack.empty();
    const bool wantsTexts = open || hud || !g_lab.empty();
    nativemenu::HoldTexts(wantsTexts);
    if (!wantsTexts || !nativemenu::TextsReady()) return;
    if (open) {
        Frame(self);
    } else {
        // Row slots 0..2 and the help slot; an opening screen rewrites them all (Rebuild).
        const struct {
            const std::string& text;
            int slot, x, y;
            float scale;
            std::uint32_t argb;
        } texts[] = {
            {g_hud.notice, nativemenu::kHelpSlot, 640, kNoticeY, kNoticeScale, kWhite},
            {g_hud.left, 0, kLeftX, kSideY, kSideScale, kWhite},
            {g_hud.center, 1, 640, g_hud.split ? kSplitCenterY : kCenterY, kCenterScale, kDim},
            {g_hud.right, 2, kRightX, kSideY, kSideScale, kWhite},
        };
        for (const auto& t : texts) {
            if (t.text.empty()) continue;
            nativemenu::SetSlotText(t.slot, t.text);
            Draw(self, t.slot, t.x, t.y, t.scale, t.argb);
        }
        // The UI lab: a slot's text is written once a frame, then drawn as often as asked.
        std::string written[10];
        for (const LabText& t : g_lab) {
            if (t.slot < 0 || t.slot > 9 || t.text.empty()) continue;
            if (written[t.slot] != t.text) {
                nativemenu::SetSlotText(t.slot, t.text);
                written[t.slot] = t.text;
            }
            Draw(self, t.slot, t.x, t.y, t.scale, t.argb);
        }
    }
}

}  // namespace

bool PauseMenuOpen() { return g_pauseMenuOpen.load(); }

bool Install() {
    if (!engine::Caps().pauseScreen) {
        RM_LOGW("match ui: pause screen signature or menu helpers missing; in-match screens stay on the overlay");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnPauseMove);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&PauseMoveDetour), reinterpret_cast<void**>(&g_origMove));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("match ui: hook failed: %s", MH_StatusToString(status));
        return false;
    }
    g_enabled = true;
    RM_LOGI("match ui: hook on uIdPause00::move queued");
    return true;
}

void SetModel(Model* model) { g_model = model; }

void Open(ScreenId first) {
    if (!g_enabled.load()) return;
    std::lock_guard lock(g_mutex);
    if (g_open.load()) return;
    for (Host& host : g_hosts) host = Host{};
    g_dual = false;
    g_hosts[0].stack.assign(1, Level{first});
    g_hosts[0].latch = true;  // the press that led here must be released first
    Rebuild(0);
    g_open = true;
    WriteAppFlags(re5::off::kAppPauseRequest, 0);
    g_pausedByUs = true;
    RM_LOGI("match ui: opened screen %d, world pause requested", static_cast<int>(first));
}

void Close() {
    std::lock_guard lock(g_mutex);
    if (g_open.load()) CloseLocked();
}

bool IsOpen() { return g_open.load(); }

void SetHud(const Hud& hud) {
    std::lock_guard lock(g_mutex);
    g_hud = hud;
}

void SetPlayerPads(int one, int two) {
    std::lock_guard lock(g_mutex);
    // Player 2's own screens need the game's seat-input helpers; without them, one screen in turns.
    g_pads[0] = one;
    g_pads[1] = engine::Caps().seatMenuInput ? two : -2;
}

void SetLabTexts(std::vector<LabText> texts) {
    std::lock_guard lock(g_mutex);
    g_lab = std::move(texts);
}

bool SplitView(int seat, View& out) {
    if (seat < 0 || seat > 1 || !g_open.load()) return false;
    std::lock_guard lock(g_viewMutex);
    if (!g_viewShown[seat]) return false;
    out = g_views[seat];
    return true;
}

bool HoldWorldPause(const char* why) {
    const auto app = App();
    if (!app) return false;
    if ((ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) & (re5::off::kAppPauseRequest | re5::off::kAppPaused)) != 0) return false;
    WriteAppFlags(re5::off::kAppPauseRequest, 0);
    static int logs = 0;
    if (logs++ < 20) RM_LOGW("match ui: the world's pause was cleared under %s (the game window came back?); paused again", why);
    return true;
}

void SetWorldPause(bool paused) {
    if (paused) WriteAppFlags(re5::off::kAppPauseRequest, 0);
    else WriteAppFlags(0, re5::off::kAppPauseRequest | re5::off::kAppPaused);
    RM_LOGW("match ui: world pause %s", paused ? "requested" : "cleared");
}

}  // namespace rmre5::matchui
