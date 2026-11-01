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
constexpr int kStatusY = 36;
constexpr float kHeaderScale = 0.55f;
constexpr float kRowScale = 0.5f;
constexpr float kHelpScale = 0.42f;
constexpr float kStatusScale = 0.38f;
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
std::string g_status;
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

// One frame of the open screen: input, then text.
void Frame(std::uintptr_t self) {
    Level* level = &g_stack.back();
    int n = static_cast<int>(g_screen.items.size());
    const std::uint32_t input = nativemenu::InputBits();
    const bool decide = (input & nativemenu::PadBits(re5::off::kPadDecideMask)) != 0;
    const bool cancel = (input & nativemenu::PadBits(re5::off::kPadCancelMask)) != 0;
    if (!decide && !cancel) g_latch = false;
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
            level->cursor = after;
            g_dirty = true;
        }
    }
    // Visible window and texts.
    const int visible = std::min(n, kMaxRows);
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
    Draw(self, nativemenu::kHeaderSlot, 640, kHeaderY, kHeaderScale, kWhite);
    for (int r = 0; r < visible; ++r) {
        const int index = level->offset + r;
        const Item& item = g_screen.items[static_cast<std::size_t>(index)];
        const bool selected = index == level->cursor;
        std::uint32_t colour = item.locked ? kLocked : (item.color != 0 ? item.color : (selected ? kSelected : kWhite));
        // The cursor row is full strength and a little larger; the others are dimmed.
        colour = (colour & 0x00FFFFFFu) | (selected ? 0xFF000000u : 0x98000000u);
        Draw(self, r, 640, kFirstRowY + r * kRowPitch, selected ? kRowScale * 1.12f : kRowScale, colour);
    }
    Draw(self, nativemenu::kHelpSlot, 640, kHelpY, kHelpScale, kWhite);
}

void __fastcall PauseMoveDetour(std::uintptr_t self, void*) {
    // While a mod screen is open the game's own pause menu stays shut (its closed state only
    // watches for the pause button); otherwise the screen runs as usual.
    const bool blockPause = g_open.load() && ReadOr<std::uint8_t>(self + re5::off::kMenuState, 1) == 0;
    if (!blockPause) g_origMove(self, nullptr);
    if (!g_enabled.load()) return;
    std::lock_guard lock(g_mutex);
    const bool wantsTexts = (g_open.load() && !g_stack.empty()) || !g_status.empty();
    nativemenu::HoldTexts(wantsTexts);
    if (!wantsTexts || !nativemenu::TextsReady()) return;
    if (g_open.load() && !g_stack.empty()) {
        Frame(self);
    } else {
        nativemenu::SetSlotText(nativemenu::kHelpSlot, g_status);
        Draw(self, nativemenu::kHelpSlot, 640, kStatusY, kStatusScale, kWhite);
    }
}

}  // namespace

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

void SetStatus(const std::string& line) {
    std::lock_guard lock(g_mutex);
    g_status = line;
}

void SetWorldPause(bool paused) {
    if (paused) WriteAppFlags(re5::off::kAppPauseRequest, 0);
    else WriteAppFlags(0, re5::off::kAppPauseRequest | re5::off::kAppPaused);
    RM_LOGW("match ui: world pause %s", paused ? "requested" : "cleared");
}

}  // namespace rmre5::matchui
