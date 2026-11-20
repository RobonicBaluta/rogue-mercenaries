// In-match native screens. See match_ui.h.
//
// The pause screen (uIdPause00) exists for the whole match and its move() runs every frame,
// paused or not; the detour calls the original and then draws the mod's screen with the
// game's menu text routine (the same one the Play menu rows use, through the message slots
// borrowed by native_menu). The world is frozen with the request bit the pause menu itself
// sets on the application object; input comes from the game's menu input query and its
// list navigation helper, exactly like the title menus.

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
constexpr float kHeaderScale = 0.55f;
constexpr float kRowScale = 0.5f;
constexpr float kHelpScale = 0.42f;
constexpr float kNoticeScale = 0.38f;
constexpr float kSideScale = 0.46f;
constexpr float kCenterScale = 0.36f;
// M10: on the game's split screen its text routine draws in the top view only (clipped at the
// middle), so the screens are laid out in the top half: closer rows, fewer at once, the help above
// the middle (1280x720 canvas; the top view is 0..360).
struct Layout {
    int headerY, firstRowY, rowPitch, helpY, maxRows;
    float headerScale, rowScale, helpScale;
};
constexpr Layout kFullLayout{kHeaderY, kFirstRowY, kRowPitch, kHelpY, kMaxRows, kHeaderScale, kRowScale, kHelpScale};
constexpr Layout kSplitLayout{22, 64, 32, 290, 7, 0.46f, 0.40f, 0.34f};
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

std::mutex g_mutex;  // the game thread (move) and the mod tick
std::vector<Level> g_stack;
Screen g_screen;
Hud g_hud;
bool g_dirty = false;
bool g_latch = false;

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

void Rebuild() {
    Model* model = g_model.load();
    g_screen = model ? model->Build(g_stack.back().id) : Screen{};
    Level& level = g_stack.back();
    const int n = static_cast<int>(g_screen.items.size());
    level.cursor = std::clamp(level.cursor, 0, std::max(n - 1, 0));
    g_dirty = true;
}

void CloseLocked() {
    g_stack.clear();
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

void MoveCursorTo(std::uintptr_t self, Level& level, int index) {
    if (index == level.cursor) return;
    level.cursor = index;
    g_dirty = true;
    nativemenu::Sound(nativemenu::SoundKind::Move, self);
}

// One frame of the open screen: input, then text.
void Frame(std::uintptr_t self) {
    if (g_pausedByUs.load()) HoldWorldPause("the mod's screen");  // the world stays still under it
    Level* level = &g_stack.back();
    int n = static_cast<int>(g_screen.items.size());
    // The mouse: the row under a moving pointer takes the cursor, the wheel steps through the list,
    // a click on a row chooses it and the right button goes back.
    const Layout& rows = g_hud.split ? kSplitLayout : kFullLayout;
    const menumouse::Input mouse = menumouse::Take(menumouse::Host::Match, true);
    const int over = RowUnderPointer(mouse, rows, std::min(n, rows.maxRows));
    if (mouse.moved && over >= 0) MoveCursorTo(self, *level, level->offset + over);
    if (mouse.wheelSteps != 0 && n > 0) MoveCursorTo(self, *level, std::clamp(level->cursor - mouse.wheelSteps, 0, n - 1));
    const bool click = mouse.leftPresses > 0 && over >= 0;
    if (click) {
        MoveCursorTo(self, *level, level->offset + over);
        static int logs = 0;
        if (logs < 60) {
            ++logs;
            RM_LOGI("match ui: mouse click on row %d of screen %d", level->cursor, static_cast<int>(level->id));
        }
    }
    const std::uint32_t input = nativemenu::InputBits();
    const bool keyDecide = (input & nativemenu::PadBits(re5::off::kPadDecideMask)) != 0;
    const bool keyCancel = (input & nativemenu::PadBits(re5::off::kPadCancelMask)) != 0;
    const bool decide = keyDecide || click;
    const bool cancel = keyCancel || mouse.rightPresses > 0;
    // One action per press, whichever device: the latch opens once nothing is held.
    if (!decide && !cancel && !mouse.leftHeld && !mouse.rightHeld) g_latch = false;
    Model* model = g_model.load();
    if ((decide || cancel) && !g_latch && model) {
        g_latch = true;
        const Decision d = model->Decide(level->id, decide ? level->cursor : -1);
        switch (d.outcome) {
            case Outcome::Stay: break;
            case Outcome::Denied: nativemenu::Sound(nativemenu::SoundKind::Deny, self); break;
            case Outcome::Refresh:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                Rebuild();
                break;
            case Outcome::Push:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                g_stack.push_back(Level{d.next});
                Rebuild();
                break;
            case Outcome::Back:
                nativemenu::Sound(nativemenu::SoundKind::Cancel, self);
                if (g_stack.size() > 1) {
                    g_stack.pop_back();
                    Rebuild();
                } else {
                    CloseLocked();
                    return;
                }
                break;
            case Outcome::Close:
            case Outcome::StartRun:
                nativemenu::Sound(nativemenu::SoundKind::Decide, self);
                CloseLocked();
                return;
        }
        // A push or pop may have moved the stack's storage.
        level = &g_stack.back();
        n = static_cast<int>(g_screen.items.size());
    } else if (n > 1) {
        const int before = level->cursor;
        const int after = nativemenu::Navigate(self, before, n);
        if (after != before && after >= 0 && after < n) {
            MoveCursorTo(self, *level, after);
            menumouse::KeyNavigated();
        }
    }
    // Visible window and texts.
    const Layout& layout = g_hud.split ? kSplitLayout : kFullLayout;
    const int visible = std::min(n, layout.maxRows);
    if (level->cursor < level->offset) level->offset = level->cursor;
    if (level->cursor >= level->offset + visible) level->offset = level->cursor - visible + 1;
    level->offset = std::clamp(level->offset, 0, std::max(n - visible, 0));
    if (g_dirty) {
        g_dirty = false;
        for (int r = 0; r < kMaxRows; ++r) {
            const int index = level->offset + r;
            std::string label = index < n ? g_screen.items[static_cast<std::size_t>(index)].label : std::string();
            if (label.size() > kRowChars) label.resize(kRowChars);
            nativemenu::SetSlotText(r, label);
        }
        std::string help = n > 0 ? g_screen.items[static_cast<std::size_t>(level->cursor)].help : g_screen.empty_help;
        if (n > visible && level->offset + visible < n) help += "\nMore below.";
        nativemenu::SetSlotText(nativemenu::kHelpSlot, nativemenu::WrapText(help, kHelpChars));
        nativemenu::SetSlotText(nativemenu::kHeaderSlot, g_screen.header);
    }
    Draw(self, nativemenu::kHeaderSlot, 640, layout.headerY, layout.headerScale, kWhite);
    for (int r = 0; r < visible; ++r) {
        const int index = level->offset + r;
        const Item& item = g_screen.items[static_cast<std::size_t>(index)];
        const bool selected = index == level->cursor;
        std::uint32_t colour = item.locked ? kLocked : (item.color != 0 ? item.color : (selected ? kSelected : kWhite));
        // The cursor row is full strength and a little larger; the others are dimmed.
        colour = (colour & 0x00FFFFFFu) | (selected ? 0xFF000000u : 0x98000000u);
        Draw(self, r, 640, layout.firstRowY + r * layout.rowPitch, selected ? layout.rowScale * 1.12f : layout.rowScale, colour);
    }
    Draw(self, nativemenu::kHelpSlot, 640, layout.helpY, layout.helpScale, kWhite);
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
    const bool wantsTexts = (g_open.load() && !g_stack.empty()) || hud;
    nativemenu::HoldTexts(wantsTexts);
    if (!wantsTexts || !nativemenu::TextsReady()) return;
    if (g_open.load() && !g_stack.empty()) {
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
            {g_hud.center, 1, 640, kCenterY, kCenterScale, kDim},
            {g_hud.right, 2, kRightX, kSideY, kSideScale, kWhite},
        };
        for (const auto& t : texts) {
            if (t.text.empty()) continue;
            nativemenu::SetSlotText(t.slot, t.text);
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
    g_stack.assign(1, Level{first});
    g_latch = true;  // the press that led here must be released first
    Rebuild();
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
