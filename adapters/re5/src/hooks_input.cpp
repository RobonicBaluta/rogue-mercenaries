#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <dinput.h>
#include <MinHook.h>

#include <cstring>
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
XInputGetStateFn g_origXInputGetState = nullptr;

bool Blocking() { return g_sink && g_sink->BlockGameInput(); }

void Tick() { TickGame("dinput"); }

template <int Index>
HRESULT STDMETHODCALLTYPE GetDeviceStateDetour(void* self, DWORD size, LPVOID data) {
    const HRESULT hr = g_stateHooks[Index].originalState(self, size, data);
    if (size == 256) {  // keyboard state array: the once-per-frame poll
        if (SUCCEEDED(hr) && data) {
            overlay::FeedKeyboard(static_cast<const unsigned char*>(data));
        }
        Tick();
    } else if ((size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2)) && SUCCEEDED(hr) && data) {
        const auto* mouse = static_cast<const DIMOUSESTATE*>(data);
        overlay::FeedMouse(mouse->lX, mouse->lY, mouse->lZ, mouse->rgbButtons);
    }
    if (Blocking() && data && SUCCEEDED(hr)) {
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

template <int Index>
HRESULT STDMETHODCALLTYPE GetDeviceDataDetour(void* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                              DWORD flags) {
    const HRESULT hr = g_dataHooks[Index].originalData(self, objectSize, data, inOut, flags);
    if (Blocking() && SUCCEEDED(hr) && inOut && !(flags & DIGDD_PEEK)) {
        *inOut = 0;  // drop buffered events while the overlay has input
    }
    return hr;
}

DWORD WINAPI XInputGetStateDetour(DWORD user, void* state) {
    const DWORD result = g_origXInputGetState(user, state);
    if (Blocking() && result == ERROR_SUCCESS && state) {
        // XINPUT_STATE: DWORD packet number followed by the gamepad block.
        std::memset(static_cast<char*>(state) + sizeof(DWORD), 0, 12);
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
