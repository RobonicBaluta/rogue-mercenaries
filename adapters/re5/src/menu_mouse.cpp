// The mouse on the mod's native screens. See menu_mouse.h.
//
// Where the pointer is: the game's own menus test the Windows cursor's position against the window
// (its pad object's "mouse in window" check, 0x0079C200), so on its menus the Windows cursor is the
// pointer, and it is used here whenever it follows the mouse. During play the game keeps it still
// (centred, hidden) and turns the camera with the DirectInput counts; then the mod moves a pointer
// of its own with those counts and draws it. The canvas is the game's 1280x720 menu layout, fitted
// into the window at one scale and centred (as the overlay's menu icons are placed).

#include "menu_mouse.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "imgui.h"
#include "imgui_internal.h"
#include "log.h"
#include "overlay.h"

namespace rmre5::menumouse {
namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int x, int y);

constexpr DWORD kScreenUpMs = 250;  // a host that took input this recently has its screen up
constexpr DWORD kStaleMs = 300;     // a host idle this long starts afresh: no clicks from before
constexpr DWORD kRecentreMs = 300;  // the game moved the Windows cursor this recently: not the pointer
constexpr int kFrozenFrames = 4;    // frames of mouse counts with a Windows cursor that did not move
constexpr long kWheelNotch = 120;
constexpr float kCanvasWidth = 1280.0f;
constexpr float kCanvasHeight = 720.0f;
constexpr long kPendingLimit = 1L << 20;

SetCursorPosFn g_origSetCursorPos = nullptr;
std::atomic<DWORD> g_lastSetCursor{0};
std::atomic<unsigned> g_setCursorCalls{0};

std::mutex g_mutex;
// From the DirectInput reads.
long g_pendingDx = 0;  // counts not yet applied to the mod's pointer
long g_pendingDy = 0;
long g_wheelTotal = 0;
unsigned g_presses[2] = {};  // left, right: press edges so far
bool g_down[2] = {};
DWORD g_lastFeed = 0;        // the game's last DirectInput read of the mouse
// The pointer, updated on the render thread.
bool g_useWindows = true;  // the Windows cursor is the pointer
bool g_osShowing = false;  // Windows draws its cursor
POINT g_lastOs{};
bool g_lastOsValid = false;
int g_frozen = 0;          // consecutive frames with counts but a still Windows cursor
float g_px = -1.0f;        // the mod's pointer, client pixels
float g_py = -1.0f;
bool g_valid = false;
float g_x = 0.0f;          // canvas
float g_y = 0.0f;
unsigned g_moveSerial = 0;
bool g_pointerShown = false;  // the mouse was used since the screen came up and since the last key

struct HostState {
    DWORD lastTake = 0;
    unsigned serial = 0;
    unsigned presses[2] = {};
    long wheel = 0;
};
HostState g_hosts[2];
std::atomic<DWORD> g_lastModScreen{0};  // a host's Take with a mod screen up
std::atomic<DWORD> g_lastMatchScreen{0};

BOOL WINAPI SetCursorPosDetour(int x, int y) {
    // The mod never moves the Windows cursor (the overlay's ImGui does not either), so these are
    // the game's.
    g_lastSetCursor = GetTickCount();
    g_setCursorCalls.fetch_add(1);
    return g_origSetCursorPos(x, y);
}

bool ModScreenUp(DWORD now) {
    const DWORD t = g_lastModScreen.load();
    return t != 0 && now - t < kScreenUpMs;
}

const char* HostName(Host host) { return host == Host::Match ? "in-match screen" : "Play menu"; }

}  // namespace

bool Install() {
    void* target = nullptr;
    MH_STATUS status = MH_CreateHookApiEx(L"user32", "SetCursorPos", reinterpret_cast<void*>(&SetCursorPosDetour),
                                          reinterpret_cast<void**>(&g_origSetCursorPos), &target);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGW("menu mouse: watch on SetCursorPos failed (%s); a still Windows cursor is still noticed",
                MH_StatusToString(status));
        return false;
    }
    return true;
}

namespace {

// Under g_mutex: the buttons' state from one reading, counting presses.
void SetButtons(bool left, bool right, long wheel, DWORD now) {
    bool pressed = false;
    const bool down[2] = {left, right};
    for (int i = 0; i < 2; ++i) {
        if (down[i] && !g_down[i]) {
            ++g_presses[i];
            pressed = true;
        }
        g_down[i] = down[i];
    }
    if ((pressed || wheel != 0) && ModScreenUp(now)) g_pointerShown = true;
}

}  // namespace

void Feed(long dx, long dy, long wheel, const unsigned char* buttons) {
    const DWORD now = GetTickCount();
    std::lock_guard lock(g_mutex);
    g_lastFeed = now;
    g_pendingDx = std::clamp(g_pendingDx + dx, -kPendingLimit, kPendingLimit);
    g_pendingDy = std::clamp(g_pendingDy + dy, -kPendingLimit, kPendingLimit);
    g_wheelTotal += wheel;
    SetButtons((buttons[0] & 0x80) != 0, (buttons[1] & 0x80) != 0, wheel, now);
}

Input Take(Host host, bool modScreen) {
    const DWORD now = GetTickCount();
    std::lock_guard lock(g_mutex);
    HostState& h = g_hosts[static_cast<int>(host)];
    const bool fresh = h.lastTake == 0 || now - h.lastTake > kStaleMs;
    h.lastTake = now;
    if (modScreen) {
        if (!ModScreenUp(now)) {
            g_pointerShown = false;  // shown again once the mouse is used on this screen
            static int logs = 0;
            if (logs < 40) {
                ++logs;
                RM_LOGI("menu mouse: %s up; pointer: %s (Windows cursor %s; the game moved it %u times so far)",
                        HostName(host), g_useWindows ? "the Windows cursor" : "the mod's, moved by the mouse counts",
                        g_osShowing ? "shown" : "hidden", g_setCursorCalls.load());
            }
        }
        g_lastModScreen = now;
        if (host == Host::Match) g_lastMatchScreen = now;
    }
    Input in;
    if (fresh || overlay::IsFocused()) {
        h.serial = g_moveSerial;
        h.presses[0] = g_presses[0];
        h.presses[1] = g_presses[1];
        h.wheel = g_wheelTotal;
        if (overlay::IsFocused()) return in;  // the overlay has the mouse
    }
    in.valid = g_valid;
    in.windowsCursor = g_useWindows;
    in.x = g_x;
    in.y = g_y;
    in.leftHeld = g_down[0];
    in.rightHeld = g_down[1];
    in.moved = h.serial != g_moveSerial;
    h.serial = g_moveSerial;
    in.leftPresses = static_cast<int>(g_presses[0] - h.presses[0]);
    in.rightPresses = static_cast<int>(g_presses[1] - h.presses[1]);
    h.presses[0] = g_presses[0];
    h.presses[1] = g_presses[1];
    const long wheel = g_wheelTotal - h.wheel;
    in.wheelSteps = static_cast<int>(wheel / kWheelNotch);
    h.wheel += static_cast<long>(in.wheelSteps) * kWheelNotch;
    return in;
}

void KeyNavigated() {
    std::lock_guard lock(g_mutex);
    g_pointerShown = false;
}

bool CaptureGameMouse() {
    const DWORD t = g_lastMatchScreen.load();
    return t != 0 && GetTickCount() - t < kScreenUpMs;
}

void OnDraw() {
    const ImGuiIO& io = ImGui::GetIO();
    const float width = io.DisplaySize.x;
    const float height = io.DisplaySize.y;
    if (width < 1.0f || height < 1.0f) return;
    const DWORD now = GetTickCount();
    auto* window = static_cast<HWND>(overlay::WindowHandle());
    const bool foreground = window != nullptr && GetForegroundWindow() == window;
    POINT os{};
    const bool osOk = foreground && GetCursorPos(&os) && ScreenToClient(window, &os);
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    const bool osShowing = GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;
    const DWORD lastSet = g_lastSetCursor.load();
    const bool recentred = lastSet != 0 && now - lastSet < kRecentreMs;

    std::lock_guard lock(g_mutex);
    if (foreground && now - g_lastFeed > kScreenUpMs) {
        // The game is not reading the mouse through DirectInput now: its buttons from Windows.
        SetButtons((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0, 0, now);
    }
    const long dx = g_pendingDx;
    const long dy = g_pendingDy;
    g_pendingDx = 0;
    g_pendingDy = 0;
    const bool osMoved = osOk && g_lastOsValid && (os.x != g_lastOs.x || os.y != g_lastOs.y);
    // The mouse moved but the Windows cursor did not (and it is not held at the window's edge): the
    // game holds it still, as it does during play.
    const bool atEdge = os.x <= 0 || os.y <= 0 || os.x >= static_cast<LONG>(width) - 1 || os.y >= static_cast<LONG>(height) - 1;
    if (std::labs(dx) + std::labs(dy) >= 2 && osOk && !atEdge) {
        g_frozen = osMoved ? 0 : std::min(g_frozen + 1, 1000);
    } else if (osMoved) {
        g_frozen = 0;
    }
    const bool useWindows = osOk && !recentred && g_frozen < kFrozenFrames;
    if (useWindows != g_useWindows) {
        g_useWindows = useWindows;
        static int logs = 0;
        if (logs < 40) {
            ++logs;
            RM_LOGI("menu mouse: pointer is now %s (%s)", useWindows ? "the Windows cursor" : "the mod's, moved by the mouse counts",
                    !foreground ? "the game window is not in front"
                    : recentred ? "the game moves the Windows cursor itself"
                    : !useWindows ? "the Windows cursor does not follow the mouse"
                                  : "the Windows cursor follows the mouse");
        }
    }
    g_osShowing = osShowing;
    g_lastOs = os;
    g_lastOsValid = osOk;

    float px = 0.0f;
    float py = 0.0f;
    bool inside = false;
    if (useWindows) {
        px = static_cast<float>(os.x);
        py = static_cast<float>(os.y);
        inside = px >= 0.0f && py >= 0.0f && px < width && py < height;
        // The mod's pointer carries on from here if the game takes the Windows cursor.
        g_px = std::clamp(px, 0.0f, width - 1.0f);
        g_py = std::clamp(py, 0.0f, height - 1.0f);
    } else {
        if (g_px < 0.0f) {
            g_px = width * 0.5f;
            g_py = height * 0.5f;
        }
        // About one pixel per count at 1080 lines, more on bigger windows.
        const float speed = std::max(1.0f, height / 1080.0f);
        g_px = std::clamp(g_px + static_cast<float>(dx) * speed, 0.0f, width - 1.0f);
        g_py = std::clamp(g_py + static_cast<float>(dy) * speed, 0.0f, height - 1.0f);
        px = g_px;
        py = g_py;
        inside = foreground;
    }
    const float k = std::min(width / kCanvasWidth, height / kCanvasHeight);
    const float ox = (width - kCanvasWidth * k) * 0.5f;
    const float oy = (height - kCanvasHeight * k) * 0.5f;
    const float x = (px - ox) / k;
    const float y = (py - oy) / k;
    if (inside && (!g_valid || std::fabs(x - g_x) >= 0.5f || std::fabs(y - g_y) >= 0.5f)) {
        ++g_moveSerial;
        if (ModScreenUp(now)) g_pointerShown = true;
    }
    g_valid = inside;
    g_x = x;
    g_y = y;

    if (!ModScreenUp(now)) {
        g_pointerShown = false;
        return;
    }
    // Windows draws its own cursor while it is the pointer and shown.
    if (!g_pointerShown || !inside || overlay::IsFocused() || (useWindows && osShowing)) return;
    const float scale = std::clamp(height / 720.0f, 1.0f, 3.0f);
    ImGui::RenderMouseCursor(ImVec2(px, py), scale, ImGuiMouseCursor_Arrow, IM_COL32_WHITE, IM_COL32_BLACK,
                             IM_COL32(0, 0, 0, 48));
}

}  // namespace rmre5::menumouse
