#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <dinput.h>
#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "hooks.h"
#include "log.h"
#include "overlay.h"

namespace rmre5 {

namespace detail {
extern HookSink* g_sink;
extern HookStatus g_status;
}  // namespace detail
using detail::g_sink;
using detail::g_status;

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, LPVOID data);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data,
                                                    LPDWORD inOut, DWORD flags);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD user, void* state);

struct DeviceHook {
    void* target = nullptr;
    GetDeviceStateFn originalState = nullptr;
    GetDeviceDataFn originalData = nullptr;
};

// dinput8 may implement the A and W interfaces with distinct functions; each
// distinct target gets its own trampoline.
DeviceHook g_stateHooks[2];
DeviceHook g_dataHooks[2];
// The raw input of the last polls (RawKeyDown and friends); written on the game thread.
unsigned char g_rawKeys[256] = {};
unsigned char g_rawJoy[32] = {};
unsigned short g_rawXButtons = 0;
XInputGetStateFn g_origXInputGetState = nullptr;

bool Blocking() { return g_sink && g_sink->BlockGameInput(); }

// The mod's own bindings (SetOwnedInputs) and, per binding, how its buttons are being held.
constexpr ULONGLONG kComboWindowMs = 100;
struct ComboHold {
    ULONGLONG pressedAt = 0;
    bool owned = false;   // completed: the mod's until every button is up
    bool passed = false;  // not completed in time: the game's until every button is up
};
std::mutex g_ownedMutex;
std::vector<OwnedInput> g_owned;
std::vector<ComboHold> g_xHolds;
std::vector<ComboHold> g_joyHolds;

int Bits(unsigned v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// Which of a binding's held buttons the game must not see now.
unsigned HideFor(unsigned held, unsigned all, ComboHold& h, ULONGLONG now) {
    if (held == 0) {
        h = {};
        return 0;
    }
    if (h.passed) return 0;
    if (held == all || h.owned || Bits(all) == 1) {
        h.owned = h.owned || held == all;
        return held;
    }
    if (h.pressedAt == 0) h.pressedAt = now;
    if (now - h.pressedAt < kComboWindowMs) return held;  // waiting for the rest of the combo
    h.passed = true;
    return 0;
}

unsigned short HideXInput(unsigned short buttons) {
    std::lock_guard lock(g_ownedMutex);
    const ULONGLONG now = GetTickCount64();
    unsigned short hide = 0;
    for (std::size_t i = 0; i < g_owned.size() && i < g_xHolds.size(); ++i) {
        if (g_owned[i].xinput == 0) continue;
        hide |= static_cast<unsigned short>(HideFor(buttons & g_owned[i].xinput, g_owned[i].xinput, g_xHolds[i], now));
    }
    return hide;
}

unsigned HideJoy(const BYTE* rgb, int count) {
    std::lock_guard lock(g_ownedMutex);
    const ULONGLONG now = GetTickCount64();
    unsigned down = 0;
    for (int i = 0; i < count && i < 32; ++i) {
        if (rgb[i] & 0x80) down |= 1u << i;
    }
    unsigned hide = 0;
    for (std::size_t i = 0; i < g_owned.size() && i < g_joyHolds.size(); ++i) {
        if (g_owned[i].joy == 0) continue;
        hide |= HideFor(down & g_owned[i].joy, g_owned[i].joy, g_joyHolds[i], now);
    }
    return hide;
}

bool KeyOwned(int scancode) {
    std::lock_guard lock(g_ownedMutex);
    for (const OwnedInput& o : g_owned) {
        if (o.key == scancode) return true;
    }
    return false;
}

void Tick() { TickGame("dinput"); }

// Device kind by object pointer, learned once through GetCapabilities.
enum class Kind { Unknown, Keyboard, Mouse, Other };
struct KnownDevice {
    void* device = nullptr;
    Kind kind = Kind::Unknown;
};
KnownDevice g_devices[16];

const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::Keyboard: return "keyboard";
        case Kind::Mouse: return "mouse";
        case Kind::Other: return "other";
        default: return "unknown";
    }
}

Kind KindOf(void* device, const char* via, DWORD size) {
    for (const auto& known : g_devices) {
        if (known.device == device) return known.kind;
    }
    using GetCapabilitiesFn = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
    auto getCaps = reinterpret_cast<GetCapabilitiesFn>((*reinterpret_cast<void***>(device))[3]);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    Kind kind = Kind::Other;
    if (SUCCEEDED(getCaps(device, &caps))) {
        const BYTE type = GET_DIDEVICE_TYPE(caps.dwDevType);
        kind = type == DI8DEVTYPE_KEYBOARD ? Kind::Keyboard : type == DI8DEVTYPE_MOUSE ? Kind::Mouse : Kind::Other;
    }
    for (auto& known : g_devices) {
        if (!known.device) {
            known.device = device;
            known.kind = kind;
            RM_LOGI("dinput device %p is %s (first seen via %s, size %lu)", device, KindName(kind), via, size);
            break;
        }
    }
    return kind;
}

// Keys and buttons pressed while the overlay owned input stay hidden from the
// game until released, so the key that closes the menu never leaks through.
bool g_heldKeys[256]{};
bool g_heldButtons[8]{};
bool g_bufferedButtons[8]{};

void FilterKeyboard(unsigned char* keys, bool blocking) {
    for (int i = 0; i < 256; ++i) {
        if (!blocking && (keys[i] & 0x80) && KeyOwned(i)) {
            keys[i] = 0;  // the mod's key
            continue;
        }
        const bool down = (keys[i] & 0x80) != 0;
        if (blocking) {
            g_heldKeys[i] = g_heldKeys[i] || down;
            keys[i] = 0;
        } else if (g_heldKeys[i]) {
            if (down) {
                keys[i] = 0;
            } else {
                g_heldKeys[i] = false;
            }
        }
    }
}

void FilterMouse(void* data, DWORD size, bool blocking) {
    auto* mouse = static_cast<DIMOUSESTATE*>(data);
    const int buttons = size == sizeof(DIMOUSESTATE2) ? 8 : 4;
    BYTE* rgb = mouse->rgbButtons;
    if (blocking) {
        for (int i = 0; i < buttons; ++i) g_heldButtons[i] = g_heldButtons[i] || (rgb[i] & 0x80);
        std::memset(data, 0, size);
        return;
    }
    for (int i = 0; i < buttons; ++i) {
        if (!g_heldButtons[i]) continue;
        if (rgb[i] & 0x80) {
            rgb[i] = 0;
        } else {
            g_heldButtons[i] = false;
        }
    }
}

template <int Index>
HRESULT STDMETHODCALLTYPE GetDeviceStateDetour(void* self, DWORD size, LPVOID data) {
    const HRESULT hr = g_stateHooks[Index].originalState(self, size, data);
    if (FAILED(hr) || !data) {
        if (size == 256) Tick();
        return hr;
    }
    const Kind kind = KindOf(self, "GetDeviceState", size);
    const bool blocking = Blocking();
    if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
        auto* joy = reinterpret_cast<DIJOYSTATE*>(data);
        std::memcpy(g_rawJoy, joy->rgbButtons, sizeof(g_rawJoy));
        if (!blocking) {
            const unsigned hide = HideJoy(joy->rgbButtons, 32);
            for (int i = 0; i < 32; ++i) {
                if (hide & (1u << i)) joy->rgbButtons[i] = 0;
            }
        }
    }
    if (size == 256) {  // keyboard state array: the once-per-frame poll
        std::memcpy(g_rawKeys, data, sizeof(g_rawKeys));
        overlay::FeedKeyboard(static_cast<const unsigned char*>(data));
        Tick();
        FilterKeyboard(static_cast<unsigned char*>(data), Blocking());
    } else if (kind == Kind::Mouse && (size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2))) {
        const auto* mouse = static_cast<const DIMOUSESTATE*>(data);
        overlay::FeedMouse(mouse->lX, mouse->lY, mouse->lZ, mouse->rgbButtons);
        FilterMouse(data, size, blocking);
    } else if (blocking) {
        std::memset(data, 0, size);
        if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
            // Centered POV hats are 0xFFFFFFFF, not 0.
            auto* pov = reinterpret_cast<DIJOYSTATE*>(data)->rgdwPOV;
            for (int i = 0; i < 4; ++i) {
                pov[i] = 0xFFFFFFFF;
            }
        }
    }
    return hr;
}

// Buffered mouse records feed the overlay cursor; records for keys or buttons
// that must stay hidden are removed from the batch the game receives.
void FilterBuffered(Kind kind, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, bool blocking) {
    auto* base = reinterpret_cast<unsigned char*>(data);
    long dx = 0, dy = 0, wheel = 0;
    bool mouseEvents = false;
    DWORD kept = 0;
    for (DWORD i = 0; i < *inOut; ++i) {
        auto* record = reinterpret_cast<DIDEVICEOBJECTDATA*>(base + i * objectSize);
        const DWORD ofs = record->dwOfs;
        const bool pressed = (record->dwData & 0x80) != 0;
        bool drop = blocking;
        if (kind == Kind::Mouse) {
            mouseEvents = true;
            if (ofs == DIMOFS_X) dx += static_cast<LONG>(record->dwData);
            else if (ofs == DIMOFS_Y) dy += static_cast<LONG>(record->dwData);
            else if (ofs == DIMOFS_Z) wheel += static_cast<LONG>(record->dwData);
            else if (ofs >= DIMOFS_BUTTON0 && ofs <= DIMOFS_BUTTON7) {
                const DWORD button = ofs - DIMOFS_BUTTON0;
                g_bufferedButtons[button] = pressed;
                if (blocking && pressed) g_heldButtons[button] = true;
                if (!blocking && g_heldButtons[button]) {
                    drop = true;
                    if (!pressed) g_heldButtons[button] = false;
                }
            }
        } else if (kind == Kind::Keyboard && ofs < 256) {
            if (KeyOwned(static_cast<int>(ofs))) drop = true;
            if (blocking && pressed) g_heldKeys[ofs] = true;
            if (!blocking && g_heldKeys[ofs]) {
                drop = true;
                if (!pressed) g_heldKeys[ofs] = false;
            }
        }
        if (!drop) {
            if (kept != i) std::memcpy(base + kept * objectSize, record, objectSize);
            ++kept;
        }
    }
    *inOut = kept;
    if (mouseEvents) {
        unsigned char buttons[4];
        for (int i = 0; i < 4; ++i) buttons[i] = g_bufferedButtons[i] ? 0x80 : 0;
        overlay::FeedMouse(dx, dy, wheel, buttons);
    }
}

template <int Index>
HRESULT STDMETHODCALLTYPE GetDeviceDataDetour(void* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                              DWORD flags) {
    const HRESULT hr = g_dataHooks[Index].originalData(self, objectSize, data, inOut, flags);
    if (SUCCEEDED(hr) && inOut && data && !(flags & DIGDD_PEEK) && objectSize >= sizeof(DIDEVICEOBJECTDATA)) {
        FilterBuffered(KindOf(self, "GetDeviceData", objectSize), objectSize, data, inOut, Blocking());
    } else if (SUCCEEDED(hr) && inOut && !data && Blocking() && !(flags & DIGDD_PEEK)) {
        *inOut = 0;  // flush-only call
    }
    return hr;
}

DWORD WINAPI XInputGetStateDetour(DWORD user, void* state) {
    const DWORD result = g_origXInputGetState(user, state);
    if (result == ERROR_SUCCESS && state && user == 0) {
        // XINPUT_STATE: DWORD packet number, then XINPUT_GAMEPAD whose first WORD is the buttons.
        std::memcpy(&g_rawXButtons, static_cast<const char*>(state) + sizeof(DWORD), sizeof(g_rawXButtons));
    }
    if (Blocking() && result == ERROR_SUCCESS && state) {
        // XINPUT_STATE: DWORD packet number followed by the gamepad block.
        std::memset(static_cast<char*>(state) + sizeof(DWORD), 0, 12);
    } else if (result == ERROR_SUCCESS && state && user == 0) {
        auto* buttons = reinterpret_cast<unsigned short*>(static_cast<char*>(state) + sizeof(DWORD));
        const unsigned short hide = HideXInput(*buttons);
        if (hide) *buttons = static_cast<unsigned short>(*buttons & ~hide);
    }
    return result;
}

bool HookOne(void* target, void* detour, void** original, const char* name) {
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(target);
    }
    if (status != MH_OK) {
        RM_LOGE("hook %s at %p failed: %s", name, target, MH_StatusToString(status));
        return false;
    }
    RM_LOGI("hook %s at %p queued", name, target);
    return true;
}

template <class Device>
void CollectTargets(Device* device, std::vector<void*>& state, std::vector<void*>& data) {
    void** vtable = *reinterpret_cast<void***>(device);
    auto add = [](std::vector<void*>& list, void* fn) {
        for (void* existing : list) {
            if (existing == fn) {
                return;
            }
        }
        list.push_back(fn);
    };
    add(state, vtable[9]);   // GetDeviceState
    add(data, vtable[10]);   // GetDeviceData
}

}  // namespace

void SetOwnedInputs(const std::vector<OwnedInput>& inputs) {
    std::lock_guard lock(g_ownedMutex);
    const bool same = inputs.size() == g_owned.size() &&
                      std::equal(inputs.begin(), inputs.end(), g_owned.begin(), [](const OwnedInput& a, const OwnedInput& b) {
                          return a.key == b.key && a.xinput == b.xinput && a.joy == b.joy;
                      });
    if (same) return;
    g_owned = inputs;
    g_xHolds.assign(inputs.size(), ComboHold{});
    g_joyHolds.assign(inputs.size(), ComboHold{});
}

bool RawKeyDown(int scancode) { return scancode >= 0 && scancode < 256 && (g_rawKeys[scancode] & 0x80) != 0; }
bool RawJoyButton(int index) { return index >= 0 && index < 32 && (g_rawJoy[index] & 0x80) != 0; }
unsigned short RawXInputButtons() { return g_rawXButtons; }

void TickGame(const char* source) {
    // The main thread both polls input and presents (observed on build
    // 11465250). DirectInput polling stops while the window is unfocused, so
    // either source may drive the tick; only the first thread seen counts.
    const DWORD thread = GetCurrentThreadId();
    if (g_status.gameThreadId == 0) {
        g_status.gameThreadId = thread;
        RM_LOGI("game thread is t%05lu (first tick from %s)", thread, source);
    } else if (g_status.gameThreadId != thread) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            RM_LOGW("tick source %s runs on t%05lu, not the game thread; ignored", source, thread);
        }
        return;
    }
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 8) {
        return;  // at most ~120 Hz
    }
    last = now;
    ++g_status.ticks;
    if (g_sink) {
        g_sink->GameTick();
    }
}

void RecordDirectInputCreate(const void* riid) {
    const auto& iid = *static_cast<const IID*>(riid);
    const char* name = IsEqualIID(iid, IID_IDirectInput8A) ? "IDirectInput8A"
                       : IsEqualIID(iid, IID_IDirectInput8W) ? "IDirectInput8W"
                                                              : "other";
    RM_LOGI("game called DirectInput8Create for %s", name);
}

bool InstallInputHooks(void* realDirectInput8Create) {
    auto create = reinterpret_cast<DirectInput8CreateFn>(realDirectInput8Create);
    std::vector<void*> stateTargets;
    std::vector<void*> dataTargets;
    HINSTANCE instance = GetModuleHandleW(nullptr);

    IDirectInput8A* inputA = nullptr;
    if (SUCCEEDED(create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&inputA), nullptr))) {
        IDirectInputDevice8A* keyboard = nullptr;
        if (SUCCEEDED(inputA->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
            CollectTargets(keyboard, stateTargets, dataTargets);
            keyboard->Release();
        }
        inputA->Release();
    }
    IDirectInput8W* inputW = nullptr;
    if (SUCCEEDED(create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&inputW), nullptr))) {
        IDirectInputDevice8W* keyboard = nullptr;
        if (SUCCEEDED(inputW->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
            CollectTargets(keyboard, stateTargets, dataTargets);
            keyboard->Release();
        }
        inputW->Release();
    }
    if (stateTargets.empty() || stateTargets.size() > 2 || dataTargets.size() > 2) {
        RM_LOGE("unexpected DirectInput device vtables (state=%zu data=%zu)", stateTargets.size(), dataTargets.size());
        return false;
    }

    bool ok = true;
    void* stateDetours[2] = {reinterpret_cast<void*>(&GetDeviceStateDetour<0>),
                             reinterpret_cast<void*>(&GetDeviceStateDetour<1>)};
    void* dataDetours[2] = {reinterpret_cast<void*>(&GetDeviceDataDetour<0>),
                            reinterpret_cast<void*>(&GetDeviceDataDetour<1>)};
    for (std::size_t i = 0; i < stateTargets.size(); ++i) {
        g_stateHooks[i].target = stateTargets[i];
        ok &= HookOne(stateTargets[i], stateDetours[i], reinterpret_cast<void**>(&g_stateHooks[i].originalState),
                      "IDirectInputDevice8::GetDeviceState");
    }
    for (std::size_t i = 0; i < dataTargets.size(); ++i) {
        g_dataHooks[i].target = dataTargets[i];
        ok &= HookOne(dataTargets[i], dataDetours[i], reinterpret_cast<void**>(&g_dataHooks[i].originalData),
                      "IDirectInputDevice8::GetDeviceData");
    }
    g_status.input = ok;

    if (HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll")) {
        if (void* target = reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState"))) {
            g_status.xinput = HookOne(target, reinterpret_cast<void*>(&XInputGetStateDetour),
                                      reinterpret_cast<void**>(&g_origXInputGetState), "XInputGetState");
        }
    } else {
        RM_LOGW("xinput1_3.dll not loaded; controller input is not blocked while the overlay is focused");
    }
    return ok;
}

}  // namespace rmre5
