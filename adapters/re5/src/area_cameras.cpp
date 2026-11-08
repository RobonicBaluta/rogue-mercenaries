// Area cameras. See area_cameras.h.

#include "area_cameras.h"

#include <windows.h>

#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::areacams {
namespace {

// The zone check and the forced zone are thiscalls on the player camera (uCameraCtrl).
using CheckFn = int(__fastcall*)(std::uintptr_t ctrl);
using ForceFn = std::uint32_t(__fastcall*)(std::uintptr_t ctrl, void* edx, std::uint32_t index);
using SetFn = void(__fastcall*)(std::uintptr_t ctrl, void* edx, int index, int apply);

CheckFn g_origCheck = nullptr;
ForceFn g_origForce = nullptr;
std::atomic<bool> g_runWantsOff{false};
std::atomic<int> g_mode{static_cast<int>(Mode::Auto)};
std::atomic<int> g_logged{0};
std::atomic<bool> g_checkSeen{false};
std::atomic<bool> g_setFaulted{false};

constexpr int kLogCap = 300;

bool Log() { return g_logged.fetch_add(1) < kLogCap; }

// The game's "no zone" camera: what the check itself does when the player stands in none.
bool SetNoZoneCamera(std::uintptr_t ctrl) {
    __try {
        reinterpret_cast<SetFn>(re5::addr::kFnAreaCameraSet)(ctrl, nullptr, -1, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall CheckDetour(std::uintptr_t ctrl) {
    if (!g_checkSeen.exchange(true)) RM_LOGI("area cameras: the player camera's zone check runs (camera %08X)", static_cast<unsigned>(ctrl));
    if (!Off() || g_setFaulted.load() || ReadOr<std::uintptr_t>(re5::addr::kAreaCameraResource, 0) == 0) {
        const int before = ReadOr<std::int32_t>(ctrl + re5::off::kCamCtrlZoneCamera, -1);
        const int result = g_origCheck(ctrl);
        const int after = ReadOr<std::int32_t>(ctrl + re5::off::kCamCtrlZoneCamera, -1);
        if (after != before && Log()) RM_LOGI("area cameras: the game's zone camera %d -> %d", before, after);
        return result;
    }
    // Off: no zone. A forced one (a script's, from before) ends too.
    const auto flags = ReadOr<std::uint32_t>(ctrl + re5::off::kCamCtrlAreaFlags, 0);
    if (flags & 1u) {
        SafeStore<std::uint32_t>(ctrl + re5::off::kCamCtrlAreaFlags, flags & ~1u);
        if (Log()) RM_LOGI("area cameras: a script's zone camera ended (off in runs)");
    }
    const int previous = ReadOr<std::int32_t>(ctrl + re5::off::kCamCtrlZoneCamera, -1);
    SafeStore<std::int32_t>(ctrl + re5::off::kCamCtrlPrevZoneCamera, previous);
    SafeStore<std::int32_t>(ctrl + re5::off::kCamCtrlZoneCamera, -1);
    if (previous != -1) {
        if (!SetNoZoneCamera(ctrl)) {
            g_setFaulted = true;  // the game's own from now on
            RM_LOGW("area cameras: the camera set faulted; the game's zone cameras stay on");
            SafeStore<std::int32_t>(ctrl + re5::off::kCamCtrlZoneCamera, previous);
            return g_origCheck(ctrl);
        }
        if (Log()) RM_LOGI("area cameras: zone camera %d off, the player camera back (off in runs)", previous);
    }
    SafeStore<std::uintptr_t>(ctrl + re5::off::kCamCtrlArea, 0);
    return 2;
}

std::uint32_t __fastcall ForceDetour(std::uintptr_t ctrl, void* edx, std::uint32_t index) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (Off() && !g_setFaulted.load()) {
        if (Log()) RM_LOGI("area cameras: zone camera %u refused (asked from %08X; off in runs)", index, static_cast<unsigned>(caller));
        return 0;
    }
    if (Log()) RM_LOGI("area cameras: zone camera %u forced (asked from %08X)", index, static_cast<unsigned>(caller));
    return g_origForce(ctrl, edx, index);
}

bool Queue(std::uintptr_t address, void* detour, void** original, const char* what) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("area cameras: hook %s at 0x%08X failed: %s", what, static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    if (!engine::Caps().areaCameras) {
        RM_LOGW("area cameras: signatures missing; the game's zone cameras stay on in runs");
        return false;
    }
    if (!Queue(re5::addr::kFnAreaCameraCheck, reinterpret_cast<void*>(&CheckDetour), reinterpret_cast<void**>(&g_origCheck),
               "zone check") ||
        !Queue(re5::addr::kFnAreaCameraForce, reinterpret_cast<void*>(&ForceDetour), reinterpret_cast<void**>(&g_origForce),
               "forced zone")) {
        return false;
    }
    RM_LOGI("area cameras: zone check and forced zone hooks queued");
    return true;
}

void SetRunWantsOff(bool off) {
    if (g_runWantsOff.exchange(off) != off && GetMode() == Mode::Auto) {
        RM_LOGI("area cameras: %s", off ? "off (a run plays a campaign area)" : "the game's own");
    }
}

void SetMode(Mode mode) { g_mode = static_cast<int>(mode); }
Mode GetMode() { return static_cast<Mode>(g_mode.load()); }

bool Off() {
    switch (GetMode()) {
        case Mode::Game: return false;
        case Mode::Off: return true;
        case Mode::Auto: break;
    }
    return g_runWantsOff.load();
}

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::Game: return "game";
        case Mode::Off: return "off";
        case Mode::Auto: break;
    }
    return "auto";
}

}  // namespace rmre5::areacams
