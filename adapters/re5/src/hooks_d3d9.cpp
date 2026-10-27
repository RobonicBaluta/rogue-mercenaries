#include <windows.h>

#include <d3d9.h>
#include <MinHook.h>

#include "hooks.h"
#include "log.h"
#include "overlay.h"

namespace rmre5 {

namespace detail {
extern HookStatus g_status;
}
using detail::g_status;

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SwapPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                  const RGNDATA*, DWORD);

PresentFn g_origPresent = nullptr;
ResetFn g_origReset = nullptr;
SwapPresentFn g_origSwapPresent = nullptr;
thread_local bool t_inPresent = false;

HRESULT STDMETHODCALLTYPE PresentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                                         const RGNDATA* dirty) {
    if (!t_inPresent) {
        t_inPresent = true;
        if (++g_status.presents % 600 == 1) {
            RM_LOGD("present heartbeat %llu ticks %llu (device Present)", g_status.presents, g_status.ticks);
        }
        TickGame("present");
        overlay::OnPresent(device);
        t_inPresent = false;
    }
    return g_origPresent(device, src, dst, window, dirty);
}

HRESULT STDMETHODCALLTYPE SwapPresentDetour(IDirect3DSwapChain9* chain, const RECT* src, const RECT* dst, HWND window,
                                             const RGNDATA* dirty, DWORD flags) {
    if (!t_inPresent) {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(chain->GetDevice(&device))) {
            t_inPresent = true;
            if (++g_status.presents % 600 == 1) {
                RM_LOGD("present heartbeat %llu ticks %llu (swap chain Present)", g_status.presents, g_status.ticks);
            }
            TickGame("present");
            overlay::OnPresent(device);
            t_inPresent = false;
            device->Release();
        }
    }
    return g_origSwapPresent(chain, src, dst, window, dirty, flags);
}

HRESULT STDMETHODCALLTYPE ResetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    overlay::OnBeforeReset();
    const HRESULT hr = g_origReset(device, params);
    if (SUCCEEDED(hr)) {
        overlay::OnAfterReset();
    }
    RM_LOGI("IDirect3DDevice9::Reset -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
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

}  // namespace

bool InstallD3d9Hooks() {
    // A throwaway device on a hidden window exposes the d3d9 implementation's
    // vtable; the game's device shares the same function bodies.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RogueMercenariesD3D9Probe";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    if (!window) {
        RM_LOGE("d3d9 probe window failed: %lu", GetLastError());
        return false;
    }

    bool ok = false;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.hDeviceWindow = window;
        IDirect3DDevice9* device = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp,
                                       &device);
        if (SUCCEEDED(hr) && device) {
            void** deviceVtable = *reinterpret_cast<void***>(device);
            IDirect3DSwapChain9* chain = nullptr;
            void** chainVtable = nullptr;
            if (SUCCEEDED(device->GetSwapChain(0, &chain))) {
                chainVtable = *reinterpret_cast<void***>(chain);
            }
            ok = HookOne(deviceVtable[17], reinterpret_cast<void*>(&PresentDetour),
                         reinterpret_cast<void**>(&g_origPresent), "IDirect3DDevice9::Present");
            ok &= HookOne(deviceVtable[16], reinterpret_cast<void*>(&ResetDetour),
                          reinterpret_cast<void**>(&g_origReset), "IDirect3DDevice9::Reset");
            if (chainVtable) {
                ok &= HookOne(chainVtable[3], reinterpret_cast<void*>(&SwapPresentDetour),
                              reinterpret_cast<void**>(&g_origSwapPresent), "IDirect3DSwapChain9::Present");
                chain->Release();
            }
            device->Release();
        } else {
            RM_LOGE("d3d9 probe device creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        }
        d3d->Release();
    }
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_status.d3d9 = ok;
    return ok;
}

}  // namespace rmre5
