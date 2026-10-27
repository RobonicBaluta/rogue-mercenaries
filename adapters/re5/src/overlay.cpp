#include "overlay.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "dev_audio.h"
#include "dev_capture.h"
#include "log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rmre5::overlay {
namespace {

std::mutex g_mutex;  // ImGui state is shared by the render, input and window threads
DrawCallback g_draw = nullptr;
std::atomic<int> g_toggleKey{VK_F7};
std::atomic<bool> g_focused{false};
std::atomic<DWORD> g_lastToggle{0};
std::atomic<DWORD> g_lastDirectInputKeys{0};
bool g_initialized = false;
bool g_deviceObjectsLost = false;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
unsigned char g_prevKeys[256]{};
float g_cursorX = -1.0f;
float g_cursorY = -1.0f;

void Toggle() {
    // Both the WndProc and DirectInput paths can see the same key press.
    const DWORD now = GetTickCount();
    if (now - g_lastToggle.load() < 250) {
        return;
    }
    g_lastToggle = now;
    SetFocused(!g_focused.load());
}

bool DirectInputDrivesKeys() { return GetTickCount() - g_lastDirectInputKeys.load() < 500; }

bool IsInputMessage(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           msg == WM_INPUT;
}

LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        RestoreDevAudioOnExit();
    }
    if (msg == WM_KEYDOWN && static_cast<int>(wParam) == g_toggleKey.load() && !(lParam & (1 << 30))) {
        Toggle();
        return 0;
    }
    if (g_initialized && g_focused.load() && !DirectInputDrivesKeys()) {
        std::lock_guard lock(g_mutex);
        ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam);
    }
    if (g_focused.load() && IsInputMessage(msg)) {
        return 0;
    }
    return CallWindowProcW(g_originalWndProc, window, msg, wParam, lParam);
}

int ToggleScancode() {
    return static_cast<int>(MapVirtualKeyW(static_cast<UINT>(g_toggleKey.load()), MAPVK_VK_TO_VSC));
}

// DirectInput scancodes (DIK_*) to ImGui keys used for menu navigation.
struct KeyMap {
    int dik;
    ImGuiKey key;
};
constexpr KeyMap kKeys[] = {
    {0xC8, ImGuiKey_UpArrow},   {0xD0, ImGuiKey_DownArrow}, {0xCB, ImGuiKey_LeftArrow}, {0xCD, ImGuiKey_RightArrow},
    {0x1C, ImGuiKey_Enter},     {0x9C, ImGuiKey_KeypadEnter}, {0x39, ImGuiKey_Space},   {0x01, ImGuiKey_Escape},
    {0x0F, ImGuiKey_Tab},       {0x0E, ImGuiKey_Backspace}, {0x11, ImGuiKey_W},         {0x1E, ImGuiKey_A},
    {0x1F, ImGuiKey_S},         {0x20, ImGuiKey_D},         {0x02, ImGuiKey_1},         {0x03, ImGuiKey_2},
    {0x04, ImGuiKey_3},         {0x05, ImGuiKey_4},         {0x2A, ImGuiKey_LeftShift}, {0x1D, ImGuiKey_LeftCtrl},
};

bool Initialize(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(device->GetCreationParameters(&params))) {
        return false;
    }
    g_window = params.hFocusWindow;
    if (!g_window) {
        IDirect3DSwapChain9* chain = nullptr;
        D3DPRESENT_PARAMETERS present{};
        if (SUCCEEDED(device->GetSwapChain(0, &chain))) {
            chain->GetPresentParameters(&present);
            chain->Release();
            g_window = present.hDeviceWindow;
        }
    }
    if (!g_window) {
        RM_LOGE("overlay: no window handle from device");
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.88f;
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX9_Init(device)) {
        RM_LOGE("overlay: ImGui backend init failed");
        return false;
    }
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    RM_LOGI("overlay initialized on window %p", static_cast<void*>(g_window));
    return true;
}

}  // namespace

void SetDrawCallback(DrawCallback callback) { g_draw = callback; }
void SetToggleKey(int virtualKey) { g_toggleKey = virtualKey; }
bool IsFocused() { return g_focused.load(); }
bool IsInitialized() { return g_initialized; }

void SetFocused(bool focused) {
    g_focused = focused;
    RM_LOGD("overlay focus %s", focused ? "on" : "off");
}

void FeedKeyboard(const unsigned char* keys) {
    g_lastDirectInputKeys = GetTickCount();
    const int toggle = ToggleScancode();
    if (toggle > 0 && toggle < 256 && (keys[toggle] & 0x80) && !(g_prevKeys[toggle] & 0x80)) {
        Toggle();
    }
    if (g_initialized && g_focused.load()) {
        std::lock_guard lock(g_mutex);
        ImGuiIO& io = ImGui::GetIO();
        for (const auto& entry : kKeys) {
            const bool down = (keys[entry.dik] & 0x80) != 0;
            if (down != ((g_prevKeys[entry.dik] & 0x80) != 0)) {
                io.AddKeyEvent(entry.key, down);
            }
        }
    }
    std::memcpy(g_prevKeys, keys, sizeof(g_prevKeys));
}

void FeedMouse(long dx, long dy, long wheel, const unsigned char* buttons) {
    if (!g_initialized || !g_focused.load()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    ImGuiIO& io = ImGui::GetIO();
    if (g_cursorX < 0.0f) {
        g_cursorX = io.DisplaySize.x * 0.5f;
        g_cursorY = io.DisplaySize.y * 0.5f;
    }
    g_cursorX = std::clamp(g_cursorX + static_cast<float>(dx), 0.0f, io.DisplaySize.x - 1.0f);
    g_cursorY = std::clamp(g_cursorY + static_cast<float>(dy), 0.0f, io.DisplaySize.y - 1.0f);
    io.AddMousePosEvent(g_cursorX, g_cursorY);
    for (int i = 0; i < 3; ++i) {
        io.AddMouseButtonEvent(i, (buttons[i] & 0x80) != 0);
    }
    if (wheel) {
        io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / 120.0f);
    }
}

void OnPresent(IDirect3DDevice9* device) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        static bool failed = false;
        if (failed) {
            return;
        }
        if (!Initialize(device)) {
            failed = true;
            return;
        }
        g_initialized = true;
    }
    if (g_deviceObjectsLost || device->TestCooperativeLevel() != D3D_OK) {
        return;
    }

    // Draw into the back buffer regardless of what the game left bound.
    IDirect3DSurface9* previousTarget = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    device->GetRenderTarget(0, &previousTarget);
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        device->SetRenderTarget(0, backBuffer);
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_focused.load();
    ImGui::NewFrame();
    if (g_draw) {
        g_draw();
    }
    ImGui::EndFrame();
    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }

    if (previousTarget) {
        device->SetRenderTarget(0, previousTarget);
        previousTarget->Release();
    }
    if (backBuffer) {
        backBuffer->Release();
    }
    MaybeCaptureBackBuffer(device);
}

void OnBeforeReset() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        g_deviceObjectsLost = true;
    }
}

void OnAfterReset() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        ImGui_ImplDX9_CreateDeviceObjects();
        g_deviceObjectsLost = false;
    }
}

}  // namespace rmre5::overlay
