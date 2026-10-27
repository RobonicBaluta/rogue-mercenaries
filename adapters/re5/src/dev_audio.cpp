// Developer option: mute this process' audio session so automated test
// launches stay silent on a machine someone else is using. Windows may keep
// an app's mute state across runs, so every mute leaves a marker file and the
// next launch (or a clean window close) unmutes again.

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <fstream>

#include "dev_audio.h"
#include "log.h"

namespace rmre5 {
namespace {

std::filesystem::path g_marker;

}  // namespace

bool SetOwnAudioMute(bool mute) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool done = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager* manager = nullptr;
    ISimpleAudioVolume* volume = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&manager))) &&
        SUCCEEDED(manager->GetSimpleAudioVolume(nullptr, FALSE, &volume))) {
        done = SUCCEEDED(volume->SetMute(mute ? TRUE : FALSE, nullptr));
    }
    if (volume) volume->Release();
    if (manager) manager->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return done;
}

void ManageDevAudio(const std::filesystem::path& dataDir, bool mute) {
    g_marker = dataDir / L"dev_muted.flag";
    std::error_code ec;
    const bool markerPresent = std::filesystem::exists(g_marker, ec);
    if (!mute && !markerPresent) {
        return;
    }
    if (mute) {
        std::filesystem::create_directories(dataDir, ec);
        std::ofstream(g_marker) << "audio muted by developer option\n";
    }
    // The game's session appears some seconds after launch; keep applying.
    for (int attempt = 0; attempt < 30; ++attempt) {
        const bool ok = SetOwnAudioMute(mute);
        if (attempt == 0 || attempt == 29) {
            RM_LOGI("dev: set own audio mute=%d -> %s (attempt %d)", mute, ok ? "ok" : "failed", attempt);
        }
        Sleep(1000);
    }
    if (!mute) {
        std::filesystem::remove(g_marker, ec);
        RM_LOGI("dev: audio unmuted and marker removed");
    }
}

void RestoreDevAudioOnExit() {
    std::error_code ec;
    if (!g_marker.empty() && std::filesystem::exists(g_marker, ec) && SetOwnAudioMute(false)) {
        std::filesystem::remove(g_marker, ec);
        RM_LOGI("dev: audio unmuted on window close");
    }
}

}  // namespace rmre5
