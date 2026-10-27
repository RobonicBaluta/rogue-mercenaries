// Developer probe for the game's menu screens (uId* classes). Finds every
// class derived from uIdBase through MT Framework DTI, locates its vtable
// from the getDTI stub, and hooks the per-frame move() (vtable slot 6) so the
// live screens are known. Snapshot/diff commands reveal which fields change
// when the menu cursor moves. Developer mode only; never shipped enabled.

#include "menu_probe.h"

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "log.h"
#include "memory.h"

namespace rmre5::menuprobe {
namespace {

constexpr std::uintptr_t kDtiUIdBase = 0x011F5000;
constexpr std::uintptr_t kTextBegin = 0x00401000;
constexpr std::uintptr_t kTextEnd = 0x00F4A000;
constexpr std::uintptr_t kRdataBegin = 0x00F4A000;
constexpr std::uintptr_t kRdataEnd = 0x010CA000;
constexpr std::size_t kMoveSlot = 6;
constexpr int kMaxHooks = 128;

using MoveFn = void(__thiscall*)(std::uintptr_t self);

struct HookSlot {
    void* target = nullptr;
    MoveFn original = nullptr;
};
std::array<HookSlot, kMaxHooks> g_hooks;

struct Live {
    std::uintptr_t object = 0;
    DWORD lastSeen = 0;
    bool announced = false;
};
std::mutex g_mutex;
std::map<std::uintptr_t, Live> g_live;  // object -> state
std::map<std::uintptr_t, std::vector<unsigned char>> g_snapshots;

void Seen(std::uintptr_t self) {
    std::lock_guard lock(g_mutex);
    auto& entry = g_live[self];
    entry.object = self;
    entry.lastSeen = GetTickCount();
}

template <int N>
void __fastcall MoveDetour(std::uintptr_t self, void*) {
    Seen(self);
    g_hooks[N].original(self);
}

template <int... N>
constexpr std::array<void*, sizeof...(N)> MakeDetours(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&MoveDetour<N>)...};
}
const auto kDetours = MakeDetours(std::make_integer_sequence<int, kMaxHooks>{});

void CollectDtis(std::uintptr_t dti, std::set<std::uintptr_t>& out, int depth) {
    if (depth > 8) return;
    for (auto child = ReadOr<std::uintptr_t>(dti + 0xC, 0); child; child = ReadOr<std::uintptr_t>(child + 8, 0)) {
        out.insert(child);
        CollectDtis(child, out, depth + 1);
    }
}

std::uint32_t ObjectSize(std::uintptr_t object) {
    const auto dti = engine::DtiOf(object);
    const auto size = ReadOr<std::uint32_t>(dti + 0x1C, 0) & 0xFFFFFF;
    return size > 0 && size <= 0x8000 ? size : 0x200;
}

}  // namespace

bool Install() {
    std::set<std::uintptr_t> dtis;
    CollectDtis(kDtiUIdBase, dtis, 0);
    // getDTI stubs: B8 <imm32 dti> C3
    std::map<std::uintptr_t, std::uintptr_t> stubToDti;
    for (std::uintptr_t p = kTextBegin; p + 6 < kTextEnd; ++p) {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        if (b[0] != 0xB8 || b[5] != 0xC3) continue;
        std::uintptr_t imm;
        std::memcpy(&imm, b + 1, 4);
        if (dtis.count(imm)) stubToDti[p] = imm;
    }
    std::set<std::uintptr_t> targets;
    int vtables = 0;
    for (std::uintptr_t p = kRdataBegin; p + 4 <= kRdataEnd; p += 4) {
        const auto value = *reinterpret_cast<const std::uintptr_t*>(p);
        if (!stubToDti.count(value)) continue;
        const std::uintptr_t vtable = p - 0x10;  // getDTI is slot 4
        const auto move = ReadOr<std::uintptr_t>(vtable + kMoveSlot * 4, 0);
        if (move >= kTextBegin && move < kTextEnd) {
            targets.insert(move);
            ++vtables;
        }
    }
    int hooked = 0;
    for (auto target : targets) {
        if (hooked >= kMaxHooks) break;
        auto& slot = g_hooks[hooked];
        slot.target = reinterpret_cast<void*>(target);
        if (MH_CreateHook(slot.target, kDetours[hooked], reinterpret_cast<void**>(&slot.original)) == MH_OK &&
            MH_QueueEnableHook(slot.target) == MH_OK) {
            ++hooked;
        }
    }
    RM_LOGI("menu probe: %zu uId classes, %zu getDTI stubs, %d vtables, %d move hooks queued", dtis.size(),
            stubToDti.size(), vtables, hooked);
    return hooked > 0;
}

void OnGameTick() {
    const DWORD now = GetTickCount();
    std::lock_guard lock(g_mutex);
    for (auto it = g_live.begin(); it != g_live.end();) {
        Live& live = it->second;
        if (!live.announced) {
            live.announced = true;
            RM_LOGI("menu screen opened: %s obj=%08X state=%u", engine::ClassName(live.object).c_str(),
                    static_cast<unsigned>(live.object), ReadOr<std::uint32_t>(live.object + 0x14, 0) & 0xFF);
        }
        if (now - live.lastSeen > 1000) {
            RM_LOGI("menu screen closed: obj=%08X", static_cast<unsigned>(live.object));
            it = g_live.erase(it);
        } else {
            ++it;
        }
    }
}

std::string Describe() {
    std::lock_guard lock(g_mutex);
    std::string out;
    for (const auto& [object, live] : g_live) {
        char line[160];
        std::snprintf(line, sizeof(line), "%s obj=%08X state=%u size=0x%X\n", engine::ClassName(object).c_str(),
                      static_cast<unsigned>(object), ReadOr<std::uint32_t>(object + 0x14, 0) & 0xFF,
                      ObjectSize(object));
        out += line;
    }
    return out;
}

void Snapshot() {
    std::lock_guard lock(g_mutex);
    g_snapshots.clear();
    for (const auto& [object, live] : g_live) {
        std::vector<unsigned char> bytes(ObjectSize(object));
        if (SafeCopy(bytes.data(), object, bytes.size())) g_snapshots[object] = std::move(bytes);
    }
    RM_LOGI("menu snapshot of %zu screens", g_snapshots.size());
}

void Diff() {
    std::lock_guard lock(g_mutex);
    for (const auto& [object, before] : g_snapshots) {
        std::vector<unsigned char> now(before.size());
        if (!SafeCopy(now.data(), object, now.size())) {
            RM_LOGI("menu diff obj=%08X unreadable", static_cast<unsigned>(object));
            continue;
        }
        RM_LOGI("menu diff %s obj=%08X", engine::ClassName(object).c_str(), static_cast<unsigned>(object));
        for (std::size_t off = 0; off + 4 <= now.size(); off += 4) {
            std::uint32_t a, b;
            std::memcpy(&a, before.data() + off, 4);
            std::memcpy(&b, now.data() + off, 4);
            if (a != b) {
                float fa, fb;
                std::memcpy(&fa, &a, 4);
                std::memcpy(&fb, &b, 4);
                RM_LOGI("  +%03zX: %08X -> %08X (%d -> %d, %.3f -> %.3f)", off, a, b, static_cast<int>(a),
                        static_cast<int>(b), fa, fb);
            }
        }
    }
}

}  // namespace rmre5::menuprobe
