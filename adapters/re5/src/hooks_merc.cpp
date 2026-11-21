// Mercenaries countdown control. The authoritative countdown is a field of
// the match object; game work +0x680 is only a per-frame mirror, so writes
// there are lost. The detour applies the mod's floor / end request right
// before the game's own update, on the game's thread.

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5 {
namespace {

using UpdateCountdownFn = void(__thiscall*)(std::uintptr_t self, float dt);
UpdateCountdownFn g_origUpdate = nullptr;

std::atomic<std::uintptr_t> g_object{0};
std::atomic<DWORD> g_lastSeen{0};
std::atomic<float> g_floor{-1.0f};
std::atomic<bool> g_endRequested{false};
std::atomic<bool> g_hideHud{false};

using StartStateFn = void(__fastcall*)(std::uintptr_t self, void* edx);
StartStateFn g_origStartState = nullptr;
std::atomic<bool> g_skipIntro{false};
std::atomic<DWORD> g_startSeen{0};
std::atomic<DWORD> g_shotsSeen{0};

// The match's intro (its camera shots, every player's controls locked) is the game's main state 6,
// re5::addr::kFnMercStartState. A run's match does not play it (the user, 2026-11-11: "announcer at
// the start of the arena, you can't do much ... remove it completely"; the run's keys-and-buttons
// hold had made it unskippable): at the step that starts the next shot (or waits for one, where a
// pad's press would skip) the step becomes the intro's end, the way the game goes there itself when
// its shot table ends, before the game's own handler runs. Fade out, the match's resources and the
// match follow, the game's own steps.
void __fastcall StartStateDetour(std::uintptr_t self, void* edx) {
    const DWORD tick = GetTickCount();
    g_startSeen = tick;
    std::uint8_t step = 0;
    if (self && SafeRead(self + re5::off::kGameStateStep, step)) {
        if (g_skipIntro.load() && (step == re5::off::kMercStartShot || step == re5::off::kMercStartShotWait) &&
            SafeStore<std::uint8_t>(self + re5::off::kGameStateStep, re5::off::kMercStartEnd)) {
            RM_LOGI("mercenaries: the intro skipped (step %02X to %02X)", step, re5::off::kMercStartEnd);
            step = re5::off::kMercStartEnd;
        }
        if (step >= re5::off::kMercStartShot && step <= re5::off::kMercStartShotPause) g_shotsSeen = tick;
    }
    g_origStartState(self, edx);
}

void __fastcall UpdateCountdownDetour(std::uintptr_t self, void*, float dt) {
    if (g_object.exchange(self) != self) {
        RM_LOGI("mercenaries match object %08X", static_cast<unsigned>(self));
    }
    g_lastSeen = GetTickCount();
    float countdown = 0.0f;
    std::int32_t state = -1;
    std::uintptr_t work = 0;
    std::int32_t kills = 0;
    if (SafeRead(self + re5::off::kMercCountdown, countdown) && SafeRead(self + re5::off::kMercState, state) &&
        state == 0) {
        if (g_endRequested.exchange(false)) {
            // Zero lets the game's own time-up path end the match next update.
            SafeStore(self + re5::off::kMercCountdown, 0.0f);
            RM_LOGI("mercenaries countdown forced to 0 (was %.2f)", countdown);
        } else {
            const float floor = g_floor.load();
            if (floor > 0.0f && countdown < floor) {
                SafeStore(self + re5::off::kMercCountdown, floor);
            }
            // While the run holds the match, only the mod ends it: the update compares the
            // kill counter with its cap (150, 400 in Duo), so it sees zero for that one call.
            if (floor > 0.0f && engine::Caps().mercHud) {
                work = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
                if (!work || !SafeRead(work + re5::off::kMercKills, kills) || kills <= 0 ||
                    !SafeStore(work + re5::off::kMercKills, std::int32_t{0})) {
                    work = 0;
                }
            }
        }
    }
    g_origUpdate(self, dt);
    if (work) {
        // The update itself never counts kills; keep any it did add anyway.
        SafeStore(work + re5::off::kMercKills, kills + ReadOr<std::int32_t>(work + re5::off::kMercKills, 0));
        static bool logged = false;
        if (kills >= 150 && !logged) {
            logged = true;
            RM_LOGI("mercenaries kill cap held off at %d kills", kills);
        }
    }
}

using DrawFn = void(__thiscall*)(std::uintptr_t self);
DrawFn g_origCockpitTimer = nullptr;
DrawFn g_origMerceHud = nullptr;

std::atomic<int> g_counter{-1};

// Cockpit sprites (children at +0x12D0): digit templates 0..9, and the clock's big seconds
// digits (tens, units) centred where the clock was. The minutes, hundredths and the two
// separators (292, 293) stay off.
constexpr std::size_t kCockpitSprites = 0x12D0;
constexpr int kDigitTemplate0 = 276;
constexpr int kClockSecondsTens = 288;
constexpr int kClockSecondsUnits = 289;
constexpr std::size_t kSpriteDraw = 0x8;
constexpr std::size_t kSpriteUv = 0x24;  // 16 bytes: the source rectangle
constexpr std::size_t kCockpitShowTimer = 0x344;

std::uintptr_t CockpitSprite(std::uintptr_t cockpit, int index) {
    return ReadOr<std::uintptr_t>(cockpit + kCockpitSprites + static_cast<std::size_t>(index) * 4, 0);
}

// The two newest cockpits the timer draw has seen (a split screen has two; a stage change makes
// new ones), for the developer's hudprobe.
std::atomic<std::uintptr_t> g_cockpits[2];

void NoteCockpit(std::uintptr_t self) {
    if (g_cockpits[0].load() == self || g_cockpits[1].load() == self) return;
    g_cockpits[0] = g_cockpits[1].load();
    g_cockpits[1] = self;
}

// The cockpit's move clears its sprites' draw bytes every frame, so a skipped timer draw
// leaves the clock off screen. In a run the same two digits show the mod's counter instead,
// under the conditions the timer draw itself checks (a match with a timer, nothing paused).

void __fastcall CockpitTimerDetour(std::uintptr_t self, void*) {
    NoteCockpit(self);
    if (!g_hideHud.load()) {
        g_origCockpitTimer(self);
        return;
    }
    const int counter = g_counter.load();
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    if (counter < 0 || !app || (ReadOr<std::uint32_t>(app + re5::off::kAppFlags, ~0u) & 0x70000400u) != 0 ||
        (ReadOr<std::uint32_t>(app + re5::off::kAppFlags + 4, ~0u) & 0x8000u) != 0 ||
        ReadOr<std::uint8_t>(self + kCockpitShowTimer, 0) == 0) {
        return;
    }
    const int value = std::min(counter, 99);
    const std::pair<int, int> digits[] = {{kClockSecondsTens, value / 10}, {kClockSecondsUnits, value % 10}};
    for (const auto& [target, digit] : digits) {
        const std::uintptr_t sprite = CockpitSprite(self, target);
        const std::uintptr_t glyph = CockpitSprite(self, kDigitTemplate0 + digit);
        std::array<std::uint32_t, 4> uv{};
        if (!sprite || !glyph || !SafeRead(glyph + kSpriteUv, uv)) continue;
        SafeStore(sprite + kSpriteUv, uv);
        SafeStore(sprite + kSpriteDraw, std::uint8_t{1});
    }
}

using PageElementsFn = std::uintptr_t(__thiscall*)(std::uintptr_t layout, std::uint32_t page);
using SetElementDrawFn = void(__thiscall*)(std::uintptr_t layout, int draw, std::uint32_t page, std::uint32_t element);

// Hidden, the Mercenaries HUD's draw only does what it does while the game is paused: it
// clears its two layout pages and leaves the score, combo and bonus panels off.
void __fastcall MerceHudDrawDetour(std::uintptr_t self, void*) {
    if (!g_hideHud.load()) {
        g_origMerceHud(self);
        return;
    }
    const auto layout = ReadOr<std::uintptr_t>(re5::addr::kLayoutManager, 0);
    if (!layout) return;
    const auto pageElements = reinterpret_cast<PageElementsFn>(re5::addr::kFnLayoutPageElements);
    const auto setElementDraw = reinterpret_cast<SetElementDrawFn>(re5::addr::kFnLayoutSetElementDraw);
    for (std::uintptr_t handle = self + 0x44; handle <= self + 0x48; handle += 4) {
        std::uint32_t page = 0;
        if (!SafeRead(handle, page)) continue;
        const std::uintptr_t elements = pageElements(layout, page);
        const auto count = elements ? ReadOr<std::uint32_t>(elements + 0xC, 0) : 0u;
        for (std::uint32_t e = 0; e < count; ++e) setElementDraw(layout, 0, page, e);
    }
}

using AddTimeFn = void(__thiscall*)(std::uintptr_t self, int seconds);
using ActionFn = void(__thiscall*)(std::uintptr_t self);
AddTimeFn g_origAddTime = nullptr;
ActionFn g_origTimeBonus = nullptr;
std::atomic<bool> g_suppressAddTime{false};
std::atomic<int> g_pickups{0};

void __fastcall AddTimeDetour(std::uintptr_t self, void*, int seconds) {
    if (g_suppressAddTime.load()) return;  // a run holds the countdown; bonuses are chests instead
    g_origAddTime(self, seconds);
}

std::mutex g_pickupMutex;
std::vector<std::array<float, 3>> g_pickupPositions;  // where each opened statue stood

void __fastcall TimeBonusActionDetour(std::uintptr_t self, void*) {
    const auto before = ReadOr<std::uint8_t>(self + 0x14, 0xFF);
    const auto object = ReadOr<std::uintptr_t>(self + 0x18, 0);
    g_origTimeBonus(self);
    if (before == 1 && object && ReadOr<std::uint8_t>(self + 0x14, 0) == 2) {
        g_pickups.fetch_add(1);
        {
            std::lock_guard lock(g_pickupMutex);
            g_pickupPositions.push_back({ReadOr<float>(object + re5::off::kUnitPosition, 0.0f),
                                         ReadOr<float>(object + re5::off::kUnitPosition + 4, 0.0f),
                                         ReadOr<float>(object + re5::off::kUnitPosition + 8, 0.0f)});
            if (g_pickupPositions.size() > 16) g_pickupPositions.erase(g_pickupPositions.begin());
        }
        RM_LOGI("time bonus picked up: object %08X (%s, %d s)", static_cast<unsigned>(object),
                engine::ClassName(object).c_str(), ReadOr<std::int16_t>(object + 0x1364, 0));
    }
}

}  // namespace

void SetMercTimerFloor(float seconds) { g_floor = seconds; }
void SetMercHudHidden(bool hidden) { g_hideHud = hidden; }
void SetMercHudCounter(int value) { g_counter = value; }
void SuppressMercAddTime(bool suppress) { g_suppressAddTime = suppress; }
int TakeTimeBonusPickups() { return g_pickups.exchange(0); }

bool TakeTimeBonusPosition(float pos[3]) {
    std::lock_guard lock(g_pickupMutex);
    if (g_pickupPositions.empty()) return false;
    for (int k = 0; k < 3; ++k) pos[k] = g_pickupPositions.front()[static_cast<std::size_t>(k)];
    g_pickupPositions.erase(g_pickupPositions.begin());
    return true;
}
void SimulateTimeBonusPickup() { g_pickups.fetch_add(1); }

void RequestMercMatchEnd() { g_endRequested = true; }
void ClearMercMatchEnd() { g_endRequested = false; }

void SkipMercIntro(bool skip) { g_skipIntro = skip; }
bool MercStartRunning() {
    const DWORD seen = g_startSeen.load();
    return seen != 0 && GetTickCount() - seen < 300;
}
bool MercIntroShotsPlaying() {
    const DWORD seen = g_shotsSeen.load();
    return seen != 0 && GetTickCount() - seen < 300;
}

std::uintptr_t CockpitObject(int index) { return index == 0 || index == 1 ? g_cockpits[index].load() : 0; }

MercTimerView ReadMercTimer() {
    MercTimerView view;
    view.object = g_object.load();
    view.recent = view.object != 0 && GetTickCount() - g_lastSeen.load() < 500;
    if (view.recent) {
        view.countdown = ReadOr<float>(view.object + re5::off::kMercCountdown, 0.0f);
        view.state = ReadOr<std::int32_t>(view.object + re5::off::kMercState, -1);
    }
    return view;
}

bool InstallMercHooks() {
    if (!engine::Caps().mercCountdown) {
        RM_LOGE("mercenaries countdown hook skipped: signature missing");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnMercUpdateCountdown);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdateCountdownDetour),
                                     reinterpret_cast<void**>(&g_origUpdate));
    if (status == MH_OK) {
        status = MH_QueueEnableHook(target);
    }
    if (status != MH_OK) {
        RM_LOGE("hook cMercs::UpdateCountdown failed: %s", MH_StatusToString(status));
        return false;
    }
    RM_LOGI("hook cMercs::UpdateCountdown at 0x%08X queued", static_cast<unsigned>(re5::addr::kFnMercUpdateCountdown));
    if (engine::Caps().mercIntro) {
        auto* start = reinterpret_cast<void*>(re5::addr::kFnMercStartState);
        MH_STATUS s = MH_CreateHook(start, reinterpret_cast<void*>(&StartStateDetour), reinterpret_cast<void**>(&g_origStartState));
        if (s == MH_OK) s = MH_QueueEnableHook(start);
        if (s == MH_OK) {
            RM_LOGI("hook the Mercenaries start at 0x%08X queued (a run skips the intro)", static_cast<unsigned>(re5::addr::kFnMercStartState));
        } else {
            RM_LOGE("hook the Mercenaries start failed: %s; the intro plays", MH_StatusToString(s));
        }
    } else {
        RM_LOGW("the Mercenaries start's signature is missing; the intro plays");
    }
    if (engine::Caps().timeBonus) {
        auto* addTime = reinterpret_cast<void*>(re5::addr::kFnMercAddCountdown);
        auto* action = reinterpret_cast<void*>(re5::addr::kFnTimeBonusAction);
        MH_STATUS a = MH_CreateHook(addTime, reinterpret_cast<void*>(&AddTimeDetour), reinterpret_cast<void**>(&g_origAddTime));
        if (a == MH_OK) a = MH_QueueEnableHook(addTime);
        MH_STATUS b = MH_CreateHook(action, reinterpret_cast<void*>(&TimeBonusActionDetour), reinterpret_cast<void**>(&g_origTimeBonus));
        if (b == MH_OK) b = MH_QueueEnableHook(action);
        if (a == MH_OK && b == MH_OK) {
            RM_LOGI("hooks cMercs::AddTime and the time-bonus pickup queued (chests)");
        } else {
            RM_LOGE("time-bonus hooks failed: %s / %s", MH_StatusToString(a), MH_StatusToString(b));
        }
    }
    if (engine::Caps().mercHud) {
        auto* timer = reinterpret_cast<void*>(re5::addr::kFnCockpitDrawTimer);
        auto* hud = reinterpret_cast<void*>(re5::addr::kFnMerceHudDraw);
        MH_STATUS a = MH_CreateHook(timer, reinterpret_cast<void*>(&CockpitTimerDetour), reinterpret_cast<void**>(&g_origCockpitTimer));
        if (a == MH_OK) a = MH_QueueEnableHook(timer);
        MH_STATUS b = MH_CreateHook(hud, reinterpret_cast<void*>(&MerceHudDrawDetour), reinterpret_cast<void**>(&g_origMerceHud));
        if (b == MH_OK) b = MH_QueueEnableHook(hud);
        if (a == MH_OK && b == MH_OK) {
            RM_LOGI("hooks cockpit timer draw and Mercenaries HUD draw queued (clock and score)");
        } else {
            RM_LOGE("HUD hooks failed: %s / %s", MH_StatusToString(a), MH_StatusToString(b));
        }
    }
    return true;
}

}  // namespace rmre5
