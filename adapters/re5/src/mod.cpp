// Roguelite mod for RE5: drives rm::RunController from engine hooks and draws
// the run UI. Arenas map onto Mercenaries matches: a match starting while the
// run waits for its next arena begins that arena; reaching the kill budget
// completes it and (optionally) ends the match through the game's own timer.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine.h"
#include "effects_pack.h"
#include "file_redirect.h"
#include "area_cameras.h"
#include "stage_fixes.h"
#include "imgui.h"
#include "icons.h"
#include "portraits.h"
#include "imgui_internal.h"
#include "log.h"
#include "memory.h"
#include "menu_probe.h"
#include "autopilot.h"
#include "native_menu.h"
#include "match_ui.h"
#include "itembox_guard.h"
#include "director.h"
#include "resources.h"
#include "world.h"
#include "host.h"
#include "mod.h"
#include "overlay.h"
#include "probe.h"
#include "re5_layout.h"
#include "version.h"

#include "rm/content.h"
#include "rm/diagnostics.h"
#include "rm/effects.h"
#include "rm/screen_fx.h"
#include "rm/gun_levels.h"
#include "rm/inventory_plan.h"
#include "rm/profile.h"
#include "rm/rosters.h"
#include "rm/run_controller.h"
#include "rm/unlock_all.h"
#include "rm/telemetry.h"
#include "rm/common.h"

namespace rmre5 {
namespace {

double Now() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// Short on-screen messages derived from core events.
class ToastSink final : public rm::IEventSink {
public:
    struct Toast {
        std::string text;
        double until = 0.0;
    };

    void OnEvent(const rm::Event& event) override {
        std::string text;
        auto field = [&](const char* key) -> std::string {
            const rm::FieldValue* v = event.Find(key);
            if (!v) return {};
            if (const auto* s = std::get_if<std::string>(v)) return *s;
            if (const auto* i = std::get_if<std::int64_t>(v)) return std::to_string(*i);
            if (const auto* d = std::get_if<double>(v)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.2f", *d);
                return buf;
            }
            if (const auto* b = std::get_if<bool>(v)) return *b ? "yes" : "no";
            return {};
        };
        switch (event.type) {
            case rm::EventType::PerkAcquired:
                if (event.level != rm::EventLevel::Info) return;
                text = !field("perk").empty() ? "New perk: " + field("perk") : "New weapon: " + field("weapon");
                break;
            case rm::EventType::PerkTriggered:
                // Per-kill triggers would flood the screen; show the notable ones.
                if (field("trigger") == "kill") return;
                text = field("source") + " (" + field("trigger") + ")";
                break;
            case rm::EventType::ArenaStarted: text = "Arena " + std::to_string(event.arena) + " started"; break;
            case rm::EventType::ArenaCompleted: text = "Arena " + std::to_string(event.arena) + " cleared!"; break;
            case rm::EventType::Purchase:
                text = "Bought " + field("kind") + (field("perk").empty() ? "" : ": " + field("perk"));
                break;
            case rm::EventType::Error: text = "Error: " + field("message"); break;
            case rm::EventType::CharacterUnlocked: text = "Character unlocked: " + field("character"); break;
            default: return;
        }
        if (text.size() < 4) return;
        std::lock_guard lock(mutex_);
        toasts_.push_back({text, Now() + 4.0});
        while (toasts_.size() > 5) toasts_.pop_front();
    }

    std::deque<Toast> Active() {
        std::lock_guard lock(mutex_);
        const double now = Now();
        while (!toasts_.empty() && toasts_.front().until < now) toasts_.pop_front();
        return toasts_;
    }

    void Push(std::string text) {
        std::lock_guard lock(mutex_);
        toasts_.push_back({std::move(text), Now() + 5.0});
        while (toasts_.size() > 5) toasts_.pop_front();
    }

private:
    std::mutex mutex_;
    std::deque<Toast> toasts_;
};

struct MatchState {
    bool inMatch = false;
    int mode = -1;
    int stage = -1;
    float timer = 0.0f;
    std::string localClass;
    std::string localCharacter;  // core id, empty if unmapped
    int localHp = 0;
    int localMaxHp = 0;
    double zeroHpSince = -1.0;
    std::array<double, 2> seatZeroHpSince{-1.0, -1.0};  // M10: each human player's (sPlayer slot)
};

struct ModState {
    Paths paths;
    Settings settings;
    std::mutex mutex;  // serializes every RunController call (game and render paths)
    std::shared_ptr<const rm::Content> content;
    std::vector<std::string> contentErrors;
    std::unique_ptr<rm::JsonlSink> jsonl;
    ToastSink toasts;
    std::unique_ptr<rm::FanoutSink> fanout;
    std::unique_ptr<rm::ProfileStore> store;
    std::unique_ptr<rm::RunController> run;
    MatchState match;
    bool arenaEndRequested = false;  // timer forced to 0 after a clear
    bool devQuiet = false;           // developer: no director refills, native spawns blocked
    bool devBreakGrabs = false;      // developer: every Majini grab broken, as Iron Grip does (weapon tests)
    std::uint64_t chestsPlacedFor = 0;  // (match generation, arena) whose chests are placed
    double arenaBeganAt = -1.0;         // when the current arena began (director grace period)
    bool matchUiOpenRequested = false;  // native results screen to open once the tick's lock is released
    bool itemBoxRequested = false;      // M7.1: the game's Organize/Buy screen, from the SHOP's organize row
    struct Relock {
        std::array<float, 3> pos{};
        double at = 0.0;
    };
    std::vector<Relock> relockChests;   // M8 keys: chests opened without a key, placed again
    bool shopUiRequested = false;       // back to the SHOP screen when that screen closes
    bool organizeUiRequested = false;   // dev "organize" in co-op: the mod's grid screen (opened after the tick)
    std::string notice;       // short line shown instead of the status (chest contents)
    int organizePick = -1;    // ORGANIZE: grid cell picked up (-1 = none)
    double noticeUntil = 0.0;
    std::string noticeNext;   // shown once `notice` is over (an arena's guests after its name)
    // Weapon item ids whose models this match loaded (the character's own loadout): the only
    // weapons that can be created mid-match without hanging the game.
    std::set<std::uint32_t> loadedWeaponItems;
    int matchGeneration = 0;         // +1 on every match detected
    int currentStage = -1;           // stage the autopilot armed for the current match
    // Units the director spawned this match -> their run id ("dir_<ticket>"). A kill report
    // takes the id out, so a later unit in the same memory is not mistaken for it.
    std::unordered_map<std::uintptr_t, std::string> directorUnits;
    // The boss of a boss arena (M6) and the health the arena gives it.
    struct Boss {
        std::uintptr_t unit = 0;  // 0 before its spawn and once its death is reported
        int hp = 0;
        double spawnedAt = 0.0;
        bool dead = false;
        std::uintptr_t fallen = 0;  // the unit once dead, for its fall (DriveBossFall)
        double diedAt = 0.0;
    } boss;
    struct StageTransition {
        bool pending = false;
        int generation = 0;     // matchGeneration when the match end was requested
        double startedAt = 0.0;
        std::vector<std::pair<std::uint32_t, int>> carry;  // (item id, count) per slot record
        std::array<std::uint32_t, 9> cells{};             // item id per grid cell (ORGANIZE layout)
        std::array<int, 9> counts{};                      // and its count
        int hp = -1;            // player health at the transition (-1 = unknown)
    } transition;
    bool autoOpenedForPhase = false;
    rm::Phase lastPhase = rm::Phase::Idle;
    std::string lastStatus;
    std::uint64_t seedInput = 0;
    int selectedCharacter = 0;
    std::string selectedSkin;  // skin id for the selected character (empty = default)
    std::string selectedLength;  // the ladder length of the next run (empty = the first)
    int coopPlayers = 1;         // M10 local co-op: 1 solo, 2 split screen (the Play menu's PLAYERS)
    std::string p2Skin;          // M10: player 2's chosen skin id (empty: their character's first outfit that fits)
    int p2Character = -1;        // M10: player 2's chosen character (index in Characters(); -1: their outfit's)
    int menuPlayer = 0;          // M10: whose choices CHARACTERS, SKINS and PERKS show and change (0 or 1)
    std::string menuOtherSkin;   // M10: the other player's outfit while those screens run (clashes)
    int p2Costume = -1;          // M10: player 2's costume slot in the run's first match
    int p1Pad = -2;              // M10: player 1's XInput user (-1 the keyboard, -2 not known yet)
    int p2Pad = -1;              // M10: player 2's XInput user (-1 not known yet)
    bool coopLobbyNeeded = false;  // M10: a resumed co-op run's next match shows the lobby (player 2 joins again)
    int itemBoxSeat = 0;         // M10: whose ORGANIZE INVENTORY the game's screen holds
    rm::PerkCategory perkCategory = rm::PerkCategory::Style;  // slot opened from the PERKS screen
    std::string diagnosticsResult;
    // Set when any mod code threw; the game keeps running with vanilla rules.
    std::atomic<bool> disabled{false};
    std::string disabledReason;
    // Set when the player picks ROGUE MERCENARIES in the Play menu: the next
    // Mercenaries match starts a run with the character chosen in game.
    bool rogueModePending = false;
    // Developer UI demo (settings.uiDemo): scripted run on a sandbox profile.
    double demoStart = -1.0;
    int demoStep = 0;
};

ModState* g = nullptr;

void DisableMod(const char* where, const char* what) {
    if (!g->disabled.exchange(true)) {
        g->disabledReason = std::string(where) + ": " + what;
        RM_LOGE("mod disabled after exception in %s: %s", where, what);
    }
}

// No C++ exception may unwind into game frames (the game's handler exits the
// process). Every entry point from hooks or the overlay goes through here.
template <class R, class F>
R Guarded(const char* where, R fallback, F&& body) {
    if (g->disabled.load()) return fallback;
    try {
        return body();
    } catch (const std::exception& e) {
        DisableMod(where, e.what());
    } catch (...) {
        DisableMod(where, "unknown exception");
    }
    return fallback;
}

rm::Capabilities VerifiedCapabilities() {
    const auto& caps = engine::Caps();
    const auto& status = Status();
    rm::Capabilities out;
    if (status.combat && caps.enemyOnHit && caps.characterSubHp && caps.hitAttacker) {
        out.Add(rm::cap::kOutgoingDamage);
        out.Add(rm::cap::kKillEvents);
        out.Add(rm::cap::kEnemyHp);
    }
    if (status.combat && caps.playerSubHp && caps.players) {
        out.Add(rm::cap::kIncomingDamage);
        out.Add(rm::cap::kHeal);
    }
    // M8: a hit lands while the player's current action is a hand-to-hand move or the knife
    // (re5::off::kPlayerAction): melee.
    if (status.combat && caps.players) out.Add(rm::cap::kMeleeDetect);
    if (caps.inventory) {
        out.Add(rm::cap::kInventory);
        out.Add(rm::cap::kWeaponId);  // hits and kills carry the equipped weapon's base id
    }
    // Spawning and the native-spawn gate (E1/E2); deaths are seen by the damage hook and
    // by the death detector, so every director unit's death is reported.
    if (g->settings.director && caps.enemySpawn && director::Available() && status.combat) {
        out.Add(rm::cap::kDirector);
    }
    return out;
}

void SetStatus(std::string text) {
    RM_LOGI("ui: %s", text.c_str());
    g->lastStatus = std::move(text);
}

void Report(const rm::Status& status, const char* what) {
    if (status.ok()) {
        SetStatus(std::string(what) + ": ok");
    } else {
        SetStatus(std::string(what) + " failed: " + status.message);
    }
}

// --- game-thread logic --------------------------------------------------------------

void ObserveMatch(double now) {
    MatchState& m = g->match;
    const auto work = engine::ReadGameWork();
    const auto players = engine::ReadPlayers();
    m.mode = work ? work->mode : -1;
    m.stage = work ? work->stage : -1;
    m.timer = work ? work->mercTimer : 0.0f;
    const auto* local = players.localSlot >= 0 ? &players.players[players.localSlot] : nullptr;
    m.localClass = local ? local->className : std::string{};
    m.localCharacter = local ? engine::CharacterIdForClass(local->className) : std::string{};
    m.localHp = local ? local->hp.hp : 0;
    m.localMaxHp = local ? local->hp.max : 0;
    if (local && m.localHp <= 0 && m.localMaxHp > 0) {
        if (m.zeroHpSince < 0) m.zeroHpSince = now;
    } else {
        m.zeroHpSince = -1.0;
    }
    for (std::size_t slot = 0; slot < m.seatZeroHpSince.size(); ++slot) {
        const auto& p = players.players[slot];
        if (p.object && p.control == 0 && p.hp.hp <= 0 && p.hp.max > 0) {
            if (m.seatZeroHpSince[slot] < 0) m.seatZeroHpSince[slot] = now;
        } else {
            m.seatZeroHpSince[slot] = -1.0;
        }
    }
    const MercTimerView merc = ReadMercTimer();
    if (merc.recent) {
        m.timer = merc.countdown;
    }
    const bool wasInMatch = m.inMatch;
    // A running Mercenaries countdown (state 0) with a local player is a match.
    m.inMatch = local != nullptr && m.timer > 0.0f && std::isfinite(m.timer) && (!merc.recent || merc.state == 0);
    if (m.inMatch != wasInMatch) {
        RM_LOGI("match %s (mode=%d stage=%d timer=%.1f local=%s)", m.inMatch ? "detected" : "ended", m.mode,
                m.stage, m.timer, m.localClass.c_str());
        if (m.inMatch) {
            ++g->matchGeneration;
            ClearMercMatchEnd();  // never inherit an end request from a previous match
            g->arenaEndRequested = false;
        }
    }
}

// --- native mod menu content -------------------------------------------------

std::string Plain(const std::string& s);
// CONTINUE RUN (defined with the resume point's adapter state).
bool ContinueRun();
bool NativeItemBoxReady();
// ---- ORGANIZE: the inventory grid ------------------------------------------------------
// The game's item grid has nine cells (record +0x18 = 0..8); the in-match inventory and the
// d-pad shortcuts read the cell, so swapping the cells of two records rearranges the grid.
constexpr int kGridCells = 9;
constexpr std::size_t kSlotCell = 0x18;

struct GridCell {
    std::uintptr_t record = 0;
    std::uint32_t id = 0;
    int count = 0;
};

// The grenade launcher is one item per round type loaded (0x0125 explosive, 0x0126, 0x0127,
// 0x0139..0x013B; the Organize table), and the game files the gun as a 0x010C record: the build's
// 0x0125 came back as "010C x12" in the stage change's snapshot (user playtest 2026-11-08, classic
// Jill). A run loads only 0x0125's model archive, not one for 0x010C, so the next stage could not
// make "010C" and left the gun in the storage. Records are compared by their record id and the gun
// is made again as 0x0125.
constexpr std::uint32_t kItemGrenadeLauncher = 0x010C;
constexpr std::uint32_t kItemLauncherExplosive = 0x0125;
bool IsLauncherRoundsItem(std::uint32_t id) { return (id >= 0x0125 && id <= 0x0127) || (id >= 0x0139 && id <= 0x013B); }
// The id an inventory record of this item carries.
std::uint32_t RecordItemId(std::uint32_t id) { return IsLauncherRoundsItem(id) ? kItemGrenadeLauncher : id; }
// The item to create for a record id.
std::uint32_t MakeItemId(std::uint32_t id) { return id == kItemGrenadeLauncher ? kItemLauncherExplosive : id; }
// A slot record's item, as RecordItemId.
std::uint32_t SlotItemId(std::uintptr_t slot) { return RecordItemId(ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0)); }

std::array<GridCell, kGridCells> ReadGrid() {
    std::array<GridCell, kGridCells> grid{};
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return grid;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const int cell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
        if (cell < 0 || cell >= kGridCells) continue;
        grid[static_cast<std::size_t>(cell)] = GridCell{slot, SlotItemId(slot), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0)};
    }
    return grid;
}

std::uintptr_t ItemRecord(std::uint32_t id) {
    const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((id >> 8) & 0xFF) * 4, 0);
    return table ? table + (id & 0xFF) * 0x48 : 0;
}

// A gun's record: kind 3 (dword 12), or no stack maximum (dword 8 = -1) with a magazine (dword 2).
// The flamethrower (0x0121) is kind 0 with a "magazine" of 100 (its fuel): as an item it was made
// a stack of one, its fuel 1 and no grid cell (2026-11-07). The knife and bare hands are no guns.
bool IsWeaponRecord(std::uintptr_t record, std::uint32_t id) {
    if (!record || id == re5::addr::kItemUnarmed || id == re5::addr::kItemKnife) return false;
    if (ReadOr<std::int32_t>(record + 0x30, 0) == 3) return true;
    return (id >> 8) == 1 && ReadOr<std::int32_t>(record + 0x20, 0) == -1 && ReadOr<std::int32_t>(record + 8, 0) > 1;
}

int ItemStackMax(std::uint32_t id) {
    const auto record = ItemRecord(id);
    if (!record || IsWeaponRecord(record, id)) return 0;  // weapons do not stack
    return std::max(1, ReadOr<std::int32_t>(record + 0x20, 1));
}

// The skin build's stack sizes (M8, E13): the item records' maximum (dword 8) for the build's
// items while its run lasts; slots created from then on stack that far. Put back when the run ends.
std::map<std::uint32_t, std::int32_t> g_stackOriginal;

bool WriteRecordInt(std::uintptr_t address, std::int32_t value) {
    if (SafeStore(address, value)) return true;
    DWORD old = 0;  // the table may sit in read-only data
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_READWRITE, &old)) return false;
    const bool ok = SafeStore(address, value);
    VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), old, &old);
    RM_LOGI("item table: %08X written after lifting its page protection (%s)", static_cast<unsigned>(address), ok ? "ok" : "failed");
    return ok;
}

void ApplyBuildStacks() {
    const rm::SkinBuild& build = g->run->state().character.build;
    auto apply = [](std::uint32_t id, auto&& wanted) {
        const auto record = ItemRecord(id);
        if (!record || IsWeaponRecord(record, id)) return;
        const auto current = ReadOr<std::int32_t>(record + 0x20, 0);
        if (!g_stackOriginal.count(id)) g_stackOriginal[id] = current;
        const int max = wanted(g_stackOriginal[id]);
        if (current == max || max < 1) return;
        RM_LOGI("build: item %04X stacks to %d (was %d) -> %s", id, max, current, WriteRecordInt(record + 0x20, max) ? "ok" : "failed");
    };
    for (const auto& [item, max] : build.stack_max) {
        apply(static_cast<std::uint32_t>(item), [max = max](int) { return max; });
    }
    // Heavy Metal: every ammo stack (category 2) this many times its own size.
    if (build.ammo_stack_mult > 1.0) {
        for (std::uint32_t id = 0x0201; id <= 0x0211; ++id) {
            apply(id, [&build](int original) { return static_cast<int>(std::lround(original * build.ammo_stack_mult)); });
        }
    }
    // One item's stack times a factor (a style perk's: Sharpshooter's rifle rounds).
    for (const auto& [item, mult] : build.stack_mult) {
        const bool ammo = item >= 0x0201 && item <= 0x0211;
        const double factor = mult * (ammo ? std::max(1.0, build.ammo_stack_mult) : 1.0);
        apply(static_cast<std::uint32_t>(item), [factor](int original) { return static_cast<int>(std::lround(original * factor)); });
    }
}

void RestoreBuildStacks() {
    for (const auto& [id, original] : g_stackOriginal) {
        if (const auto record = ItemRecord(id)) WriteRecordInt(record + 0x20, original);
        RM_LOGI("build: item %04X stacks to %d again", id, original);
    }
    g_stackOriginal.clear();
}

// A run's guns keep their upgrade levels (re5_layout.h kSlotUpgrades) for the whole run. The match
// recreates the inventory at every stage with its own levels (the character's Mercenaries loadout
// guns come upgraded, other guns stock) and the Organize/Buy screen changes them, so the levels
// are recorded before a stage change and when the screen closes, and written back after the
// inventory is rebuilt.
using WeaponLevels = std::array<std::uint32_t, 2>;
struct RunWeaponLevels {
    std::string runId;
    std::map<std::uint32_t, WeaponLevels> byItem;
    std::map<std::uint32_t, int> shopSteps;  // SHOP upgrades already counted in byItem's capacity and reload
};
RunWeaponLevels g_weaponLevels;

std::map<std::uint32_t, WeaponLevels>& RunLevels() {
    if (g_weaponLevels.runId != g->run->state().run_id) {
        g_weaponLevels.runId = g->run->state().run_id;
        g_weaponLevels.byItem.clear();
        g_weaponLevels.shopSteps.clear();
    }
    return g_weaponLevels.byItem;
}

WeaponLevels SlotLevels(std::uintptr_t slot) {
    return {ReadOr<std::uint32_t>(slot + re5::off::kSlotUpgrades, 0), ReadOr<std::uint32_t>(slot + re5::off::kSlotUpgrades + 4, 0)};
}

void RecordWeaponLevels() {
    if (!g->run->HasActiveRun()) return;
    for (const GridCell& c : ReadGrid()) {
        if (c.record && ItemStackMax(c.id) == 0) RunLevels()[c.id] = SlotLevels(c.record);
    }
}

// The SHOP upgrades of a gun: its highest level in the run, less the level it starts at.
int ShopUpgradesOf(std::uint32_t id) {
    int steps = 0;
    for (const rm::WeaponInstance& w : g->run->state().weapons) {
        const rm::WeaponBaseDef* base = g->content->FindWeapon(w.base_id);
        if (base != nullptr && base->game_item >= 0 && RecordItemId(static_cast<std::uint32_t>(base->game_item)) == id) steps = std::max(steps, w.level - 1);
    }
    return steps;
}

// The game's own level tables for this item, as the record names it (the grenade launcher's is 0x010C).
rm::GunLevelTables GunTables(std::uint32_t rawItem) {
    rm::GunLevelTables tables;
    const std::size_t row = (rawItem & 0xFF) * re5::addr::kWeaponLevelStride;
    for (std::size_t level = 0; level < tables.capacity.size(); ++level) {
        tables.capacity[level] = ReadOr<std::int32_t>(re5::addr::kWeaponCapacityLevels + row + level * 4, 0);
        tables.reload[level] = ReadOr<float>(re5::addr::kWeaponReloadLevels + row + level * 4, 1.0f);
    }
    return tables;
}

// Each SHOP upgrade also moves the gun's own capacity and reload levels one step, as far as that
// gun has levels (the user, 2026-11-09). The game reads them live: rounds at the next reload.
void RaiseForShopUpgrades(const GridCell& c, WeaponLevels& levels, int steps) {
    const std::uint32_t raw = ReadOr<std::uint32_t>(c.record + re5::off::kSlotItemId, 0);
    if ((raw & 0xFF00) != 0x100 || raw >= 0x150) return;
    const rm::GunStatLevels before{static_cast<int>((levels[0] >> 24) & 0xFF), static_cast<int>((levels[0] >> 16) & 0xFF)};
    const rm::GunLevelTables tables = GunTables(raw);
    const rm::GunStatLevels after = rm::RaiseGunLevels(tables, before, steps);
    levels[0] = (levels[0] & 0x0000FFFFu) | (static_cast<std::uint32_t>(after.reload) << 16) | (static_cast<std::uint32_t>(after.capacity) << 24);
    RM_LOGI("upgrades: %04X +%d: capacity level %d -> %d (%d rounds), reload level %d -> %d (x%.2f)", c.id, steps, before.capacity, after.capacity,
            tables.capacity[static_cast<std::size_t>(std::clamp(after.capacity, 0, rm::kGunStatLevels - 1))], before.reload, after.reload,
            tables.reload[static_cast<std::size_t>(std::clamp(after.reload, 0, rm::kGunStatLevels - 1))]);
}

// A gun record's max (+0xC) is its full magazine at the capacity level it was made with (dev inv:
// the SIG 556 at level 5 max 65, the M92F at 10 max 50). The game's reload reads the level live, the
// inventory's reload stops at the max (the user, 2026-11-11: "when I reload normally it was 80, but
// when I inventory reload it went to 65"), so the max follows the level the run gave it.
void SyncGunMax(const GridCell& c) {
    const std::uint32_t raw = ReadOr<std::uint32_t>(c.record + re5::off::kSlotItemId, 0);
    if ((raw & 0xFF00) != 0x100 || raw >= 0x150 || ItemStackMax(c.id) != 0) return;
    const int level = ReadOr<std::uint8_t>(c.record + re5::off::kSlotUpgrades + 3, 0);
    if (level >= rm::kGunStatLevels) return;
    const int capacity = GunTables(raw).capacity[static_cast<std::size_t>(level)];
    const int max = ReadOr<std::int32_t>(c.record + re5::off::kSlotMax, 0);
    if (capacity <= 0 || max <= 1 || capacity == max) return;  // no table entry, or no magazine (stun rod, rocket launcher)
    SafeStore<std::int32_t>(c.record + re5::off::kSlotMax, capacity);
    RM_LOGI("upgrades: %04X magazine %d -> %d (capacity level %d)", c.id, max, capacity, level);
}

void ApplyWeaponLevels() {
    if (!g->run->HasActiveRun()) return;
    auto& levels = RunLevels();
    for (const GridCell& c : ReadGrid()) {
        if (!c.record) continue;
        auto it = levels.find(c.id);
        const bool gun = ItemStackMax(c.id) == 0;
        int& counted = g_weaponLevels.shopSteps[c.id];
        const int due = gun && engine::Caps().weaponLevels ? ShopUpgradesOf(c.id) : counted;
        if (due > counted) {
            WeaponLevels raised = it != levels.end() ? it->second : SlotLevels(c.record);
            RaiseForShopUpgrades(c, raised, due - counted);
            counted = due;
            it = levels.insert_or_assign(c.id, raised).first;
        }
        if (it != levels.end() && SlotLevels(c.record) != it->second) {
            SafeStore(c.record + re5::off::kSlotUpgrades, it->second[0]);
            SafeStore(c.record + re5::off::kSlotUpgrades + 4, it->second[1]);
            RM_LOGI("upgrades: %04X levels %08X %08X", c.id, it->second[0], it->second[1]);
        }
        if (gun && engine::Caps().weaponLevels) SyncGunMax(c);
    }
}

// A SHOP upgrade shows on the gun right away, wherever it was bought from.
void DriveWeaponLevels(double now) {
    static double nextAt = 0.0;
    if (now < nextAt) return;
    nextAt = now + 0.5;
    ApplyWeaponLevels();
}

// Every character's knife: 0x0101, Excella's 0x010E, Wesker's 0x0123, the others' 0x0124.
bool IsKnifeItem(std::uint32_t id) { return id == re5::addr::kItemKnife || id == 0x010E || id == 0x0123 || id == 0x0124; }

// The last item that went to the run's storage, for the shop's and rewards' header line.
std::string g_stashNote;
double g_stashNoteAt = -1.0e9;
// Guns that went to the storage since the player last opened ORGANIZE: their note stays up until
// then (user playtest 2026-11-08: a rocket launcher taken with a full grid went to the storage, the
// note was gone 10 s later and the gun was upgraded there, out of reach).
std::vector<std::string> g_stashGuns;
std::string g_stashGunsRun;
std::vector<std::string>& StashGuns() {
    if (g_stashGunsRun != g->run->state().run_id) {
        g_stashGunsRun = g->run->state().run_id;
        g_stashGuns.clear();
    }
    return g_stashGuns;
}
std::string StashNote() {
    const auto& guns = StashGuns();
    if (!guns.empty()) {
        return "   " + guns.back() + (guns.size() > 1 ? " +" + std::to_string(guns.size() - 1) : std::string()) + " IN STORAGE";
    }
    return Now() - g_stashNoteAt < 10.0 ? "   " + g_stashNote : std::string();
}
// ORGANIZE opened: the guns' notes have done their job.
void StashGunsSeen() { StashGuns().clear(); }

// A gun handed out now would go to the storage: no grid cell is free (GiveItem).
bool GridFull() {
    if (!engine::Caps().inventory || !engine::LocalPlayer()) return false;
    for (const GridCell& c : ReadGrid()) {
        if (!c.record) return false;
    }
    return true;
}

// The weapon (weapons.json) an item or inventory record is: by record id, so the grenade
// launcher's 0x010C record is the "grenade_launcher" of game item 0x0125.
const rm::WeaponBaseDef* WeaponForItem(std::uint32_t id) {
    for (const rm::WeaponBaseDef& w : g->content->weapons()) {
        if (w.game_item >= 0 && RecordItemId(static_cast<std::uint32_t>(w.game_item)) == RecordItemId(id)) return &w;
    }
    return nullptr;
}

// Name of an inventory item for the game font: the run's weapons and catalog first.
std::string GridItemName(std::uint32_t id) {
    if (const rm::WeaponBaseDef* w = WeaponForItem(id)) return w->name;
    for (const rm::EquipmentDef& e : g->content->loadouts().equipment) {
        if (e.game_item == static_cast<std::int64_t>(id)) return e.name;
    }
    if (IsKnifeItem(id)) return "Knife";
    switch (id) {
        case 0x010A: return "Proximity Bomb";
        case 0x010C: return "Grenade Launcher";
        case 0x010D: return "Rocket Launcher";
        case 0x010F: return "Bow";
        case 0x0202: return "Machine Gun Ammo";
        case 0x0204: return "Rifle Ammo";
        case 0x0209: return "Magnum Ammo";
        default: break;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Item %04X", id);
    return buf;
}

// M5.2: the maps a run picks its arenas from. Mercenaries stages are their stage index
// (0..stage_count-1); campaign areas of maps.json their stage number, loaded by the host in
// place of Mercenaries stage 0 ("campaign_arenas": false leaves them out).
constexpr int kFirstCampaignStage = 100;

std::vector<int> RotationMaps() {
    std::vector<int> keys;
    for (int i = 0; i < std::max(1, g->settings.stageCount); ++i) keys.push_back(i);
    if (g->settings.campaignArenas && host::Available()) {
        for (const rm::MapDef& m : g->content->maps()) {
            if (m.stage >= kFirstCampaignStage && m.rotation) keys.push_back(m.stage);
        }
    }
    return keys;
}

int g_forcedNextMap = -1;  // developer "nextmap": the next pick
std::optional<std::vector<std::string>> g_devHiddenModels;  // developer "hidemodels": models not built

// The first arena's enemy threat level (economy enemy_threat; 0 without levels).
int FirstArenaLevel() {
    const rm::EconomyConfig& eco = g->content->economy();
    const rm::RouteDef* route = g->content->FindRoute(eco.run.starting_route);
    return eco.enemy_threat.Level(1, route != nullptr ? route->difficulty_tier : 1);
}

// A random map of the rotation other than `avoid`; never a boss arena (M6: those end a run's
// map). With `level` (> 0), only maps whose mildest normal enemy is at or under it, as the run
// map draws its own (rm::GenerateRunMap); the run map takes the menu's first map as given, and
// Experimental Facility (only uEm14 with level-3 arms) opened a run at level 1 with six of them
// (user playtest 2026-11-07, Excella).
int PickMap(int avoid, int level = 0) {
    if (g_forcedNextMap >= 0) return std::exchange(g_forcedNextMap, -1);
    std::vector<int> keys = RotationMaps();
    std::erase_if(keys, [](int key) {
        const rm::MapDef* m = g->content->FindMapByStage(key);
        return m != nullptr && m->boss.has_value();
    });
    if (level > 0) {
        const rm::ThreatTable& table = g->content->economy().enemy_threat.table;
        std::vector<int> fitting;
        for (const int key : keys) {
            const rm::MapDef* m = g->content->FindMapByStage(key);
            if (m == nullptr || table.Lowest(*m) <= level) fitting.push_back(key);
        }
        if (!fitting.empty()) keys = std::move(fitting);
    }
    if (keys.size() > 1) std::erase(keys, avoid);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return keys[static_cast<std::size_t>(static_cast<std::uint64_t>(counter.QuadPart) % keys.size())];
}

int PlayerTwoCostume();

// Arms the autopilot for the next match on map `key`: a Mercenaries stage directly, a campaign
// area through Mercenaries stage 0 with the host loading its stage number instead.
void ArmMap(int key, int costume) {
    const bool campaign = key >= kFirstCampaignStage;
    g->currentStage = key;
    host::SetStageOverride(campaign ? key : -1);
    autopilot::Plan plan{campaign ? 0 : key, costume, std::max(1, g->settings.stageCount)};
    // M10: a co-op run's matches are the game's split-screen ones (the first with its lobby, where
    // player 2 joins; the later ones without it).
    const bool running = g->run->HasActiveRun();
    const bool coop = running ? g->run->SeatCount() > 1 : g->coopPlayers == 2;
    if (coop && engine::Caps().splitScreen) {
        plan.mode = 2;
        plan.costume2 = running ? g->p2Costume : PlayerTwoCostume();
        plan.skip_lobby = running && !g->coopLobbyNeeded;
    }
    autopilot::Arm(plan);
}

// The director map of the stage being played. maps.json keys Mercenaries maps by their stage
// index (0..7) and campaign areas by their stage number (100 and up), which the host loads in
// place of the chosen Mercenaries stage (M5).
int CurrentMapKey() { return host::StageOverride() >= 0 ? host::StageOverride() : g->currentStage; }
const rm::MapDef* CurrentMap() { return g->content->FindMapByStage(CurrentMapKey()); }

// The player's weapon cell (+0x2DD8) says which grid cell is in the hands: the game reads it to start
// the hands' motions on that cell's gun. The grid was rearranged without it, so after ORGANIZE or a
// stage change it could name a cell holding another gun. A special value (over 8) is left alone.
void SyncHandCell() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1 || ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1) continue;
        const int cell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
        const int known = ReadOr<std::int32_t>(player + re5::off::kPlayerWeaponCell, -1);
        if (cell < 0 || cell >= kGridCells || known < 0 || known >= kGridCells || known == cell) return;
        SafeStore<std::int32_t>(player + re5::off::kPlayerWeaponCell, cell);
        RM_LOGI("inventory: the hands' cell %d -> %d (%04X)", known + 1, cell + 1, SlotItemId(slot));
        return;
    }
}

// Moves the item in cell `from` to cell `to`, swapping with whatever is there.
bool SwapGridCells(int from, int to) {
    if (from < 0 || from >= kGridCells || to < 0 || to >= kGridCells || from == to) return false;
    const auto grid = ReadGrid();
    const GridCell& a = grid[static_cast<std::size_t>(from)];
    const GridCell& b = grid[static_cast<std::size_t>(to)];
    if (!a.record) return false;
    SafeStore<std::int32_t>(a.record + kSlotCell, to);
    if (b.record) SafeStore<std::int32_t>(b.record + kSlotCell, from);
    RM_LOGI("organize: %04X cell %d -> %d%s", a.id, from + 1, to + 1, b.record ? " (swapped)" : "");
    SyncHandCell();
    return true;
}

// The element choice (MatchElement): where it goes once every waiting gun has its element (the
// screen it was opened over: nullopt goes back to it).
std::optional<nativemenu::ScreenId> g_elementNext;

std::string Capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::uint32_t ElementColour(rm::Element e) {
    switch (e) {
        case rm::Element::Incendiary: return 0xFFFF8C3Cu;
        case rm::Element::Shock: return 0xFFFFE65Au;
        case rm::Element::Cryo: return 0xFF8CD7FFu;
        case rm::Element::Explosive: return 0xFFFF5A50u;
        case rm::Element::Toxic: return 0xFF8CF064u;
    }
    return 0;
}

// Rarity colours (0 = common: the game's own colour).
std::uint32_t RarityColour(std::string_view rarity) {
    if (rarity == "uncommon") return 0xFF7CDC7Cu;
    if (rarity == "rare") return 0xFF5CA8FFu;
    if (rarity == "epic") return 0xFFC884FFu;
    if (rarity == "legendary") return 0xFFFFB040u;
    if (rarity == "exotic") return 0xFF4CF0E6u;
    return 0;
}

// ---- Supported third-party mods (2026-11-08) ---------------------------------------------------
// A mod skin (rm::SkinDef::mod) wears a mod the player got from its creator's page and unzipped
// into <game>\RogueMercenaries\mods (any folder below it): its archives, found there by name and
// size, are read in place of the game's own while that skin is chosen or played (file_redirect),
// so the game folder is never written and the costume it stands in for keeps its own look. A mod
// skin without its mod is listed in SKINS but not selectable; pressing it opens the mod's page.
std::map<std::string, std::vector<fileredirect::Redirect>> g_modSkinFiles;  // installed mod skin -> its redirects
std::map<std::string, int> g_modPortraits;  // installed mod skin -> its picture's portrait slot
std::string g_modRedirectSkin = "-";  // the skin whose redirects are set ("" none, "-" never set)
bool g_devRedirects = false;          // dev redirect set them by hand: left alone

std::filesystem::path ModsFolder() { return g->paths.modDir / L"mods"; }

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Every mod skin whose files are all in the mods folder (by name, any case, and the size listed).
void ScanModSkins() {
    std::map<std::string, std::vector<std::filesystem::path>> byName;  // lowercase file name -> paths
    std::error_code ec;
    const std::filesystem::path root = ModsFolder();
    if (std::filesystem::is_directory(root, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) byName[LowerAscii(rm::PathText(it->path().filename()))].push_back(it->path());
        }
    }
    std::map<std::string, std::vector<fileredirect::Redirect>> found;
    rm::StringSet installed;
    std::string line;
    int modIndex = 0;
    for (const rm::CharacterDef& c : g->content->characters()) {
        for (const rm::SkinDef& s : c.skins) {
            if (!s.mod) continue;
            const int portraitSlot = portraits::kModPortraitBase + modIndex++;  // one per mod skin, in content order
            std::vector<fileredirect::Redirect> redirects;
            std::string missing;
            for (const rm::SkinModFile& f : s.mod->files) {
                static const std::vector<std::filesystem::path> kNone;
                const auto named = byName.find(LowerAscii(f.name));
                const std::vector<std::filesystem::path>& candidates = named != byName.end() ? named->second : kNone;
                std::optional<std::filesystem::path> match;
                std::vector<std::filesystem::path> ordered = candidates;  // the first in path order: a mod's variants
                std::sort(ordered.begin(), ordered.end());
                for (const auto& p : ordered) {
                    std::error_code sizeError;
                    const auto size = std::filesystem::file_size(p, sizeError);
                    if (!sizeError && f.Accepts(static_cast<std::int64_t>(size))) {
                        match = p;
                        break;
                    }
                }
                if (!match) {
                    missing += " " + f.name + (named != byName.end() ? " (another version: its size differs)" : "");
                    continue;
                }
                const std::string tail = "Image\\Archive\\" + f.name;
                redirects.push_back({std::wstring(tail.begin(), tail.end()), match->wstring()});
            }
            // The mod's own folder: the one under mods that holds its (first) costume file.
            std::filesystem::path modRoot = root;
            if (!redirects.empty()) {
                const auto rel = std::filesystem::path(redirects.front().target).lexically_relative(root);
                if (!rel.empty() && std::distance(rel.begin(), rel.end()) > 1) modRoot = root / *rel.begin();
            }
            // Its folders of new files (a costume's voice lines): found by the end of their path.
            for (const std::string& folder : s.mod->folders) {
                if (!missing.empty()) break;
                std::string want = LowerAscii("nativePC_MT\\" + folder);
                std::replace(want.begin(), want.end(), '/', '\\');
                std::optional<std::filesystem::path> dir;
                std::error_code walk;
                for (auto it = std::filesystem::recursive_directory_iterator(modRoot, std::filesystem::directory_options::skip_permission_denied, walk);
                     !walk && it != std::filesystem::recursive_directory_iterator(); it.increment(walk)) {
                    if (!it->is_directory(walk)) continue;
                    std::string have = LowerAscii(rm::PathText(it->path()));
                    std::replace(have.begin(), have.end(), '/', '\\');
                    if (have.size() >= want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0) {
                        dir = it->path();
                        break;
                    }
                }
                if (!dir) {
                    missing += " the folder " + folder + " (the mod's main files)";
                    continue;
                }
                std::string tail = LowerAscii(folder);
                std::replace(tail.begin(), tail.end(), '/', '\\');
                tail += "\\";
                redirects.push_back({std::wstring(tail.begin(), tail.end()), dir->wstring()});
            }
            if (missing.empty()) {
                // Its picture: a file of that name (or path's end) in the mod's own folder.
                if (s.mod->portrait) {
                    std::string wantTail = LowerAscii(s.mod->portrait->file);
                    std::replace(wantTail.begin(), wantTail.end(), '/', '\\');
                    const std::string fileName = wantTail.substr(wantTail.find_last_of('\\') == std::string::npos ? 0 : wantTail.find_last_of('\\') + 1);
                    const auto pictures = byName.find(fileName);
                    if (pictures != byName.end()) {
                        for (const auto& p : pictures->second) {
                            const auto inside = p.lexically_relative(modRoot);
                            if (inside.empty() || inside.native().rfind(L"..", 0) == 0) continue;
                            std::string have = LowerAscii(rm::PathText(p));
                            std::replace(have.begin(), have.end(), '/', '\\');
                            if (have.size() < wantTail.size() || have.compare(have.size() - wantTail.size(), wantTail.size(), wantTail) != 0) continue;
                            portraits::ModPortrait pic;
                            pic.file = p;
                            pic.entry = s.mod->portrait->entry;
                            if (s.mod->portrait->crop.size() == 4) {
                                for (int k = 0; k < 4; ++k) pic.crop[static_cast<std::size_t>(k)] = s.mod->portrait->crop[static_cast<std::size_t>(k)];
                            }
                            portraits::SetModPortrait(portraitSlot, std::move(pic));
                            g_modPortraits[s.id] = portraitSlot;
                            break;
                        }
                    }
                }
                found[s.id] = std::move(redirects);
                installed.insert(s.id);
            }
            line += " " + s.id + (missing.empty() ? " installed" : " missing" + missing) + ";";
            if (missing.empty() && s.mod->files.size() == 1 && found.count(s.id) && !found[s.id].empty()) {
                const std::filesystem::path chosen(found[s.id].front().target);
                line.pop_back();
                line += " (" + rm::PathText(chosen.lexically_relative(root)) + ");";
            }
        }
    }
    const bool changed = found.size() != g_modSkinFiles.size() ||
                         !std::equal(found.begin(), found.end(), g_modSkinFiles.begin(),
                                     [](const auto& a, const auto& b) { return a.first == b.first; });
    g_modSkinFiles = std::move(found);
    g->run->SetInstalledModSkins(std::move(installed));
    if (changed || g_modRedirectSkin == "-") RM_LOGI("mods: %s:%s", rm::PathText(ModsFolder()).c_str(), line.empty() ? " no mod skins" : line.c_str());
    g_modRedirectSkin = "-";  // set again on the next tick
}

// ---- M10 local co-op: each player's turn -------------------------------------------------------
// The skin build's maximum health (M8, Barry): once per match, scaled from the maximum the match
// started with; health is then full (a stage change restores the carried health after this).
// The maximum the match started with, and what the build made of it (one per match).
struct BuildHealthState {
    int generation = -1;
    int baseMax = 0;
    int appliedMax = 0;
};
BuildHealthState g_buildHealth;

// What the adapter keeps of one player (the inventory and health a stage change carries, the
// build's health). The active seat's is in the live state (g->transition, g_buildHealth); the
// other seats' wait here, swapped in by SeatTurn as the core's seats are (rm::SeatScope).
struct AdapterSeat {
    std::vector<std::pair<std::uint32_t, int>> carry;
    std::array<std::uint32_t, 9> cells{};
    std::array<int, 9> counts{};
    int hp = -1;
    BuildHealthState buildHealth;
};
std::array<AdapterSeat, 2> g_adapterSeats;
int g_adapterSeat = 0;

void SwapAdapterSeat(AdapterSeat& s) {
    using std::swap;
    swap(g->transition.carry, s.carry);
    swap(g->transition.cells, s.cells);
    swap(g->transition.counts, s.counts);
    swap(g->transition.hp, s.hp);
    swap(g_buildHealth, s.buildHealth);
}

// Each player's specials, moves, weapon card and flamethrower state (LateSeat, defined with them).
void SwapLateSeat(int index);

void ActivateAdapterSeat(int seat) {
    if (seat == g_adapterSeat || seat < 0 || seat >= static_cast<int>(g_adapterSeats.size())) return;
    SwapAdapterSeat(g_adapterSeats[static_cast<std::size_t>(g_adapterSeat)]);
    SwapLateSeat(g_adapterSeat);
    SwapAdapterSeat(g_adapterSeats[static_cast<std::size_t>(seat)]);
    SwapLateSeat(seat);
    g_adapterSeat = seat;
}

// The XInput user of a seat's pad: player 1's (-1: the keyboard), player 2's.
int PadUserOfSeat(int seat) { return seat == 0 ? (g->p1Pad >= -1 ? g->p1Pad : 0) : g->p2Pad; }

// The overlay's widgets on a player's half of the split screen (1280x720 layout lines).
constexpr float kSplitHalfLines = 360.0f;
float g_drawShiftY = 0.0f;

// One player's turn: the run's seat (the core's player-facing calls act for it), the adapter's
// per-player state, and LocalPlayer() answering that player's unit (its sPlayer slot is its seat:
// a split-screen match's player 2 is slot 1). Seat 0 changes nothing. Hold g->mutex.
class SeatTurn {
public:
    explicit SeatTurn(int seat)
        : seat_(Clamp(seat)), previous_(g_adapterSeat), scope_(*g->run, seat_), acting_(seat_ > 0 ? engine::HumanPlayerInSlot(seat_) : 0) {
        ActivateAdapterSeat(seat_);
    }
    ~SeatTurn() { ActivateAdapterSeat(previous_); }
    SeatTurn(const SeatTurn&) = delete;
    SeatTurn& operator=(const SeatTurn&) = delete;
    [[nodiscard]] int seat() const { return seat_; }
    /// The seat of a human player's unit (0 for anything else and in solo).
    static int OfUnit(std::uintptr_t unit) {
        const int slot = engine::HumanPlayerSlot(unit);
        return Clamp(slot);
    }

private:
    static int Clamp(int seat) { return seat > 0 && seat < g->run->SeatCount() ? seat : 0; }
    int seat_ = 0;
    int previous_ = 0;
    rm::SeatScope scope_;
    engine::ActingPlayerScope acting_;
};

// `fn` once per player of the run, each in its turn (once in solo; a seat whose player is not in
// the match is skipped, so player 1's unit never stands in for them).
template <class Fn>
void ForEachSeatTurn(Fn&& fn) {
    const int count = std::max(1, g->run->SeatCount());
    for (int seat = 0; seat < count; ++seat) {
        if (seat > 0 && engine::HumanPlayerInSlot(seat) == 0) continue;
        SeatTurn turn(seat);
        fn();
    }
}

// The seat whose screen the between-arena menus show: the first player still to choose a reward
// or still in the shop (player 1 otherwise).
int UiSeat(rm::RunController& run) {
    const int count = run.SeatCount();
    if (count < 2) return 0;
    const rm::Phase phase = run.phase();
    for (int seat = 0; seat < count; ++seat) {  // a gun refined to Epic: its player chooses the element
        rm::SeatScope scope(run, seat);
        if (!run.PendingElementChoices().empty()) return seat;
    }
    for (int seat = 0; seat < count; ++seat) {
        if (phase == rm::Phase::Shop) {
            const auto& status = run.state().seat_status;
            if (static_cast<std::size_t>(seat) < status.size() && !status[static_cast<std::size_t>(seat)].shop_done) return seat;
        } else if (phase == rm::Phase::RewardSelection) {
            rm::SeatScope scope(run, seat);
            if (!run.state().reward_chosen) return seat;
        }
    }
    return 0;
}

// A player who fell in the last arena starts this one with the co-op revive share of their health.
void ReviveSeat(int seat) {
    const auto unit = engine::HumanPlayerInSlot(seat);
    const auto hp = unit ? engine::ReadHp(unit) : std::nullopt;
    if (!hp || hp->max <= 0) return;
    const int wanted = std::max(1, static_cast<int>(std::lround(hp->max * g->content->economy().coop.revive_health)));
    if (wanted < hp->hp) engine::WriteHp(unit, wanted);
    RM_LOGI("co-op: player %d is back with %d/%d health", seat + 1, std::min(wanted, hp->hp), hp->max);
    g->notice = "PLAYER " + std::to_string(seat + 1) + " IS BACK";
    g->noticeUntil = Now() + 4.0;
}

// ---- M10 local co-op: player 2's choice ------------------------------------------------------
const rm::SkinAvailability* SelectedSkin(const rm::CharacterAvailability& c);
int PortraitOf(const std::string& skinId, int costume);

// Every outfit player 2 may wear: the selectable skins of the selectable characters.
struct PlayerTwoOption {
    const rm::CharacterAvailability* character = nullptr;
    const rm::SkinAvailability* skin = nullptr;
};
std::vector<PlayerTwoOption> PlayerTwoOptions(const std::vector<rm::CharacterAvailability>& characters) {
    std::vector<PlayerTwoOption> out;
    for (const auto& c : characters) {
        if (c.gate != rm::CharacterGate::Selectable) continue;
        for (const auto& s : c.skins) {
            if (s.gate == rm::CharacterGate::Selectable) out.push_back({&c, &s});
        }
    }
    return out;
}

// Player 1's skin id as the menu has it (empty: none).
std::string PlayerOneSkin(const std::vector<rm::CharacterAvailability>& characters) {
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return {};
    const rm::SkinAvailability* s = SelectedSkin(characters[static_cast<std::size_t>(g->selectedCharacter)]);
    return s != nullptr ? s->id : std::string();
}

// Two different outfits of one game costume slot cannot be worn together when one is a mod's (the
// slot is one file; RunController::AddSeat refuses them too).
bool SkinsClash(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a == b) return false;
    const rm::SkinDef* sa = nullptr;
    const rm::SkinDef* sb = nullptr;
    for (const rm::CharacterDef& c : g->content->characters()) {
        for (const rm::SkinDef& s : c.skins) {
            if (s.id == a) sa = &s;
            if (s.id == b) sb = &s;
        }
    }
    return sa != nullptr && sb != nullptr && sa->costume_index == sb->costume_index && (sa->mod || sb->mod);
}

struct PlayerTwoPick {
    std::string character;
    std::string skin;
    std::string label;  // "Sheva  BSAA"
    int costume = -1;
    int portrait = -1;
};
// Player 2's outfit: the chosen one while it can be worn, else the first outfit in another costume
// slot than player 1's.
PlayerTwoPick PlayerTwoChoice(const std::vector<rm::CharacterAvailability>& characters) {
    const auto options = PlayerTwoOptions(characters);
    const std::string one = PlayerOneSkin(characters);
    const PlayerTwoOption* pick = nullptr;
    for (const auto& o : options) {
        if (o.skin->id == g->p2Skin && !SkinsClash(one, o.skin->id)) pick = &o;
    }
    const std::string chosen = g->p2Character >= 0 && g->p2Character < static_cast<int>(characters.size())
                                   ? characters[static_cast<std::size_t>(g->p2Character)].id
                                   : std::string();
    for (std::size_t i = 0; pick == nullptr && !chosen.empty() && i < options.size(); ++i) {
        if (options[i].character->id == chosen && !SkinsClash(one, options[i].skin->id)) pick = &options[i];
    }
    const std::string oneCharacter = g->selectedCharacter >= 0 && g->selectedCharacter < static_cast<int>(characters.size())
                                         ? characters[static_cast<std::size_t>(g->selectedCharacter)].id
                                         : std::string();
    for (int pass = 0; pass < 2; ++pass) {  // another character first, then any
        for (std::size_t i = 0; pick == nullptr && i < options.size(); ++i) {
            const auto& o = options[i];
            if (pass == 0 && o.character->id == oneCharacter) continue;
            bool sameSlot = false;
            for (const auto& c : characters) {
                for (const auto& s : c.skins) sameSlot = sameSlot || (s.id == one && s.costume_index == o.skin->costume_index);
            }
            if (!sameSlot) pick = &o;
        }
    }
    PlayerTwoPick out;
    if (pick == nullptr) return out;
    out.character = pick->character->id;
    out.skin = pick->skin->id;
    out.label = pick->character->name + (pick->character->skins.size() > 1 ? "  " + pick->skin->name : std::string());
    out.costume = static_cast<int>(pick->skin->costume_index);
    out.portrait = PortraitOf(pick->skin->id, out.costume);
    return out;
}

int PlayerTwoCostume() { return PlayerTwoChoice(g->run->Characters()).costume; }

// Player 2's choice made explicit for the menu: the character of their outfit, and a skin of that
// character that does not clash with player 1's (empty when none does).
void SyncPlayerTwo(const std::vector<rm::CharacterAvailability>& characters) {
    const auto open = [&](int i) {
        return i >= 0 && i < static_cast<int>(characters.size()) && characters[static_cast<std::size_t>(i)].gate == rm::CharacterGate::Selectable;
    };
    const std::string one = PlayerOneSkin(characters);
    // An outfit they can wear names their character.
    for (std::size_t i = 0; i < characters.size(); ++i) {
        if (!open(static_cast<int>(i))) continue;
        for (const auto& s : characters[i].skins) {
            if (s.id == g->p2Skin && s.gate == rm::CharacterGate::Selectable && !SkinsClash(one, s.id)) {
                g->p2Character = static_cast<int>(i);
                return;
            }
        }
    }
    // Else their character's first outfit that fits; without a character, what they would play.
    if (!open(g->p2Character)) {
        const PlayerTwoPick pick = PlayerTwoChoice(characters);
        g->p2Character = -1;
        for (std::size_t i = 0; i < characters.size(); ++i) {
            if (characters[i].id == pick.character) g->p2Character = static_cast<int>(i);
        }
        g->p2Skin = pick.skin;
        return;
    }
    const auto& c = characters[static_cast<std::size_t>(g->p2Character)];
    g->p2Skin.clear();
    for (const auto& s : c.skins) {
        if (g->p2Skin.empty() && s.gate == rm::CharacterGate::Selectable && !SkinsClash(one, s.id)) g->p2Skin = s.id;
    }
}

// M10: CHARACTERS, SKINS and PERKS show and change player 2's choice while g->menuPlayer is 1: for
// the call, their character and skin take player 1's place in the menu's selection (so the screens'
// code serves both). g->menuOtherSkin is the other player's outfit (a clash is red there).
class MenuPlayerScope {
public:
    explicit MenuPlayerScope(nativemenu::ScreenId id) {
        using S = nativemenu::ScreenId;
        g->menuOtherSkin.clear();
        if (g->coopPlayers != 2 || (id != S::Characters && id != S::Skins && id != S::Perks && id != S::PerkList)) return;
        const auto characters = g->run->Characters();
        if (g->menuPlayer != 1) {
            g->menuOtherSkin = PlayerTwoChoice(characters).skin;
            return;
        }
        g->menuOtherSkin = PlayerOneSkin(characters);
        SyncPlayerTwo(characters);
        Swap();
        swapped_ = true;
    }
    ~MenuPlayerScope() {
        if (swapped_) Swap();
    }
    MenuPlayerScope(const MenuPlayerScope&) = delete;
    MenuPlayerScope& operator=(const MenuPlayerScope&) = delete;

private:
    static void Swap() {
        std::swap(g->selectedCharacter, g->p2Character);
        std::swap(g->selectedSkin, g->p2Skin);
    }
    bool swapped_ = false;
};

struct PlayerRow {
    std::string label;  // "PLAYER 2   LEON"
    std::string who;    // "Leon S. Kennedy, Jacket."
    int portrait = -1;
    bool none = true;
};
int SelectedPortrait(rm::RunController& run);
std::string Upper(std::string s);
PlayerRow PlayerRowOf(rm::RunController& run, int player) {
    const auto characters = run.Characters();
    PlayerRow out;
    out.label = "PLAYER " + std::to_string(player + 1) + "   NONE";
    out.who = "No character is open for this player.";
    std::string id, name, skin;
    if (player == 0) {
        if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return out;
        const auto& c = characters[static_cast<std::size_t>(g->selectedCharacter)];
        const rm::SkinAvailability* s = SelectedSkin(c);
        id = c.id;
        name = c.name;
        skin = s != nullptr && c.skins.size() > 1 ? s->name : std::string();
        out.portrait = SelectedPortrait(run);
    } else {
        const PlayerTwoPick p = PlayerTwoChoice(characters);
        if (p.skin.empty()) return out;
        for (const auto& c : characters) {
            if (c.id != p.character) continue;
            id = c.id;
            name = c.name;
            for (const auto& s : c.skins) {
                if (s.id == p.skin && c.skins.size() > 1) skin = s.name;
            }
        }
        out.portrait = p.portrait;
    }
    out.label = "PLAYER " + std::to_string(player + 1) + "   " + Upper(Plain(id));
    out.who = Plain(name) + (skin.empty() ? std::string() : ", " + Plain(skin)) + ".";
    out.none = false;
    return out;
}

// The skins worn now: each seat's in a run, else the menu's (player 2's too with PLAYERS 2).
std::vector<std::string> WornSkins() {
    rm::RunController& run = *g->run;
    std::vector<std::string> out;
    if (run.HasActiveRun()) {
        for (int seat = 0; seat < run.SeatCount(); ++seat) {
            rm::SeatScope scope(run, seat);
            out.push_back(run.state().skin_id);
        }
    } else {
        out.push_back(g->selectedSkin);
        if (g->coopPlayers == 2) {
            static std::string key, pick;
            const std::string now = g->p2Skin + "|" + g->selectedSkin + "|" + std::to_string(g->selectedCharacter) + "|" +
                                    std::to_string(g_modSkinFiles.size());
            if (now != key) {
                key = now;
                pick = PlayerTwoChoice(run.Characters()).skin;
            }
            out.push_back(pick);
        }
    }
    return out;
}

// The redirects of the skins chosen or played now (each tick; only a change touches them). Two
// players wear two skins; they never share a costume slot when one is a mod's (SkinsClash).
void UpdateModRedirects() {
    if (g_devRedirects) return;
    const std::filesystem::path effects = effectspack::Ready();  // the mod's own effects (effects_pack.h)
    std::string key;
    std::vector<fileredirect::Redirect> redirects;
    std::vector<std::string> wearing;
    for (const std::string& skin : WornSkins()) {
        const auto it = g_modSkinFiles.find(skin);
        if (it == g_modSkinFiles.end()) continue;
        key += (key.empty() ? "" : ",") + skin;
        redirects.insert(redirects.end(), it->second.begin(), it->second.end());
        wearing.push_back(skin);
    }
    key += effects.empty() ? "" : "|effects";
    if (key == g_modRedirectSkin) return;
    g_modRedirectSkin = key;
    if (!effects.empty()) {
        const std::string tail = std::string("Image\\Archive\\") + rm::fx::kEffectsArchive + ".arc";
        redirects.push_back({fileredirect::NormaliseTail(std::wstring(tail.begin(), tail.end())), effects.wstring()});
    }
    fileredirect::SetRedirects(redirects);
    for (const std::string& skin : wearing) {
        RM_LOGI("mods: %s wears its mod (%zu files read from the mods folder)", skin.c_str(), g_modSkinFiles[skin].size());
    }
}

// A mod's page in the browser (only a Nexus Mods page, as the content allows).
void OpenModPage(const rm::SkinMod& mod) {
    if (mod.url.rfind("https://www.nexusmods.com/", 0) != 0) return;
    const std::wstring url(mod.url.begin(), mod.url.end());
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    RM_LOGI("mods: opening %s (%s)", mod.url.c_str(), result > 32 ? "ok" : "failed");
}

// The offers a reward or shop screen lists, logged when they change ("rewards: ...", "shop: ...").
void LogOffersOnce(const char* what, const std::vector<rm::OfferView>& offers) {
    static std::map<std::string, std::string> last;
    std::string line;
    for (const auto& o : offers) line += " | " + o.kind + " " + o.title + " (" + o.rarity + (o.price > 0 ? ", " + std::to_string(o.price) : "") + ")";
    if (last[what] == line) return;
    last[what] = line;
    RM_LOGI("%s offers:%s", what, line.c_str());
}

// Help line for a reward or shop offer in the game font (letters, digits, space, comma, period).
std::string OfferDetail(const rm::OfferView& o) {
    if (o.kind == "upgrade" && !o.detail.empty()) return Plain(o.detail) + "\nMore rounds, faster reload, if it has levels left.";
    if (!o.detail.empty()) return Plain(o.detail);
    if (o.kind == "heal") return "Restores half of your health.";
    if (o.kind == "upgrade") return "One more damage level for this weapon.";
    if (o.kind == "weapon") return "A new weapon for the run.";
    if (o.kind == "supplies") return "Supplies to spend in the shop.";
    if (o.kind == "reroll") return "Reroll the next reward offer once.";
    if (o.kind == "refine") return "Raises a gun one rarity.";
    return Plain(o.kind);
}

std::string CategoryTag(rm::PerkCategory c) {
    switch (c) {
        case rm::PerkCategory::Style: return "STYLE - ";
        case rm::PerkCategory::Supply: return "SUPPLY - ";
        case rm::PerkCategory::Support: return "SUPPORT - ";
        default: return "";
    }
}

constexpr std::array<rm::PerkCategory, 3> kSlotCategories = {rm::PerkCategory::Style, rm::PerkCategory::Supply,
                                                             rm::PerkCategory::Support};

std::string SelectedCharacterId(rm::RunController& run) {
    const auto characters = run.Characters();
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return {};
    return characters[static_cast<std::size_t>(g->selectedCharacter)].id;
}

// Perks of one category the selected character may use (its own and the generic ones), the
// character's own ones first, then by name.
std::vector<const rm::PerkDef*> MenuPerks(rm::RunController& run, rm::PerkCategory category) {
    std::vector<const rm::PerkDef*> out;
    const rm::CharacterDef* character = run.content().FindCharacter(SelectedCharacterId(run));
    for (const auto& perk : run.content().perks()) {
        if (!perk.enabled || !run.capabilities().HasAll(perk.requires_capabilities)) continue;
        if (perk.category != category) continue;
        if (character != nullptr && !rm::CharacterMayUsePerk(perk, *character)) continue;
        out.push_back(&perk);
    }
    std::stable_sort(out.begin(), out.end(), [](const rm::PerkDef* a, const rm::PerkDef* b) {
        if (a->characters.empty() != b->characters.empty()) return !a->characters.empty();
        return a->name < b->name;
    });
    return out;
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Plural(std::int64_t n, const char* unit) {
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
}

// The game's menu font has no '%', '+', ':' or '/' glyphs; spell them out.
std::string Plain(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool digitNext = i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]));
        // A sign starts a number; inside a name ("AK-74", "PSG-1") the hyphen stays (the font has it).
        const bool wordBefore = i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]));
        if (c == '%') out += " percent";
        else if (c == '+' && digitNext) out += "plus ";
        else if (c == '-' && digitNext && !wordBefore) out += "minus ";
        else if (c == ':') out += ",";
        else if (c == '/') out += " or ";
        else if (c == '\'') continue;
        else out += c;
    }
    return out;
}

// The skin the next run wears: the chosen one if it is unlocked, else the
// character's default.
const rm::SkinAvailability* SelectedSkin(const rm::CharacterAvailability& c) {
    const rm::SkinAvailability* fallback = nullptr;
    for (const auto& s : c.skins) {
        if (s.gate != rm::CharacterGate::Selectable) continue;
        if (s.id == g->selectedSkin) return &s;
        if (fallback == nullptr) fallback = &s;
    }
    return fallback;
}

// Skin id for a run with this character (empty = the default skin).
std::string SkinForRun(const std::vector<rm::CharacterAvailability>& characters, const std::string& characterId) {
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return {};
    const auto& c = characters[g->selectedCharacter];
    if (c.id != characterId) return {};
    const rm::SkinAvailability* skin = SelectedSkin(c);
    return skin ? skin->id : std::string();
}

// A mod character (Leon) wears another character's costumes (Chris's): the match's player class
// then names that one. The character whose own skins include a costume slot.
std::string CostumeOwner(std::int64_t costume) {
    for (const rm::CharacterDef& c : g->content->characters()) {
        for (const rm::SkinDef& s : c.skins) {
            if (!s.mod && s.costume_index == costume) return c.id;
        }
    }
    return {};
}

// True when a run of `characterId` in skin `skinId` is what the match's player class shows: its own
// class, or a mod skin worn in that class's costume.
bool PlaysAs(const std::string& classCharacter, const std::string& characterId, const std::string& skinId) {
    if (classCharacter.empty() || classCharacter == characterId) return true;
    const rm::CharacterDef* def = g->content->FindCharacter(characterId);
    const rm::SkinDef* skin = def != nullptr ? rm::FindSkin(*def, skinId) : nullptr;
    return skin != nullptr && skin->mod && CostumeOwner(skin->costume_index) == classCharacter;
}

// The character a run started in this match plays: the selected one when its chosen skin is a mod
// skin worn in the match's class (Leon in Chris's costume), else the class's own.
std::string MatchRunCharacter(const std::string& classCharacter) {
    const auto characters = g->run->Characters();
    if (g->selectedCharacter >= 0 && g->selectedCharacter < static_cast<int>(characters.size())) {
        const auto& c = characters[static_cast<std::size_t>(g->selectedCharacter)];
        if (c.id != classCharacter) {
            const rm::SkinAvailability* skin = SelectedSkin(c);
            if (skin != nullptr && PlaysAs(classCharacter, c.id, skin->id)) return c.id;
        }
    }
    return classCharacter;
}

// The costume slot a character would wear: its chosen skin if it is the selected character,
// else its default.
int CostumeOf(const std::vector<rm::CharacterAvailability>& characters, std::size_t index) {
    const auto& c = characters[index];
    const rm::SkinAvailability* skin = static_cast<int>(index) == g->selectedCharacter ? SelectedSkin(c) : nullptr;
    return static_cast<int>(skin && skin->costume_index >= 0 ? skin->costume_index : c.costume_index);
}

// The picture for a skin: its mod's own when it is an installed mod skin, else its costume's bust.
int PortraitOf(const std::string& skinId, int costume) {
    const auto pic = g_modPortraits.find(skinId);
    return pic != g_modPortraits.end() ? pic->second : costume;
}

// The selected character's picture (its chosen skin's: a mod skin's own; -1 = none selected).
int SelectedPortrait(rm::RunController& run) {
    const auto characters = run.Characters();
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return -1;
    const auto index = static_cast<std::size_t>(g->selectedCharacter);
    const rm::SkinAvailability* skin = SelectedSkin(characters[index]);
    const int costume = CostumeOf(characters, index);
    return skin != nullptr ? PortraitOf(skin->id, costume) : costume;
}

// The selected character's costume slot (-1 = none selected).
int SelectedCostume(rm::RunController& run) {
    const auto characters = run.Characters();
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return -1;
    return CostumeOf(characters, static_cast<std::size_t>(g->selectedCharacter));
}

// The skin the next run wears, as content (its build), and its name; null without a selection.
const rm::SkinDef* SelectedSkinDef(rm::RunController& run, std::string* name = nullptr) {
    const auto characters = run.Characters();
    if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) return nullptr;
    const auto& c = characters[static_cast<std::size_t>(g->selectedCharacter)];
    const rm::SkinAvailability* skin = SelectedSkin(c);
    const rm::CharacterDef* def = run.content().FindCharacter(c.id);
    if (skin == nullptr || def == nullptr) return nullptr;
    if (name != nullptr) *name = skin->name;
    return rm::FindSkin(*def, skin->id);
}

// A character or skin by id: the character's name (with the skin's for a skin) and its costume
// slot. False when the id is unknown.
bool FindLook(const std::vector<rm::CharacterAvailability>& characters, const std::string& id, std::string& name,
              int& costume) {
    for (const auto& c : characters) {
        if (c.id == id) {
            name = c.name;
            costume = static_cast<int>(c.costume_index);
            return true;
        }
        for (const auto& s : c.skins) {
            if (s.id != id) continue;
            name = c.name + " " + s.name;
            costume = static_cast<int>(s.costume_index >= 0 ? s.costume_index : c.costume_index);
            return true;
        }
    }
    return false;
}

const char* WeaponIcon(rm::WeaponCategory c) {
    switch (c) {
        case rm::WeaponCategory::Shotgun: return "shotgun";
        case rm::WeaponCategory::Smg:
        case rm::WeaponCategory::MachineGun: return "smg";
        case rm::WeaponCategory::Rifle: return "rifle";
        case rm::WeaponCategory::Magnum: return "magnum";
        case rm::WeaponCategory::Bow: return "bow";
        case rm::WeaponCategory::Launcher: return "explosion";
        case rm::WeaponCategory::Melee: return "shockwave";
        case rm::WeaponCategory::Flamethrower: return "fire";
        default: return "handgun";
    }
}

// "12 min 5 sec", or "40 sec" under a minute.
std::string DurationText(double seconds) {
    const int total = std::max(0, static_cast<int>(seconds + 0.5));
    return total >= 60 ? std::to_string(total / 60) + " min " + std::to_string(total % 60) + " sec"
                       : std::to_string(total) + " sec";
}

// RUN RESULTS (M7.3): the last finished run, opened by itself when the game is back at the Play
// menu and from CREDITS.
// The ladder's boss of map `key` in a sentence (maps.json): "the Uroboros", "Wesker".
std::string LadderBossName(int key) {
    const rm::MapDef* m = key >= 0 ? g->content->FindMapByStage(key) : nullptr;
    return m && m->boss ? Plain(m->boss->Phrase()) : std::string("a boss");
}

// "4", "4 and 8", "4, 8 and 12": the boss arenas of ladder lengths.
std::string ArenaList(const std::vector<const rm::EconomyConfig::Ladder::Length*>& tiers) {
    std::string out;
    for (std::size_t k = 0; k < tiers.size(); ++k) {
        if (k > 0) out += k + 1 == tiers.size() ? " and " : ", ";
        out += std::to_string(tiers[k]->arenas);
    }
    return out;
}

std::string Ordinal(int n) {
    const int tens = n % 100, ones = n % 10;
    const char* suffix = tens >= 11 && tens <= 13 ? "th" : ones == 1 ? "st" : ones == 2 ? "nd" : ones == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

// "Beat the alternates on arenas 4 and 8 for a 13th arena": how a run earns the finale.
std::string FinaleHowTo(const rm::EconomyConfig::Ladder& ladder) {
    std::vector<const rm::EconomyConfig::Ladder::Length*> needs;
    for (const std::string& id : ladder.finale.needs_hard) {
        if (const auto* l = ladder.Find(id)) needs.push_back(l);
    }
    const auto* after = ladder.Find(ladder.finale.after);
    const std::string arena = Ordinal((after ? after->arenas : 0) + 1) + " arena";
    if (needs.empty()) return "Then a " + arena;
    return std::string(needs.size() > 1 ? "Beat the alternates on arenas " : "Beat the alternate on arena ") + ArenaList(needs) +
           " for a " + arena;
}

// RUN LENGTH (the ladder): every length, the open ones to choose; a locked one says which boss
// opens it.
// "Beat the hard boss of a 4 arena run to open it.", "Beat an 8 arena run to open it.": what opens a
// locked run length (its unlocked_by key).
std::string LadderOpenHelp(const std::string& key) {
    const rm::EconomyConfig::Ladder& ladder = g->content->economy().ladder;
    if (key == "finale") return "Win the arena after the last boss to open it.";
    const auto colon = key.find(':');
    const auto* by = colon == std::string::npos ? nullptr : ladder.Find(key.substr(0, colon));
    if (by == nullptr) return "Locked.";
    const std::string n = std::to_string(by->arenas);
    const std::string a = n.front() == '8' || n == "11" || n == "18" ? "an " : "a ";
    return key.substr(colon + 1) == "hard" ? "Beat the hard boss of " + a + n + " arena run to open it."
                                           : "Beat " + a + n + " arena run to open it.";
}

nativemenu::Screen RunLengthScreen(rm::RunController& run) {
    nativemenu::Screen screen;
    screen.header = "RUN LENGTH";
    const rm::EconomyConfig::Ladder& ladder = g->content->economy().ladder;
    for (const auto& v : run.LadderLengths()) {
        const auto* def = ladder.Find(v.id);
        nativemenu::Item item;
        item.label = v.endless ? Upper(Plain(v.name)) : std::to_string(v.arenas) + " ARENAS   " + Upper(Plain(v.name));
        item.icon = "flag";
        if (v.endless) {
            // Endless (the user, 2026-11-19): no last arena; the bosses of the lengths open, in turn.
            item.locked = !v.open;
            // Two lines of at most 44 (the box shows two; a longer line wraps and pushes the second out).
            item.help = v.open ? "No last arena, it ends when you fall.\nA boss every " + std::to_string(v.boss_every) +
                                     " arenas, stronger each round."
                               : LadderOpenHelp(v.unlocked_by);
        } else if (!v.open || def == nullptr) {
            item.locked = true;
            item.help = def != nullptr ? LadderOpenHelp(def->unlocked_by) : "Locked.";
        } else {
            // "Ends on the Uroboros", "... or U-8"; a longer run fights the shorter
            // lengths' bosses on the way: "Bosses on arenas 4 and 8, the last Uroboros Mkono,
            // Uroboros Aheri or Jill."
            std::vector<int> ends{def->easy_boss};
            if (v.hard_open) ends.insert(ends.end(), def->hard_bosses.begin(), def->hard_bosses.end());
            const auto tiers = ladder.BossTiers(def->id);
            if (tiers.size() > 1) {
                item.help = "Bosses on arenas " + ArenaList(tiers) + ", the last ";
            } else {
                item.help = "Ends on ";
            }
            for (std::size_t k = 0; k < ends.size(); ++k) {
                if (k > 0) item.help += k + 1 == ends.size() ? " or " : ", ";
                item.help += LadderBossName(ends[k]);
            }
            item.help += ".";
            if (v.finale_possible) item.help += "\n" + FinaleHowTo(ladder) + ".";
            else if (!v.hard_open && !def->hard_bosses.empty()) item.help += "\nBeat it to face a harder boss next time.";
        }
        screen.items.push_back(std::move(item));
    }
    return screen;
}

// START RUN: the first arena's map armed for the selected character (the game's flow then starts
// its match and the run with it, HandleNativeMenu).
void ArmFirstArena(rm::RunController& run) {
    if (!g->settings.directStart || !autopilot::Available() || autopilot::Armed()) return;
    const auto characters = run.Characters();
    std::int64_t costume = 0;
    if (g->selectedCharacter >= 0 && g->selectedCharacter < static_cast<int>(characters.size())) {
        const auto& c = characters[g->selectedCharacter];
        const rm::SkinAvailability* skin = SelectedSkin(c);
        costume = std::max<std::int64_t>(skin && skin->costume_index >= 0 ? skin->costume_index : c.costume_index, 0);
    }
    ArmMap(PickMap(-1, FirstArenaLevel()), static_cast<int>(costume));
}

nativemenu::Screen RunResultsScreen(rm::RunController& run) {
    nativemenu::Screen screen;
    const auto& last = run.last_summary();
    if (!last) {
        screen.header = "RUN RESULTS";
        screen.empty_help = "No run has ended since the game started.";
        return screen;
    }
    const rm::RunSummary& s = *last;
    const auto characters = run.Characters();
    std::string character, look;
    int costume = -1;
    FindLook(characters, s.character_id, character, costume);
    if (!s.skin_id.empty()) FindLook(characters, s.skin_id, look, costume);
    screen.header = std::string(s.completed ? "RUN COMPLETE" : "RUN OVER") + (character.empty() ? "" : "   " + Upper(character));
    screen.portrait = s.skin_id.empty() ? costume : PortraitOf(s.skin_id, costume);
    auto add = [&](std::string label, std::string help, const char* icon) {
        nativemenu::Item item;
        item.label = std::move(label);
        item.help = std::move(help);
        item.icon = icon;
        screen.items.push_back(std::move(item));
        return &screen.items.back();
    };
    const int reached = s.arenas_cleared + 1;  // the arena a failed run ended in
    if (s.completed) {
        const bool boss = !s.arenas.empty() && s.arenas.back().boss;
        add("ALL " + std::to_string(s.arenas_cleared) + " ARENAS CLEARED",
            boss && !s.arenas.back().arena_name.empty() ? "The boss of " + Plain(s.arenas.back().arena_name) + " is dead."
                                                        : "Every arena of the run cleared.",
            "trophy");
    } else {
        add(s.endless ? Upper(Plural(s.arenas_cleared, "arena")) + " CLEARED"
                      : std::to_string(s.arenas_cleared) + " OF " + std::to_string(s.arena_count) + " ARENAS CLEARED",
            std::string(s.end_reason == "died" ? "Killed" : "Ended") + " in arena " + std::to_string(reached) +
                (s.final_arena.empty() ? "." : ", " + Plain(s.final_arena) + "."),
            "last_stand");
    }
    // What the run's boss wins opened on the ladder: right under the first row, seen without scrolling.
    for (const rm::LadderUnlock& u : s.ladder_unlocks) {
        const std::string boss = LadderBossName(u.boss);  // in a sentence: "the Uroboros", "U-8"
        const rm::MapDef* map = g->content->FindMapByStage(u.boss);
        const std::string name = map && map->boss ? Upper(Plain(map->boss->name)) : std::string("A BOSS");
        const std::string arenas = std::to_string(u.arenas);
        switch (u.kind) {
            case rm::LadderUnlock::Kind::HardBoss:
                add("NEW BOSS   " + name, "Arena " + arenas + " of a run can now be " + boss + ".\nChoose it in the NEXT ROUTE before it.",
                    "explosion");
                break;
            case rm::LadderUnlock::Kind::Length: {
                if (g->content->economy().ladder.endless.Is(u.length_id)) {
                    add("ENDLESS OPEN", "Arenas until you fall, a boss every " +
                                            std::to_string(g->content->economy().ladder.endless.boss_every) +
                                            ".\nPick ENDLESS when START RUN asks how long.",
                        "flag");
                    break;
                }
                const auto tiers = g->content->economy().ladder.BossTiers(u.length_id);
                add(arenas + " ARENAS OPEN",
                    (tiers.size() > 1 ? arenas + " arenas, bosses on " + ArenaList(tiers) + ", the last " + boss
                                      : arenas + " arenas, ending on " + boss) +
                        ".\nSTART RUN now asks how long the run is.",
                    "flag");
                break;
            }
            case rm::LadderUnlock::Kind::Finale: {
                // "Beat the alternates on arenas 4 and 8 of a 12-arena run / for a 13th arena, against Wesker."
                const rm::EconomyConfig::Ladder& ladder = g->content->economy().ladder;
                std::vector<const rm::EconomyConfig::Ladder::Length*> needs;
                for (const std::string& id : ladder.finale.needs_hard) {
                    if (const auto* l = ladder.Find(id)) needs.push_back(l);
                }
                const std::string beat = needs.empty() ? "Beat its boss"
                                         : needs.size() > 1 ? "Beat the alternates on arenas " + ArenaList(needs)
                                                            : "Beat the alternate on arena " + ArenaList(needs);
                add("ONE MORE ARENA",
                    beat + " of a " + arenas + "-arena run\nfor a " + Ordinal(u.arenas + 1) + " arena, against " + boss + ".", "explosion");
                break;
            }
            case rm::LadderUnlock::Kind::Exotic:
                add("EXOTIC GUNS", "A legendary gun can now be refined once more, to exotic:\ntwo elements and two legendary effects.",
                    "trophy");
                break;
        }
    }
    add(Upper(Plural(s.total_kills, "kill")), "Enemies killed in this run.", "crosshair");
    add("TIME " + Upper(DurationText(s.time_seconds)), "Time spent fighting in the run's arenas.", "clock");
    add(std::to_string(s.supplies_earned) + " SUPPLIES EARNED", std::to_string(s.supplies_spent) + " spent in the shop.",
        "supplies");
    const std::string balance = "\nBalance " + Plural(run.profile().credits.balance(), "credit") + ".";
    add(Upper(Plural(s.credits_banked, "credit")) + " BANKED",
        (s.completed                ? std::string("Earned in the run, with the bonus for finishing it.")
         : s.credits_pending > 0 ? "A death keeps part of the " + Plural(s.credits_pending, "credit") + " earned."
                                 : std::string("This run earned no credits.")) +
            balance,
        "credits");
    if (!s.already_banked && s.arenas_cleared > s.previous_best_arena) {
        add("NEW BEST RUN", s.previous_best_arena > 0 ? "Your record was " + Plural(s.previous_best_arena, "arena") + "."
                                                      : "Your first record.",
            "medal");
    }
    for (const std::string& id : s.unlocked) {
        std::string name;
        int unlockedCostume = -1;
        if (!FindLook(characters, id, name, unlockedCostume)) continue;
        nativemenu::Item* item = add("UNLOCKED   " + Upper(name), "This run met its challenge.\nChoose it in CHARACTERS.", "person");
        item->portrait = unlockedCostume;
    }
    for (const rm::ArenaResult& a : s.arenas) {
        add("ARENA " + std::to_string(a.arena) + (a.arena_name.empty() ? "" : "   " + Upper(Plain(a.arena_name))),
            std::string(a.boss ? "Boss arena. " : "") + "Rating " + std::string(rm::RatingName(a.rating)) + ", " +
                DurationText(a.time_seconds) + ", " + Plural(a.kills, "kill") + (a.no_damage ? ", no damage taken." : "."),
            a.boss ? "explosion" : "flag");
    }
    if (!s.completed && reached <= s.arena_count) {
        nativemenu::Item* item = add("ARENA " + std::to_string(reached) + (s.final_arena.empty() ? "" : "   " + Upper(Plain(s.final_arena))),
                                     s.end_reason == "died" ? "You fell here." : "The run ended here.", "last_stand");
        item->locked = true;
    }
    return screen;
}

// ---- The mod's ORGANIZE screen (the grid; with two players the run's storage and selling too) ----
// Its rows: the nine cells, the storage's records, then with an item picked TO THE STORAGE (a cell's)
// and SELL. The storage is the run's (both players'). Defined with the storage, below.
struct OrganizeRow {
    enum Kind { Cell, Stash, ToStorage, Sell } kind = Cell;
    int index = 0;
};
constexpr int kStashPick = 100;  // g->organizePick for a storage record: kStashPick + its index
std::vector<std::pair<std::uint32_t, int>> StashRecords();
std::vector<OrganizeRow> OrganizeRows(int pick, int stashCount);
bool TakeFromStash(int index, std::string& why);
bool PutCellInStash(int cell, std::string& why);
std::int64_t SellCell(int cell);
std::int64_t SellStash(int index);

// The mod menu's first screen, row by row (Build and Decide walk the same list).
enum class MainRow { Continue, Start, Players, PlayerOne, PlayerTwo, Characters, Perks, Credits };
std::vector<MainRow> MainRows(rm::RunController& run) {
    std::vector<MainRow> rows;
    if (run.CanResumeRun()) rows.push_back(MainRow::Continue);
    rows.push_back(MainRow::Start);
    const bool two = engine::Caps().splitScreen && g->coopPlayers == 2;
    if (engine::Caps().splitScreen) rows.push_back(MainRow::Players);
    if (two) {
        rows.push_back(MainRow::PlayerOne);
        rows.push_back(MainRow::PlayerTwo);
    } else {
        rows.push_back(MainRow::Characters);
    }
    rows.push_back(MainRow::Perks);
    rows.push_back(MainRow::Credits);
    return rows;
}

// Screens of the native menu, built from the run controller. Called on the
// game's menu thread, so everything happens under the mod mutex.
class MenuModel final : public nativemenu::Model {
public:
    nativemenu::Screen Build(nativemenu::ScreenId id) override {
        std::lock_guard lock(g->mutex);
        return Guarded("menu build", nativemenu::Screen{}, [&] { return BuildTurn(id); });
    }

    nativemenu::Decision Decide(nativemenu::ScreenId id, int index) override {
        std::lock_guard lock(g->mutex);
        return Guarded("menu decide", nativemenu::Decision{}, [&] { return DecideTurn(id, index); });
    }

private:
    using Item = nativemenu::Item;
    using Outcome = nativemenu::Outcome;
    using ScreenId = nativemenu::ScreenId;

    std::string CreditsLine() const { return Plural(g->run->profile().credits.balance(), "credit"); }

    // M10: the between-arena screens show and act for the player whose turn it is (UiSeat).
    nativemenu::Screen BuildTurn(ScreenId id) {
        const int seat = UiSeat(*g->run);
        SeatTurn turn(seat);
        MenuPlayerScope menuPlayer(id);
        nativemenu::Screen screen = BuildLocked(id);
        if (g->coopPlayers == 2 && (id == ScreenId::Characters || id == ScreenId::Skins || id == ScreenId::Perks || id == ScreenId::PerkList)) {
            screen.header = "PLAYER " + std::to_string(g->menuPlayer + 1) + "   " + screen.header;
        }
        if (turn.seat() > 0 && (id == ScreenId::MatchRewards || id == ScreenId::MatchShop || id == ScreenId::MatchOrganize ||
                                id == ScreenId::MatchElement)) {
            screen.header = "PLAYER " + std::to_string(turn.seat() + 1) + "   " + screen.header;
        }
        return screen;
    }

    nativemenu::Decision DecideTurn(ScreenId id, int index) {
        SeatTurn turn(UiSeat(*g->run));
        MenuPlayerScope menuPlayer(id);
        const nativemenu::Decision d = DecideLocked(id, index);
        if (g->menuPlayer == 1 && id == ScreenId::Skins && d.outcome == Outcome::Refresh) {
            RM_LOGI("menu: player 2 wears %s", g->selectedSkin.c_str());  // still swapped: player 2's
        }
        return d;
    }

    nativemenu::Screen BuildLocked(ScreenId id) {
        rm::RunController& run = *g->run;
        const rm::Profile& profile = run.profile();
        nativemenu::Screen screen;
        switch (id) {
            case ScreenId::Main: {
                screen.header = "ROGUE MERCENARIES   " + Upper(CreditsLine());
                screen.portrait = SelectedPortrait(run);
                const bool two = g->coopPlayers == 2;
                for (const MainRow row : MainRows(run)) {
                    switch (row) {
                        case MainRow::Continue: {
                            const rm::ActiveRunCheckpoint& cp = *run.interrupted_run();
                            const rm::CharacterDef* who = g->content->FindCharacter(cp.character_id);
                            // M10: a two-player run names player 2's character too (its resume point's seats).
                            std::string second;
                            const nlohmann::json resume = nlohmann::json::parse(cp.resume, nullptr, false);
                            if (const auto seats = resume.is_object() ? resume.find("seats") : resume.end();
                                resume.is_object() && seats != resume.end() && seats->is_array() && !seats->empty() && seats->front().is_object()) {
                                const std::string p2Id = seats->front().value("character_id", std::string());
                                if (const rm::CharacterDef* p2 = g->content->FindCharacter(p2Id)) second = Plain(p2->name);
                            }
                            screen.items.push_back(Item{"CONTINUE RUN",
                                                        (who ? Plain(who->name) : std::string("Your run")) +
                                                            (second.empty() ? std::string() : " and " + second) + ", " +
                                                            Plural(cp.arenas_cleared, "arena") + " cleared.\n" +
                                                            (second.empty() ? "The arena you left starts again." : "After the lobby the arena you left starts again."),
                                                        false, 0, "flag"});
                            break;
                        }
                        case MainRow::Start:
                            screen.items.push_back(Item{"START RUN",
                                                        run.CanResumeRun() ? "A new run. The one you can continue\nis banked as interrupted."
                                                        : two ? "Two players, the screen split in two. Player 2 joins\nwith START on a second controller."
                                                              : "Solo Mercenaries on any stage. Pick your character,\nthe run starts with the match.",
                                                        false, 0, "crosshair"});
                            break;
                        case MainRow::Players:
                            screen.items.push_back(Item{two ? "PLAYERS   2, SPLIT SCREEN" : "PLAYERS   1",
                                                        "Press to switch between one player and two\n(split screen, a controller for player 2).",
                                                        false, 0, "person"});
                            break;
                        case MainRow::PlayerOne:
                        case MainRow::PlayerTwo: {
                            const PlayerRow p = PlayerRowOf(run, row == MainRow::PlayerTwo ? 1 : 0);
                            Item item{p.label, p.who + "\nPlays on the " + (row == MainRow::PlayerTwo ? "bottom" : "top") +
                                                   " half. Press to choose.",
                                      p.none, 0, "person"};
                            item.portrait = p.portrait;
                            screen.items.push_back(std::move(item));
                            break;
                        }
                        case MainRow::Characters:
                            screen.items.push_back(Item{"CHARACTERS", "Choose who you play. Locked characters show\nhow to unlock them.",
                                                        false, 0, "person"});
                            break;
                        case MainRow::Perks:
                            screen.items.push_back(Item{"PERKS", "Permanent perks. Locked ones are bought with credits.", false, 0, "medal"});
                            break;
                        case MainRow::Credits:
                            screen.items.push_back(Item{"CREDITS", "Your credits, lifetime records and weapon unlocks.", false, 0, "credits"});
                            break;
                    }
                }
                break;
            }
            case ScreenId::WhichPlayer: {
                screen.header = "PERKS   " + Upper(CreditsLine());
                for (int player = 0; player < 2; ++player) {
                    const PlayerRow p = PlayerRowOf(run, player);
                    Item item{p.label, p.who + "\nPress to see the perks of player " + std::to_string(player + 1) + ".", p.none, 0, "medal"};
                    item.portrait = p.portrait;
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::RunResults:
                screen = RunResultsScreen(run);
                break;
            case ScreenId::RunLength:
                screen = RunLengthScreen(run);
                break;
            case ScreenId::Characters: {
                screen.header = "CHARACTERS";
                const auto characters = run.Characters();
                for (std::size_t i = 0; i < characters.size(); ++i) {
                    const auto& c = characters[i];
                    Item item;
                    item.label = Upper(c.name);
                    item.portrait = CostumeOf(characters, i);
                    // A mod character (Leon): its mod's picture; without its mod, where to get it.
                    const rm::CharacterDef* def = run.content().FindCharacter(c.id);
                    const rm::SkinDef* firstMod = nullptr;
                    if (def != nullptr) {
                        for (const rm::SkinDef& s : def->skins) {
                            if (s.mod && firstMod == nullptr) firstMod = &s;
                        }
                    }
                    if (firstMod != nullptr) {
                        const rm::SkinAvailability* worn = static_cast<int>(i) == g->selectedCharacter ? SelectedSkin(c) : nullptr;
                        for (const auto& s : c.skins) {
                            if (worn == nullptr && s.gate == rm::CharacterGate::Selectable) worn = &s;
                        }
                        const auto pic = worn != nullptr ? g_modPortraits.find(worn->id) : g_modPortraits.end();
                        if (pic != g_modPortraits.end()) item.portrait = pic->second;
                    }
                    if (c.gate == rm::CharacterGate::NotInstalled && firstMod != nullptr) {
                        item.locked = true;
                        item.label += "   MOD";
                        const rm::SkinMod& mod = *firstMod->mod;
                        item.help = (mod.author.empty() ? std::string("A free mod on Nexus Mods.") : "A free mod by " + Plain(mod.author) + ".") +
                                    " Press to open its page,\nendorse it, and unzip it in RogueMercenaries, mods.";
                    } else if (c.gate == rm::CharacterGate::Selectable) {
                        const bool selected = static_cast<int>(i) == g->selectedCharacter;
                        if (selected) item.label += "   SELECTED";
                        item.help = selected ? "Your character for the next run." : "Press to select this character.";
                    } else if (c.gate == rm::CharacterGate::Locked) {
                        item.locked = true;
                        item.help = c.unlock_text.empty() ? "Locked." : c.unlock_text + ".";
                        if (c.challenge.target > 0) {
                            item.help += "\n" + std::to_string(c.challenge.current) + " of " +
                                         std::to_string(c.challenge.target) + " done.";
                        } else if (c.unlock_cost_credits > 0) {
                            item.help += "\nUnlock for " + Plural(c.unlock_cost_credits, "credit") + ".";
                        }
                    } else {
                        item.locked = true;
                        item.help = "Not available in this version.";
                    }
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::Perks: {
                // One slot per category; each row opens that category's list.
                screen.header = "PERKS   " + Upper(CreditsLine());
                screen.portrait = SelectedPortrait(run);
                const std::string character = SelectedCharacterId(run);
                for (rm::PerkCategory c : kSlotCategories) {
                    Item item;
                    const rm::PerkDef* equipped = character.empty() ? nullptr : run.EquippedPerk(character, c);
                    item.label = CategoryTag(c) + (equipped ? Upper(Plain(equipped->name)) : std::string("NONE"));
                    if (equipped) item.icon = equipped->icon;
                    item.help = equipped ? Plain(equipped->description) + "\nPress to change it."
                                         : "Nothing chosen for this slot.\nPress to choose.";
                    screen.items.push_back(std::move(item));
                }
                // M8: the skin's build is the fourth slot; the row opens SKINS.
                std::string skinName;
                if (const rm::SkinDef* skin = SelectedSkinDef(run, &skinName)) {
                    Item item;
                    const rm::SkinBuild& build = skin->build;
                    item.label = "SKIN - " + Upper(Plain(build.name.empty() ? skinName : build.name));
                    item.icon = build.icon;
                    item.help = build.summary.empty() ? Upper(skinName) + " has no build.\nPress to change skins."
                                                      : Plain(build.summary) + "\nPress to change skins.";
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::PerkList: {
                const std::string character = SelectedCharacterId(run);
                screen.header = Upper(std::string(rm::PerkCategoryName(g->perkCategory))) + "   " + Upper(CreditsLine());
                screen.empty_help = "No perk of this kind for this character yet.";
                screen.portrait = SelectedPortrait(run);
                const rm::PerkDef* equipped = character.empty() ? nullptr : run.EquippedPerk(character, g->perkCategory);
                for (const rm::PerkDef* perk : MenuPerks(run, g->perkCategory)) {
                    Item item;
                    item.label = Upper(Plain(perk->name));
                    const bool locked = perk->unlock_cost_credits > 0 && !profile.unlocked_perks.count(perk->id);
                    const bool selected = equipped != nullptr && equipped->id == perk->id;
                    if (selected) item.label += "   SELECTED";
                    // The price on the row itself: the help holds two lines, and a long description
                    // pushed it out (the user, 2026-11-09).
                    if (locked) item.label += "   " + Upper(Plural(perk->unlock_cost_credits, "credit"));
                    item.locked = locked;
                    item.color = RarityColour(rm::RarityName(perk->rarity));
                    item.icon = perk->icon;
                    item.help = Plain(perk->description);
                    if (locked) item.help += "\nUnlock for " + Plural(perk->unlock_cost_credits, "credit") + ".";
                    else if (selected) item.help += "\nYour pick for this slot. Press to remove it.";
                    else item.help += "\nPress to pick it for this slot.";
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::MatchResults: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "ARENA " + std::to_string(s.arena) + " CLEARED";
                Item item;
                item.label = "CONTINUE";
                if (s.last_result) {
                    const auto& r = *s.last_result;
                    item.help = "Rating " + std::string(rm::RatingName(r.rating)) + ", " +
                                std::to_string(static_cast<int>(r.time_seconds)) + " seconds, " + Plural(r.kills, "kill") +
                                (r.no_damage ? ", no damage taken." : ".") + "\nSupplies " + std::to_string(r.supplies_awarded) +
                                ", credits " + std::to_string(r.credits_accrued) + ".";
                } else {
                    item.help = "Press to see your rewards.";
                }
                screen.items.push_back(std::move(item));
                break;
            }
            case ScreenId::MatchElement: {
                // A gun refined to Epic: two elements and a random one (the user, 2026-11-11).
                const auto pending = run.PendingElementChoices();
                screen.empty_help = "Nothing to choose. Press to continue.";
                if (pending.empty()) {
                    screen.header = "ELEMENT";
                    break;
                }
                const rm::ElementChoice& c = pending.front();
                const rm::WeaponBaseDef* gun = g->content->FindWeapon(c.base_id);
                screen.header = "CHOOSE AN ELEMENT   " + Upper(Plain(gun != nullptr ? gun->name : c.base_id));
                for (const rm::Element e : c.offer) {
                    const rm::ElementDef* d = g->content->FindElement(e);
                    const rm::ElementDef* ev = g->content->FindElement(e, true);
                    if (d == nullptr) continue;
                    Item item;
                    const bool evolves = ev != nullptr && ev != d;
                    if (c.evolved && evolves) {
                        item.label = Upper(Plain(ev->name));
                        item.help = Upper(Plain(d->name)) + " evolved. " + Plain(ev->description) + ".";
                    } else {
                        item.label = Upper(Plain(d->name)) + (evolves ? "   " + Upper(Plain(ev->name)) + " AT LEGENDARY" : std::string());
                        item.help = Capitalised(Plain(d->description)) + ".";
                    }
                    item.color = ElementColour(e);
                    screen.items.push_back(std::move(item));
                }
                Item random;
                random.label = "RANDOM ELEMENT";
                random.help = "Any of the five, each as likely: the rare ones too.\nWhat it draws is final.";
                screen.items.push_back(std::move(random));
                break;
            }
            case ScreenId::MatchRewards: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "CHOOSE A REWARD" + StashNote();
                screen.empty_help = "Nothing to choose. Press to continue.";
                LogOffersOnce("rewards", s.reward_offers);
                const bool gridFull = GridFull();
                for (const auto& o : s.reward_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title));
                    item.help = OfferDetail(o);
                    item.color = RarityColour(o.rarity);
                    if (o.kind == "weapon" && gridFull) {
                        // No grid cell: the gun goes to the storage (KeepInRunStash).
                        item.label += "   TO STORAGE";
                        item.help += "\nYour grid is full: it goes to the storage (ORGANIZE).";
                    }
                    screen.items.push_back(std::move(item));
                }
                if (s.reroll_tokens > 0) {
                    Item item;
                    item.label = "REROLL";
                    item.help = "Draw three other rewards. " + Plural(s.reroll_tokens, "token") + " left.";
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::MatchShop: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "SHOP   " + std::to_string(s.supplies) + " SUPPLIES" + StashNote();
                LogOffersOnce("shop", s.shop_offers);
                const bool gridFull = GridFull();
                for (const auto& o : s.shop_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title)) + (o.purchased ? "   BOUGHT" : "");
                    item.help = OfferDetail(o) + "\nPrice " + std::to_string(o.price) + " supplies.";
                    item.color = RarityColour(o.rarity);
                    if (o.kind == "weapon" && !o.purchased && gridFull) {
                        item.label += "   TO STORAGE";
                        item.help += " Grid full: it goes to the storage.";
                    }
                    if (!o.purchased && o.price > s.supplies) item.locked = true;
                    if (o.restricted) {  // M8: the skin's build cannot use this gun
                        item.locked = true;
                        item.help = OfferDetail(o) + "\n" + Plain(run.state().character.name) + " cannot use this weapon.";
                    }
                    screen.items.push_back(std::move(item));
                }
                if (s.restock_price >= 0) {
                    // NEW STOCK: every offer drawn again (a weapon upgrade among them); the skin
                    // build's free ones first, then for supplies, dearer each time.
                    Item reroll;
                    if (s.restock_price == 0) {
                        reroll.label = "NEW STOCK   FREE";
                        reroll.help = "Replaces the offers above, a weapon upgrade among them.\n" +
                                      Plural(s.free_shop_rerolls, "free restock") + " left at this shop.";
                    } else {
                        reroll.label = "NEW STOCK   " + std::to_string(s.restock_price) + " SUPPLIES";
                        reroll.help = "Replaces the offers above, a weapon upgrade among them.\nDearer each time at this shop.";
                        reroll.locked = s.restock_price > s.supplies;
                    }
                    reroll.icon = "supplies";
                    screen.items.push_back(std::move(reroll));
                }
                Item organize;
                organize.label = "ORGANIZE INVENTORY";
                const bool native = NativeItemBoxReady() && run.SeatCount() < 2;  // co-op: the mod's grid (MatchOrganize)
                organize.help = native ? "Arrange, combine and sell items in the game screen.\nA sale pays half the SHOP price. Buy and upgrade here."
                                       : "Swap the cells of your item grid, use the run's storage,\nor sell an item for half its SHOP price.";
                if (const auto& guns = StashGuns(); !guns.empty()) {
                    organize.label += "   " + guns.back() + (guns.size() > 1 ? " +" + std::to_string(guns.size() - 1) : std::string()) +
                                      " IN STORAGE";
                    organize.color = 0xFFFFD27Fu;
                    organize.help = "Your storage holds a gun your grid had no room for.\nSwap it into the grid here.";
                }
                screen.items.push_back(std::move(organize));
                Item leave;
                leave.label = "LEAVE SHOP";
                leave.help = "On to the next route.";
                screen.items.push_back(std::move(leave));
                break;
            }
            case ScreenId::MatchOrganize: {
                const auto grid = ReadGrid();
                const auto stash = StashRecords();
                const int pick = g->organizePick;
                const bool picked = pick >= 0 && pick < kGridCells && grid[static_cast<std::size_t>(pick)].record;
                const bool pickedStash = pick >= kStashPick && pick - kStashPick < static_cast<int>(stash.size());
                const std::uint32_t pickedId = picked ? grid[static_cast<std::size_t>(pick)].id
                                                      : pickedStash ? stash[static_cast<std::size_t>(pick - kStashPick)].first : 0;
                const int pickedCount = picked ? grid[static_cast<std::size_t>(pick)].count
                                               : pickedStash ? stash[static_cast<std::size_t>(pick - kStashPick)].second : 0;
                screen.header = picked        ? "MOVE " + Upper(Plain(GridItemName(pickedId))) + " TO"
                                : pickedStash ? "TAKE " + Upper(Plain(GridItemName(pickedId))) + " OUT"
                                              : "ORGANIZE   PICK AN ITEM";
                for (const OrganizeRow& row : OrganizeRows(pick, static_cast<int>(stash.size()))) {
                    Item item;
                    if (row.kind == OrganizeRow::Cell) {
                        const GridCell& c = grid[static_cast<std::size_t>(row.index)];
                        item.label = std::to_string(row.index + 1) + "   " +
                                     (c.record ? Upper(Plain(GridItemName(c.id))) + (c.count > 1 ? "  " + std::to_string(c.count) : "")
                                               : std::string("EMPTY"));
                        if (picked && row.index == pick) {
                            item.label += "   PICKED";
                            item.color = 0xFFFFD27Fu;
                            item.help = "Press again to put it back.";
                        } else if (picked) {
                            item.help = c.record ? "Press to swap the two items." : "Press to move it here.";
                        } else if (pickedStash) {
                            item.help = "Press to take it out of the storage\ninto your grid.";
                        } else {
                            item.help = c.record ? "Press to pick it up." : "An empty cell.";
                        }
                    } else if (row.kind == OrganizeRow::Stash) {
                        const auto& [stashId, stashCount] = stash[static_cast<std::size_t>(row.index)];
                        item.label = "STORAGE   " + Upper(Plain(GridItemName(stashId))) + (stashCount > 1 ? "  " + std::to_string(stashCount) : "");
                        if (pickedStash && kStashPick + row.index == pick) {
                            item.label += "   PICKED";
                            item.color = 0xFFFFD27Fu;
                            item.help = "Press again to leave it there.";
                        } else if (picked) {
                            item.help = "Press to put the picked item in the storage.";
                        } else {
                            item.help = "In the run's storage (both players').\nPress to pick it.";
                        }
                    } else if (row.kind == OrganizeRow::ToStorage) {
                        item.label = "TO THE STORAGE";
                        item.help = "The run's storage keeps it for a later shop\n(both players can take it out).";
                    } else {
                        const std::int64_t value = g->run->SaleValue(MakeItemId(RecordItemId(pickedId)), std::max(0, pickedCount));
                        item.label = "SELL   " + std::to_string(value) + (value == 1 ? " SUPPLY" : " SUPPLIES");
                        item.help = "Half its shop price, paid in supplies.\nA sold gun leaves the run.";
                    }
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::MatchRunOver: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "RUN COMPLETE";
                Item item;
                item.label = "FINISH";
                item.help = "All " + std::to_string(s.arena_count) + " arenas cleared.\n" +
                            Plural(s.credits_pending, "credit") + " banked when you leave.";
                screen.items.push_back(std::move(item));
                break;
            }
            case ScreenId::MatchRoute: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "NEXT ROUTE";
                screen.empty_help = "No route to choose.";
                // The run map's next arenas (M5.3): the arena and its tier, warmer for harder
                // tiers; the help names the route, its rewards and where it leads.
                for (const auto& o : s.route_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title)) + (o.boss ? std::string("   BOSS")
                                                                 : o.tier > 0 ? "   TIER " + std::to_string(o.tier) : "");
                    if (o.tier == 2) item.color = 0xFFFFD27Fu;
                    if (o.tier >= 3 || o.boss) item.color = 0xFFFF7A5Cu;
                    item.help = Plain(o.detail);
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::Skins: {
                ScanModSkins();  // a mod unzipped while the game runs shows at once
                const auto characters = run.Characters();
                if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) break;
                const auto& c = characters[g->selectedCharacter];
                const rm::SkinAvailability* current = SelectedSkin(c);
                screen.header = "SKINS   " + Upper(c.name);
                const rm::CharacterDef* def = run.content().FindCharacter(c.id);
                for (const auto& s : c.skins) {
                    Item item;
                    item.label = Upper(s.name);
                    item.portrait = static_cast<int>(s.costume_index);
                    // M8: a skin with a build says what it does; the others what their trait does.
                    const rm::SkinDef* skinDef = def != nullptr ? rm::FindSkin(*def, s.id) : nullptr;
                    if (skinDef != nullptr && !skinDef->build.name.empty()) {
                        // A mod skin says MOD instead: its build is the outfit's it stands in for (in its help).
                        if (!skinDef->mod) item.label += " - " + Upper(Plain(skinDef->build.name));
                        item.icon = skinDef->build.icon;
                    }
                    std::string summary = skinDef != nullptr ? skinDef->build.summary : std::string();
                    if (summary.empty() && skinDef != nullptr && !skinDef->starting_passive.empty()) {
                        if (const rm::PerkDef* trait = run.content().FindPerk(skinDef->starting_passive)) summary = trait->description;
                    }
                    if (skinDef != nullptr && skinDef->mod) {
                        item.label += "   MOD";
                        const auto pic = g_modPortraits.find(s.id);
                        if (s.gate == rm::CharacterGate::Selectable && pic != g_modPortraits.end()) item.portrait = pic->second;
                    }
                    if (s.gate == rm::CharacterGate::NotInstalled && skinDef != nullptr && skinDef->mod) {
                        // Not installed: where to get it (the menu's two help lines of 54).
                        item.locked = true;
                        const rm::SkinMod& mod = *skinDef->mod;
                        item.help = (mod.author.empty() ? std::string("A free mod on Nexus Mods.") : "A free mod by " + Plain(mod.author) + ".") +
                                    " Press to open its page,\nendorse it, and unzip it in RogueMercenaries, mods.";
                    } else if (s.gate == rm::CharacterGate::Selectable && SkinsClash(g->menuOtherSkin, s.id)) {
                        // M10: two outfits of one costume slot cannot be worn together when one is a mod's.
                        item.locked = true;
                        item.help = "Player " + std::string(g->menuPlayer == 1 ? "1" : "2") +
                                    " wears this costume slot already.\nA mod outfit replaces the slot, one file of the game.";
                    } else if (s.gate == rm::CharacterGate::Selectable) {
                        const bool selected = current != nullptr && current->id == s.id;
                        if (selected) item.label += "   SELECTED";
                        item.help = !summary.empty() ? Plain(summary)
                                    : selected       ? "Your skin for the next run."
                                                     : "Press to wear this skin.";
                    } else {
                        item.locked = true;
                        item.help = s.unlock_text.empty() ? "Locked." : s.unlock_text + ".";
                        if (s.challenge.target > 0) {
                            item.help += "\n" + std::to_string(s.challenge.current) + " of " +
                                         std::to_string(s.challenge.target) + " done.";
                        }
                    }
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::Credits: {
                const auto& stats = profile.stats;
                screen.header = "CREDITS";
                screen.items = {
                    {Upper(CreditsLine()), "Earned by finishing runs. Spent on perks,\nweapons and characters.", false, 0,
                     "credits"},
                    {Upper(Plural(stats.credits_earned, "credit")) + " EARNED", "All credits earned by your runs so far.",
                     false, 0, "credits"},
                    {Upper(Plural(stats.runs_completed, "run")) + " COMPLETED", Plural(stats.runs_failed, "run") + " ended early.",
                     false, 0, "trophy"},
                    {Upper(Plural(stats.total_kills, "kill")), "Enemies killed in all runs.", false, 0, "crosshair"},
                    {"BEST RUN " + Upper(Plural(stats.best_arena, "arena")), "Most arenas cleared in one run.", false, 0, "flag"},
                    {Upper(Plural(stats.s_rank_arenas, "S rating")),
                     Plural(stats.no_damage_arenas, "arena") + " cleared without taking damage.", false, 0, "medal"},
                };
                if (const auto& last = run.last_summary()) {
                    screen.items.push_back({std::string("LAST RUN   ") + (last->completed ? "COMPLETE" : "OVER"),
                                            "The results of your last run. Press to see them.", false, 0, "clock"});
                }
                for (const auto& weapon : run.content().weapons()) {
                    if (!weapon.enabled || weapon.unlock_cost_credits <= 0) continue;
                    Item item;
                    const bool owned = profile.unlocked_weapons.count(weapon.id) > 0;
                    item.label = Upper(weapon.name) + (owned ? "   UNLOCKED" : "");
                    item.icon = WeaponIcon(weapon.category);
                    item.locked = !owned;
                    item.help = owned ? "This weapon can appear in your runs."
                                      : "Weapon. Unlock for " + Plural(weapon.unlock_cost_credits, "credit") + ".";
                    screen.items.push_back(std::move(item));
                }
                break;
            }
        }
        return screen;
    }

    nativemenu::Decision DecideLocked(ScreenId id, int index) {
        rm::RunController& run = *g->run;
        nativemenu::Decision d;
        auto purchase = [&](rm::UnlockKind kind, const std::string& unlockId, std::int64_t cost) {
            if (run.profile().credits.balance() < cost) {
                d.outcome = Outcome::Denied;
                return;
            }
            const auto receipt = run.PurchaseUnlock(kind, unlockId);
            Report(receipt.ok() ? rm::Status::Success() : receipt.status(), ("menu unlock " + unlockId).c_str());
            d.outcome = receipt.ok() ? Outcome::Refresh : Outcome::Denied;
        };
        switch (id) {
            case ScreenId::Main: {
                // CONTINUE RUN comes first while a run can go on; the other rows follow it (MainRows).
                const auto rows = MainRows(run);
                if (index < 0 || index >= static_cast<int>(rows.size())) break;
                const MainRow row = rows[static_cast<std::size_t>(index)];
                if (row == MainRow::Continue) {
                    if (!ContinueRun()) {
                        d.outcome = Outcome::Denied;
                        break;
                    }
                    d.outcome = Outcome::StartRun;
                    break;
                }
                if (row == MainRow::Players) {
                    g->coopPlayers = g->coopPlayers == 2 ? 1 : 2;
                    g->menuPlayer = 0;
                    RM_LOGI("menu: players %d", g->coopPlayers);
                    d.outcome = Outcome::Refresh;
                    break;
                }
                if (row == MainRow::PlayerOne || row == MainRow::PlayerTwo || row == MainRow::Characters) {
                    // M10: each player chooses on the same CHARACTERS and SKINS screens (MenuPlayerScope).
                    g->menuPlayer = row == MainRow::PlayerTwo ? 1 : 0;
                    d.outcome = Outcome::Push;
                    d.next = ScreenId::Characters;
                    break;
                }
                if (row == MainRow::Perks) {
                    g->menuPlayer = 0;
                    d.outcome = Outcome::Push;
                    d.next = g->coopPlayers == 2 && engine::Caps().splitScreen ? ScreenId::WhichPlayer : ScreenId::Perks;
                    break;
                }
                if (row == MainRow::Start && g->coopPlayers == 2 && PlayerTwoChoice(run.Characters()).skin.empty()) {
                    d.outcome = Outcome::Denied;  // no outfit player 2 can wear
                    break;
                }
                if (row == MainRow::Start) {
                    // The ladder: with more than one length open, RUN LENGTH picks it.
                    const auto lengths = run.LadderLengths();
                    const auto open = std::count_if(lengths.begin(), lengths.end(), [](const auto& l) { return l.open; });
                    if (open > 1) {
                        d.outcome = Outcome::Push;
                        d.next = ScreenId::RunLength;
                        break;
                    }
                    const auto first = std::find_if(lengths.begin(), lengths.end(), [](const auto& l) { return l.open; });
                    g->selectedLength = first == lengths.end() ? std::string() : first->id;
                    ArmFirstArena(run);
                    d.outcome = Outcome::StartRun;
                } else {
                    d.outcome = Outcome::Push;
                    d.next = ScreenId::Credits;
                }
                break;
            }
            case ScreenId::WhichPlayer: {
                if (index < 0 || index > 1) break;
                g->menuPlayer = index;
                d.outcome = Outcome::Push;
                d.next = ScreenId::Perks;
                break;
            }
            case ScreenId::RunLength: {
                const auto lengths = run.LadderLengths();
                if (index < 0 || index >= static_cast<int>(lengths.size())) break;
                if (!lengths[static_cast<std::size_t>(index)].open) {
                    d.outcome = Outcome::Denied;
                    break;
                }
                g->selectedLength = lengths[static_cast<std::size_t>(index)].id;
                ArmFirstArena(run);
                d.outcome = Outcome::StartRun;
                break;
            }
            case ScreenId::Characters: {
                const auto characters = run.Characters();
                if (index < 0 || index >= static_cast<int>(characters.size())) break;
                const auto& c = characters[index];
                if (c.gate == rm::CharacterGate::NotInstalled) {
                    // A mod character without its mod: its page.
                    if (const rm::CharacterDef* def = run.content().FindCharacter(c.id)) {
                        for (const rm::SkinDef& s : def->skins) {
                            if (s.mod) {
                                OpenModPage(*s.mod);
                                break;
                            }
                        }
                    }
                    d.outcome = Outcome::Refresh;
                    break;
                }
                if (c.gate == rm::CharacterGate::Selectable) {
                    if (g->selectedCharacter != index) g->selectedSkin.clear();
                    g->selectedCharacter = index;
                    d.outcome = Outcome::Push;
                    d.next = ScreenId::Skins;
                } else if (c.gate == rm::CharacterGate::Locked && c.unlock_cost_credits > 0) {
                    purchase(rm::UnlockKind::Character, c.id, c.unlock_cost_credits);
                } else {
                    d.outcome = Outcome::Denied;
                }
                break;
            }
            case ScreenId::Perks: {
                if (index == static_cast<int>(kSlotCategories.size())) {
                    d.outcome = Outcome::Push;
                    d.next = ScreenId::Skins;
                    break;
                }
                if (index < 0 || index >= static_cast<int>(kSlotCategories.size())) break;
                g->perkCategory = kSlotCategories[static_cast<std::size_t>(index)];
                d.outcome = Outcome::Push;
                d.next = ScreenId::PerkList;
                break;
            }
            case ScreenId::PerkList: {
                const std::string character = SelectedCharacterId(run);
                const auto perks = MenuPerks(run, g->perkCategory);
                if (character.empty() || index < 0 || index >= static_cast<int>(perks.size())) break;
                const rm::PerkDef& perk = *perks[static_cast<std::size_t>(index)];
                if (perk.unlock_cost_credits > 0 && !run.profile().unlocked_perks.count(perk.id)) {
                    purchase(rm::UnlockKind::Perk, perk.id, perk.unlock_cost_credits);
                    break;
                }
                const rm::PerkDef* equipped = run.EquippedPerk(character, g->perkCategory);
                const rm::Status status = equipped != nullptr && equipped->id == perk.id
                                              ? run.UnequipPerk(character, g->perkCategory)
                                              : run.EquipPerk(character, perk.id);
                RM_LOGI("menu %s %s for %s -> %s", equipped != nullptr && equipped->id == perk.id ? "unequip" : "equip",
                        perk.id.c_str(), character.c_str(), status.ok() ? "ok" : status.message.c_str());
                d.outcome = status.ok() ? Outcome::Refresh : Outcome::Denied;
                break;
            }
            case ScreenId::MatchResults: {
                if (index < 0) break;  // cancel does nothing here
                const rm::Status s = run.ContinueToRewards();
                Report(s, "continue");
                if (!s.ok()) { d.outcome = Outcome::Denied; break; }
                d.outcome = Outcome::Push;
                d.next = run.phase() == rm::Phase::RewardSelection ? ScreenId::MatchRewards : ScreenId::MatchRunOver;
                if (!run.PendingElementChoices().empty()) {  // a chest refined a gun to Epic in the arena
                    g_elementNext = d.next;
                    d.next = ScreenId::MatchElement;
                }
                break;
            }
            case ScreenId::MatchElement: {
                const auto pending = run.PendingElementChoices();
                if (!pending.empty()) {
                    if (index < 0) {  // it has to be chosen
                        d.outcome = Outcome::Denied;
                        break;
                    }
                    const rm::ElementChoice& c = pending.front();
                    const int option = index < static_cast<int>(c.offer.size()) ? index : -1;
                    const rm::Status st = run.ChooseElement(c.instance_id, option);
                    Report(st, "choose element");
                    if (!st.ok()) {
                        d.outcome = Outcome::Denied;
                        break;
                    }
                    std::string chosen;
                    for (const auto& w : run.state().weapons) {
                        if (w.instance_id == c.instance_id && !w.elements.empty()) chosen = std::string(rm::ElementName(w.elements.front()));
                    }
                    RM_LOGI("element: %s gets %s (%s%s)", c.base_id.c_str(), chosen.c_str(), option < 0 ? "random" : "chosen",
                            c.evolved ? ", evolved" : "");
                    if (const rm::ElementDef* el = rm::ParseElement(chosen) ? g->content->FindElement(*rm::ParseElement(chosen), c.evolved) : nullptr) {
                        g->toasts.Push(el->name + (option < 0 ? " (random)" : ""));
                    }
                    if (!run.PendingElementChoices().empty()) {
                        d.outcome = Outcome::Refresh;
                        break;
                    }
                }
                if (g_elementNext) {
                    d.outcome = Outcome::Push;
                    d.next = *g_elementNext;
                } else {
                    d.outcome = Outcome::Back;
                }
                g_elementNext.reset();
                break;
            }
            case ScreenId::MatchRewards: {
                if (index < 0) break;
                const rm::RunSnapshot s = run.Snapshot(Now());
                const int offers = static_cast<int>(s.reward_offers.size());
                if (index < offers) {
                    const rm::Status st = run.ChooseReward(static_cast<std::size_t>(index));
                    Report(st, "choose reward");
                    if (!st.ok()) { d.outcome = Outcome::Denied; break; }
                    const rm::OfferView& o = s.reward_offers[static_cast<std::size_t>(index)];
                    RM_LOGI("reward chosen: %s %s (%s): %s", o.kind.c_str(), o.title.c_str(), o.rarity.c_str(), o.detail.c_str());
                    if (run.phase() == rm::Phase::RewardSelection && run.PendingElementChoices().empty()) {
                        d.outcome = Outcome::Refresh;  // M10: the other player's turn
                        break;
                    }
                    d.outcome = Outcome::Push;
                    d.next = run.phase() == rm::Phase::Shop             ? ScreenId::MatchShop
                             : run.phase() == rm::Phase::RewardSelection ? ScreenId::MatchRewards
                                                                         : ScreenId::MatchRunOver;
                    if (!run.PendingElementChoices().empty()) {  // refined to Epic: its element first
                        g_elementNext = d.next;
                        d.next = ScreenId::MatchElement;
                    }
                } else if (index == offers && s.reroll_tokens > 0) {
                    const rm::Status st = run.RerollReward();
                    Report(st, "reroll");
                    d.outcome = st.ok() ? Outcome::Refresh : Outcome::Denied;
                } else if (offers == 0) {
                    d.outcome = Outcome::Close;
                }
                break;
            }
            case ScreenId::MatchShop: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                const int offers = static_cast<int>(s.shop_offers.size());
                if (index >= 0 && index < offers) {
                    const rm::Status st = run.Purchase(static_cast<std::size_t>(index));
                    Report(st, "purchase");
                    if (!st.ok()) g->toasts.Push(st.message);
                    if (st.ok()) {
                        const rm::OfferView& o = s.shop_offers[static_cast<std::size_t>(index)];
                        RM_LOGI("shop: bought %s %s (%s) for %lld", o.kind.c_str(), o.title.c_str(), o.rarity.c_str(),
                                static_cast<long long>(o.price));
                    }
                    d.outcome = st.ok() ? Outcome::Refresh : Outcome::Denied;
                    if (st.ok() && !run.PendingElementChoices().empty()) {  // refined to Epic: its element, then back here
                        g_elementNext.reset();
                        d.outcome = Outcome::Push;
                        d.next = ScreenId::MatchElement;
                    }
                    break;
                }
                const int rerollRow = s.restock_price >= 0 ? 1 : 0;
                if (rerollRow && index == offers) {  // NEW STOCK
                    const rm::Status st = run.RerollShop();
                    Report(st, "reroll shop");
                    if (st.ok()) RM_LOGI("shop: new stock (%lld supplies)", static_cast<long long>(s.restock_price));
                    d.outcome = st.ok() ? Outcome::Refresh : Outcome::Denied;
                    break;
                }
                if (index == offers + rerollRow) {  // ORGANIZE INVENTORY
                    // The game's own Organize/Buy screen when it can open here (M7.1); the mod's
                    // grid screen otherwise (it lists the storage too). The storage's guns are seen there.
                    StashGunsSeen();
                    if (NativeItemBoxReady() && run.SeatCount() < 2) {
                        g->itemBoxRequested = true;
                        g->itemBoxSeat = g->run->ActiveSeat();  // M10: this player's grid in the screen
                        d.outcome = Outcome::Close;
                        break;
                    }
                    g->organizePick = -1;
                    d.outcome = Outcome::Push;
                    d.next = ScreenId::MatchOrganize;
                    break;
                }
                // "Leave shop" row or cancel.
                const rm::Status st = run.LeaveShop();
                Report(st, "leave shop");
                if (!st.ok()) { d.outcome = Outcome::Denied; break; }
                if (run.phase() == rm::Phase::Shop) {
                    d.outcome = Outcome::Refresh;  // M10: the other player's shop
                    break;
                }
                d.outcome = Outcome::Push;
                d.next = ScreenId::MatchRoute;
                break;
            }
            case ScreenId::MatchOrganize: {
                const auto grid = ReadGrid();
                const auto stash = StashRecords();
                int& pick = g->organizePick;
                if (index < 0) {  // cancel: drop the picked item, or back to the shop
                    if (pick >= 0) {
                        pick = -1;
                        d.outcome = Outcome::Refresh;
                    } else {
                        d.outcome = Outcome::Back;
                    }
                    break;
                }
                const auto rows = OrganizeRows(pick, static_cast<int>(stash.size()));
                if (index >= static_cast<int>(rows.size())) break;
                const OrganizeRow row = rows[static_cast<std::size_t>(index)];
                const bool pickedCell = pick >= 0 && pick < kGridCells;
                const bool pickedStash = pick >= kStashPick;
                std::string why;
                d.outcome = Outcome::Refresh;
                if (row.kind == OrganizeRow::Cell) {
                    if (pickedStash) {
                        if (!TakeFromStash(pick - kStashPick, why)) {
                            d.outcome = Outcome::Denied;
                            if (!why.empty()) g->toasts.Push(why);
                        }
                        pick = -1;
                    } else if (!pickedCell) {
                        if (!grid[static_cast<std::size_t>(row.index)].record) {
                            d.outcome = Outcome::Denied;
                            break;
                        }
                        pick = row.index;
                    } else if (pick == row.index) {
                        pick = -1;
                    } else {
                        SwapGridCells(pick, row.index);
                        pick = -1;
                    }
                } else if (row.kind == OrganizeRow::Stash) {
                    if (pickedCell) {
                        if (!PutCellInStash(pick, why)) d.outcome = Outcome::Denied;
                        if (!why.empty()) g->toasts.Push(why);
                        pick = -1;
                    } else {
                        pick = kStashPick + row.index == pick ? -1 : kStashPick + row.index;
                    }
                } else if (row.kind == OrganizeRow::ToStorage) {
                    if (!PutCellInStash(pick, why)) d.outcome = Outcome::Denied;
                    if (!why.empty()) g->toasts.Push(why);
                    pick = -1;
                } else {
                    const std::int64_t paid = pickedCell ? SellCell(pick) : SellStash(pick - kStashPick);
                    if (paid <= 0) d.outcome = Outcome::Denied;
                    else g->toasts.Push("Sold for " + std::to_string(paid) + (paid == 1 ? " supply" : " supplies"));
                    pick = -1;
                }
                break;
            }
            case ScreenId::MatchRoute: {
                if (index < 0) break;
                const rm::Status st = run.ChooseRoute(static_cast<std::size_t>(index));
                Report(st, "choose route");
                if (st.ok()) {
                    RM_LOGI("route: choice %d of %zu, %s on stage %d (layer %d node %d)", index + 1,
                            run.RouteChoices().size(), run.state().route_id.c_str(), run.PlannedMapKey(),
                            run.state().map_layer, run.state().map_node);
                }
                d.outcome = st.ok() ? Outcome::Close : Outcome::Denied;
                break;
            }
            case ScreenId::MatchRunOver: {
                if (index < 0) break;
                d.outcome = Outcome::Close;  // the tick then banks the run and lets the match end
                break;
            }
            case ScreenId::RunResults:
                d.outcome = Outcome::Back;
                break;
            case ScreenId::Skins: {
                const auto characters = run.Characters();
                if (g->selectedCharacter < 0 || g->selectedCharacter >= static_cast<int>(characters.size())) break;
                const auto& skins = characters[g->selectedCharacter].skins;
                if (index < 0 || index >= static_cast<int>(skins.size())) break;
                if (skins[index].gate == rm::CharacterGate::Selectable && SkinsClash(g->menuOtherSkin, skins[index].id)) {
                    d.outcome = Outcome::Denied;  // the other player's outfit takes its costume slot
                } else if (skins[index].gate == rm::CharacterGate::Selectable) {
                    g->selectedSkin = skins[index].id;
                    d.outcome = Outcome::Refresh;
                } else if (skins[index].gate == rm::CharacterGate::NotInstalled) {
                    // A mod skin without its mod: its page, to get it there (and endorse it).
                    const rm::CharacterDef* def = run.content().FindCharacter(characters[g->selectedCharacter].id);
                    const rm::SkinDef* skinDef = def != nullptr ? rm::FindSkin(*def, skins[index].id) : nullptr;
                    if (skinDef != nullptr && skinDef->mod) OpenModPage(*skinDef->mod);
                    d.outcome = Outcome::Refresh;
                } else {
                    d.outcome = Outcome::Denied;
                }
                break;
            }
            case ScreenId::Credits: {
                int row = 6;  // rows before the weapon list
                if (run.last_summary()) {
                    if (index == row) {
                        d.outcome = Outcome::Push;
                        d.next = ScreenId::RunResults;
                        break;
                    }
                    ++row;
                }
                for (const auto& weapon : run.content().weapons()) {
                    if (!weapon.enabled || weapon.unlock_cost_credits <= 0) continue;
                    if (row++ != index) continue;
                    if (!run.profile().unlocked_weapons.count(weapon.id)) {
                        purchase(rm::UnlockKind::Weapon, weapon.id, weapon.unlock_cost_credits);
                    }
                    break;
                }
                break;
            }
        }
        return d;
    }
};

MenuModel g_menuModel;

// Native menu flow: the run starts with the match, using the character the
// player picked in the game's own character select.
// The run's map (M5.3), one line per layer: "stage route" per node, "->" its links.
void LogRunMap() {
    const auto& layers = g->run->state().run_map.layers;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        std::string line;
        for (std::size_t n = 0; n < layers[l].size(); ++n) {
            const rm::RunMapNode& node = layers[l][n];
            line += (n > 0 ? " | " : "") + std::to_string(node.map_key) + " " + node.route_id;
            for (const std::string& guest : node.guests) line += " +" + guest;
            if (!node.next.empty()) {
                line += " ->";
                for (const int next : node.next) line += " " + std::to_string(next);
            }
        }
        RM_LOGI("run map: arena %zu: %s", l + 1, line.c_str());
    }
}

// M10: the pads, when the lobby did not tell (WatchLobbyPads): player 2 the last connected XInput
// pad, player 1 the first other one (none: the keyboard). Then the lobby's fields with both players
// in: the later arenas skip the lobby with them.
void RecordCoopLobby() {
    if (g->p2Pad < 0) {
        for (int user = 3; user >= 0 && g->p2Pad < 0; --user) {
            if (XInputConnected(user)) g->p2Pad = user;
        }
    }
    if (g->p1Pad < -1) {
        g->p1Pad = -1;
        for (int user = 0; user < 4 && g->p1Pad < 0; ++user) {
            if (user != g->p2Pad && XInputConnected(user)) g->p1Pad = user;
        }
    }
    RM_LOGI("co-op: pads: player 1 %s, player 2 %s", g->p1Pad >= 0 ? ("XInput user " + std::to_string(g->p1Pad)).c_str() : "keyboard",
            g->p2Pad >= 0 ? ("XInput user " + std::to_string(g->p2Pad)).c_str() : "unknown");
    const auto gw = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    std::vector<std::pair<int, std::uint32_t>> fields;
    for (const int off : {0x490, 0x494, 0x610, 0x614, 0x618, 0x61C, 0x620, 0x624, 0x654, 0x658}) {
        fields.emplace_back(off, gw ? ReadOr<std::uint32_t>(gw + static_cast<std::size_t>(off), 0) : 0);
    }
    autopilot::SetLobby(fields);
}

// M10: the split-screen match's second player joins the run. Their character is the match's
// slot-1 class (a mod character in its costume when player 2 chose one), the skin player 2 chose
// when the lobby kept its costume, else that class's own outfit in the costume the lobby left.
void JoinPlayerTwo(rm::RunController& run) {
    const auto players = engine::ReadPlayers();
    const engine::PlayerInfo* two = nullptr;
    for (const auto& p : players.players) {
        if (p.object && p.slot == 1 && p.control == 0) two = &p;
    }
    if (two == nullptr) {
        RM_LOGW("co-op: no second player in this match (players %d); the run is solo", players.count);
        g->toasts.Push("Rogue Mercenaries: player 2 did not join; the run is solo");
        return;
    }
    const auto gw = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    const int costume = gw ? ReadOr<std::int32_t>(gw + re5::off::kGameWorkCharacter + 4, -1) : -1;
    const std::string classCharacter = engine::CharacterIdForClass(two->className);
    const PlayerTwoPick choice = PlayerTwoChoice(run.Characters());
    std::string character = classCharacter;
    std::string skin;
    if (!choice.skin.empty() && choice.costume == costume && PlaysAs(classCharacter, choice.character, choice.skin)) {
        character = choice.character;
        skin = choice.skin;
    } else if (const rm::CharacterDef* def = g->content->FindCharacter(classCharacter)) {
        for (const rm::SkinDef& s : def->skins) {
            if (!s.mod && s.costume_index == costume && skin.empty()) skin = s.id;
        }
    }
    const rm::Status status = run.AddSeat(character, skin);
    Report(status, ("co-op: player 2 joins as " + character + " (" + (skin.empty() ? std::string("default") : skin) + ")").c_str());
    if (!status.ok()) {
        g->toasts.Push("Rogue Mercenaries: player 2 could not join: " + status.message);
        return;
    }
    g->p2Costume = costume;
    RecordCoopLobby();
    g->toasts.Push("Rogue Mercenaries: player 2 joined the run");
}

void HandleNativeMenu(double now) {
    nativemenu::SetModel(&g_menuModel);
    matchui::SetModel(&g_menuModel);
    if (nativemenu::TakeChosen()) {
        g->rogueModePending = true;
        g->toasts.Push((g->coopPlayers == 2 || g->coopLobbyNeeded) && engine::Caps().splitScreen
                           ? "Rogue Mercenaries: two players. Player 2 presses START to join, then both ready"
                           : "Rogue Mercenaries: choose Solo, a stage and Chris or Sheva");
    }
    if (!g->rogueModePending) return;
    rm::RunController& run = *g->run;
    if (nativemenu::MenuAtInput() && run.phase() == rm::Phase::Idle && run.state().arena_index == 0) {
        g->rogueModePending = false;  // backed out to the Play menu
        autopilot::Disarm("back at the Play menu");
        host::SetStageOverride(-1);
        return;
    }
    if (nativemenu::MenuAtInput() && g->coopLobbyNeeded && run.HasActiveRun()) {
        g->rogueModePending = false;
        g->coopLobbyNeeded = false;
        g->transition = {};
        autopilot::Disarm("back at the Play menu");
        host::SetStageOverride(-1);
        Report(run.SuspendRun(now), "suspend run (its lobby was left)");
        RM_LOGI("co-op: the resumed run's lobby was left; the run waits on CONTINUE RUN");
        return;
    }
    if (!g->match.inMatch) return;
    g->rogueModePending = false;
    if (run.HasActiveRun()) return;  // continue the run already in progress
    if (g->match.localCharacter.empty()) {
        g->toasts.Push("Rogue Mercenaries: this character is not supported");
        return;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const auto seed = static_cast<std::uint64_t>(counter.QuadPart) ^ 0x9E3779B97F4A7C15ull;
    run.SetArenaPool(RotationMaps(), CurrentMapKey());  // the run map starts where the match is
    const std::string runCharacter = MatchRunCharacter(g->match.localCharacter);  // Leon in Chris's costume
    const rm::Status status = run.StartRun(runCharacter, seed, SkinForRun(run.Characters(), runCharacter), g->selectedLength);
    Report(status, ("start run (" + runCharacter + ")").c_str());
    if (!status.ok()) {
        g->toasts.Push("Rogue Mercenaries: " + status.message);
        return;
    }
    if (g->coopPlayers == 2 && engine::Caps().splitScreen) JoinPlayerTwo(run);
    if (g->content->economy().ladder.enabled) {
        std::string beaten;
        for (const std::string& key : run.profile().stats.bosses_beaten) beaten += (beaten.empty() ? "" : ", ") + key;
        RM_LOGI("ladder: %s run, %d arenas; bosses beaten: %s", run.state().length_id.c_str(), run.state().arena_count,
                beaten.empty() ? "none" : beaten.c_str());
    }
    LogRunMap();
    for (const auto& perk : g->settings.devGrantPerks) {
        Report(run.DevGrantPerk(perk), ("dev grant " + perk).c_str());
    }
    (void)now;
}

// Puts `count` of game item `id` into the local player's inventory through the
// game's own helpers (verified: AddToStacks tops up a stack, Create makes a
// new slot the way PickUpItem does). Returns false when nothing could be placed.
// Base id (weapons.json) of the weapon in the player's hands: the inventory record flagged as
// equipped, mapped through the weapons' game_item ids. Empty when unknown.
std::optional<std::string> EquippedWeaponBaseId() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return std::nullopt;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1) continue;
        const auto id = SlotItemId(slot);
        if ((id >> 8) != 1) continue;
        if (const rm::WeaponBaseDef* w = WeaponForItem(id)) return w->id;
        return std::nullopt;
    }
    return std::nullopt;
}

// Enemies seen alive this arena, for deaths that never pass the damage hook (a plaga burnt by
// a flash grenade, a fall): health going to zero, or vanishing after being alive a while.
struct EnemyTrack {
    int hp = 0;
    int max = 0;
    double firstSeen = 0.0;
    double lastSeen = 0.0;
};
std::unordered_map<std::uintptr_t, EnemyTrack> g_enemyTracks;

// The run's id of a dying enemy: the director's own id for its units (reported once), the
// unit address and max health for anything else.
std::string EnemyKillId(std::uintptr_t enemy, int maxHp) {
    if (const auto it = g->directorUnits.find(enemy); it != g->directorUnits.end()) {
        std::string id = std::move(it->second);
        g->directorUnits.erase(it);
        if (enemy == g->boss.unit) {
            g->boss.unit = 0;
            g->boss.dead = true;
            g->boss.fallen = enemy;
            g->boss.diedAt = Now();
        }
        return id;
    }
    char id[32];
    std::snprintf(id, sizeof(id), "em_%08X_%d", static_cast<unsigned>(enemy), maxHp);
    return id;
}

// Enemies a build special hurt (M8: Twin Fury, Backup), the mod's elements burnt, or the player
// hit, and when: a death within kSpecialKillCredit is the player's (a Majini the flamethrower set
// burning dies of the game's own fire, outside any hit, about 4 s later).
std::map<std::uintptr_t, double> g_specialVictims;
// The rarity remake's element statuses (defined with DriveElements).
bool FrozenNow(std::uintptr_t enemy, double now);
void QueueProcs(std::uintptr_t enemy, const std::vector<rm::WeaponProc>& procs);
void PlagueOnDeath(std::uintptr_t enemy, int maxHp, double now);
constexpr double kSpecialKillCredit = 6.0;
void BuildKillNotice(const rm::KillOutcome& outcome, double now);

void ReportEnemyDeath(std::uintptr_t enemy, int maxHp, double now, const char* how) {
    rm::KillEvent kill;
    kill.now = now;
    const auto victim = g_specialVictims.find(enemy);
    kill.by_player = victim != g_specialVictims.end() && now - victim->second < kSpecialKillCredit;
    if (kill.by_player) {
        kill.was_melee = false;
        kill.enemy_class = engine::ClassName(enemy);
        kill.target_hp_max = maxHp;
        kill.target_frozen = FrozenNow(enemy, now);  // a frozen one shatters (rm::ProcKind::Shatter)
    }
    kill.target_is_boss = enemy == g->boss.unit;
    kill.enemy_id = EnemyKillId(enemy, maxHp);
    PlagueOnDeath(enemy, maxHp, now);
    const rm::KillOutcome out = g->run->OnEnemyKilled(kill);
    if (!out.procs.empty()) QueueProcs(enemy, out.procs);
    if (out.counted) {
        RM_LOGI("enemy %08X died (%s): counted, %d remain%s", static_cast<unsigned>(enemy), how, out.remaining,
                kill.by_player ? " (by the special)" : "");
    }
    if (kill.by_player) BuildKillNotice(out, now);
}

void DetectEnemyDeaths(double now) {
    if (!g->match.inMatch || g->run->phase() != rm::Phase::ArenaActive) {
        g_enemyTracks.clear();
        return;
    }
    const auto states = engine::ReadEnemyStates();
    for (const engine::EnemyState& e : states) {
        auto it = g_enemyTracks.find(e.object);
        if (it == g_enemyTracks.end()) {
            if (e.hp > 0) g_enemyTracks[e.object] = EnemyTrack{e.hp, e.max, now, now};
            continue;
        }
        EnemyTrack& t = it->second;
        if (t.hp > 0 && e.hp <= 0) ReportEnemyDeath(e.object, t.max, now, "health reached zero");
        t.hp = e.hp;
        t.lastSeen = now;
    }
    for (auto it = g_enemyTracks.begin(); it != g_enemyTracks.end();) {
        if (it->second.lastSeen < now) {
            if (it->second.hp > 0 && now - it->second.firstSeen > 1.0) {
                ReportEnemyDeath(it->first, it->second.max, now, "removed from the enemy list");
            }
            it = g_enemyTracks.erase(it);
        } else {
            ++it;
        }
    }
}

// A weapon's model is loaded in this match: the character's own loadout put it there, or its
// class archive is in the loader's lists (M3 preloads every gun of the content in a run).
// The grenade launcher's record id stands for the item it is made from (MakeItemId).
bool WeaponModelLoaded(std::uint32_t id) {
    if (g->loadedWeaponItems.count(id) || g->loadedWeaponItems.count(RecordItemId(id))) return true;
    const std::uint32_t make = MakeItemId(id);
    return resources::Available() && (make >> 8) == 1 && resources::WeaponLoaded(static_cast<std::uint16_t>(make));
}

// The weapons rewards, the shop and chests may hand out: those whose model is loaded.
void RefreshPlaceableWeapons() {
    rm::StringSet placeable;
    std::string names;
    for (const rm::WeaponBaseDef& w : g->content->weapons()) {
        if (w.game_item >= 0 && WeaponModelLoaded(static_cast<std::uint32_t>(w.game_item))) {
            placeable.insert(w.id);
            names += " " + w.id;
        }
    }
    RM_LOGI("inventory: %zu weapons placeable in the run:%s", placeable.size(), names.c_str());
    g->run->SetPlaceableWeapons(std::move(placeable));
}

void ForgetBackGun(std::uintptr_t record, const char* why);  // the inventory's stale back gun, below

// Returns the units placed (stacked or in new slots); 0 when nothing fit or no player.
int GiveItem(const char* source, std::int64_t gameItem, int count) {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player || gameItem < 0 || count <= 0) return 0;
    const auto inventory = player + re5::off::kPlayerInventory;
    // A grenade launcher record (0x010C) is made from its explosive-rounds item (MakeItemId).
    const int id = static_cast<int>(MakeItemId(static_cast<std::uint32_t>(gameItem)));
    const std::uint32_t recordId = RecordItemId(static_cast<std::uint32_t>(id));
    std::uintptr_t stack = 0;
    for (int i = 0; i < re5::off::kInventorySlots && !stack; ++i) {
        const auto slot = inventory + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) == 1 && SlotItemId(slot) == recordId) stack = slot;
    }
    using AddFn = int(__thiscall*)(std::uintptr_t inventory, int item, int count);
    using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t inventory, int item, int count, int a, int b, int c,
                                                 int flags, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                                 int a8, int a9, std::uintptr_t player, int position, int zero);
    const auto addToStacks = reinterpret_cast<AddFn>(re5::addr::kFnInventoryAddToStacks);
    const auto create = reinterpret_cast<CreateFn>(re5::addr::kFnInventoryCreate);
    // Weapons (IsWeaponRecord: kind 3, or the flamethrower) do not stack: one record, loaded with
    // a full magazine (the record's count is the ammo in the gun, the flamethrower's fuel).
    const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((id >> 8) & 0xFF) * 4, 0);
    const auto record = table ? table + (id & 0xFF) * 0x48 : 0;
    if (IsWeaponRecord(record, static_cast<std::uint32_t>(id))) {
        if (stack) {
            // Already held (e.g. a build's starting gun the costume carries too): never a second one.
            RM_LOGI("%s: weapon %04X is already held, not created again", source, id);
            return 1;
        }
        if (!WeaponModelLoaded(static_cast<std::uint32_t>(id))) {
            // Its model is not loaded in this match; creating it hangs the game.
            RM_LOGW("%s: weapon %04X is not loaded for this character, not created", source, id);
            return 0;
        }
        // A gun needs a grid cell to be picked: the game files some (the flamethrower) in a cell of
        // their own outside the grid (0xA; seen 2026-11-07: never selectable, its fuel untouched).
        int freeCell = -1;
        {
            const auto grid = ReadGrid();
            for (int c = 0; c < kGridCells && freeCell < 0; ++c) {
                if (!grid[static_cast<std::size_t>(c)].record) freeCell = c;
            }
        }
        if (freeCell < 0) {
            RM_LOGI("%s: weapon %04X not created: no free cell in the grid", source, id);
            return 0;
        }
        const int magazine = std::max(1, ReadOr<std::int32_t>(record + 8, 1));
        const auto made = create(inventory, id, magazine, -1, -1, -1, 0xD, 0, 0, 0, 0, 0, 0, 0, 0, 0, player, 0, 0);
        RM_LOGI("%s: weapon %04X created with %d rounds -> %s", source, id, magazine, made ? "ok" : "no slot");
        // A new record cannot be the back gun yet: a cache pointing at it is a discarded record's.
        ForgetBackGun(made, "a new gun took a discarded record");
        if (made) {
            const int cell = ReadOr<std::int32_t>(made + kSlotCell, -1);
            if (cell < 0 || cell >= kGridCells) {
                SafeStore<std::int32_t>(made + kSlotCell, freeCell);
                RM_LOGI("%s: weapon %04X came in cell %d, outside the grid; moved to cell %d", source, id, cell, freeCell + 1);
            }
        }
        return made ? 1 : 0;
    }
    // Top up existing stacks first; whatever does not fit goes into new slots (one unit each,
    // then topped up to the slot's own maximum), so a full grenade stack or a spray (max 1 per
    // slot) still receives the item while the 24-slot case has room.
    int left = stack ? addToStacks(inventory, id, count) : count;
    const int stacked = count - left;
    int createdSlots = 0;
    for (int attempt = 0; left > 0 && attempt < 8; ++attempt) {
        const auto made = create(inventory, id, 1, -1, -1, -1, 0xD, 0, 0, 0, 0, 0, 0, 0, 0, 0, player, 0, 0);
        if (!made) break;
        ++createdSlots;
        --left;
        if (left > 0) left = addToStacks(inventory, id, left);
    }
    RM_LOGI("%s: item %04X x%d -> %d stacked, %d new slot(s), %d did not fit", source, id, count, stacked,
            createdSlots, left);
    return count - left;
}

// Ammo item for a weapon category (dev itemnames: 0x0201 handgun, 0x0202 machine gun and SMG,
// 0x0203 shells, 0x0204 rifle, 0x0209 magnum).
std::uint32_t AmmoItemFor(rm::WeaponCategory category) {
    switch (category) {
        case rm::WeaponCategory::Shotgun: return 0x0203;
        case rm::WeaponCategory::Smg:
        case rm::WeaponCategory::MachineGun: return 0x0202;
        case rm::WeaponCategory::Rifle: return 0x0204;
        case rm::WeaponCategory::Magnum: return 0x0209;
        case rm::WeaponCategory::Bow: return 0x020B;
        case rm::WeaponCategory::Launcher: return 0x0206;  // the grenade launcher's explosive rounds
        case rm::WeaponCategory::Melee:
        case rm::WeaponCategory::Flamethrower: return 0;  // none
        default: return re5::addr::kItemHandgunAmmo;
    }
}

std::uint32_t PartnerAmmoSwap(std::uint32_t id);  // with the partner, below

// The ammo for the gun in the player's hands (Sheva's Scavenger: drops become it); 0 for one
// without (knife, stun rod, flamethrower, the rocket launcher's single rocket). The grenade
// launcher is one item per round type loaded (0x0125.., the Organize table), its rounds 0x0206..
std::uint32_t HeldAmmoItem() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return 0;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1 || ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1) continue;
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        static const std::pair<std::uint32_t, std::uint32_t> kLauncherRounds[] = {
            {0x0125, 0x0206}, {0x0126, 0x0207}, {0x0127, 0x0208}, {0x0139, 0x020E}, {0x013A, 0x020F}, {0x013B, 0x0210}};
        for (const auto& [launcher, rounds] : kLauncherRounds) {
            if (id == launcher) return rounds;
        }
        // The launcher's own record (RecordItemId): the rounds it is made with.
        if (id == kItemGrenadeLauncher) return 0x0206;
        if (id == 0x010D) return 0;  // the rocket launcher: one rocket, never reloaded
        if (const rm::WeaponBaseDef* w = WeaponForItem(id)) return AmmoItemFor(w->category);
        return 0;
    }
    return 0;
}

// Clears an inventory record the way the game discards an item: the record's weapon object
// (the holstered model) is deleted too. Falls back to wiping the record without the routine.
bool ClearSlotNative(std::uintptr_t slot) {
    using ClearFn = void(__thiscall*)(std::uintptr_t record, int deleteObject);
    __try {
        reinterpret_cast<ClearFn>(re5::addr::kFnInventorySlotClear)(slot, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The inventory's back gun (kInventoryBackGun) is a cached record that the game never forgets: a
// record the mod discards stays cached, and the next item made in it was taken for the gun on the
// back. A gun made there was attached to the back joint every frame, so the hands held nothing
// (2026-11-19, a player's "ada's px4 was bugged": the first gun the mod made in a match took the
// record of the costume's long gun the loadout cut had discarded; build/runs/20261119-100849-invisible-gun).
// Any player whose cache holds `record` forgets it; the game then picks its back gun again.
void ForgetBackGun(std::uintptr_t record, const char* why) {
    if (record == 0) return;
    const engine::PlayersSnapshot players = engine::ReadPlayers();
    for (int i = 0; i < players.count && i < static_cast<int>(players.players.size()); ++i) {
        const std::uintptr_t player = players.players[static_cast<std::size_t>(i)].object;
        if (!player) continue;
        const std::uintptr_t cache = player + re5::off::kPlayerInventory + re5::off::kInventoryBackGun;
        if (ReadOr<std::uintptr_t>(cache, 0) != record) continue;
        SafeStore<std::uintptr_t>(cache, 0);
        RM_LOGI("inventory: player %08X's back gun forgotten (%s, record %08X)", static_cast<unsigned>(player), why,
                static_cast<unsigned>(record));
    }
}

void ClearSlot(std::uintptr_t slot) {
    ForgetBackGun(slot, "the record was discarded");
    if (engine::Caps().slotClear && ClearSlotNative(slot)) return;
    SafeStore<std::int32_t>(slot + re5::off::kSlotCount, 0);
    SafeStore<std::uint8_t>(slot + re5::off::kSlotValid, 0);
}

bool TakeOutOfHand(std::uintptr_t player, std::uintptr_t slot, int cell);

// Every run starts with the pistol only: the character's own Mercenaries loadout is cut down
// to the pistol, its ammo and the knife (records invalidated, which frees their grid slots).
void StripToPistol() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return;
    const auto inv = player + re5::off::kPlayerInventory;
    // The gun in the player's hands stays whatever it is (removing the equipped weapon's
    // record hangs the game: S.T.A.R.S. Chris and his Samurai Edge); every other weapon,
    // ammo (except the pistol's rounds), grenade and spray goes. A knife in hand is no gun:
    // S.T.A.R.S. Wesker starts with his (0x0123) and kept only it, his Samurai Edge removed (user
    // playtest 2026-11-07).
    std::uint32_t equipped = 0;  // the gun in hand, if one is
    std::uint32_t held = 0;      // whatever is in hand
    std::uintptr_t equippedSlot = 0;
    std::uint32_t handgun = 0;   // the loadout's handgun out of hand, if it has one
    int handgunCell = -1;
    const auto isHandgun = [](std::uint32_t id) {
        const rm::WeaponBaseDef* w = WeaponForItem(id);
        return w != nullptr && w->category == rm::WeaponCategory::Handgun;
    };
    std::string loaded;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        if (ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) == 1) {
            held = id;
            if ((id >> 8) == 1 && id != re5::addr::kItemUnarmed && !IsKnifeItem(id)) {
                equipped = id;
                equippedSlot = slot;
            }
        } else if (handgun == 0 && (id >> 8) == 1 && isHandgun(id)) {
            handgun = id;
            handgunCell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
        }
        // Every weapon the game put here has its model loaded for this stage.
        if ((id >> 8) == 1 && id != re5::addr::kItemUnarmed && id != re5::addr::kItemKnife &&
            ReadOr<std::int32_t>(slot + re5::off::kSlotMax, 0) > 0 && (id < 0x0106 || id > 0x0108)) {
            g->loadedWeaponItems.insert(id);
            char buf[8];
            std::snprintf(buf, sizeof(buf), " %04X", id);
            loaded += buf;
        }
    }
    RM_LOGI("inventory: weapons loaded for this character:%s", loaded.c_str());
    RefreshPlaceableWeapons();
    std::int64_t pistol = re5::addr::kItemM92F;
    std::uint32_t ammo = re5::addr::kItemHandgunAmmo;
    if (const rm::WeaponBaseDef* w = g->content->FindWeapon(g->run->state().loadout.weapon_id)) {
        if (w->game_item >= 0) {
            pistol = w->game_item;
            ammo = AmmoItemFor(w->category);
        }
    }
    // A build that starts with a handgun in the hands (Hound Wolf Sheva: her costume is Fairy Tale's,
    // which holds its M3; the user, 2026-11-11: "she also starts with an M3, make her start with a
    // pistol"): the hands take the loadout's own handgun, through the game's own change of weapon,
    // and the other gun goes with the rest of the loadout.
    if (equipped != 0 && !isHandgun(equipped) && g->run->state().character.build.pistol_in_hand) {
        if (handgun != 0 && handgunCell >= 0 && handgunCell < kGridCells && TakeOutOfHand(player, equippedSlot, handgunCell)) {
            RM_LOGI("inventory: the hands took the handgun %04X (cell %d), not %04X: the build starts with a pistol", handgun,
                    handgunCell + 1, equipped);
            equipped = handgun;
            held = handgun;
        } else {
            RM_LOGW("inventory: the build starts with a pistol, but %04X stays in the hands (%s)", equipped,
                    handgun == 0 ? "no handgun in this costume's loadout" : "the change of weapon did not take");
        }
    }
    if (equipped != 0) {
        pistol = equipped;  // the hands win over the roll
        if (const rm::WeaponBaseDef* w = WeaponForItem(equipped)) {
            ammo = AmmoItemFor(w->category);
            const rm::Status swapped = g->run->SwapStartingWeapon(w->id);
            if (!swapped.ok()) RM_LOGW("inventory: starting weapon swap to %s failed: %s", w->id.c_str(), swapped.message.c_str());
        }
    }
    int removed = 0;
    bool havePistol = false;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        const bool isEquipped = ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) == 1;
        if (id == static_cast<std::uint32_t>(pistol)) havePistol = true;
        const bool keep = isEquipped || id == re5::addr::kItemUnarmed || IsKnifeItem(id) || id == ammo ||
                          id == static_cast<std::uint32_t>(pistol);
        if (keep) continue;
        ClearSlot(slot);
        ++removed;
    }
    if (!havePistol && equipped == 0) {
        // No gun in hand and no pistol: give the rolled one (magazine from the item record).
        const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((pistol >> 8) & 0xFF) * 4, 0);
        const int magazine = table ? std::max(1, ReadOr<std::int32_t>(table + (pistol & 0xFF) * 0x48 + 8, 1)) : 1;
        GiveItem("pistol", pistol, magazine);
    }
    RM_LOGI("inventory: stripped to the pistol %04X (in hand %04X%s, %d record(s) removed%s)", static_cast<unsigned>(pistol), held,
            held != 0 && equipped == 0 ? ", no gun" : "", removed, havePistol || equipped ? "" : ", pistol created");
}

// The rolled loadout's equipment, at the first arena. Items without a game id stay run-only.
void ApplyLoadoutToInventory() {
    if (!engine::Caps().inventory || !engine::LocalPlayer()) {
        RM_LOGW("loadout: inventory not available; items stay run-only");
        return;
    }
    StripToPistol();
    for (const rm::LoadoutItem& item : g->run->state().loadout.items) {
        if (item.game_item >= 0) GiveItem(("loadout " + item.id).c_str(), item.game_item, item.count);
    }
}

// Items perks earned since the last tick.
constexpr double kStageChangeTimeoutSeconds = 75.0;
constexpr double kInventorySettleSeconds = 1.5;

std::uintptr_t GameWorkPtr() {
    return ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
}

// Every used slot record as (item id, count), in slot order.
std::vector<std::pair<std::uint32_t, int>> ReadInventory() {
    std::vector<std::pair<std::uint32_t, int>> out;
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return out;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        out.emplace_back(SlotItemId(slot), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0));
    }
    return out;
}

bool IsWeaponItem(std::uint32_t id) { return IsWeaponRecord(ItemRecord(id), id); }

// The flamethrower's fuel (its record's count, up to its magazine of 100) comes back by itself in
// a run: Mercenaries has no gas pumps to refill it at (the user, 2026-11-07). After it has not
// been fired for kRefuelDelay, kRefuelPerSecond a second.
constexpr std::uint32_t kItemFlamethrower = 0x0121;
// Halved 2026-11-11 (the user: "flamethrower need to be nerfed (the tank regeneration)"): 1.5 s and
// 5 a second (empty to full in 20 s) before. The wait back to 1.5 s the same day (the user: "make
// flamethrower wait timer shorter, its too long"); the rate stays halved.
constexpr double kRefuelDelay = 1.5;
constexpr double kRefuelPerSecond = 2.5;  // empty to full in 40 s

// The flamethrower (uWp21) binds its tank and hose to the player's back every frame, whatever is
// in hand (its update, 0x00BA4FF0; user playtest 2026-11-08: "it shows the model on the character at
// all times, even when it has some other weapon on"). Out of hand its model is scaled down to
// nothing, in hand back to its own size: uCoord's scale (re5::off::kUnitScale), written only where
// it reads as the unit's own (the same on all three axes, kOwnScaleMin..kOwnScaleMax) or as the mod
// left it, and again if the game sets it back. Its own size is the character's: 1 on Chris, 0.85 on
// Sheva (adapter.log 2026-11-11, every unit on Sheva; the mod took only 1 then and left hers in
// sight, the user: "flamethrower still has it on hand when not having it equpied").
constexpr float kHiddenScale = 0.001f;
constexpr float kOwnScaleMin = 0.25f;
constexpr float kOwnScaleMax = 4.0f;

std::uintptr_t EquippedGunSlot();

std::uintptr_t g_flameUnit = 0;  // the local player's flamethrower unit (uWp21), once found
// Its scan and scale (per player: SeatTurn swaps them).
struct FlameHideState {
    double lastScan = -10.0;
    bool refused = false;
    float own = 0.0f;    // its scale in hand, as the game set it (0: not seen yet)
    int lastState = -1;  // logged: 0 shown, 1 hidden
    int rewrites = 0;
};
FlameHideState g_flameHide;
// Its refuel (per player).
struct RefuelState {
    int lastCount = -1;
    double lastFiredAt = 0.0, lastTickAt = 0.0, owed = 0.0;
};
RefuelState g_refuel;

void HideHolsteredFlamethrower(double now) {
    std::uintptr_t& unit = g_flameUnit;
    double& lastScan = g_flameHide.lastScan;
    bool& refused = g_flameHide.refused;
    float& own = g_flameHide.own;
    int& lastState = g_flameHide.lastState;
    int& rewrites = g_flameHide.rewrites;
    const auto player = engine::LocalPlayer();
    if (!player || !g->match.inMatch) {
        unit = 0;
        refused = false;
        own = 0.0f;
        lastState = -1;
        rewrites = 0;
        return;
    }
    // Its unit, among the player's pointers (looked for once a second: it comes with the first equip).
    if (now - lastScan >= 1.0) {
        lastScan = now;
        for (std::size_t off = 0; off < 0x4000; off += 4) {
            const auto p = ReadOr<std::uintptr_t>(player + off, 0);
            if (p < 0x00400000 || p > 0x7FFF0000 || (p & 3)) continue;
            if (ReadOr<std::uintptr_t>(p, 0) == re5::addr::kVtableWp21) {
                if (p != unit) {
                    RM_LOGI("flamethrower: its unit %08X (player +%zX)", static_cast<unsigned>(p), off);
                    refused = false;
                    own = 0.0f;
                    lastState = -1;
                    rewrites = 0;
                }
                unit = p;
                break;
            }
        }
    }
    if (unit == 0 || refused) return;
    if (ReadOr<std::uintptr_t>(unit, 0) != re5::addr::kVtableWp21) {
        unit = 0;  // gone
        lastState = -1;
        return;
    }
    const std::uintptr_t slot = EquippedGunSlot();
    const bool hide = !(slot && ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) == kItemFlamethrower);
    float scale[3];
    for (int k = 0; k < 3; ++k) scale[k] = ReadOr<float>(unit + re5::off::kUnitScale + k * 4, -1.0f);
    const bool uniform = std::fabs(scale[0] - scale[1]) < 0.0005f && std::fabs(scale[0] - scale[2]) < 0.0005f;
    const bool hidden = uniform && std::fabs(scale[0] - kHiddenScale) < 0.0005f;
    if (!uniform || (!hidden && (scale[0] < kOwnScaleMin || scale[0] > kOwnScaleMax))) {
        refused = true;  // not a scale this layout expects: left as it is
        RM_LOGW("flamethrower: its unit %08X reads %.4f %.4f %.4f at +0x50, not its scale; its model is left as it is",
                static_cast<unsigned>(unit), scale[0], scale[1], scale[2]);
        return;
    }
    if (!hidden && std::fabs(scale[0] - own) >= 0.0005f) {
        RM_LOGI("flamethrower: its own scale %.3f", scale[0]);
        own = scale[0];  // the game's (the character's), first seen or set again
    }
    if (hidden == hide) {
        lastState = hide ? 1 : 0;  // as wanted (the game sets its own scale again itself when it is equipped)
        return;
    }
    if (!hide && own <= 0.0f) {
        refused = true;  // hidden, not by the mod: left as it is
        RM_LOGW("flamethrower: its unit %08X was hidden before its own scale was seen; its model is left as it is",
                static_cast<unsigned>(unit));
        return;
    }
    const float want = hide ? kHiddenScale : own;
    for (int k = 0; k < 3; ++k) SafeStore<float>(unit + re5::off::kUnitScale + k * 4, want);
    if (lastState == (hide ? 1 : 0)) {
        if (++rewrites == 30) RM_LOGW("flamethrower: the game keeps setting its scale back; written again each frame");
        return;
    }
    lastState = hide ? 1 : 0;
    RM_LOGI("flamethrower: %s", hide ? "out of hand, its model hidden" : "in hand, its model shown");
}

void RefuelFlamethrower(double now) {
    int& lastCount = g_refuel.lastCount;
    double& lastFiredAt = g_refuel.lastFiredAt;
    double& lastTickAt = g_refuel.lastTickAt;
    double& owed = g_refuel.owed;
    const auto player = engine::LocalPlayer();
    if (!player || !g->match.inMatch || !g->run->HasActiveRun() || g->run->phase() != rm::Phase::ArenaActive) {
        lastCount = -1;
        return;
    }
    if (now - lastTickAt < 0.5) return;
    const double dt = std::min(1.0, now - lastTickAt);
    lastTickAt = now;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) != kItemFlamethrower) continue;
        const int count = ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0);
        const auto record = ItemRecord(kItemFlamethrower);
        const int full = std::max(1, record ? ReadOr<std::int32_t>(record + 8, 100) : 100);
        if (lastCount >= 0 && count < lastCount) {
            lastFiredAt = now;
            owed = 0.0;
        }
        lastCount = count;
        if (count >= full || now - lastFiredAt < kRefuelDelay) return;
        owed += kRefuelPerSecond * g->run->state().character.build.fuel_regen_mult * dt;
        const int add = std::min(full - count, static_cast<int>(owed));
        if (add <= 0) return;
        owed -= add;
        SafeStore<std::int32_t>(slot + re5::off::kSlotCount, count + add);
        lastCount = count + add;
        if (count + add >= full) RM_LOGI("flamethrower: fuel back to %d", full);
        return;
    }
    lastCount = -1;
}

// The rounds in a held gun (its record's count).
void SetWeaponRounds(std::uint32_t id, int rounds) {
    const auto player = engine::LocalPlayer();
    if (!player) return;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (SlotItemId(slot) != RecordItemId(id)) continue;
        const int before = ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0);
        if (before != rounds) {
            SafeStore<std::int32_t>(slot + re5::off::kSlotCount, rounds);
            RM_LOGI("carried: weapon %04X rounds %d -> %d", id, before, rounds);
        }
        return;
    }
}

// Before the match ends for the next stage: remember every item and count.
void SnapshotInventory() {
    g->transition.carry = ReadInventory();
    const auto grid = ReadGrid();
    for (std::size_t c = 0; c < grid.size(); ++c) {
        g->transition.cells[c] = grid[c].record ? grid[c].id : 0;
        g->transition.counts[c] = grid[c].record ? grid[c].count : 0;
    }
    RecordWeaponLevels();
    std::string text;
    for (const auto& [id, count] : g->transition.carry) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s%04X x%d", text.empty() ? "" : ", ", id, count);
        text += buf;
    }
    RM_LOGI("stage change: inventory snapshot (%zu records): %s", g->transition.carry.size(), text.c_str());
}

int KeepInRunStash(std::uint32_t id, int count, const char* why);
// The adapter's part of a run's resume point (defined with the run's storage).
std::string AdapterResumeState();
bool RestoreAdapterResumeState(const std::string& text);

static_assert(rm::kGridCellCount == kGridCells, "the layout planner's grid is the game's");
constexpr int kCellKnife = 9, kCellOutside = 0xA, kCellUnarmed = 0xB;

bool CallEquipCell(std::uintptr_t player, int cell) {
    __try {
        reinterpret_cast<void(__thiscall*)(std::uintptr_t, int, int)>(re5::addr::kFnPlayerEquipCell)(player, cell, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The hands let go of `slot`, through the game's own change of weapon, to `cell`. True when they did.
bool TakeOutOfHand(std::uintptr_t player, std::uintptr_t slot, int cell) {
    if (!engine::Caps().equipCell) return false;
    SafeStore<std::int32_t>(player + re5::off::kPlayerWeaponCell, cell);
    if (!CallEquipCell(player, cell)) return false;
    return ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1;
}

// A new stack record of one (GiveItem would top up a stack that is there: a split wants its own).
std::uintptr_t MakeStackRecord(std::uintptr_t player, std::uint32_t id) {
    using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t inventory, int item, int count, int a, int b, int c,
                                                 int flags, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                                 int a8, int a9, std::uintptr_t player, int position, int zero);
    const std::uintptr_t made = reinterpret_cast<CreateFn>(re5::addr::kFnInventoryCreate)(
        player + re5::off::kPlayerInventory, static_cast<int>(MakeItemId(id)), 1, -1, -1, -1, 0xD, 0, 0, 0, 0, 0, 0, 0, 0, 0, player, 0, 0);
    ForgetBackGun(made, "a new stack took a discarded record");
    return made;
}

std::uintptr_t FindRecord(std::uintptr_t player, std::uint32_t id) {
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) == 1 && SlotItemId(slot) == RecordItemId(id)) return slot;
    }
    return 0;
}

// The inventory becomes the snapshot's layout, cell by cell (rm::PlanGrid, tests/test_inventory_plan.cpp):
// after the Organize screen, and in a new match, where the game has recreated the character's own
// loadout. Item totals could not say which record a change was about: a sold or stored gun that was
// in the hands stayed in its cell beside what took that cell (clearing a held record hangs the game),
// and a split stack stayed one (the user, 2026-11-09: "weird stuff when you sell or even when you put
// stuff back"). What is sold or stored leaves the hands first, through the game's own weapon change.
// What cannot be made (a gun whose model is not loaded here) waits in the run's storage (ORGANIZE).
void ApplyCarriedInventory() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return;
    RefreshPlaceableWeapons();
    rm::GridLayout layout{};
    bool anyCell = false;
    for (std::size_t c = 0; c < layout.size(); ++c) {
        const std::uint32_t id = g->transition.cells[c];
        if (id == 0) continue;
        layout[c] = rm::WantedCell{id, g->transition.counts[c], IsWeaponItem(id)};
        anyCell = true;
    }
    if (!anyCell) {
        // A resume point from before the layouts were saved: the items in order, the rest to the storage.
        std::size_t next = 0;
        for (const auto& [id, count] : g->transition.carry) {
            if (id == 0 || id == re5::addr::kItemUnarmed || IsKnifeItem(id)) continue;
            if (next < layout.size()) layout[next++] = rm::WantedCell{id, count, IsWeaponItem(id)};
            else KeepInRunStash(id, std::max(0, count), "did not fit after the stage change");
        }
    }

    std::vector<rm::GridRecord> records;
    std::vector<std::uintptr_t> slots;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        records.push_back(rm::GridRecord{static_cast<int>(slots.size()), SlotItemId(slot), ReadOr<std::int32_t>(slot + kSlotCell, -1),
                                         ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0),
                                         ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) == 1});
        slots.push_back(slot);
    }
    const rm::GridPlan plan = rm::PlanGrid(records, layout);
    const auto goes = [&plan](int handle) {
        return std::find(plan.clear.begin(), plan.clear.end(), handle) != plan.clear.end() ||
               std::find(plan.out_of_hand.begin(), plan.out_of_hand.end(), handle) != plan.out_of_hand.end();
    };
    for (const int handle : plan.out_of_hand) {
        // Another gun that stays, else the knife, else bare hands.
        int hands = -1;
        for (const rm::GridRecord& r : records) {
            if (hands >= 0) break;
            if (r.handle != handle && !goes(r.handle) && r.cell >= 0 && r.cell < kGridCells && IsWeaponItem(r.item) && !IsKnifeItem(r.item)) hands = r.cell;
        }
        if (hands < 0) {
            hands = kCellUnarmed;
            for (const rm::GridRecord& r : records) {
                if (r.cell == kCellKnife) hands = kCellKnife;
            }
        }
        const std::uintptr_t slot = slots[static_cast<std::size_t>(handle)];
        const std::uint32_t id = SlotItemId(slot);
        if (TakeOutOfHand(player, slot, hands)) {
            ClearSlot(slot);
            RM_LOGI("carried: %04X left the hands for cell %d and is gone", id, hands + 1);
        } else {
            // Never cleared while held. Out of the grid so the layout is exact; it goes with this match.
            SafeStore<std::int32_t>(slot + kSlotCell, kCellOutside);
            RM_LOGW("carried: %04X is still in the hands; out of the grid until they let go of it", id);
        }
    }
    for (const int handle : plan.clear) {
        const std::uintptr_t slot = slots[static_cast<std::size_t>(handle)];
        RM_LOGI("carried: %04X x%d gone", SlotItemId(slot), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0));
        ClearSlot(slot);
    }
    for (const auto& [handle, cell] : plan.move) SafeStore<std::int32_t>(slots[static_cast<std::size_t>(handle)] + kSlotCell, cell);
    for (const auto& [handle, count] : plan.set_count) SafeStore<std::int32_t>(slots[static_cast<std::size_t>(handle)] + re5::off::kSlotCount, count);
    int made = 0;
    for (const auto& [cell, what] : plan.create) {
        std::uintptr_t record = 0;
        if (what.weapon) {
            if (GiveItem("carried", what.item, 1) > 0) record = FindRecord(player, what.item);
        } else {
            record = MakeStackRecord(player, what.item);
        }
        if (!record) {
            KeepInRunStash(what.item, std::max(0, what.count), "could not be made in its cell");
            continue;
        }
        SafeStore<std::int32_t>(record + kSlotCell, cell);
        SafeStore<std::int32_t>(record + re5::off::kSlotCount, what.count);
        ++made;
    }
    for (const rm::WantedCell& extra : plan.surplus) KeepInRunStash(extra.item, std::max(0, extra.count), "is a second of a gun");
    RM_LOGI("inventory: layout applied: %zu gone, %zu left the hands, %zu moved, %zu recounted, %d made of %zu, %zu to the storage", plan.clear.size(),
            plan.out_of_hand.size(), plan.move.size(), plan.set_count.size(), made, plan.create.size(), plan.surplus.size());
    g->transition.carry.clear();
    g->transition.cells = {};
    g->transition.counts = {};
    SyncHandCell();
    ApplyWeaponLevels();
    // Two records in a cell are what this replaced: said if it ever comes back.
    std::array<int, kGridCells> inCell{};
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const int cell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
        if (cell >= 0 && cell < kGridCells && ++inCell[static_cast<std::size_t>(cell)] == 2) RM_LOGW("inventory: cell %d holds two records", cell + 1);
    }
}

// Health carries over a stage change (a new match starts at full health).
void RestoreCarriedHp() {
    const int wanted = g->transition.hp;
    g->transition.hp = -1;
    const auto local = engine::LocalPlayer();
    const auto hp = local ? engine::ReadHp(local) : std::nullopt;
    if (wanted <= 0 || !hp || wanted >= hp->hp) return;
    engine::WriteHp(local, wanted);
    RM_LOGI("carried: health %d -> %d/%d", hp->hp, wanted, hp->max);
}


// Route chosen: end this match through the game's own time-up, accept its results screen
// behind the fader and let the title-flow script restart the match on another stage.
void StartStageTransition(double now) {
    ForEachSeatTurn([] {
        SnapshotInventory();
        g->transition.hp = -1;
        if (const auto local = engine::LocalPlayer()) {
            if (const auto hp = engine::ReadHp(local)) g->transition.hp = hp->hp;
        }
    });
    // The point the run goes on from after a crash or a quit (2026-11-07): the run between arenas
    // and what the next arena starts with.
    Report(g->run->SaveResumePoint(AdapterResumeState()), "resume point");
    const auto gw = GameWorkPtr();
    const int costume = gw ? std::max(0, ReadOr<std::int32_t>(gw + re5::off::kGameWorkCharacter, 0)) : 0;
    // The chosen run map node's arena (M5.3); a random one without (or with dev "nextmap").
    const int planned = g->run->PlannedMapKey();
    const int stage = g_forcedNextMap >= 0 || planned < 0 ? PickMap(g->currentStage) : planned;
    g->transition.pending = true;
    g->transition.generation = g->matchGeneration;
    g->transition.startedAt = now;
    RM_LOGI("stage change: stage %d -> %d (costume slot %d, %s), ending the match", g->currentStage, stage, costume,
            stage == planned ? "run map" : "random pick");
    ArmMap(stage, costume);
    g->arenaEndRequested = true;
    RequestMercMatchEnd();
}

// Into the grid, and what does not fit into the run's storage (ORGANIZE), to be taken out at a
// later shop on any stage: the shop refunded a Flash Grenade, a gun and Magnum Ammo with no room
// for them and a reward herb was paid out as supplies (user playtests 2026-11-05 and 11-07; the
// user's wish: what does not fit waits in the storage). A gun goes in with a full magazine.
// Returns how many the player has now, in the grid or the storage.
int GiveOrStash(const char* source, std::int64_t id, int count) {
    if (id <= 0 || count <= 0) return 0;
    const int placed = GiveItem(source, id, count);
    if (placed >= count) return placed;
    const auto item = static_cast<std::uint32_t>(id);
    if (IsWeaponItem(item)) {
        if (placed > 0) return placed;
        const auto record = ItemRecord(item);
        const int magazine = record ? std::max(1, ReadOr<std::int32_t>(record + 8, 1)) : 1;
        return KeepInRunStash(item, magazine, "did not fit") > 0 ? 1 : 0;
    }
    return placed + KeepInRunStash(item, count - placed, "did not fit");
}

void ApplyItemGrants() {
    for (const rm::ItemGrant& grant : g->run->ConsumeItemGrants()) {
        const bool fromPerk = grant.source != "reward" && grant.source != "shop";
        const std::string label = (fromPerk ? "perk " : "") + grant.source;
        if (GiveOrStash(label.c_str(), grant.game_item, grant.count) == 0) {
            g->toasts.Push(label + ": inventory and storage full");
        }
    }
}

// Rewards and shop purchases place their item at once, in the grid or the run's storage; the
// core fails the purchase (and refunds it) only when neither has room.
bool RunStashHolds(std::uint32_t recordId);  // with the run's storage, below
bool RunInItemBox();

class InventoryBridge final : public rm::InventoryPort {
public:
    int Give(std::int64_t gameItem, int count, std::string_view source) override {
        if (!g->match.inMatch) return 0;
        return GiveOrStash(std::string(source).c_str(), gameItem, count);
    }
    // In the grid (any record) or the run's storage. Unknown while no player is in a match or the
    // run sits in the Organize screen (the save data holds it then).
    std::optional<bool> Holds(std::int64_t gameItem) override {
        if (gameItem < 0 || !g->match.inMatch || !engine::Caps().inventory || !engine::LocalPlayer() || RunInItemBox()) return std::nullopt;
        const std::uint32_t record = RecordItemId(static_cast<std::uint32_t>(gameItem));
        for (const auto& [id, count] : ReadInventory()) {
            if (id == record) return true;
        }
        return RunStashHolds(record);
    }
};
InventoryBridge g_inventoryBridge;

// A one-use gun the run owns (the rocket launcher: weapons.json "restocks") is back at the start of
// every arena, in the grid or the run's storage (the user, 2026-11-10: "you get one per stage once
// you buy it"). Selling it in ORGANIZE gives it up (TakeRunFromSave).
void RestockWeapons() {
    for (const std::int64_t item : g->run->RestockWeaponItems()) {
        if (g_inventoryBridge.Holds(item).value_or(true)) continue;
        const int placed = GiveOrStash("restock", item, 1);
        const std::string name = GridItemName(static_cast<std::uint32_t>(item));
        RM_LOGI("restock: %04X %s", static_cast<unsigned>(item), placed > 0 ? "back for this arena" : "no room in the grid or the storage");
        g->toasts.Push(name + (placed > 0 ? " is back for this arena" : ": no room for it this arena"));
    }
}

constexpr float kStartTolerance = 1000.0f;  // engine units (10 m) from the map's start
constexpr float kStartLevel = 40.0f;        // or another level there (s204: the boat under the ledge)

// The skin build's maximum health (M8: Barry, Midnight Wesker) times the health a run gained
// (Excella). Applied at an arena's start (health then full; a stage change restores the carried
// health after this) and whenever the gains change: a rise also adds to the current health.
void ApplyBuildHealth() {
    const auto& state = g->run->state();
    const double mult = state.character.build.max_health_mult * (1.0 + state.stats.max_health);
    const auto local = engine::LocalPlayer();
    const auto hp = local ? engine::ReadHp(local) : std::nullopt;
    // Every tick during an arena: never a player still loading (no maximum) or a dead one.
    if (!hp || hp->max <= 0 || hp->hp <= 0) return;
    BuildHealthState& b = g_buildHealth;
    if (b.generation != g->matchGeneration) b = {g->matchGeneration, hp->max, hp->max};
    const int target = std::clamp(static_cast<int>(std::lround(b.baseMax * mult)), 1, 32767);
    if (target == b.appliedMax) return;
    if (b.appliedMax == b.baseMax && hp->max == b.baseMax) {
        if (!engine::SetHealth(local, target)) return;  // the match's first change: full health
    } else {
        const int rise = std::max(0, target - b.appliedMax);
        SafeStore<std::int16_t>(local + re5::off::kMaxHp, static_cast<std::int16_t>(target));
        SafeStore<std::int16_t>(local + re5::off::kHp, static_cast<std::int16_t>(std::min(target, hp->hp + rise)));
    }
    RM_LOGI("build: maximum health %d -> %d (x%.2f)", b.appliedMax, target, mult);
    b.appliedMax = target;
}

// A kill's build effects in the HUD's top line (keys, Excella's stats).
void BuildKillNotice(const rm::KillOutcome& outcome, double now) {
    const rm::RunState& state = g->run->state();
    if (outcome.key_dropped) {
        g->notice = "KEY FOUND   " + Upper(Plural(state.keys, "key"));
        g->noticeUntil = now + 3.0;
        RM_LOGI("keys: a key dropped, %d held", state.keys);
    }
    if (!outcome.stat_gained.empty()) {
        static const std::map<std::string, std::string> names = {
            {"damage", "DAMAGE"}, {"defense", "DEFENSE"}, {"max_health", "HEALTH"}, {"crit", "CRITICALS"},
            {"move_speed", "SPEED"}, {"heal", "HEALING"}, {"supplies", "SUPPLIES"}};
        const auto it = names.find(outcome.stat_gained);
        const std::string stat = it != names.end() ? it->second : Upper(outcome.stat_gained);
        g->notice = Upper(Plain(state.character.build.name)) + "   " + stat + " UP " +
                    std::to_string(static_cast<int>(std::lround(outcome.stat_amount * 100.0))) + " PERCENT";
        g->noticeUntil = now + 3.0;
        RM_LOGI("build: melee kill, %s +%.2f (%d gains)", outcome.stat_gained.c_str(), outcome.stat_amount, state.stats.gains);
    }
}

// M6.2: a campaign area starts the player at one of its entrances at random; the map's start is
// where its points, chests and boss are, so the player goes there. Tried on the match's first
// tick (under the loading fade, so it does not show as a teleport) and again when the arena
// begins. True when there is nothing more to do.
bool MoveToMapStart(const char* when) {
    const rm::MapDef* map = CurrentMap();
    if (map == nullptr || !map->start) return true;
    const auto local = engine::LocalPlayer();
    if (!local) return false;
    const float to[3] = {static_cast<float>((*map->start)[0]), static_cast<float>((*map->start)[1]),
                         static_cast<float>((*map->start)[2])};
    const float dx = ReadOr<float>(local + re5::off::kUnitPosition, 0.0f) - to[0];
    const float dy = ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f) - to[1];
    const float dz = ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f) - to[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= kStartTolerance && std::fabs(dy) <= kStartLevel) return true;
    const int copies = engine::WarpUnit(local, to);
    if (copies == 0) {
        // Not placed by the game yet (or not safely movable): tried again on the next tick.
        static int waitedGeneration = -1;
        if (waitedGeneration != g->matchGeneration) {
            waitedGeneration = g->matchGeneration;
            RM_LOGI("%s: the player is not placed yet (%.0f %.0f %.0f); moving to the map's start later", when,
                    to[0] + dx, to[1] + dy, to[2] + dz);
        }
        return false;
    }
    RM_LOGI("%s: player %.1f m from the map's start (%.1f m %s); moved there (%d copies)", when, distance / 100.0f,
            std::fabs(dy) / 100.0f, dy < 0.0f ? "below" : "above", copies);
    return true;
}
int g_startMovedGeneration = -1;

// An arena began: the director's grace starts, and the HUD names the map and the route (or the
// boss) for a few seconds (M5.3).
int g_arenaGeneration = -1;        // the match the arena began in (-1: none watched)
double g_arenaMatchGoneSince = -1.0;

void StopBossWatch();  // below, with the developer watch

void ArenaBegan(double now) {
    g->arenaBeganAt = now;
    g_arenaGeneration = g->matchGeneration;
    g_arenaMatchGoneSince = -1.0;
    g->boss = {};
    StopBossWatch();
    ApplyBuildHealth();
    ApplyBuildStacks();
    {
        const rm::SkinBuild& build = g->run->state().character.build;
        world::DropRules drops;
        drops.all = static_cast<int>(build.drops_as);
        if (build.drops_as_held_ammo) drops.heldAmmo = &HeldAmmoItem;
        drops.ownAmmo = &PartnerAmmoSwap;  // acts only while the mod's partner is out (Josh's backup)
        for (const auto& [from, to] : build.pickup_as) {
            drops.swaps.emplace_back(static_cast<std::uint16_t>(from), static_cast<std::uint16_t>(to));
        }
        world::SetDropRules(drops);
    }
    const rm::RunSnapshot s = g->run->Snapshot(now);
    const rm::MapDef* map = CurrentMap();
    MoveToMapStart("arena start");
    std::string line = map != nullptr ? Upper(Plain(map->name)) : std::string();
    if (map != nullptr && map->boss) line += "   BOSS " + Upper(Plain(map->boss->name));
    else if (!s.route_name.empty()) line += (line.empty() ? "" : "   ") + Upper(Plain(s.route_name));
    // The run-planned guests the director fields here (M3.3): named after the arena.
    std::vector<std::string> guests;
    std::string guestLog;
    if (const rm::Director* d = g->run->director()) {
        for (const rm::MapEnemy& e : d->map().roster) {
            if (e.home < 0) continue;
            guests.push_back(e.unit_class);
            guestLog += (guestLog.empty() ? ", guests " : ", ") + e.unit_class + " from " + std::to_string(e.home);
        }
    }
    const std::string from = rm::GuestHomes(*g->content, guests);
    g->noticeNext = from.empty() ? std::string() : "WITH ENEMIES FROM " + Upper(Plain(from));
    // A build whose guns take a random element each arena (Field Trials) names it next.
    if (!s.arena_element.empty()) {
        const rm::SkinBuild& build = g->run->state().character.build;
        const std::string trial = Upper(Plain(build.name)) + "   " + Upper(Plain(s.arena_element)) + " GUNS";
        g->noticeNext = g->noticeNext.empty() ? trial : trial + "   " + g->noticeNext;
        RM_LOGI("arena %d: the build's element is %s", s.arena, s.arena_element.c_str());
    }
    if (line.empty()) return;
    g->notice = line;
    g->noticeUntil = now + 5.0;
    RM_LOGI("arena %d: %s (stage %d, enemy threat %d%s)", s.arena, line.c_str(), CurrentMapKey(), g->run->ArenaThreatLevel(),
            guestLog.c_str());
}

// An arena's match the mod did not end: left (the pause menu's quit) or started over (its
// retry). The run is abandoned and settled like any other end. Before, it stayed active and the
// next START RUN went on with the same arena on another stage, with its enemies and chests from
// the first (user playtest 2026-11-05: Chris's arena 3 on stages 505, 105 and 407, the last time
// as Jill, whose death then ended Chris's run). A death, a cleared arena or a finished run leave
// the arena phase before their match ends, and stage changes happen between arenas.
constexpr double kArenaMatchGoneSeconds = 1.0;
constexpr double kSaveGuardAfterMatch = 30.0;  // seconds the save stays held after a run's match

void WatchArenaMatch(double now) {
    rm::RunController& run = *g->run;
    if (run.phase() != rm::Phase::ArenaActive) g_arenaGeneration = -1;  // set again by ArenaBegan
    if (g_arenaGeneration < 0) {
        g_arenaMatchGoneSince = -1.0;
        return;
    }
    if (!g->match.inMatch) {
        if (g_arenaMatchGoneSince < 0.0) g_arenaMatchGoneSince = now;
    } else {
        g_arenaMatchGoneSince = -1.0;
    }
    const bool restarted = g->match.inMatch && g->matchGeneration != g_arenaGeneration;
    const bool left = g_arenaMatchGoneSince >= 0.0 && now - g_arenaMatchGoneSince > kArenaMatchGoneSeconds;
    if (!restarted && !left) return;
    g_arenaGeneration = -1;
    // Suspended when it has a point to go on from (CONTINUE RUN plays this arena again), else
    // abandoned (its first arena: nothing to go back to).
    const int arena = run.state().arena_index;
    const rm::Status suspended = run.SuspendRun(now);
    if (suspended.ok()) {
        RM_LOGI("run: arena %d's match %s without the mod (the pause menu's %s?); the run is suspended (CONTINUE RUN)", arena,
                restarted ? "started over" : "was left", restarted ? "retry" : "quit");
        g->transition = {};
        g->matchUiOpenRequested = false;
        g_enemyTracks.clear();
        autopilot::Disarm("run suspended");
        RestoreBuildStacks();
        world::SetDropRules({});
        autopilot::ReturnToTitle(true);
        nativemenu::OpenOnPlayMenu(nativemenu::ScreenId::Main);  // CONTINUE RUN at its top
        host::SetStageOverride(-1);
        g->toasts.Push("Run suspended: CONTINUE RUN goes on from arena " + std::to_string(arena));
        return;
    }
    RM_LOGI("run: arena %d's match %s without the mod (the pause menu's %s?); the run is abandoned (%s)", arena,
            restarted ? "started over" : "was left", restarted ? "retry" : "quit", suspended.message.c_str());
    Report(run.AbandonRun(now), "abandon run (its match was left)");
    g->toasts.Push("Run abandoned: its match was quit or restarted");
}

void TryBeginArena(double now) {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || !g->match.inMatch) return;
    const rm::Phase phase = run.phase();
    const bool waitingFirst = phase == rm::Phase::Idle && run.state().arena_index == 0;
    const bool waitingNext = phase == rm::Phase::RouteSelection && run.state().route_chosen;
    if (!waitingFirst && !waitingNext) return;
    if (!PlaysAs(g->match.localCharacter, run.state().character_id, run.state().skin_id)) {
        static double lastWarn = -100.0;
        if (now - lastWarn > 10.0) {
            lastWarn = now;
            g->toasts.Push("Playing " + g->match.localCharacter + " but the run is for " +
                           run.state().character_id + "; arena not started");
        }
        return;
    }
    // The inventory is complete only once the match object runs (the game adds the
    // character's own loadout after the player appears), so item writes wait for it.
    // The game fills the character's own loadout shortly after its match object starts
    // running, so inventory writes wait a moment longer than the first countdown update.
    static int runningGeneration = -1;
    static double runningSince = 0.0;
    const bool countdownSeen = ReadMercTimer().recent;
    if (countdownSeen && runningGeneration != g->matchGeneration) {
        runningGeneration = g->matchGeneration;
        runningSince = now;
    }
    const bool matchRunning = countdownSeen && now - runningSince >= kInventorySettleSeconds;
    if (waitingNext && g->transition.pending) {
        if (now - g->transition.startedAt > kStageChangeTimeoutSeconds && g->matchGeneration == g->transition.generation) {
            RM_LOGW("stage change: no new match after %.0f s; continuing in the current match",
                    now - g->transition.startedAt);
            g->toasts.Push("Stage change failed; the run continues here");
            g->transition.pending = false;
        } else {
            // Waiting for the match on the next stage.
            if (g->matchGeneration == g->transition.generation || !matchRunning) return;
            if (g->coopLobbyNeeded) {
                // A resumed co-op run's first match: player 2 joined again in its lobby.
                g->coopLobbyNeeded = false;
                const engine::PlayerInfo* two = nullptr;
                const auto players = engine::ReadPlayers();
                for (const auto& p : players.players) {
                    if (p.object && p.slot == 1 && p.control == 0) two = &p;
                }
                if (two != nullptr) {
                    RecordCoopLobby();
                    rm::SeatScope seat(run, 1);
                    const std::string classCharacter = engine::CharacterIdForClass(two->className);
                    if (!PlaysAs(classCharacter, run.state().character_id, run.state().skin_id)) {
                        RM_LOGW("co-op: player 2 plays %s but their seat is %s", classCharacter.c_str(), run.state().character_id.c_str());
                    }
                } else {
                    RM_LOGW("co-op: the resumed run's match has no second player (players %d)", players.count);
                    g->toasts.Push("Rogue Mercenaries: player 2 is not in this match");
                }
            }
            const rm::Status status = run.BeginArena(now);
            Report(status, "begin arena");
            g->arenaEndRequested = false;
            if (status.ok()) {
                ArenaBegan(now);
                ForEachSeatTurn([] {
                    ApplyBuildHealth();  // (player 1's: done by ArenaBegan) before the carried health
                    ApplyCarriedInventory();
                    RestockWeapons();
                    RestoreCarriedHp();
                });
                for (const int seat : run.ConsumeRevivedSeats()) ReviveSeat(seat);
                g->transition.pending = false;
                RM_LOGI("stage change: arena %d started on stage %d", run.state().arena_index, g->currentStage);
            }
            return;
        }
    }
    if (waitingNext && g->settings.stagePerArena && autopilot::CanChangeStage() && engine::Caps().inventory) {
        StartStageTransition(now);
        return;
    }
    if (waitingFirst && !matchRunning) return;
    const rm::Status status = run.BeginArena(now);
    Report(status, "begin arena");
    g->arenaEndRequested = false;
    if (status.ok()) ArenaBegan(now);
    if (status.ok() && waitingFirst) ForEachSeatTurn([] { ApplyLoadoutToInventory(); });
}

// The arena start's hold (the user, 2026-11-11: "when you start a stage and change weapon fast, it
// maintains that in the inventory ... so people can't do that or see how the inventory changes"):
// from a run's match start until its inventory is laid out (ApplyLoadoutToInventory at the first
// arena, ApplyCarriedInventory after a stage change or CONTINUE RUN, both in TryBeginArena), the game
// sees no key or button (hooks_input; the sticks and the mouse's movement still reach it), so no gun
// of the game's own loadout is switched to and the inventory is not opened before the run's is in.
// At most kInventoryHoldSeconds a match, should the layout never come (a run for another character,
// a stage change that failed), counted from the end of the match's start (the game's main state 6,
// MercStartRunning: its intro took the whole 10 s in alpha.17, the layout came 1.5..3 s after the
// hold, and the user, 2026-11-11: "inventory is still accesible before the change happens...").
// Not while the intro's shots play (MercIntroShotsPlaying: the run's skip did not end them; the game
// locks the controls then): a press skips them, the game's own way (2026-11-11, the user: "announcer
// sitll plays and you can't skip it yet").
constexpr double kInventoryHoldSeconds = 10.0;
std::atomic<bool> g_holdButtons{false};

// TryBeginArena's wait: a run's match whose inventory is not laid out yet.
bool InventoryPending() {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || !g->match.inMatch) return false;
    const rm::Phase phase = run.phase();
    if (phase == rm::Phase::Idle && run.state().arena_index == 0) return true;
    return phase == rm::Phase::RouteSelection && run.state().route_chosen && g->transition.pending &&
           g->matchGeneration != g->transition.generation;
}

void UpdateInventoryHold(double now) {
    static int generation = -1;
    static double since = 0.0;
    static bool expired = false;
    bool hold = InventoryPending();
    const bool intro = MercIntroShotsPlaying();
    if (hold && generation != g->matchGeneration) {
        generation = g->matchGeneration;
        since = now;
        expired = false;
    }
    if (MercStartRunning()) since = now;
    if (hold && !expired && now - since > kInventoryHoldSeconds) {
        RM_LOGW("inventory: no layout %.0f s into the match; keys and buttons go back to the game", kInventoryHoldSeconds);
        expired = true;
    }
    hold = hold && !expired && !intro;
    if (hold != g_holdButtons.load()) {
        g_holdButtons = hold;
        RM_LOGI("inventory: keys and buttons %s (match %d, %.1f s in)",
                hold ? "held until the layout" : intro ? "with the game (the intro's shots play; a press skips them)"
                                                       : "back to the game",
                g->matchGeneration, now - since);
    }
}

// Every 2 s of a director arena: live enemies the director did not spawn are logged once each
// (a stage's own special enemies may come by another path than the blocked set spawn), and
// in developer mode the director units' mean distance to the player (do they come?).
void TraceDirectorArena(double now, const std::optional<std::array<double, 3>>& player) {
    static double next = 0.0;
    static std::set<std::uintptr_t> reported;
    if (now < next) return;
    next = now + 2.0;
    double distance = 0.0;
    int counted = 0;
    for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
        if (e.hp <= 0) continue;
        const float x = ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f);
        const float y = ReadOr<float>(e.object + re5::off::kUnitPosition + 4, 0.0f);
        const float z = ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f);
        if (g->directorUnits.count(e.object) == 0) {
            if (reported.insert(e.object).second) {
                RM_LOGW("director: untracked enemy %08X %s hp %d/%d at %.0f %.0f %.0f (not spawned by the director)",
                        static_cast<unsigned>(e.object), engine::ClassName(e.object).c_str(), e.hp, e.max, x, y, z);
            }
            continue;
        }
        if (player) {
            distance += std::sqrt(std::pow(x - (*player)[0], 2) + std::pow(z - (*player)[2], 2));
            ++counted;
        }
    }
    if (g->settings.developerMode && counted > 0) {
        RM_LOGI("director: %d units, mean distance to the player %.0f", counted, distance / counted);
    }
}

// Bosses that fight from where they are and move themselves (U-8 of the capsule
// dome: its Shift takes it around the dome); the mod never moves them.
// Uroboros Aheri (uEm42) is a mass that rises beside the tanker's deck from its story spot (its
// map's boss "spawn", s511 set Uro3_Battle) and never walks: moved, it hung in the air or sank into
// the deck (2026-11-08: the user, "it spawned incorrectly ... you were hitting it from below").
bool BossHoldsGround(const std::string& cls) { return cls == "uEm43" || cls == "uEm42"; }

// Enemies that cannot reach the player (behind a door the story opens, on a ledge, a boss at a
// door) are moved: one far from the player that has not moved 1.5 m in 12 s (8 s for a boss), and
// the last few when nothing has died for a while and they are far. The director gives each one
// another point within the player's reach and drops the point it was stuck at (user playtest
// 2026-11-03: Spencer's mansion, the Uroboros at a door). The very last enemy comes to the player
// sooner, to the nearest point, when nothing has hurt it for a while (the user, 2026-11-09: "sometime
// is very hard to find him"). Bosses never by these two rules, only when they stand stuck.
struct StuckTrack {
    float anchor[3] = {};
    double anchorAt = 0.0;
    double lastRescue = -1.0e9;
    bool seenNoted = false;   // "in the player's sight" logged since anchorAt
    bool warped = false;      // moved, and not yet seen leaving the point it was moved to
    float warpedFrom[3] = {};
    int failedWarps = 0;      // moves it came straight back from
    int lonePulls = 0;        // pulls towards the player as the very last enemy
    int lastHp = -1;
    double hurtAt = 0.0;      // the last time its health went down
};

bool PlayerFacing(std::uintptr_t player, float& fx, float& fz);

// Whether the player may be looking at `pos`. The camera looks over his shoulder the way he
// faces, so a point counts when it is in front of a spot behind him (or close to him whichever
// way he looks) and no wall stands between his eyes and it. Unknown facing or no collision
// query counts as seen.
constexpr float kSeenNear = 600.0f;     // this close, only a wall hides it
constexpr float kCameraBack = 300.0f;   // the camera's spot behind the player
constexpr float kSeenCone = 0.42f;      // cos 65 degrees from the camera's spot
constexpr float kEyeHeight = 160.0f;
constexpr float kBodyHeight = 120.0f;
bool PlayerMaySee(std::uintptr_t local, const std::array<double, 3>& player, const float pos[3]) {
    const float px = static_cast<float>(player[0]), py = static_cast<float>(player[1]), pz = static_cast<float>(player[2]);
    const float dx = pos[0] - px, dz = pos[2] - pz;
    if (std::sqrt(dx * dx + dz * dz) > kSeenNear) {
        float fx = 0.0f, fz = 0.0f;
        if (!local || !PlayerFacing(local, fx, fz)) return true;
        const float vx = pos[0] - (px - fx * kCameraBack), vz = pos[2] - (pz - fz * kCameraBack);
        const float len = std::sqrt(vx * vx + vz * vz);
        if (len > 1.0f && (vx * fx + vz * fz) / len < kSeenCone) return false;  // beside or behind the view
    }
    const float eye[3] = {px, py + kEyeHeight, pz};
    const float body[3] = {pos[0], pos[1] + kBodyHeight, pos[2]};
    const auto hit = engine::CastSegment(eye, body);
    return !hit || !hit->Blocked();
}

// M10 and a player's report (2026-11-18: enemies "teleport all over the map ... any time the
// enemy wasn't right in front of me"): the facing guess above misses where the camera looks (the
// right stick turns it, not the player), and in co-op it only knew one player. Now a point is seen
// when it is near any standing player with no wall between, or inside any game camera's view
// (each split-screen half has one) with nothing between that camera and it.
struct Watchers {
    std::vector<std::array<double, 3>> players;  // standing human players
    std::vector<std::uintptr_t> units;           // the same players' units (the facing fallback)
    std::vector<engine::CameraView> views;
};
constexpr float kViewMargin = 1.15f;  // a little wider than the camera's own view

Watchers ReadWatchers() {
    Watchers w;
    const int seats = std::max(1, g->run->SeatCount());
    for (int seat = 0; seat < seats; ++seat) {
        const auto unit = seats > 1 ? engine::HumanPlayerInSlot(seat) : engine::LocalPlayer();
        if (!unit) continue;
        const auto hp = engine::ReadHp(unit);
        if (seats > 1 && (!hp || hp->hp <= 0)) continue;  // a fallen player sees nothing
        w.players.push_back({ReadOr<float>(unit + re5::off::kUnitPosition, 0.0f), ReadOr<float>(unit + re5::off::kUnitPosition + 4, 0.0f),
                             ReadOr<float>(unit + re5::off::kUnitPosition + 8, 0.0f)});
        w.units.push_back(unit);
    }
    w.views = engine::ReadCameraViews();
    return w;
}

bool InCameraView(const engine::CameraView& v, const float point[3]) {
    float f[3] = {v.target[0] - v.eye[0], v.target[1] - v.eye[1], v.target[2] - v.eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1.0e-3f) return true;  // no direction: count it as seen
    for (float& c : f) c /= fl;
    float u[3] = {v.up[0], v.up[1], v.up[2]};
    float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1.0e-3f) return true;
    for (float& c : r) c /= rl;
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    const float to[3] = {point[0] - v.eye[0], point[1] - v.eye[1], point[2] - v.eye[2]};
    const float z = to[0] * f[0] + to[1] * f[1] + to[2] * f[2];
    if (z <= 0.0f) return false;  // behind the camera
    const float tanV = std::tan(v.fov * 0.5f * 3.14159265f / 180.0f) * kViewMargin;
    const float tanH = tanV * v.aspect;
    const float x = to[0] * r[0] + to[1] * r[1] + to[2] * r[2];
    const float y = to[0] * u[0] + to[1] * u[1] + to[2] * u[2];
    return std::fabs(x) <= z * tanH && std::fabs(y) <= z * tanV;
}

bool AnyoneMaySee(const Watchers& w, const float pos[3]) {
    const float body[3] = {pos[0], pos[1] + kBodyHeight, pos[2]};
    const auto clear = [&](const float from[3]) {
        const auto hit = engine::CastSegment(from, body);
        return !hit || !hit->Blocked();
    };
    for (const auto& p : w.players) {
        const float dx = pos[0] - static_cast<float>(p[0]), dz = pos[2] - static_cast<float>(p[2]);
        const float eye[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]) + kEyeHeight, static_cast<float>(p[2])};
        if (std::sqrt(dx * dx + dz * dz) <= kSeenNear && clear(eye)) return true;
    }
    if (w.views.empty()) {
        // No camera to read: each player's facing, as before.
        for (std::size_t i = 0; i < w.players.size(); ++i) {
            if (PlayerMaySee(w.units[i], w.players[i], pos)) return true;
        }
        return false;
    }
    for (const auto& v : w.views) {
        if (InCameraView(v, body) && clear(v.eye)) return true;
    }
    return false;
}

// The nearest standing player to `pos` (horizontally): its index in w.players, and the distance.
std::pair<std::size_t, float> NearestWatcher(const Watchers& w, const float pos[3]) {
    std::pair<std::size_t, float> best{0, std::numeric_limits<float>::max()};
    for (std::size_t i = 0; i < w.players.size(); ++i) {
        const float d = static_cast<float>(std::hypot(pos[0] - w.players[i][0], pos[2] - w.players[i][2]));
        if (d < best.second) best = {i, d};
    }
    return best;
}

// Nothing in a straight line between `p` and the player (a boss's way to him: the Uroboros
// came from behind a door, user playtest 2026-11-04). No collision query counts as open.
bool OpenToPlayer(const std::array<double, 3>& player, const std::array<double, 3>& p) {
    const float from[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]) + kBodyHeight, static_cast<float>(p[2])};
    const float to[3] = {static_cast<float>(player[0]), static_cast<float>(player[1]) + kBodyHeight, static_cast<float>(player[2])};
    const auto hit = engine::CastSegment(from, to);
    return !hit || !hit->Blocked();
}
std::unordered_map<std::uintptr_t, StuckTrack> g_stuckTracks;
// Spots where an enemy stood stuck this match. On the Port (stage 204, floors at about 1, 3.6,
// 4.3 and 7 m) the same few dead ends caught one enemy after another, some of them right at a
// director point, and some 94 moves in two arenas did not end it (user playtest 2026-11-07): no
// enemy is moved to, nor preferably spawned at, a point near one.
std::vector<std::array<double, 3>> g_trapSpots;
constexpr double kTrapRadius = 400.0;  // 4 m
constexpr double kSameFloor = 250.0;   // 2.5 m: the player's floor
constexpr std::size_t kMaxTrapSpots = 64;

bool NearTrap(const std::array<double, 3>& p) {
    for (const auto& t : g_trapSpots) {
        const double dx = p[0] - t[0], dy = p[1] - t[1], dz = p[2] - t[2];
        if (dx * dx + dy * dy + dz * dz < kTrapRadius * kTrapRadius) return true;
    }
    return false;
}

void NoteTrap(const float pos[3], const std::string& cls) {
    const std::array<double, 3> at = {pos[0], pos[1], pos[2]};
    if (NearTrap(at) || g_trapSpots.size() >= kMaxTrapSpots) return;
    g_trapSpots.push_back(at);
    RM_LOGI("director: %s stuck at %.0f %.0f %.0f; no enemy goes within 4 m of it this match (%zu such spots)", cls.c_str(), pos[0],
            pos[1], pos[2], g_trapSpots.size());
}
double g_stuckCheckAt = 0.0;
int g_stuckRemaining = -1;
double g_stuckRemainingSince = 0.0;
constexpr double kStuckCheckEvery = 0.5;
// A Majini that has not seen the player stands where it is (its search circle can be 5 m), so
// "unmoved 12 s, 9 m away" moved idle enemies about all the time: "is it normal for enemies to
// teleport all over the map? ... pretty much any time the enemy wasn't right in front of me" (a
// player's report, 2026-11-18). Now an enemy stands unmoved 30 s, and only while the fight is
// quiet (nothing died for a while), before it is moved; one move at a time, some seconds apart.
constexpr double kStuckSeconds = 30.0;
constexpr double kQuietBeforeMoves = 20.0;     // no kill for this long before a stuck one is moved
constexpr double kMoveSpacing = 6.0;           // seconds between two moves (bosses aside)
constexpr double kBossStuckSeconds = 8.0;
constexpr double kBossLogEvery = 5.0;
constexpr float kStuckMove = 150.0f;           // moved less than this ...
constexpr float kStuckFar = 900.0f;            // ... while farther than this from the player
constexpr int kLastEnemies = 3;
constexpr double kLastEnemiesQuiet = 25.0;     // no kill for this long ...
constexpr float kLastEnemiesFar = 1500.0f;     // ... and this far: pulled in
constexpr int kFailedWarpsToFinish = 2;        // one of the last back where it was after this many moves: finished
// The very last enemy came "too close" and moved "too much" (the user, 2026-11-18: the Mines'
// crawler, often on a wall above the player's floor): it waits longer, is pulled only from
// farther, never to a point nearer than kLonePullMin, and its wait starts over after a pull.
constexpr double kLoneFarSeconds = 20.0;       // the very last enemy, unhurt this long and ...
constexpr float kLoneFar = 1500.0f;            // ... this far away (or 8 m away on another floor), is pulled in
constexpr float kLoneOtherFloorFar = 800.0f;
constexpr double kLoneQuietSeconds = 40.0;     // ... or unhurt this long and not near
constexpr float kLoneNear = 1000.0f;
constexpr float kLonePullMin = 1200.0f;        // a pull's point is at least this far from the player
constexpr double kLonePullEvery = 30.0;        // seconds between two pulls of the last enemy
constexpr int kMaxLonePulls = 2;               // then, still far and unhurt, it is finished
double g_lastMoveAt = -1.0e9;

void RescueStuckEnemies(double now, const std::optional<std::array<double, 3>>& player) {
    rm::RunController& run = *g->run;
    if (!player || now < g_stuckCheckAt) return;
    g_stuckCheckAt = now + kStuckCheckEvery;
    const int remaining = run.RemainingEnemies();
    if (remaining != g_stuckRemaining) {
        g_stuckRemaining = remaining;
        g_stuckRemainingSince = now;
    }
    const bool lastFew = remaining <= kLastEnemies && now - g_stuckRemainingSince > kLastEnemiesQuiet;
    for (auto it = g_stuckTracks.begin(); it != g_stuckTracks.end();) {
        it = g->directorUnits.count(it->first) ? std::next(it) : g_stuckTracks.erase(it);
    }
    // Every standing player and camera (co-op: both): an enemy fighting player 2 is not "far",
    // and one either player may see stays where it is.
    Watchers watchers = ReadWatchers();
    if (watchers.players.empty()) watchers.players.push_back(*player);
    for (const auto& [unit, id] : g->directorUnits) {
        const auto hp = engine::ReadHp(unit);
        if (!hp || hp->hp <= 0) continue;
        float pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = ReadOr<float>(unit + re5::off::kUnitPosition + k * 4, 0.0f);
        auto [it, fresh] = g_stuckTracks.try_emplace(unit);
        StuckTrack& t = it->second;
        if (fresh || (t.lastHp >= 0 && hp->hp < t.lastHp)) t.hurtAt = now;
        t.lastHp = hp->hp;
        const float moved = std::sqrt(std::pow(pos[0] - t.anchor[0], 2.0f) + std::pow(pos[1] - t.anchor[1], 2.0f) +
                                      std::pow(pos[2] - t.anchor[2], 2.0f));
        const auto [nearestIndex, away] = NearestWatcher(watchers, pos);
        const std::array<double, 3>& nearest = watchers.players[nearestIndex];
        if (t.warped && moved > kStuckMove) {
            // It left the point it was moved to: straight back where it was, the move did not
            // take. A Licker (uEm32) in a campaign area went back to the same spot after every
            // move (8 position copies written, 10 for a Majini), the last enemy 51 m away, and
            // the arena never ended (user playtest 2026-11-05, stage 310).
            t.warped = false;
            const float back = std::sqrt(std::pow(pos[0] - t.warpedFrom[0], 2.0f) + std::pow(pos[1] - t.warpedFrom[1], 2.0f) +
                                         std::pow(pos[2] - t.warpedFrom[2], 2.0f));
            if (back < kStuckMove) {
                ++t.failedWarps;
                RM_LOGI("director: %s is back at %.0f %.0f %.0f after its move (%d)", engine::ClassName(unit).c_str(), pos[0], pos[1],
                        pos[2], t.failedWarps);
            }
        }
        if (t.failedWarps >= kFailedWarpsToFinish && remaining <= kLastEnemies && unit != g->boss.unit && now - t.lastRescue >= 5.0) {
            // One of the last that cannot be moved and has not come to the player: finished, so
            // the arena can end (counted like any death, DetectEnemyDeaths). Never a boss.
            const bool done = director::Damage(unit, hp->hp);
            RM_LOGI("director: %s cannot be moved (back at %.0f %.0f %.0f after %d moves), %.0f m from the player, %d left; %s",
                    engine::ClassName(unit).c_str(), pos[0], pos[1], pos[2], t.failedWarps, away / 100.0f, remaining,
                    done ? "finished so the arena can end" : "its damage was refused");
            t.failedWarps = done ? 0 : t.failedWarps;
            t.lastRescue = now;
            continue;
        }
        const bool boss = unit == g->boss.unit;
        if (boss) {
            static double loggedAt = -1.0e9;
            if (now - loggedAt >= kBossLogEvery) {
                loggedAt = now;
                RM_LOGI("director: boss %s at %.0f %.0f %.0f, %.1f m from the player (%.1f m above), %s", engine::ClassName(unit).c_str(),
                        pos[0], pos[1], pos[2], away / 100.0f, (pos[1] - static_cast<float>(nearest[1])) / 100.0f,
                        engine::CurrentActionName(unit).c_str());
            }
            // U-8 holds its dome and moves around it itself (cEm43ActShift); moved
            // every 8 s it did not close in, it seemed to teleport all around (user playtest
            // 2026-11-04). It is never moved. Other bosses only when they stand stuck, like any
            // enemy.
            if (BossHoldsGround(engine::ClassName(unit))) continue;
            // The Uroboros' own trip under the floor to reach the player (Dive, UnderGround, Float) is
            // no being stuck: it stands parked until it comes up.
            const std::string doing = engine::CurrentActionName(unit);
            if (doing == "cEm40ActDive" || doing == "cEm40ActUnderGround" || doing == "cEm40ActFloat") {
                for (int k = 0; k < 3; ++k) t.anchor[k] = pos[k];
                t.anchorAt = now;
                continue;
            }
        }
        const bool lone = remaining == 1 && !boss;
        if (fresh || moved > kStuckMove) {
            for (int k = 0; k < 3; ++k) t.anchor[k] = pos[k];
            t.anchorAt = now;
            t.seenNoted = false;
            if (!lastFew && !lone) continue;
        }
        const bool quiet = now - g_stuckRemainingSince > kQuietBeforeMoves;
        // The very last enemy only by its own rule below (towards the player): moved as "stuck" to
        // any point in reach it was taken 36 m away, from 12 m (2026-11-18).
        const bool stuck = !lone && away > kStuckFar && (boss ? now - t.anchorAt > kBossStuckSeconds : quiet && now - t.anchorAt > kStuckSeconds);
        const bool straggler = !lone && lastFew && !boss && away > kLastEnemiesFar;
        const double unhurt = now - std::max(t.hurtAt, g_stuckRemainingSince);
        const bool otherFloor = std::fabs(pos[1] - static_cast<float>(nearest[1])) > static_cast<float>(kSameFloor);
        const bool lost = lone && ((unhurt > kLoneFarSeconds && (away > kLoneFar || (otherFloor && away > kLoneOtherFloorFar))) ||
                                   (unhurt > kLoneQuietSeconds && away > kLoneNear));
        if ((!stuck && !straggler && !lost) || now - t.lastRescue < (lost ? kLonePullEvery : 15.0)) continue;
        if (lost && t.lonePulls >= kMaxLonePulls) {
            // A Majini that never saw the player walks back to its own spot after a pull (34 m in
            // 30 s on the Village, 2026-11-18), and pulled again and again it seemed to teleport
            // all the time. After two pulls the last enemy is finished so the arena can end
            // (counted like any death, DetectEnemyDeaths), as one that cannot be moved is.
            const bool done = director::Damage(unit, hp->hp);
            RM_LOGI("director: %s, the last one, %.0f m from the player and unhurt %.0f s after %d pulls; %s", engine::ClassName(unit).c_str(),
                    away / 100.0f, unhurt, t.lonePulls, done ? "finished so the arena can end" : "its damage was refused");
            t.lastRescue = now;
            continue;
        }
        if (!boss && now - g_lastMoveAt < kMoveSpacing) continue;  // one at a time
        // Never before the players' eyes: one they may be looking at stays where it is, and the
        // others go only to a point out of their sight (user playtest 2026-11-04: enemies vanished
        // in view and appeared elsewhere).
        if (AnyoneMaySee(watchers, pos)) {
            if (!t.seenNoted) {
                t.seenNoted = true;
                RM_LOGI("director: %s still %.0f s at %.0f %.0f %.0f, %.0f m from the player, in his sight; left there",
                        engine::ClassName(unit).c_str(), now - t.anchorAt, pos[0], pos[1], pos[2], away / 100.0f);
            }
            continue;
        }
        if (stuck) NoteTrap(pos, engine::ClassName(unit));
        // Out of his sight, never near a spot one stood stuck at, and his own floor first (as a
        // boss's spawn): from another floor it often found no way down to him.
        // Failing those, anywhere out of his sight, as before.
        // The point is chosen near the player nearest to it (co-op: the one it was after).
        const std::optional<std::array<double, 3>> target = nearest;
        const float awayNow = away;
        auto accept = [&](bool sameFloor, bool avoidTraps) {
            return [&, sameFloor, avoidTraps](const std::array<double, 3>& p) {
                const float at[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
                if (AnyoneMaySee(watchers, at) || (avoidTraps && NearTrap(p))) return false;
                if (boss && !OpenToPlayer(nearest, p)) return false;
                // A pull brings the last one nearer (one went from 15 m to 30 m, every nearer point
                // being in sight), and never nearer than kLonePullMin.
                if (lost) {
                    const float d = NearestWatcher(watchers, at).second;
                    if (d < kLonePullMin || d > awayNow - kStuckMove) return false;
                }
                return !sameFloor || std::fabs(p[1] - nearest[1]) <= kSameFloor;
            };
        };
        auto to = run.DirectorRelocate(id, target, accept(true, true), lost);
        if (!to) to = run.DirectorRelocate(id, target, accept(false, true), lost);
        if (!to) to = run.DirectorRelocate(id, target, accept(false, false), lost);
        if (!to) {
            if (lost) {
                // No nearer point out of sight: counts as a pull, so it cannot wait forever.
                ++t.lonePulls;
                t.lastRescue = now;
                RM_LOGI("director: %s, the last one, %.0f m from the player: no nearer point out of sight (%d)",
                        engine::ClassName(unit).c_str(), away / 100.0f, t.lonePulls);
                continue;
            }
            t.lastRescue = now - 10.0;  // no point out of sight now: again in 5 s
            continue;
        }
        const float dest[3] = {static_cast<float>((*to)[0]), static_cast<float>((*to)[1]), static_cast<float>((*to)[2])};
        const int copies = engine::WarpUnit(unit, dest);
        RM_LOGI("director: %s %s %.0f s at %.0f %.0f %.0f, %.0f m from the player; moved to %.0f %.0f %.0f (%d copies)",
                engine::ClassName(unit).c_str(), stuck ? "stuck" : lost ? "the last one, not being hurt" : "one of the last, quiet", now - t.anchorAt,
                pos[0], pos[1], pos[2], away / 100.0f, dest[0], dest[1], dest[2], copies);
        for (int k = 0; k < 3; ++k) {
            t.warpedFrom[k] = pos[k];
            t.anchor[k] = dest[k];
        }
        t.warped = copies > 0;
        t.anchorAt = now;
        t.lastRescue = now;
        if (lost) {
            t.hurtAt = now;  // its wait for another pull starts over
            ++t.lonePulls;
        }
        if (!boss) g_lastMoveAt = now;
    }
}

// The Uroboros (uEm40) has a brain of its own (uEmIntel40: its near and far grabs, front and back
// slams, roar, run, a path to the player, damage reactions and regeneration). Its constructor starts
// it in the story's waiting mode (+0x20C0), where it only walks and never attacks, and the brain
// leaves that mode when story flag 9 is set, which no run does: it walked straight on and stood
// (user playtests 4 and 5), and the two moves the mod ordered in its place were all it ever did (user
// playtest 2026-11-08: "he only uses grab and he spams that all the time"). A run's Uroboros is
// released the way the brain releases it: waiting mode off, and its roar asked for. The story's
// health for its arms and head comes with it, since they take damage from then on
// (research/re5-engine-map.md "The Uroboros' brain").
constexpr std::int16_t kEm40ArmLife = 420;   // s103_em00 ARM_L_LIFE, ARM_R_LIFE 220, + the 200 of its apply
constexpr std::int16_t kEm40HeadLife = 375;  // HEAD_LIFE 175 + 200

constexpr double kBossFallSeconds = 3.0;  // a dead boss's fall before the arena's screens (DriveArena)

// A dead boss's fall. The mod counts a boss dead when its health reaches zero, but some never die
// there: Wesker (uEm81) fights on at 0 of 8500 (his death is scripted in the campaign), and the
// others only start their death from the hit that kills them, not from health taken some other way
// (Popokarimu walked on at zero after SubHp; build/runs/20261108-011728-bossarena-uEm44-s108). So a
// boss that has not begun a death of its own shortly after (an action named Die/Dead) is given
// its own: Wesker his Shrink, the weakened stagger the campaign's injection follows (6 s;
// build/runs/20261107-233900-boss-uEm81-s506), the others their death. The Uroboros dies by itself.
struct ScriptedFall {
    const char* cls;
    const char* action;
    double after;  // seconds after its death: a killing hit's own death starts at once
};
constexpr ScriptedFall kScriptedFalls[] = {
    {"uEm81", "cEm80ActShrink", 0.0},    {"uEm80", "cEm80ActShrink", 0.0},    {"uEm44", "cEm44ActDie", 0.3},
    {"uEm84", "cEm84ActDieNormal", 0.3}, {"uEm83", "cEm83ActDieNormal", 0.3},
    // Uroboros Wesker fights on at zero health too (his end is the story's): his last pinned fall.
    {"uEm82", "cEm82DmgLastPinionDie", 0.0},
    // Uroboros Mkono (uEm41) has the Uroboros' deaths; its own health is 32767 (its end in the story
    // is the flamethrower room's), so one is given if it has not begun its own.
    {"uEm41", "cEm40ActDead", 0.3},
};
std::uintptr_t g_fallDone = 0;

bool FallingByItself(const std::string& action) {
    return action.find("Die") != std::string::npos || action.find("Dead") != std::string::npos;
}

// A boss's scripted kills, turned into a plain attack in a run as they begin (before the player
// is caught). Wesker's knife-hand (cEm80ActAtkSyutoStart, then AtkSyuto with the player in
// SyutoEsc_PL, then AtkSyutoDie) kills at full health past the run's health, its perks and the
// developer invulnerability, with the story's death fade (2026-11-08, scripts/test_black_screen.sh:
// two of four arenas, build/runs/20261108-150643-blackscreen r3, r4). His other grabs (the knee,
// the choke) take the camera for their close-up and give it back.
struct ScriptedKill {
    const char* cls;
    const char* start;   // the move's first action
    const char* instead; // the attack started in its place
};
constexpr ScriptedKill kScriptedKills[] = {
    {"uEm81", "cEm80ActAtkSyutoStart", "cEm80ActAtkKick"},
    {"uEm80", "cEm80ActAtkSyutoStart", "cEm80ActAtkKick"},
};

// A boss's hold that ends in a kill. The Uroboros' near grab and Uroboros Mkono's share one hold
// (cEm40ActNearCatchOk, cEm41ActNearCatchOk: update 0x008ABF90): it loops, then plays its kill and
// sets the player's health to 0, past the run's health and the developer invulnerability (2026-11-08,
// build/runs/20261108-174744-bossarena-uEm41-s308: caught at full health, dead 11 s later). The
// player mashes to break free, which starts the grabber's NearEscape: it puts the player into his
// own escape (0x008B0880). In a run that escape comes after kHoldRelease if he is still held (the
// far grabs drain a little and pull him in; they do not kill). research/re5-engine-map.md.
struct BossHold {
    const char* hold;
    const char* escape;
};
constexpr BossHold kLethalHolds[] = {
    {"cEm40ActNearCatchOk", "cEm40ActNearEscape"},
    {"cEm41ActNearCatchOk", "cEm40ActNearEscape"},
};
constexpr double kHoldRelease = 4.0;

const BossHold* LethalHold(const std::string& action) {
    for (const BossHold& h : kLethalHolds) {
        if (action == h.hold) return &h;
    }
    return nullptr;
}

bool ReleaseHold(std::uintptr_t grabber, const BossHold& h) {
    const auto ids = engine::ActionIdsOf(grabber, h.escape);
    return !ids.empty() && engine::StartAction(grabber, ids.front(), h.escape);
}

void ReleaseBossHolds(double now) {
    static std::uintptr_t heldBy = 0;
    static double since = 0.0;
    static bool done = false;
    const std::uintptr_t unit = g->boss.unit;
    const std::string action = unit && g->run->HasActiveRun() ? engine::CurrentActionName(unit) : std::string();
    const BossHold* h = LethalHold(action);
    if (h == nullptr) {
        heldBy = 0;
        return;
    }
    if (heldBy != unit) {
        heldBy = unit;
        since = now;
        done = false;
    }
    if (done || now - since < kHoldRelease) return;
    done = true;
    const bool ok = ReleaseHold(unit, *h);
    RM_LOGI("boss: %s held the player %.1f s in %s (a kill at its end); %s", engine::ClassName(unit).c_str(), now - since,
            action.c_str(), ok ? (std::string(h->escape) + ", he is let go").c_str() : "no escape found, left as it is");
}

void TameBossKills() {
    const std::uintptr_t unit = g->boss.unit;
    if (!unit || !g->run->HasActiveRun()) return;
    static std::uintptr_t lastUnit = 0;
    static std::string lastAction;
    const std::string action = engine::CurrentActionName(unit);
    if (unit == lastUnit && action == lastAction) return;
    lastUnit = unit;
    lastAction = action;
    const std::string cls = engine::ClassName(unit);
    for (const ScriptedKill& k : kScriptedKills) {
        if (cls != k.cls || action != k.start) continue;
        const auto ids = engine::ActionIdsOf(unit, k.instead);
        const bool ok = !ids.empty() && engine::StartAction(unit, ids.front(), k.instead);
        RM_LOGI("boss: %s began %s (a scripted kill); %s", cls.c_str(), k.start,
                ok ? (std::string(k.instead) + " instead").c_str() : "left as it is (no replacement)");
        return;
    }
}

// The player camera's event camera (a grab's close-up) held while the player is in none of the
// moves that use it: logged once a time, with what was going on (2026-11-08: one boss arena went
// black for good with the player still aiming, build/runs/20261108-012025-bossarena-uEm81-s506,
// not seen again in five).
void WatchEventCamera(double now) {
    static double since = -1.0;
    static bool logged = false;
    const auto cam = areacams::LastCamera();
    const auto local = engine::LocalPlayer();
    const bool event = cam && local && g->match.inMatch && ReadOr<std::uintptr_t>(cam + re5::off::kCamCtrlCurrent, 0) == cam + 0x2DE0;
    const std::string action = event ? engine::CurrentActionName(local) : std::string();
    const bool held = event && action.find("_PL") == std::string::npos && action.find("EventAct") == std::string::npos &&
                      action.find("damage") == std::string::npos;
    if (!held) {
        since = -1.0;
        logged = false;
        return;
    }
    if (since < 0.0) since = now;
    if (logged || now - since < 5.0) return;
    logged = true;
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    RM_LOGW("camera: the event camera has held for 5 s with the player in %s (boss %s in %s, app flags %08X)", action.c_str(),
            g->boss.unit ? engine::ClassName(g->boss.unit).c_str() : "-",
            g->boss.unit ? engine::CurrentActionName(g->boss.unit).c_str() : "-",
            app ? ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) : 0u);
}

void DriveBossFall(double now) {
    const std::uintptr_t unit = g->boss.fallen;
    if (!g->boss.dead || !unit || unit == g_fallDone || now - g->boss.diedAt > kBossFallSeconds) return;
    const std::string cls = engine::ClassName(unit);
    const ScriptedFall* fall = nullptr;
    for (const ScriptedFall& f : kScriptedFalls) {
        if (cls == f.cls) fall = &f;
    }
    if (!fall) {
        g_fallDone = unit;  // falls by itself (the Uroboros' death sequence)
        return;
    }
    const std::string current = engine::CurrentActionName(unit);
    if (current == fall->action || (FallingByItself(current) && current.find("Atk") == std::string::npos)) {
        RM_LOGI("boss fall: %s fell by itself (%s), %.2f s after its death", cls.c_str(), current.c_str(), now - g->boss.diedAt);
        g_fallDone = unit;
        return;
    }
    if (now - g->boss.diedAt < fall->after) return;
    const auto ids = engine::ActionIdsOf(unit, fall->action);
    if (!ids.empty() && engine::StartAction(unit, ids.front(), fall->action, true)) {
        g_fallDone = unit;
        RM_LOGI("boss fall: %s %s (action %d) %.2f s after its death (it was in %s)", cls.c_str(), fall->action, ids.front(),
                now - g->boss.diedAt, current.c_str());
    }
}

// Developer "watch <class>": the first enemy of that class, its action and health logged on every
// change, at zero health too (the Wesker bosses do not die there: what do they do instead).
struct UnitWatch {
    std::string cls;
    std::uintptr_t unit = 0;
    int hp = -1;
    int action = -2;
    std::uint64_t guard = 0;
    double since = 0.0;
};
UnitWatch g_watch;
// Developer "bring ... <pin s>": the unit kept where it was put for that long (re-placed every
// frame), so a test's shot finds it there whatever it does meanwhile.
struct UnitPin {
    std::uintptr_t unit = 0;
    float dest[3] = {};
    double until = 0.0;
    bool turn = false;  // held facing `yaw` too (dev bring ... away)
    float yaw = 0.0f;
};

// Developer "screenwatch on|off": what can turn the picture black, logged on every change: the
// fader's state, the application's flag word, the player camera's current camera and zone answer,
// and the player's action.
bool g_screenWatch = false;
std::string g_screenWatchLast;

void WatchScreen() {
    if (!g_screenWatch) return;
    char line[512];
    const auto fader = engine::Caps().faderState ? ReadOr<std::uintptr_t>(re5::addr::kFadeInstance, 0) : 0;
    std::uint32_t fs[4] = {};
    for (int k = 0; k < 4 && fader; ++k) fs[k] = ReadOr<std::uint32_t>(fader + re5::off::kFaderState + k * 4, 0);
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    const auto appFlags = app ? ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) : 0;
    const auto cam = areacams::LastCamera();
    const auto current = cam ? ReadOr<std::uintptr_t>(cam + re5::off::kCamCtrlCurrent, 0) : 0;
    const char* which = !current ? "-" : current == cam + 0xDF0 ? "own" : current == cam + 0x2DE0 ? "event" : "other";
    const auto local = engine::LocalPlayer();
    std::snprintf(line, sizeof(line), "fader %08X %08X %08X %08X | app flags %08X | camera %s answer %d zone %d forced %u | player %s",
                  fs[0], fs[1], fs[2], fs[3], appFlags, which, cam ? ReadOr<std::int32_t>(cam + re5::off::kCamCtrlZoneAnswer, -1) : -1,
                  cam ? ReadOr<std::int32_t>(cam + re5::off::kCamCtrlZoneCamera, -1) : -1,
                  cam ? ReadOr<std::uint32_t>(cam + re5::off::kCamCtrlAreaFlags, 0) & 1u : 0u,
                  local ? engine::CurrentActionName(local).c_str() : "-");
    if (g_screenWatchLast == line) return;
    g_screenWatchLast = line;
    RM_LOGW("screenwatch: %s", line);
}
UnitPin g_pin;

// ---- M10 F0, local co-op research (developer mode) -----------------------------------------
// "coopwatch on|off": the game work fields the Mercenaries menus write (Solo/Duo, the Duo screens,
// the players' costumes and pads), the players, the camera viewports and the virtual pads, logged
// on every change. "pad <user> ...": a virtual XInput pad (hooks.h SetVirtualPad) for a second
// player in tests. "mercenaries solo|duo|split": the game's own Mercenaries menu with no run, its
// first page answered with that mode. "saveguard on|off": the game's save write held (a test's plain
// match must not write savedata.bin).
bool g_coopWatch = false;
double g_coopNext = 0.0;
std::map<int, std::uint32_t> g_coopGw;
std::string g_coopPlayersLast, g_coopViewsLast, g_coopPadsLast;
bool g_devSaveGuard = false;
unsigned g_devMortalSeats = 0;  // "mortal <seat> on|off": dev invulnerability spares that player (M10 tests)
double g_padRelease[4] = {0.0, 0.0, 0.0, 0.0};
unsigned short g_padReleaseButtons[4] = {0, 0, 0, 0};
bool g_padReleaseSticks[4] = {false, false, false, false};

void WatchCoop(double now) {
    if (!g_coopWatch || now < g_coopNext) return;
    g_coopNext = now + 0.2;
    const auto gw = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    if (gw) {
        std::string changes;
        const auto scan = [&](int from, int to) {
            for (int off = from; off < to; off += 4) {
                const auto v = ReadOr<std::uint32_t>(gw + static_cast<std::size_t>(off), 0);
                const auto it = g_coopGw.find(off);
                if (it != g_coopGw.end() && it->second == v) continue;
                char part[48];
                if (it == g_coopGw.end()) {
                    std::snprintf(part, sizeof(part), " +%03X=%X", off, v);
                } else {
                    std::snprintf(part, sizeof(part), " +%03X %X->%X", off, it->second, v);
                }
                changes += part;
                g_coopGw[off] = v;
            }
        };
        scan(0x58, 0x5C);
        scan(0x470, 0x4D0);
        scan(0x5C8, 0x664);
        scan(0x79C, 0x7A0);
        if (!changes.empty()) RM_LOGW("coopwatch: gw%s", changes.c_str());
    }
    const auto players = engine::ReadPlayers();
    std::string line = "count " + std::to_string(players.count) + " local slot " + std::to_string(players.localSlot);
    for (const auto& p : players.players) {
        if (!p.object) continue;
        char part[192];
        std::snprintf(part, sizeof(part), " | slot %d %s number %d control %d hp %d/%d @%08X", p.slot, p.className.c_str(), p.number,
                      p.control, p.hp.hp, p.hp.max, static_cast<unsigned>(p.object));
        line += part;
    }
    if (line != g_coopPlayersLast) {
        g_coopPlayersLast = line;
        RM_LOGW("coopwatch: players %s", line.c_str());
    }
    std::string views;
    if (const auto cam = ReadOr<std::uintptr_t>(re5::addr::kSCameraInstance, 0)) {
        for (int i = 0; i < re5::off::kSCameraViewportCount; ++i) {
            const auto vp = cam + re5::off::kSCameraViewports + static_cast<std::size_t>(i) * re5::off::kViewportSize;
            const auto active = ReadOr<std::uint8_t>(vp + re5::off::kViewportActive, 0);
            const auto camera = ReadOr<std::uintptr_t>(vp + re5::off::kViewportCamera, 0);
            if (!active && !camera) continue;
            std::int32_t r[4] = {};
            for (int k = 0; k < 4; ++k) r[k] = ReadOr<std::int32_t>(vp + re5::off::kViewportRegion + static_cast<std::size_t>(k) * 4, 0);
            char part[224];
            std::snprintf(part, sizeof(part), " | vp%d active %u no %u scene %u mode %u attr %X region %d %d %d %d camera %s", i, active,
                          ReadOr<std::uint8_t>(vp + 0x11, 0), ReadOr<std::uint8_t>(vp + 0x12, 0), ReadOr<std::uint8_t>(vp + 0x13, 0),
                          ReadOr<std::uint32_t>(vp + 0x0C, 0), r[0], r[1], r[2], r[3],
                          camera ? engine::ClassName(camera).c_str() : "-");
            views += part;
        }
    }
    if (views != g_coopViewsLast) {
        g_coopViewsLast = views;
        RM_LOGW("coopwatch: viewports%s", views.empty() ? " none" : views.c_str());
    }
    char pads[256];
    std::string padLine;
    for (int u = 0; u < 4; ++u) {
        const VirtualPad v = GetVirtualPad(u);
        std::snprintf(pads, sizeof(pads), " | user %d %s%s", u, XInputPolls(u) ? "polled" : "not polled",
                      v.on ? (" virtual " + std::to_string(v.buttons) + " " + std::to_string(v.lx) + "," + std::to_string(v.ly)).c_str() : "");
        padLine += pads;
    }
    if (padLine != g_coopPadsLast) {
        g_coopPadsLast = padLine;
        RM_LOGW("coopwatch: pads%s", padLine.c_str());
    }
}

// Virtual pad presses end by themselves ("pad <user> A [ms]").
void UpdateVirtualPads(double now) {
    for (int u = 0; u < 4; ++u) {
        if (g_padRelease[u] <= 0.0 || now < g_padRelease[u]) continue;
        VirtualPad v = GetVirtualPad(u);
        v.buttons = static_cast<unsigned short>(v.buttons & ~g_padReleaseButtons[u]);
        v.lt = v.rt = 0;
        if (g_padReleaseSticks[u]) v.lx = v.ly = v.rx = v.ry = 0;
        SetVirtualPad(u, v);
        g_padRelease[u] = 0.0;
        g_padReleaseButtons[u] = 0;
        g_padReleaseSticks[u] = false;
    }
}

// `triggers`: 1 LT, 2 RT.
unsigned short PadButtonBits(const std::string& names, int& triggers) {
    static const std::map<std::string, unsigned> bits = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
        {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}};
    unsigned short out = 0;
    std::istringstream parts(names);
    std::string name;
    while (std::getline(parts, name, '+')) {
        for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (name == "LT" || name == "RT") {
            triggers |= name == "LT" ? 1 : 2;
            continue;
        }
        const auto it = bits.find(name);
        if (it != bits.end()) out = static_cast<unsigned short>(out | it->second);
    }
    return out;
}

std::uintptr_t FirstEnemyOf(const std::string& cls) {
    for (const auto& e : engine::ReadEnemyStates()) {
        if (engine::ClassName(e.object) == cls) return e.object;
    }
    return 0;
}

// Developer "watchplayer on|off": the local player's action on every change (what a boss's move
// did to him: Wesker's knee strike left the screen black once, build/runs/20261108-012025-...).
bool g_watchPlayer = false;
std::string g_watchPlayerAction;

// A boss arena logs the boss's and the player's actions on every change (the developer watch's
// lines, a few hundred a fight), so a playtest's log shows what a boss did (Wesker's knee strike
// left the screen black once in six arenas, cause unknown).
bool g_bossWatch = false;
void StartBossWatch(const std::string& cls) {
    g_watch = {};
    g_watch.cls = cls;
    g_watchPlayer = true;
    g_watchPlayerAction.clear();
    g_bossWatch = true;
}
void StopBossWatch() {
    if (!g_bossWatch) return;
    g_watch = {};
    g_watchPlayer = false;
    g_bossWatch = false;
}

// Developer "wpwatch on|off": the weapon units the player object points to (uWp*), each one's
// state at +0x10E0 (the flamethrower uWp21's update, 0x00BA4EC0, makes its stream, the effect
// list wp2103, only in states 6, 8 and 9) and that stream at +0x1C90, with the player's action,
// on every change.
bool g_wpWatch = false;
std::string g_wpWatchLast;

void WatchWeapons() {
    const auto local = engine::LocalPlayer();
    if (!g_wpWatch || !local) return;
    std::string line;
    for (std::size_t off = 0; off < 0x4000; off += 4) {
        const auto p = ReadOr<std::uintptr_t>(local + off, 0);
        if (p < 0x00400000 || p > 0x7FFF0000 || (p & 3)) continue;
        const std::string name = engine::ClassName(p);
        if (name.rfind("uWp", 0) != 0) continue;
        char part[128];
        std::snprintf(part, sizeof(part), " +%zX %s %08X state %d stream %08X", off, name.c_str(), static_cast<unsigned>(p),
                      ReadOr<std::int32_t>(p + 0x10E0, -1), ReadOr<std::uint32_t>(p + 0x1C90, 0));
        line += part;
    }
    line += " | player " + std::to_string(engine::CurrentActionId(local)) + " " + engine::CurrentActionName(local);
    if (line == g_wpWatchLast) return;
    g_wpWatchLast = line;
    RM_LOGW("wpwatch:%s", line.c_str());
}

// Uroboros Aheri's four cores (uEm42Core, at uEm42 +0x2104..+0x2110, made by its init 0x008C0450):
// each one's action and health logged on every change while it is a boss, so a playtest shows what
// shooting them does (they open and close on their own timer; damage, break and regrow are their
// actions). research/re5-engine-map.md "Uroboros Aheri".
void WatchAheriCores() {
    static std::string last[4];
    const std::uintptr_t boss = g->boss.unit;
    if (!boss || engine::ClassName(boss) != "uEm42") {
        for (auto& l : last) l.clear();
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const auto core = ReadOr<std::uintptr_t>(boss + 0x2104 + i * 4, 0);
        if (!core || engine::ClassName(core) != "uEm42Core") continue;
        const auto hp = engine::ReadHp(core);
        char line[160];
        std::snprintf(line, sizeof(line), "%s hp %d/%d flags %02X", engine::CurrentActionName(core).c_str(), hp ? hp->hp : -1,
                      hp ? hp->max : -1, ReadOr<std::uint8_t>(core + 0x18DC, 0));
        if (last[i] == line) continue;
        last[i] = line;
        const auto bossHp = engine::ReadHp(boss);
        RM_LOGI("aheri: core %d %s (Aheri %s, hp %d)", i, line, engine::CurrentActionName(boss).c_str(), bossHp ? bossHp->hp : -1);
    }
}

void WatchUnit(double now) {
    WatchAheriCores();
    if (g_pin.unit && now < g_pin.until) {
        engine::WarpUnit(g_pin.unit, g_pin.dest);
        if (g_pin.turn) engine::TurnTo(g_pin.unit, g_pin.yaw, 0);
    }
    WatchScreen();
    WatchCoop(now);
    UpdateVirtualPads(now);
    WatchWeapons();
    if (g_watchPlayer) {
        const auto local = engine::LocalPlayer();
        const std::string action = local ? engine::CurrentActionName(local) : std::string("-");
        if (action != g_watchPlayerAction) {
            const auto hp = local ? engine::ReadHp(local) : std::nullopt;
            RM_LOGW("watch: player action %d %s, hp %d", local ? engine::CurrentActionId(local) : -1, action.c_str(), hp ? hp->hp : -1);
            g_watchPlayerAction = action;
        }
    }
    UnitWatch& w = g_watch;
    if (w.cls.empty()) return;
    const std::uintptr_t unit = FirstEnemyOf(w.cls);
    if (unit != w.unit) {
        RM_LOGW("watch: %s %08X %s", w.cls.c_str(), static_cast<unsigned>(unit), unit ? "found" : "gone");
        w.unit = unit;
        w.hp = -1;
        w.action = -2;
        w.since = now;
    }
    if (!unit) return;
    const auto hp = engine::ReadHp(unit);
    const int h = hp ? hp->hp : -1;
    const int action = engine::CurrentActionId(unit);
    // What the enemy hit check (0x009E0700, its collector 0x00963C90) turns a hit away by: the
    // gate +0x10E0 == 0x80000000, the byte +0x1E24, bit 0x80 of +0x18DC; its hit record +0x129C.
    const auto gate = ReadOr<std::uint32_t>(unit + 0x10E0, 0);
    const auto block = ReadOr<std::uint8_t>(unit + 0x1E24, 0);
    const auto flags = ReadOr<std::uint32_t>(unit + 0x18DC, 0);
    const auto record = ReadOr<std::uintptr_t>(unit + 0x129C, 0);
    const std::uint64_t guard = (static_cast<std::uint64_t>(gate) << 32) ^ (static_cast<std::uint64_t>(block) << 24) ^ (flags & 0x80) ^ (record ? 1 : 0);
    if (h == w.hp && action == w.action && guard == w.guard) return;
    float pos[3];
    for (int k = 0; k < 3; ++k) pos[k] = ReadOr<float>(unit + re5::off::kUnitPosition + k * 4, 0.0f);
    RM_LOGW("watch: %s hp %d/%d action %d %s at %.0f %.0f %.0f (%.2f s since the last change); gate %08X block %u flags %08X record %08X",
            w.cls.c_str(), h, hp ? hp->max : -1, action, engine::CurrentActionName(unit).c_str(), pos[0], pos[1], pos[2],
            now - w.since, gate, block, flags, static_cast<unsigned>(record));
    w.hp = h;
    w.action = action;
    w.guard = guard;
    w.since = now;
}

void WakeUroboros() {
    const std::uintptr_t unit = g->boss.unit;
    if (!unit || g->boss.dead || engine::ClassName(unit) != "uEm40") return;
    if (!engine::Caps().bossBrain) {
        static bool warned = false;
        if (!warned) RM_LOGW("boss: uEm40's brain cannot be released (signature check failed); it will only walk");
        warned = true;
        return;
    }
    if (ReadOr<std::uint8_t>(unit + re5::off::kEm40StoryWait, 0) == 0) return;  // released already
    SafeStore<std::uint8_t>(unit + re5::off::kEm40StoryWait, 0);
    SafeStore<std::uint8_t>(unit + re5::off::kEm40WakeRoar, 1);
    for (const auto& [offset, life] : {std::pair<std::size_t, std::int16_t>{re5::off::kEm40ArmLLife, kEm40ArmLife},
                                       {re5::off::kEm40ArmRLife, kEm40ArmLife},
                                       {re5::off::kEm40HeadLife, kEm40HeadLife}}) {
        SafeStore<std::int16_t>(unit + offset, life);
        SafeStore<std::int16_t>(unit + offset + 2, life);
    }
    RM_LOGI("boss: uEm40's own brain released; arms %d, head %d", kEm40ArmLife, kEm40HeadLife);
}

// ROADMAP M1: on a stage listed in maps.json the run owns the arena. The game's own spawns
// stay off for the whole match (also between arenas), and the core's director decides what
// to spawn where; the adapter spawns it and reports which unit became which ticket.
// M10: the player the director spawns around. Solo: the local player. Co-op: each standing player
// in turn, a few seconds each, so enemies come for both; never a fallen one.
std::uintptr_t DirectorFocusPlayer(double now) {
    if (g->run->SeatCount() < 2) return engine::LocalPlayer();
    std::vector<std::uintptr_t> standing;
    for (int seat = 0; seat < 2; ++seat) {
        const auto unit = engine::HumanPlayerInSlot(seat);
        const auto hp = unit ? engine::ReadHp(unit) : std::nullopt;
        if (hp && hp->hp > 0) standing.push_back(unit);
    }
    if (standing.empty()) return engine::LocalPlayer();
    static double switchedAt = 0.0;
    static std::size_t turn = 0;
    if (now - switchedAt > 4.0) {
        switchedAt = now;
        ++turn;
    }
    return standing[turn % standing.size()];
}

void DriveDirector(double now) {
    WatchUnit(now);  // a developer watch, quiet or not
    rm::RunController& run = *g->run;
    const bool directorStage = g->match.inMatch && run.HasActiveRun() && run.capabilities().Has(rm::cap::kDirector) &&
                               CurrentMap() != nullptr;
    director::BlockNativeSpawns(directorStage || g->devQuiet);
    run.SetMapStage(g->match.inMatch ? CurrentMapKey() : -1);
    if (!g->match.inMatch) {
        g->directorUnits.clear();
        g->boss = {};
        StopBossWatch();
        g_stuckTracks.clear();
        g_trapSpots.clear();
        g_stuckRemaining = -1;
    }
    if (!run.DirectorActive() || run.phase() != rm::Phase::ArenaActive || g->devQuiet) return;
    // M6: a boss's health is the arena's, not the story's. Set while it settles (its first
    // updates may still write its own).
    if (g->boss.unit && g->boss.hp > 0 && now - g->boss.spawnedAt < 3.0) {
        const auto hp = engine::ReadHp(g->boss.unit);
        if (hp && hp->max != g->boss.hp && hp->hp > 0 && engine::SetHealth(g->boss.unit, g->boss.hp)) {
            RM_LOGI("director: boss health %d/%d -> %d", hp->hp, hp->max, g->boss.hp);
        }
    }
    // A moment before the first enemies: the arena's inventory, chests and carried state are
    // set up first, and the player gets their bearings.
    // A boss waits longer, counted down on screen (it came too soon, user playtest 2026-11-03).
    constexpr double kDirectorGraceSeconds = 3.0;
    constexpr double kBossGraceSeconds = 8.0;
    const rm::MapDef* arenaMap = CurrentMap();
    const bool bossArena = arenaMap != nullptr && arenaMap->boss.has_value();
    const double grace = bossArena ? kBossGraceSeconds : kDirectorGraceSeconds;
    // A boss waiting at its story spot for the player (Uroboros Mkono in its room) is found, not
    // counted down to.
    const bool bossWaits = bossArena && arenaMap->boss->spawn && arenaMap->boss->trigger > 0.0;
    const bool noBossYet = bossArena && run.director() != nullptr && run.director()->alive() + run.director()->pending() == 0 &&
                           run.director()->dead() == 0;
    if (g->arenaBeganAt >= 0.0 && now - g->arenaBeganAt < grace) {
        if (noBossYet) {
            const int left = static_cast<int>(std::ceil(grace - (now - g->arenaBeganAt)));
            g->notice = bossWaits ? "FIND " + Upper(Plain(arenaMap->boss->name))
                                  : Upper(Plain(arenaMap->boss->name)) + " IS COMING   " + std::to_string(left);
            g->noticeUntil = now + 0.3;
        }
        return;
    }
    if (noBossYet && bossWaits && run.director()->WaitingForPlayer()) {
        g->notice = "FIND " + Upper(Plain(arenaMap->boss->name));
        g->noticeUntil = now + 0.3;
    }
    std::optional<std::array<double, 3>> player;
    if (const auto local = DirectorFocusPlayer(now)) {
        player = std::array<double, 3>{ReadOr<float>(local + re5::off::kUnitPosition, 0.0f),
                                       ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                                       ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f)};
    }
    // A boss comes from a point with nothing between it and the player, when the map has one.
    // Never preferably near a spot an enemy stood stuck at this match (g_trapSpots).
    rm::Director::PointFilter prefer = [](const std::array<double, 3>& p) { return !NearTrap(p); };
    if (bossArena && player) prefer = [&](const std::array<double, 3>& p) { return OpenToPlayer(*player, p) && !NearTrap(p); };
    for (const rm::SpawnRequest& r : run.DirectorTick(now, player, prefer)) {
        director::SpawnSpec spec;
        spec.unitClass = r.unit_class;
        for (int i = 0; i < 3; ++i) spec.pos[i] = static_cast<float>(r.pos[i]);
        if (player) spec.facing = static_cast<float>(std::atan2((*player)[0] - r.pos[0], (*player)[2] - r.pos[2]));
        // A boss at its story spot turns the way the story turns it (its set's angle).
        if (r.facing) spec.facing = static_cast<float>(*r.facing);
        spec.weapon = static_cast<int>(r.arm);
        spec.fields = r.fields;
        // A boss senses the player across its arena: the widest search the game's own spawns use
        // (search type 1 and a circle of 8000 units, seen once in the survey; most use 1000). It
        // walked off into a wall with its descriptor's own (user playtest 2026-11-03, the Uroboros).
        if (r.boss) {
            spec.fields.emplace_back("search_type", 1.0);
            spec.fields.emplace_back("search_forward", 0.0);
            spec.fields.emplace_back("search_circle", 8000.0);
        }
        const float* pos = spec.pos;
        // A class whose archive is not loaded would hang the game when created; the director
        // stops picking it for this arena.
        if (resources::Available() && !resources::ArchiveLoaded(r.unit_class)) {
            RM_LOGW("director: %s is not loaded on this stage; not spawned, dropped for this arena", r.unit_class.c_str());
            run.DirectorClassUnavailable(r.unit_class);
            run.DirectorSpawnFailed(r.ticket);
            continue;
        }
        const std::uintptr_t unit = director::Spawn(spec);
        if (!unit) {
            RM_LOGW("director: %s at %.0f %.0f %.0f failed", r.unit_class.c_str(), pos[0], pos[1], pos[2]);
            run.DirectorSpawnFailed(r.ticket);
            continue;
        }
        std::string id = "dir_" + std::to_string(r.ticket);
        g->directorUnits[unit] = id;
        run.DirectorSpawned(r.ticket, id);
        if (r.boss) {
            g->boss = {unit, r.hp, now};
            StartBossWatch(r.unit_class);
            RM_LOGI("director: boss %s (%s) spawned, health %d", r.unit_class.c_str(),
                    CurrentMap() && CurrentMap()->boss ? CurrentMap()->boss->name.c_str() : "?", r.hp);
        }
        const rm::Director* d = run.director();
        RM_LOGI("director: %s (arm %llX, %zu fields) at %.0f %.0f %.0f -> %08X; %d alive, %d dead of %d", r.unit_class.c_str(),
                static_cast<unsigned long long>(r.arm), r.fields.size(), pos[0], pos[1], pos[2], static_cast<unsigned>(unit),
                d ? d->alive() : 0, d ? d->dead() : 0, d ? d->budget() : 0);
    }
    TraceDirectorArena(now, player);
    RescueStuckEnemies(now, player);
    WakeUroboros();
}

void DriveArena(double now) {
    rm::RunController& run = *g->run;
    if (run.phase() != rm::Phase::ArenaActive) return;

    // A boss's fall plays before the world stops for the results (the Uroboros' death, Wesker's
    // stagger: DriveBossFall); nothing ends the arena meanwhile, and a boss beaten stays beaten.
    const bool bossDown = g->boss.dead && run.IsArenaComplete();
    if (bossDown && now - g->boss.diedAt < kBossFallSeconds) return;
    // M10: in co-op a player at zero health is down (the match goes on: SetCoopDeathHold); the run
    // fails when both are.
    if (!bossDown && run.SeatCount() > 1) {
        for (int seat = 0; seat < run.SeatCount() && seat < 2; ++seat) {
            const double since = g->match.seatZeroHpSince[static_cast<std::size_t>(seat)];
            if (run.SeatDown(seat) || since < 0 || now - since < 2.0) continue;
            // Dying is not down: the other player can still help them up (the game's revive). Down
            // is a death action (die_common, an execution's die_*), or a minute at zero.
            const auto unit = engine::HumanPlayerInSlot(seat);
            const std::string action = unit ? engine::CurrentActionName(unit) : std::string();
            if (action.rfind("cPlAction_pl_act_die", 0) != 0 && now - since < 60.0) continue;
            SeatTurn turn(seat);
            Report(run.OnPlayerDied(now), seat == 0 ? "player 1 down" : "player 2 down");
            g->notice = "PLAYER " + std::to_string(seat + 1) + " IS DOWN";
            g->noticeUntil = now + 4.0;
            if (run.phase() != rm::Phase::ArenaActive) return;  // both down: the run failed
        }
    } else if (!bossDown && g->match.zeroHpSince >= 0 && now - g->match.zeroHpSince >= 2.0) {
        // Player death: HP held at zero (Mercenaries solo has no revive).
        Report(run.OnPlayerDied(now), "player died");
        return;
    }
    if (run.IsArenaComplete()) {
        const int reach = run.state().arena_count;
        const auto result = run.CompleteArena(bossDown ? g->boss.diedAt : now);
        if (result.ok()) {
            SetStatus("arena cleared in " + std::to_string(static_cast<int>(result->time_seconds)) + " s");
            RM_LOGI("arena %d cleared in %.0f s, rating %s, damage taken %.0f", result->arena, result->time_seconds,
                    std::string(rm::RatingName(result->rating)).c_str(), result->damage_taken);
            if (run.state().arena_count > reach) {
                RM_LOGI("endless: the run map now reaches arena %d", run.state().arena_count);
                LogRunMap();
            }
            if (g->settings.endMatchOnClear && g->match.inMatch) {
                RequestMercMatchEnd();
                g->arenaEndRequested = true;
                RM_LOGI("arena clear: requested mercenaries time-up to end the match");
            }
        } else {
            Report(result.status(), "complete arena");
        }
        return;
    }
}

const char* BossArchiveForRun();   // the specials' borrowed boss moves, below
const char* PartnerClassForRun();  // Josh's backup partner, below

// The plaga a Majini of this type (descriptor mPlagaType) bursts into. The game's set-record
// load (0x0072AB20) queues it for the stage's own Majini; a run-planned guest's comes with it.
const char* PlagaClass(int type) {
    switch (type) {
        case 1: return "uEm0d";
        case 2: return "uEm0e";
        case 4: return "uEm0c";
        case 8: return "uEm0b";
        default: return nullptr;
    }
}

// M3: from START RUN on, every load of the run brings every item the content hands out with the
// player's items (rewards, the shop, chests and carried-over purchases may hold any of them)
// and the armed stage's roster with its enemies (a class or weapon the stage does not use
// itself loads too): the run map node's, with its planned guests (M3.3), when the stage is the
// node's. Outside runs nothing extra loads.
void UpdatePreloads() {
    if (!resources::Available()) return;
    const bool runLoads = g->run->HasActiveRun() || g->rogueModePending || autopilot::Armed();
    const bool planned = g->run->HasActiveRun() && CurrentMapKey() == g->run->PlannedMapKey();
    const std::vector<std::string> guests = planned ? g->run->PlannedGuests() : std::vector<std::string>{};
    const char* bossArchive = runLoads ? BossArchiveForRun() : nullptr;
    const char* partnerClass = runLoads ? PartnerClassForRun() : nullptr;
    static int lastStage = -2;
    static bool lastRun = false;
    static std::vector<std::string> lastGuests;
    static const char* lastBoss = nullptr;
    static const char* lastPartner = nullptr;
    static bool lastEffects = false;
    // The mod's own effects with a run's loads, once built and redirected (UpdateModRedirects).
    const bool effects = runLoads && !effectspack::Ready().empty();
    if (runLoads == lastRun && CurrentMapKey() == lastStage && guests == lastGuests && bossArchive == lastBoss &&
        partnerClass == lastPartner && effects == lastEffects) {
        return;
    }
    lastEffects = effects;
    lastRun = runLoads;
    lastStage = CurrentMapKey();
    lastGuests = guests;
    lastBoss = bossArchive;
    lastPartner = partnerClass;
    resources::Extras extras;
    std::vector<std::string> layouts;  // the campaign area's ladders (world::SetStageLayouts)
    if (runLoads) {
        // Every gun, and every other item perks, rewards and the shop hand out: a first aid
        // spray carried to a stage that does not load it ends the game ("Failed open file ...
        // it0304.mod").
        auto addItem = [&](std::int64_t item) {
            const auto id = static_cast<std::uint16_t>(item);
            if (item > 0 && item <= 0xFFFF && std::find(extras.playerItems.begin(), extras.playerItems.end(), id) == extras.playerItems.end()) {
                extras.playerItems.push_back(id);
            }
        };
        for (const rm::WeaponBaseDef& w : g->content->weapons()) {
            if ((w.game_item >> 8) == 1) addItem(w.game_item);
        }
        for (const rm::EquipmentDef& e : g->content->loadouts().equipment) addItem(e.game_item);
        // A stage's own computer partner (the Port's Josh) comes with his own gear, which only the
        // player's own loadout may happen to load: his knife's model missing ended the game as
        // Wesker ("Failed open file ... wp0100.mod", user playtest 2026-11-03; as Sheva it was
        // there). Every player knife and the rocket launcher (Sheva's, the rest of what that
        // stage loaded then) go too, about 3 MB.
        for (const std::int64_t item : {0x0101, 0x010E, 0x0123, 0x0124, 0x010D}) addItem(item);
        for (const rm::PerkDef& perk : g->content->perks()) {
            for (const rm::EffectDef& e : perk.effects) addItem(static_cast<std::int64_t>(e.params.game_item));
        }
        auto addWeapon = [&](std::int64_t arm) {
            const auto id = static_cast<std::uint32_t>(arm);
            if (arm > 0xFFFF && std::find(extras.enemyWeapons.begin(), extras.enemyWeapons.end(), id) == extras.enemyWeapons.end()) {
                extras.enemyWeapons.push_back(id);
            }
        };
        // The chest statue (M4.1), so chests work on any stage.
        extras.enemyClasses.push_back("uOm0100");
        // The boss whose moves the build's specials borrow (M8: its motion lists; nothing of it is
        // created). As a class, so its own weapons come along.
        if (bossArchive != nullptr) extras.enemyClasses.push_back(bossArchive);
        // The boss Jill's SMG (uWp76, arm 0x70176) for the twin guns spike (E16, dev "twinguns").
        if (bossArchive != nullptr && std::string_view(bossArchive) == "uEm84") addWeapon(0x70176);
        // The partner the build's backup calls in (E22): her class with its effects (0x007D5820
        // adds the uPl01 archive); her gear is among the run's guns and knives.
        if (partnerClass != nullptr) extras.enemyClasses.push_back(partnerClass);
        // The game's Organize/Buy screen (M7.1).
        if (g->settings.nativeItemBox && engine::Caps().itemBox) extras.uiArchives.push_back("ShopResource");
        const rm::MapDef* own = CurrentMap();
        const std::optional<rm::MapDef> map =
            own == nullptr ? std::nullopt : planned ? g->run->ArenaMap(CurrentMapKey()) : std::optional<rm::MapDef>(*own);
        if (map) {
            extras.enemyArchives = map->preload;
            layouts = map->layouts;
            if (map->boss) extras.enemyClasses.push_back(map->boss->unit_class);
            auto addClass = [&](const std::string& name) {
                if (std::find(extras.enemyClasses.begin(), extras.enemyClasses.end(), name) == extras.enemyClasses.end()) {
                    extras.enemyClasses.push_back(name);
                }
            };
            // In chapter 3's areas (s3xx) the game hands a Majini given uWp5f a uWp6f and one
            // given uWp60 a uWp63 (0x0072AB20: the save's stage number / 100 == 3): both load.
            const bool chapter3 = map->stage / 100 == 3;
            auto addArm = [&](std::int64_t arm) {
                addWeapon(arm);
                if (chapter3 && arm == 0x7015F) addWeapon(0x7016F);
                if (chapter3 && arm == 0x70160) addWeapon(0x70163);
            };
            // The class archive brings what the class itself needs (its parts, its own weapons,
            // uEm10Eff for the Majini; 0x007D5820); its descriptors' weapons and plagas are added.
            for (const rm::MapEnemy& e : map->roster) {
                addClass(e.unit_class);
                for (const std::int64_t arm : e.arms) addArm(arm);
                for (const rm::EnemyProfile& p : e.profiles) {
                    for (const auto& [field, value] : p.fields) {
                        if (field == "arm") addArm(static_cast<std::int64_t>(value));
                        if (field == "plaga") {
                            if (const char* plaga = PlagaClass(static_cast<int>(value))) addClass(plaga);
                        }
                    }
                }
            }
        }
        std::string guestLine;
        for (const std::string& guest : guests) guestLine += " " + guest;
        if (!guestLine.empty()) RM_LOGI("resources: stage %d brings the run's guests:%s", CurrentMapKey(), guestLine.c_str());
    }
    if (effects) extras.enemyArchives.push_back(rm::fx::kEffectsArchive);
    world::SetStageLayouts(layouts);
    world::SetHiddenModels(g_devHiddenModels ? *g_devHiddenModels : std::vector<std::string>{});
    RM_LOGI("resources: %s, stage %d: %zu player items, %zu enemy classes, %zu enemy weapons and %zu archives go with every load",
            runLoads ? "run" : "no run", CurrentMapKey(), extras.playerItems.size(), extras.enemyClasses.size(),
            extras.enemyWeapons.size(), extras.enemyArchives.size());
    resources::SetRunExtras(std::move(extras));
}

// M4.1: a run on a map with chest positions brings its own chests. The stage's time-bonus
// statues stay off from START RUN on (they are created while the match loads); each arena
// places the positions its director picked, once, when the match is running.
void PlaceArenaChests() {
    if (!world::Available()) return;
    rm::RunController& run = *g->run;
    const bool runLoads = run.HasActiveRun() || g->rogueModePending || autopilot::Armed();
    const rm::MapDef* map = CurrentMap();
    world::BlockNativeChests(runLoads && map != nullptr && !map->chests.empty() && run.capabilities().Has(rm::cap::kDirector));
    const rm::Director* director = run.director();
    if (!director || !run.DirectorActive() || run.phase() != rm::Phase::ArenaActive || !g->match.inMatch) return;
    const std::uint64_t key = (static_cast<std::uint64_t>(g->matchGeneration) << 8) | static_cast<std::uint64_t>(run.state().arena_index);
    if (g->chestsPlacedFor == key) return;
    g->chestsPlacedFor = key;
    int placed = 0;
    for (const auto& p : director->chests()) {
        const float pos[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
        if (world::PlaceChest(pos, 30)) ++placed;
    }
    RM_LOGI("chests: %d of %zu placed for arena %d", placed, director->chests().size(), run.state().arena_index);
}

// The countdown floor only applies while an arena is being fought.
void UpdateTimerPolicy() {
    // The whole run (arenas, rewards, shop, route choice) happens inside one match, so the
    // countdown is held for as long as the run is alive; it is released when the run ends.
    const rm::Phase phase = g->run->phase();
    const bool alive = g->run->HasActiveRun() && phase != rm::Phase::RunComplete && phase != rm::Phase::RunFailed;
    const bool hold = g->settings.timerPolicy == "hold" && alive && !g->transition.pending;
    SetMercTimerFloor(hold ? g->settings.timerHoldSeconds : -1.0f);
    // A run has no clock and no score on screen, from the first arena to the stage changes
    // and the run's last screens; the floor keeps the countdown from ending the match.
    SetMercHudHidden(g->run->HasActiveRun() && !g->settings.mercHud);
}

// Developer probe: the scene request fields of the scene manager (what the game's own
// Retry/Quit paths write), logged on change.
void ProbeScene() {
    static std::uint32_t lastRequest = 0xFFFFFFFF;
    static std::uint32_t lastArg = 0xFFFFFFFF;
    const auto scene = ReadOr<std::uintptr_t>(re5::addr::kSceneManager, 0);
    if (!scene) return;
    const auto request = ReadOr<std::uint32_t>(scene + re5::off::kSceneRequest, 0);
    const auto arg = ReadOr<std::uint32_t>(scene + re5::off::kSceneRequestArg, 0);
    if (request != lastRequest || arg != lastArg) {
        RM_LOGI("scene request %08X arg %08X (was %08X/%08X)", request, arg, lastRequest, lastArg);
        lastRequest = request;
        lastArg = arg;
    }
    // Game-flow object: current scene (+0x38), transition target/arg/pending (+0x1304/+0x130C/+0x131C),
    // result (+0x12B4), busy flags (+0x12E0/+0x12E4/+0x12F0).
    static std::array<std::uint32_t, 8> lastFlow{};
    static bool flowSeen = false;
    const auto flow = ReadOr<std::uintptr_t>(re5::addr::kGameFlow, 0);
    if (!flow) return;
    const std::array<std::uint32_t, 8> now = {
        ReadOr<std::uint32_t>(flow + 0x38, 0),   ReadOr<std::uint32_t>(flow + 0x1304, 0),
        ReadOr<std::uint32_t>(flow + 0x130C, 0), ReadOr<std::uint32_t>(flow + 0x131C, 0) & 0xFF,
        ReadOr<std::uint32_t>(flow + 0x12B4, 0), ReadOr<std::uint32_t>(flow + 0x12E0, 0),
        ReadOr<std::uint32_t>(flow + 0x12E4, 0), ReadOr<std::uint32_t>(flow + 0x12F0, 0),
    };
    if (!flowSeen || now != lastFlow) {
        RM_LOGI("game flow: scene %08X target %X arg %X pending %X result %08X busy %X/%X/%X", now[0], now[1], now[2],
                now[3], now[4], now[5], now[6], now[7]);
        lastFlow = now;
        flowSeen = true;
    }
}

void ApplyHeals() {
    rm::RunController& run = *g->run;
    if (run.PendingHeal() <= 0.0) return;
    const std::uintptr_t local = engine::LocalPlayer();
    const auto hp = local ? engine::ReadHp(local) : std::nullopt;
    // Mercenaries refills HP at every match start, so a heal is kept until the
    // player is alive and hurt; a shop heal therefore carries into the fight.
    if (!hp || hp->hp <= 0 || hp->hp >= hp->max) return;
    for (const auto& request : run.ConsumeHealRequests()) {
        const auto current = engine::ReadHp(local);
        if (!current) break;
        const int amount = static_cast<int>(std::lround(request.fraction_of_max * current->max));
        engine::WriteHp(local, current->hp + amount);
        RM_LOGI("heal %+d hp from %s (%d -> %d/%d)", amount, request.source.c_str(), current->hp,
                std::min(current->hp + amount, current->max), current->max);
    }
}

// Time-bonus pickups during a run's arena are chests; the countdown add is suppressed for
// as long as a run is alive (the countdown is held anyway).
void UnitPosition(std::uintptr_t unit, float out[3]);

// M10: the player nearest a point (a chest just opened): its seat (0 in solo or unknown).
int NearestSeat(const float pos[3]) {
    const int count = g->run->SeatCount();
    int best = 0;
    float bestDistance = -1.0f;
    for (int seat = 0; seat < count; ++seat) {
        const auto unit = engine::HumanPlayerInSlot(seat);
        if (!unit) continue;
        float p[3] = {};
        UnitPosition(unit, p);
        const float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (bestDistance < 0.0f || d < bestDistance) {
            bestDistance = d;
            best = seat;
        }
    }
    return best;
}

void OpenChests(double now) {
    rm::RunController& run = *g->run;
    SuppressMercAddTime(run.HasActiveRun() && g->match.inMatch);
    // Keys (M8): a chest opened without a key stands again at its spot a moment later.
    for (auto it = g->relockChests.begin(); it != g->relockChests.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        if (run.phase() == rm::Phase::ArenaActive) {
            const std::uintptr_t unit = world::PlaceChest(it->pos.data(), 30);
            RM_LOGI("keys: locked chest back at %.0f %.0f %.0f -> %08X", it->pos[0], it->pos[1], it->pos[2],
                    static_cast<unsigned>(unit));
        }
        it = g->relockChests.erase(it);
    }
    const int pickups = TakeTimeBonusPickups();
    for (int i = 0; i < pickups; ++i) {
        float pos[3] = {};
        const bool located = TakeTimeBonusPosition(pos);
        if (run.phase() != rm::Phase::ArenaActive) break;
        // M10: the chest is the nearest player's (their items, their gun refined).
        SeatTurn turn(located && run.SeatCount() > 1 ? NearestSeat(pos) : 0);
        const auto chest = run.OpenChest(now, EquippedWeaponBaseId());
        if (!chest.ok()) {
            Report(chest.status(), "open chest");
            continue;
        }
        if (chest->locked) {
            g->notice = "LOCKED   KILL MAJINI FOR KEYS";
            g->noticeUntil = now + 3.0;
            if (located) g->relockChests.push_back({{pos[0], pos[1], pos[2]}, now + 2.5});
            RM_LOGI("keys: chest locked (no key)%s", located ? ", it comes back" : "");
            continue;
        }
        std::string line = "CHEST";
        if (!chest->weapon_name.empty()) line += "   " + Upper(Plain(chest->weapon_name));
        if (!chest->item_name.empty()) {
            line += "   " + Upper(Plain(chest->item_name)) + (chest->item_count > 1 ? " X" + std::to_string(chest->item_count) : "");
        }
        if (chest->supplies > 0) line += "   " + std::to_string(chest->supplies) + " SUPPLIES";
        if (!chest->refined_name.empty()) line += "   " + Upper(Plain(chest->refined_name)) + " REFINED TO " + Upper(chest->refined_rarity);
        if (chest->key_used) line += "   KEY USED";
        g->notice = line;
        g->noticeUntil = now + 4.0;
        RM_LOGI("chest %d: %s", chest->number, line.c_str());
    }
}

// ---- Build specials (M8): the button combo, the move, its HUD ---------------------------------
// The combo: the keyboard key (adapter.json special_key_scancode, default Q) or the controller
// buttons held together (special_pad, default LB and RB): XInput bits for XInput pads, and the
// DirectInput button numbers the game's own config.ini [JOYPAD] gives those names.
std::map<std::string, int> g_joypadButtons;

void LoadJoypadMapping() {
    wchar_t docs[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) return;
    std::ifstream in(std::filesystem::path(docs) / L"CAPCOM" / L"RESIDENT EVIL 5" / L"config.ini");
    std::string line;
    bool joypad = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '[') {
            joypad = line == "[JOYPAD]";
            continue;
        }
        const auto eq = line.find('=');
        if (!joypad || eq == std::string::npos) continue;
        const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        if (!value.empty() && std::isdigit(static_cast<unsigned char>(value[0]))) g_joypadButtons[name] = std::atoi(value.c_str());
    }
    // The game's names for the stick clicks.
    if (g_joypadButtons.count("LSTICK_PUSH")) g_joypadButtons["LS"] = g_joypadButtons["LSTICK_PUSH"];
    if (g_joypadButtons.count("RSTICK_PUSH")) g_joypadButtons["RS"] = g_joypadButtons["RSTICK_PUSH"];
    RM_LOGI("special: %zu controller buttons named by the game's config.ini", g_joypadButtons.size());
}

std::uint16_t XInputBit(const std::string& name) {
    static const std::map<std::string, int> bits = {
        {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LB", 0x0100}, {"RB", 0x0200},
        {"BACK", 0x0020}, {"START", 0x0010}, {"LS", 0x0040}, {"RS", 0x0080}};
    const auto it = bits.find(name);
    return it != bits.end() ? static_cast<std::uint16_t>(it->second) : std::uint16_t{0};
}

// The combos: slot 0 the build's special, slot 1 its special_2 (Wesker's burst).
int SpecialKey(int slot) { return slot == 1 ? g->settings.special2Key : g->settings.specialKey; }
const std::vector<std::string>& SpecialPad(int slot) { return slot == 1 ? g->settings.special2Pad : g->settings.specialPad; }

// Held now, and on which device (for the log).
bool SpecialComboDown(int slot, const char** source = nullptr) {
    // M10: player 1 has the keyboard, the DirectInput pad and their XInput pad; player 2 their pad.
    const int seat = g->run->ActiveSeat();
    if (seat == 0 && RawKeyDown(SpecialKey(slot))) {
        if (source) *source = "keyboard";
        return true;
    }
    const auto& names = SpecialPad(slot);
    if (names.empty()) return false;
    const unsigned short xbuttons = seat == 0 ? RawXInputButtons() : RawXInputButtonsOf(PadUserOfSeat(seat));
    bool xinput = true, dinput = seat == 0;
    for (const std::string& name : names) {
        const std::uint16_t bit = XInputBit(name);
        xinput = xinput && bit != 0 && (xbuttons & bit) == bit;
        const auto it = g_joypadButtons.find(name);
        dinput = dinput && it != g_joypadButtons.end() && RawJoyButton(it->second);
    }
    if (source) *source = xinput ? "XInput pad" : "DirectInput pad";
    return xinput || dinput;
}

// The binding as the input hooks hide it from the game (all of a pad combo's buttons, or none
// of them on a device that cannot name one).
OwnedInput OwnedFor(int slot) {
    OwnedInput o;
    o.key = SpecialKey(slot);
    bool xinput = true, joy = true;
    for (const std::string& name : SpecialPad(slot)) {
        const std::uint16_t bit = XInputBit(name);
        xinput = xinput && bit != 0;
        o.xinput = static_cast<unsigned short>(o.xinput | bit);
        const auto it = g_joypadButtons.find(name);
        joy = joy && it != g_joypadButtons.end() && it->second >= 0 && it->second < 32;
        if (joy) o.joy |= 1u << it->second;
    }
    if (!xinput) o.xinput = 0;
    if (!joy) o.joy = 0;
    return o;
}

std::string SpecialBindingText(int slot) {
    char key[32] = {};
    if (GetKeyNameTextA(SpecialKey(slot) << 16, key, sizeof(key)) <= 0) std::snprintf(key, sizeof(key), "KEY %02X", SpecialKey(slot));
    std::string pad;
    for (const std::string& name : SpecialPad(slot)) pad += (pad.empty() ? "" : "+") + name;
    if (g->run->ActiveSeat() > 0) return pad;  // M10: player 2 has only their pad
    return Upper(key) + (pad.empty() ? "" : " / " + pad);
}

// What the player may do a special in: on his feet and in control (standing, moving, aiming,
// firing, reloading, the knife, a melee or a dodge), not grabbed, hit, down, using an item or in
// a story action (user playtest 2026-11-03: specials while grabbed and downed).
bool ActionAllowsSpecial(const std::string& action) {
    static const char* const kAllowed[] = {
        "cPlAction_pl_act_wait", "cPlAction_pl_act_relax", "cPlAction_pl_act_walk", "cPlAction_pl_act_run",
        "cPlAction_pl_act_back", "cPlAction_pl_act_turn", "cPlAction_pl_act_wall_", "cPlAction_pl_act_weapon_",
        "cPlAction_pl_act_knife_", "cPlAction_pl_act_ready_wait", "cPlAction_pl_act_attack_", "cPlAction_pl_act_avoid_",
    };
    for (const char* prefix : kAllowed) {
        if (action.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// Sniper rifles (the weapon records whose scope type is not 0) aim over the shoulder like any gun in
// a run; the scope is a button away while aiming (the user, 2026-11-09). The game reads the scope
// type of the gun in hand every frame (the camera, the reticle: research/re5-engine-map.md "Gun
// stat tables"), so 0 plays it the normal guns' way and the real value brings the scope back; the
// aim is started over so its opening sees the change. Off again when the player stops aiming.
struct ScopeState {
    std::map<std::uintptr_t, std::int32_t> originals;  // weapon record's scope type -> the game's value
};
ScopeState g_scope;
// Each player's (M10 local co-op; index = seat): the scope open, the button last tick, the aim, the
// record of the scoped gun in hand. The records are the game's, one per gun model: a model scoped
// for one player is scoped for both while they both hold it.
struct ScopeSeat {
    bool open = false;
    bool buttonWas = false;
    double aimedAt = -1.0e9;
    std::uintptr_t held = 0;
};
std::array<ScopeSeat, 2> g_scopeSeats;
constexpr double kScopeAimGrace = 0.4;  // a moment out of the aim's actions (a shot's end) keeps it

void RestoreScopes() {
    for (const auto& [address, value] : g_scope.originals) SafeStore<std::int32_t>(address, value);
    if (!g_scope.originals.empty()) RM_LOGI("scope: the game's %zu scopes put back", g_scope.originals.size());
    g_scope = {};
    g_scopeSeats = {};
}

// The scope button: player 1's Z or their pad's right stick click; player 2's pad's.
bool ScopeButtonDown(int seat) {
    const auto joy = [](const char* name) {
        const auto it = g_joypadButtons.find(name);
        return it != g_joypadButtons.end() && RawJoyButton(it->second);
    };
    if (seat > 0) {
        const unsigned short x = RawXInputButtonsOf(PadUserOfSeat(seat));
        return (x & XInputBit("RS")) != 0 && (x & XInputBit("LS")) == 0;
    }
    if (RawKeyDown(g->settings.scopeKey)) return true;
    const unsigned short xbuttons = RawXInputButtons();
    // The right stick's click alone: with the left one too it is the second special.
    const bool right = (xbuttons & XInputBit("RS")) != 0 || joy("RS");
    const bool left = (xbuttons & XInputBit("LS")) != 0 || joy("LS");
    return right && !left;
}

// The active player's turn (SeatTurn): their gun, aim and button; the records are written once
// every player had their turn (ApplyScopes).
void DriveScope(double now) {
    if (!engine::Caps().weaponScope || !g->settings.sniperNormalAim) return;
    const int seat = std::clamp(g->run->ActiveSeat(), 0, 1);
    ScopeSeat& me = g_scopeSeats[static_cast<std::size_t>(seat)];
    const auto local = g->match.inMatch && g->run->HasActiveRun() ? engine::LocalPlayer() : 0;
    if (!local) {
        if (seat == 0 && !g_scope.originals.empty()) RestoreScopes();
        me = {};
        return;
    }
    if (g_scope.originals.empty()) {
        for (int i = 0; i < re5::addr::kWeaponRecordCount; ++i) {
            const auto address = re5::addr::kWeaponRecords + static_cast<std::size_t>(i) * re5::addr::kWeaponRecordSize + re5::off::kWeaponScopeType;
            const auto value = ReadOr<std::int32_t>(address, 0);
            if (value != 0) g_scope.originals[address] = value;
        }
        RM_LOGI("scope: %zu scoped guns aim like any gun in a run", g_scope.originals.size());
        if (g_scope.originals.empty()) return;
    }
    // The gun in hand, if it is one of them.
    std::uintptr_t held = 0;
    const auto inv = local + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots && !held; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1 || ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1) continue;
        const auto raw = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        const auto address = re5::addr::kWeaponRecords + (raw & 0xFF) * re5::addr::kWeaponRecordSize + re5::off::kWeaponScopeType;
        if ((raw & 0xFF00) == 0x100 && g_scope.originals.count(address)) held = address;
    }
    me.held = held;
    const std::string action = engine::CurrentActionName(local);
    if (held && (action.rfind("cPlAction_pl_act_weapon_ready", 0) == 0 || action.rfind("cPlAction_pl_act_weapon_fire", 0) == 0)) me.aimedAt = now;
    const bool aiming = held && now - me.aimedAt < kScopeAimGrace;
    const bool button = ScopeButtonDown(seat);
    const bool pressed = button && !me.buttonWas;
    me.buttonWas = button;
    const bool wasOpen = me.open;
    if (!aiming) me.open = false;
    else if (pressed) me.open = !me.open;
    if (me.open == wasOpen || !aiming) return;
    // The record now, before the aim starts over (the other players' turns come later this tick).
    const auto it = g_scope.originals.find(held);
    if (it != g_scope.originals.end()) SafeStore<std::int32_t>(held, me.open ? it->second : 0);
    RM_LOGI("scope: player %d %s (%s)", seat + 1, me.open ? "through the scope" : "over the shoulder", action.c_str());
    static const char* const kAimStart = "cPlAction_pl_act_weapon_ready_begin";
    const auto ids = engine::ActionIdsOf(local, kAimStart);
    if (ids.empty() || !engine::StartAction(local, ids.front(), kAimStart)) RM_LOGW("scope: the aim could not be started over");
}

// Every scoped gun's record: its own scope while a player has it open, else over the shoulder.
void ApplyScopes() {
    for (const auto& [address, value] : g_scope.originals) {
        bool open = false;
        for (const ScopeSeat& s : g_scopeSeats) open = open || (s.open && s.held == address);
        const std::int32_t wanted = open ? value : 0;
        if (ReadOr<std::int32_t>(address, wanted) != wanted) SafeStore<std::int32_t>(address, wanted);
    }
}

// The game's own screens and pause: its pause menu, a paused world, the item screen.
bool ItemScreenOpen() {
    const auto game = ReadMercTimer().object;
    const auto flags = game ? ReadOr<std::uintptr_t>(game + re5::off::kGameItemScreen, 0) : 0;
    const bool open = flags != 0 && ReadOr<std::uint8_t>(flags, 0) == 1;
    static int last = -1;
    if (static_cast<int>(open) != last) {
        if (last >= 0) RM_LOGI("hud: item screen flag %d -> %d", last, static_cast<int>(open));
        last = open;
    }
    return open;
}

bool GamePaused() {
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    const auto screens = app ? ReadOr<std::uint32_t>(app + re5::off::kAppScreenFlags, 0) : 0;
    const auto flags = app ? ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) : 0;
    return (screens & re5::off::kAppScreensPaused) != 0 || (flags & (re5::off::kAppPauseRequest | re5::off::kAppPaused)) != 0 ||
           matchui::PauseMenuOpen();
}

// Why a special cannot start now ("" when it can).
std::string SpecialBlocked(std::uintptr_t local) {
    if (!local) return "no player";
    if (matchui::IsOpen() || overlay::IsFocused()) return "a mod screen";
    if (GamePaused()) return "paused";
    if (ItemScreenOpen()) return "the item screen";
    const auto hp = engine::ReadHp(local);
    if (!hp || hp->hp <= 0) return "down";
    // Bleeding out: the game holds a dying player at 1 HP (its dying camera, uEfCamDying, turns on
    // at exactly 1: 0x00852B00), and the specials worked there (user playtest 2026-11-03).
    if (hp->hp == 1) return "dying";
    const std::string action = engine::CurrentActionName(local);
    if (!action.empty() && !ActionAllowsSpecial(action)) return action;
    return "";
}

// The player's facing on the ground plane (uCoord quaternion, +Z forward).
bool PlayerFacing(std::uintptr_t player, float& fx, float& fz) {
    const float qx = ReadOr<float>(player + re5::off::kUnitRotation, 0.0f), qy = ReadOr<float>(player + re5::off::kUnitRotation + 4, 0.0f);
    const float qz = ReadOr<float>(player + re5::off::kUnitRotation + 8, 0.0f), qw = ReadOr<float>(player + re5::off::kUnitRotation + 12, 0.0f);
    const float norm = qx * qx + qy * qy + qz * qz + qw * qw;
    if (norm < 0.9f || norm > 1.1f) return false;
    fx = 2.0f * (qx * qz + qw * qy);
    fz = 1.0f - 2.0f * (qx * qx + qy * qy);
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 0.01f) return false;
    fx /= len;
    fz /= len;
    return true;
}

// The move in progress.
struct SpecialMove {
    std::string kind;  // empty: none
    std::string move;  // a dash special's move: "dash" or "lunge"
    double start = 0.0;
    double until = 0.0;
    double nextPulse = 0.0;
    float origin[3] = {};
    float end[3] = {};           // dash and lunge: where he stops, on the floor
    std::uintptr_t victim = 0;   // lunge: the enemy struck on arrival
    bool victimStaggered = false;
    float dirX = 0.0f, dirZ = 1.0f;
    int damage = 0;
    int hits = 0;
    int pulses = 0;
    bool borrowed = false;  // a boss's own moves play it (dual_smg: the shots land during the sweep)
    double seconds = 0.0;   // dual_smg: how long it fires without them
    std::set<std::uintptr_t> staggered;  // dual_smg: enemies its shots have staggered
    bool twinGuns = false;  // dual_smg: the boss Jill's two SMGs in her hands, firing
    bool twinWanted = false;  // dual_smg: they come once the hold runs (or the sweep fires without it)
    double nextTwinShot = 0.0;
    int twinShots = 0;
};
SpecialMove g_move;

// The boss Jill's two SMGs on the player (E16; research/re5-engine-map.md "Twin guns"). Seen in game
// 2026-11-05 (the mod's own runs, scripts/test_twinguns.sh): two uWp76 units sit in her hands
// (joints 13 and 8, her table's pose 1) and fire with the game's own effects (casings, smoke,
// sound); their shots (uWp86 shells) fly but hurt no enemy whatever their attack id (eight tried),
// and a player's gun builds the shot's attack group from the player (static), so every shot's
// attack is turned off (-1: the shell's update skips it) and Twin Fury's own pulses do the damage.
// Their archive loads with every stage of a Jill Battle Suit run.
std::uintptr_t g_twinGuns[2] = {0, 0};
int g_twinGunsGeneration = -1;  // the match they were made in (gone with it)
bool g_twinGunsInSpecial = true;  // dev "twinguns special off": Twin Fury without them (a control)

bool ArmTwinGuns(std::uintptr_t local, const char* why) {
    if (g_twinGunsGeneration != g->matchGeneration) g_twinGuns[0] = g_twinGuns[1] = 0;
    if (g_twinGuns[0] && g_twinGuns[1]) return true;
    if (!engine::Caps().twinGuns || !local) return false;
    if (resources::Available() && !resources::ArchiveLoaded("uWp76")) {
        static int said = 0;
        if (said++ < 2) RM_LOGI("twin guns: uWp76 is not loaded here (%s)", why);
        return false;
    }
    for (int n = 0; n < 2; ++n) {
        if (g_twinGuns[n]) continue;
        const std::uintptr_t entry = re5::addr::kItemJointTable + static_cast<std::uintptr_t>(1 + n * 2) * 0x30;
        const int joint = ReadOr<std::int32_t>(entry, -1);
        float offset[3], euler[3];
        for (int k = 0; k < 3; ++k) {
            offset[k] = ReadOr<float>(entry + 0x10 + k * 4, 0.0f);
            euler[k] = ReadOr<float>(entry + 0x20 + k * 4, 0.0f);
        }
        const std::uintptr_t gun = engine::CreateTwinGun();
        const bool attached = gun && joint >= 0 && engine::AttachTwinGun(gun, local, joint, offset, euler);
        if (gun && !attached) engine::DeleteUnit(gun);
        g_twinGuns[n] = attached ? gun : 0;
    }
    g_twinGunsGeneration = g->matchGeneration;
    RM_LOGI("twin guns: %08X %08X in her hands (%s)", static_cast<unsigned>(g_twinGuns[0]), static_cast<unsigned>(g_twinGuns[1]), why);
    return g_twinGuns[0] && g_twinGuns[1];
}

void DisarmTwinGuns(const char* why) {
    if (g_twinGunsGeneration != g->matchGeneration) g_twinGuns[0] = g_twinGuns[1] = 0;  // gone with their match
    bool any = false;
    for (std::uintptr_t& gun : g_twinGuns) {
        if (gun) any = engine::DeleteUnit(gun) || any;
        gun = 0;
    }
    if (any) RM_LOGI("twin guns: put away (%s)", why);
}

// Both guns fire once along (fx, fz) from beside her hands; their shots hurt nobody. Returns the shots.
constexpr int kTwinMuzzleBones[2] = {11, 6};  // the boss's: gun 0 at bone 11, gun 1 at bone 6
int FireTwinGunsOnce(std::uintptr_t local, float fx, float fz) {
    if (!local || g_twinGunsGeneration != g->matchGeneration) return 0;
    const float dir[3] = {fx, 0.0f, fz};
    world::WatchCreated("uWp86");
    int fired = 0;
    for (int n = 0; n < 2; ++n) {
        if (!g_twinGuns[n]) continue;
        // From her hands, as the boss's leave hers (0x0093C2E0: bones 11 and 6). A point 0.6 m in
        // front of her looked wrong: the shots came from in front of her (user playtest 2026-11-07).
        float muzzle[3];
        if (!engine::JointPosition(local, kTwinMuzzleBones[n], muzzle)) {
            const float side = n == 0 ? 25.0f : -25.0f;  // her right and left hands
            muzzle[0] = ReadOr<float>(local + re5::off::kUnitPosition, 0.0f) + fx * 30.0f + fz * side;
            muzzle[1] = ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f) + 120.0f;
            muzzle[2] = ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f) + fz * 30.0f - fx * side;
        }
        static int traced = 0;
        if (traced < 4 && g->settings.developerMode) {
            ++traced;
            RM_LOGI("twin guns: gun %d shot from %.0f %.0f %.0f (her feet at %.0f %.0f %.0f)", n, muzzle[0], muzzle[1], muzzle[2],
                    ReadOr<float>(local + re5::off::kUnitPosition, 0.0f), ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                    ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f));
        }
        fired += engine::FireTwinGun(g_twinGuns[n], muzzle, dir) ? 1 : 0;
    }
    for (const std::uintptr_t s : world::TakeCreated()) SafeStore<std::uint32_t>(s + 0x1CA0, 0xFFFFFFFFu);
    world::WatchCreated("");
    return fired;
}
bool g_comboHeld[2] = {false, false};
double g_specialDeniedUntil = 0.0;
double g_burstUntil = -1.0;  // the burst's ring shows it acting until then

constexpr double kSmgPulse = 0.15;
constexpr double kTwinShotEvery = 0.08;  // the twin guns' fire rate (the boss's: every 3 frames)
constexpr float kSmgRange = 1400.0f;     // 14 m
constexpr float kSmgCone = 0.766f;       // cos 40 degrees
constexpr double kBackupPulse = 1.2;
constexpr float kBackupRange = 1800.0f;  // 18 m
constexpr int kBackupDamage = 150;

void EndSpecialMove(const char* why) {
    if (g_move.kind.empty()) return;
    RM_LOGI("special: %s%s%s over (%s): %d pulses, %d hits%s%s", g_move.kind.c_str(), g_move.move.empty() ? "" : " ",
            g_move.move.c_str(), why, g_move.pulses, g_move.hits,
            g_move.staggered.empty() ? "" : (", " + std::to_string(g_move.staggered.size()) + " staggered").c_str(),
            g_move.twinGuns ? (", " + std::to_string(g_move.twinShots) + " twin gun shots").c_str() : "");
    if (g_move.twinGuns) DisarmTwinGuns("the sweep is over");
    g_move = {};
}

// Enemies in front of the player within range (cone: cos of the half angle; -1 = all around).
std::vector<std::pair<std::uintptr_t, float>> EnemiesAround(std::uintptr_t player, float range, float cone, float fx, float fz) {
    std::vector<std::pair<std::uintptr_t, float>> out;
    const float px = ReadOr<float>(player + re5::off::kUnitPosition, 0.0f), pz = ReadOr<float>(player + re5::off::kUnitPosition + 8, 0.0f);
    for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
        if (e.hp <= 0) continue;
        const float dx = ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f) - px;
        const float dz = ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f) - pz;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d > range) continue;
        if (cone > -1.0f && d > 1.0f && (dx * fx + dz * fz) / d < cone) continue;
        out.emplace_back(e.object, d);
    }
    return out;
}

// ---- Wesker's moves (Uroboros Speed) -------------------------------------------------------------
// The special (slot 0) dashes, or lunges (his long-range melee) at an enemy ahead: a staggered one
// first, the state the game's melee prompt follows. He turns to face it the game's way
// (engine::TurnTo) and ends in front of it facing it, so the game's own melee prompt is up and his
// own melee move finishes it. The second special (slot 1) is the burst. Every move is checked
// against the level with the game's own segment query, the one its dodges use
// (engine::CastSegment): he stops short of walls and lands on a floor within a step of his own,
// else he does not move. The dash and the lunge play his own forward dodge (engine::StartDodge),
// started the way the game starts one. A Majini he hits takes the game's own reaction
// (engine::StartAction).
constexpr double kDashSeconds = 0.15;
constexpr double kLungeSeconds = 0.2;
constexpr double kBurstSeconds = 0.3;
constexpr float kChestHeight = 110.0f;    // the wall check's height above his feet
constexpr float kWallMargin = 60.0f;      // he stops this far before a wall
constexpr float kMaxStep = 150.0f;        // a floor further above or below is not taken
constexpr float kDashHitRadius = 120.0f;  // enemies this close to the dash's line are hit
constexpr int kDashDamage = 200;
constexpr float kLungeRange = 1200.0f;       // 12 m: an enemy lined up ahead
constexpr float kLungeCone = 0.906f;         // cos 25 degrees
constexpr float kStaggeredRange = 1000.0f;   // 10 m: a staggered enemy anywhere ahead
constexpr float kStaggeredCone = 0.643f;     // cos 50 degrees
constexpr float kLungeStop = 110.0f;         // he ends this far in front of the enemy
constexpr int kLungeDamage = 300;
constexpr int kLungeDamageStaggered = 650;   // a staggered one takes his ranged melee: blown away
constexpr float kBurstRadius = 350.0f;
constexpr int kBurstDamage = 350;
constexpr double kLungeCooldown = 1.2;       // times the special's cooldown

struct DashPlan {
    std::string move;  // "dash", "lunge"
    std::uintptr_t target = 0;
    bool staggered = false;
    float end[3] = {};
    float distance = 0.0f;  // engine units travelled
    std::string why;        // for the log: what shaped it
};
std::string g_armedMove;    // the HUD: what the button would do now
double g_armedAt = -1.0;

float Horizontal(const float a[3], const float b[3]) {
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dz * dz);
}

// A character reeling in a state the game's melee prompt follows (a head, knee or leg hit, a stagger).
bool Staggered(std::uintptr_t enemy) {
    const std::string action = engine::CurrentActionName(enemy);
    if (action.rfind("cEm", 0) != 0 || action.find("ActDmg") == std::string::npos) return false;
    for (const char* moving : {"Flyoff", "Fall", "Fire", "Ice", "Ladder"}) {
        if (action.find(moving) != std::string::npos) return false;
    }
    return true;
}

// The floor under (x, z) near height y: a cast from a step above down past a step below.
bool FloorAt(float x, float y, float z, float& floorY) {
    const float from[3] = {x, y + kMaxStep, z};
    const float to[3] = {x, y - 2.0f * kMaxStep, z};
    const auto r = engine::CastSegment(from, to);
    if (!r || r->flags == 0) return false;
    floorY = r->hit[1];
    return std::fabs(floorY - y) <= kMaxStep;
}

// How far he can go from `from` along (dx, dz), up to `want`: the wall check at chest height, then
// a floor at the end (shortened in fifths until there is one). Writes the end; 0 = no move.
float ClearDistance(const float from[3], float dx, float dz, float want, float end[3], std::string& why) {
    for (int k = 0; k < 3; ++k) end[k] = from[k];
    float reach = want;
    const float a[3] = {from[0], from[1] + kChestHeight, from[2]};
    const float b[3] = {from[0] + dx * want, from[1] + kChestHeight, from[2] + dz * want};
    const auto wall = engine::CastSegment(a, b);
    if (!wall) {
        why = "no collision query";
        return 0.0f;
    }
    char text[96];
    if (wall->Blocked()) {
        const float hitAt[3] = {wall->hit[0], from[1], wall->hit[2]};
        reach = std::clamp(Horizontal(from, hitAt) - kWallMargin, 0.0f, want);
        std::snprintf(text, sizeof(text), "wall at %.1f m (flags %X)", (reach + kWallMargin) / 100.0f, wall->flags);
    } else {
        std::snprintf(text, sizeof(text), "clear (flags %X)", wall->flags);
    }
    why = text;
    for (int fifth = 5; fifth >= 1; --fifth) {
        const float d = reach * static_cast<float>(fifth) / 5.0f;
        if (d < 30.0f) break;
        float floorY = 0.0f;
        if (FloorAt(from[0] + dx * d, from[1], from[2] + dz * d, floorY)) {
            end[0] = from[0] + dx * d;
            end[1] = floorY;
            end[2] = from[2] + dz * d;
            if (fifth < 5) why += ", floor at " + std::to_string(fifth * 20) + " percent";
            return d;
        }
    }
    why += ", no floor";
    return 0.0f;
}

// What the special's button does now, with the world checked.
DashPlan PlanDash(std::uintptr_t local, float dashDistance) {
    DashPlan plan;
    float origin[3];
    for (int k = 0; k < 3; ++k) origin[k] = ReadOr<float>(local + re5::off::kUnitPosition + k * 4, 0.0f);
    float fx = 0.0f, fz = 1.0f;
    PlayerFacing(local, fx, fz);
    // Lunge: staggered enemies ahead first (a wider cone), then the nearest lined up.
    struct Candidate {
        std::uintptr_t enemy;
        float d;
        bool staggered;
    };
    std::vector<Candidate> ahead;
    for (const auto& [enemy, d] : EnemiesAround(local, kLungeRange, kStaggeredCone, fx, fz)) {
        const bool staggered = d <= kStaggeredRange && Staggered(enemy);
        if (!staggered) {
            const float ex = ReadOr<float>(enemy + re5::off::kUnitPosition, 0.0f) - origin[0];
            const float ez = ReadOr<float>(enemy + re5::off::kUnitPosition + 8, 0.0f) - origin[2];
            if (d > 1.0f && (ex * fx + ez * fz) / d < kLungeCone) continue;
        }
        ahead.push_back({enemy, d, staggered});
    }
    std::sort(ahead.begin(), ahead.end(), [](const Candidate& a, const Candidate& b) {
        return a.staggered != b.staggered ? a.staggered : a.d < b.d;
    });
    int tried = 0;
    for (const Candidate& c : ahead) {
        if (tried++ >= 3) break;
        float at[3];
        for (int k = 0; k < 3; ++k) at[k] = ReadOr<float>(c.enemy + re5::off::kUnitPosition + k * 4, 0.0f);
        if (std::fabs(at[1] - origin[1]) > 2.0f * kMaxStep || c.d < kLungeStop + 50.0f) continue;
        const float dx = (at[0] - origin[0]) / c.d, dz = (at[2] - origin[2]) / c.d;
        const float want = c.d - kLungeStop;
        std::string why;
        float end[3];
        const float reach = ClearDistance(origin, dx, dz, want, end, why);
        if (reach >= want * 0.85f) {
            plan.move = "lunge";
            plan.target = c.enemy;
            plan.staggered = c.staggered;
            plan.distance = reach;
            for (int k = 0; k < 3; ++k) plan.end[k] = end[k];
            plan.why = engine::ClassName(c.enemy) + (c.staggered ? " staggered" : "") + " at " +
                       std::to_string(static_cast<int>(c.d / 100.0f)) + " m, " + why;
            return plan;
        }
    }
    plan.move = "dash";
    plan.distance = ClearDistance(origin, fx, fz, dashDistance, plan.end, plan.why);
    return plan;
}

// A Majini's reaction to one of his hits (other classes have other actions at these ids: the
// class name check in engine::StartAction leaves them alone).
const char* ReactionClass(int id) {
    switch (id) {
        case re5::off::kEm10DmgHead: return "cEm10ActDmgHead";
        case re5::off::kEm10DmgFlyoff: return "cEm10ActDmgFlyoff";
        case re5::off::kEm10DmgYoroke: return "cEm10ActDmgYoroke";
        default: return "";
    }
}

// One of his hits: damage, then (if it lives, and `reaction` >= 0) the game's reaction.
std::string StrikeEnemy(std::uintptr_t enemy, int damage, int reaction, double now) {
    if (!director::Damage(enemy, damage)) return "missed";
    ++g_move.hits;
    g_specialVictims[enemy] = now;
    const auto hp = engine::ReadHp(enemy);
    if (!hp || hp->hp <= 0) return "killed";
    if (reaction < 0) return "hit";
    return engine::StartAction(enemy, reaction, ReactionClass(reaction)) ? "reacts" : "hit (no reaction)";
}

// ---- The rarity remake: elements and legendary effects in the world (rm::WeaponProc) -------------
// The core decides what a hit or a kill does (a burn, a freeze, an arc, a blast); this plays it.
// The damage goes through the game's SubHp (director::Damage) and the deaths it causes are the
// player's (g_specialVictims). Reactions are the enemies' own actions, found by name in their
// table, and only for the classes whose reactions were seen in game (scripts/test_reactions.sh,
// 2026-11-08: the town, village and soldier Majini, uEm10, uEm12 and uEm14): on fire, frozen,
// acid, flash, blown away, a stagger, a head burst. The burning
// soldier's death (DeadArmyBurn) asks for an effect file no PC archive has (em1026.efl: the game
// ends), so it is never used; other classes take the damage alone. The boss takes none of it (the
// core gives it more damage instead). A hit's procs are queued inside the game's damage call and
// played on the next tick.
constexpr double kElementTick = 0.5;
constexpr double kReactionCooldown = 3.0;  // a reaction the mod starts, at most this often per enemy
constexpr const char* kReactionClasses[] = {"uEm10", "uEm12", "uEm14"};

enum class Reaction { Fire, Ice, Acid, Flash, Flyoff, Yoroke, HeadBurst, None };
constexpr std::size_t kReactionKinds = 7;
constexpr const char* kReactionSuffix[kReactionKinds] = {"ActDmgFire",   "ActDmgIce",    "ActDmgSulfate",        "ActDmgFlash",
                                                         "ActDmgFlyoff", "ActDmgYoroke", "ActDeadHeadExplosion"};

struct ClassReactions {
    std::array<int, kReactionKinds> id{};
    std::array<std::string, kReactionKinds> name;
};
std::map<std::string, ClassReactions> g_classReactions;

struct ElementStatus {
    double burnUntil = 0.0, burnDps = 0.0;
    double poisonUntil = 0.0, poisonDps = 0.0;
    double frozenUntil = 0.0;
    double nextTick = 0.0;
    double owed = 0.0;          // damage below one point, carried to the next tick
    double reactedAt = -100.0;  // the last reaction the mod started on it
    // Plague (evolved Toxic): while poisoned by it, its death lets out a cloud (PlagueOnDeath).
    double cloudFraction = 0.0, cloudRadius = 0.0, cloudSeconds = 0.0;
    bool clouded = false;       // its cloud is out already
};
std::map<std::uintptr_t, ElementStatus> g_elementStatus;

struct QueuedProc {
    std::uintptr_t enemy = 0;  // the hit's target or the kill's victim
    float at[3] = {};          // where it was then
    rm::WeaponProc proc;
};
std::vector<QueuedProc> g_queuedProcs;
std::map<std::string, int> g_procLogged;  // log lines per proc kind this match

bool StatusActive(std::uintptr_t enemy, double now, double ElementStatus::*until) {
    const auto it = g_elementStatus.find(enemy);
    return it != g_elementStatus.end() && now < it->second.*until;
}

bool FrozenNow(std::uintptr_t enemy, double now) { return StatusActive(enemy, now, &ElementStatus::frozenUntil); }

void UnitPosition(std::uintptr_t unit, float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = ReadOr<float>(unit + re5::off::kUnitPosition + k * 4, 0.0f);
}

const ClassReactions* ReactionsOf(std::uintptr_t enemy) {
    const std::string cls = engine::ClassName(enemy);
    if (std::none_of(std::begin(kReactionClasses), std::end(kReactionClasses), [&cls](const char* c) { return cls == c; })) {
        return nullptr;
    }
    auto it = g_classReactions.find(cls);
    if (it == g_classReactions.end()) {
        ClassReactions r;
        r.id.fill(-1);
        const auto names = engine::ActionNames(enemy);
        if (names.empty()) return nullptr;  // not readable yet: looked up again later
        std::string found;
        for (std::size_t k = 0; k < kReactionKinds; ++k) {
            const std::string suffix = kReactionSuffix[k];
            for (std::size_t i = 0; i < names.size(); ++i) {
                const std::string& n = names[i];
                if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    r.id[k] = static_cast<int>(i);
                    r.name[k] = n;
                    break;
                }
            }
            found += " " + suffix.substr(3) + "=" + std::to_string(r.id[k]);
        }
        RM_LOGI("elements: %s reactions:%s", cls.c_str(), found.c_str());
        it = g_classReactions.emplace(cls, std::move(r)).first;
    }
    return &it->second;
}

// Not while it grabs the player, climbs, falls, flies, gets up or dies: a reaction then could
// leave the player held or the enemy stuck.
bool CanReact(std::uintptr_t enemy) {
    const std::string action = engine::CurrentActionName(enemy);
    if (action.empty()) return false;
    for (const char* busy : {"Catch", "Hang", "Hold", "Dead", "Fall", "Ladder", "Jump", "Landing", "Window", "Wall", "Appear",
                             "Demo", "Sit", "Door", "Ride", "Rise", "Flyoff", "Eat"}) {
        if (action.find(busy) != std::string::npos) return false;
    }
    return true;
}

// Starts one of its reactions; `force` skips the cooldown (a kill's), `dead` allows it at zero health.
bool React(std::uintptr_t enemy, Reaction r, double now, bool force = false, bool dead = false) {
    if (r == Reaction::None || enemy == g->boss.unit) return false;
    const ClassReactions* cr = ReactionsOf(enemy);
    if (cr == nullptr) return false;
    const auto k = static_cast<std::size_t>(r);
    const int id = cr->id[k];
    if (id < 0) return false;
    if (engine::CurrentActionId(enemy) == id) return true;  // already in it
    ElementStatus& st = g_elementStatus[enemy];
    if (!force && now - st.reactedAt < kReactionCooldown) return false;
    if (!dead && !CanReact(enemy)) return false;
    if (!engine::StartAction(enemy, id, cr->name[k].c_str(), dead)) return false;
    st.reactedAt = now;
    return true;
}

// The mod's elemental damage. A Majini it kills takes `onKill` first: a dead one takes no action
// and would only drop when its own one ends.
bool ElementDamage(std::uintptr_t enemy, double amount, double now, Reaction onKill) {
    if (enemy == 0 || enemy == g->boss.unit) return false;
    const auto hp = engine::ReadHp(enemy);
    if (!hp || hp->hp <= 0) return false;
    const int dmg = std::max(1, static_cast<int>(std::lround(amount)));
    if (dmg >= hp->hp) React(enemy, onKill, now, true);
    g_specialVictims[enemy] = now;
    return director::Damage(enemy, dmg);
}

// Living enemies within `radius` of `at` (3D), nearest first; never `except` nor the boss.
std::vector<std::pair<std::uintptr_t, float>> EnemiesNear(const float at[3], float radius, std::uintptr_t except) {
    std::vector<std::pair<std::uintptr_t, float>> out;
    for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
        if (e.hp <= 0 || e.object == except || e.object == g->boss.unit) continue;
        float p[3];
        UnitPosition(e.object, p);
        const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= radius) out.emplace_back(e.object, d);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

// Sets it burning (a new burn keeps the higher rate and the later end); true when it took its
// on-fire reaction.
bool Ignite(std::uintptr_t enemy, double dps, double seconds, double now) {
    if (enemy == g->boss.unit || dps <= 0.0) return false;
    ElementStatus& st = g_elementStatus[enemy];
    const bool fresh = now >= st.burnUntil;
    st.burnDps = fresh ? dps : std::max(st.burnDps, dps);
    st.burnUntil = std::max(st.burnUntil, now + seconds);
    if (!fresh) return false;
    st.nextTick = std::max(st.nextTick, now + kElementTick);
    return React(enemy, Reaction::Fire, now);
}

// The equipped gun's inventory record (0: none).
std::uintptr_t EquippedGunSlot() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return 0;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) != 1) continue;
        if ((ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) >> 8) != 1) return 0;
        return slot;
    }
    return 0;
}

// A setting's effect lists, "a|b": each in turn.
std::vector<std::string> Alternatives(const std::string& value) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from <= value.size()) {
        const std::size_t bar = value.find('|', from);
        const std::string one = value.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
        if (!one.empty()) out.push_back(one);
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    return out;
}

// An element's look on `unit` (settings element_effects, by proc kind; an evolved element's own
// "<kind>_evolved" first): at most once a second for the same enemy and kind.
void PlayLook(const rm::WeaponProc& p, std::uintptr_t unit, double now) {
    if (!unit || !engine::Caps().effects || g->settings.elementEffects.empty()) return;
    const std::string kind(rm::ProcKindName(p.kind));
    auto it = p.evolved ? g->settings.elementEffects.find(kind + "_evolved") : g->settings.elementEffects.end();
    if (it == g->settings.elementEffects.end()) it = g->settings.elementEffects.find(kind);
    if (it == g->settings.elementEffects.end()) return;
    static std::map<std::pair<std::uintptr_t, std::string>, double> last;
    if (last.size() > 512) last.clear();
    double& at = last[{unit, kind}];
    if (now - at < 1.0 && at > 0.0) return;
    at = now;
    static std::map<std::string, int> logged;
    std::string played;
    for (const std::string& path : Alternatives(it->second)) {
        if (engine::PlayEffect(path.c_str(), unit)) {
            played = path;
            break;
        }
    }
    if (logged[it->second]++ < 3) {
        RM_LOGI("elements: look for %s: %s", kind.c_str(), played.empty() ? ("none in memory of " + it->second).c_str() : played.c_str());
    }
}

// Poisons it (a new poison keeps the higher rate and the later end); with Plague's numbers its
// death lets out a cloud. True when it took its acid reaction.
bool PoisonEnemy(std::uintptr_t enemy, double dps, double seconds, double now, double cloudFraction = 0.0, double cloudRadius = 0.0) {
    if (enemy == g->boss.unit || dps <= 0.0) return false;
    ElementStatus& st = g_elementStatus[enemy];
    const bool fresh = now >= st.poisonUntil;
    st.poisonDps = fresh ? dps : std::max(st.poisonDps, dps);
    st.poisonUntil = std::max(st.poisonUntil, now + seconds);
    if (cloudFraction > 0.0) {
        st.cloudFraction = std::max(st.cloudFraction, cloudFraction);
        st.cloudRadius = std::max(st.cloudRadius, cloudRadius);
        st.cloudSeconds = std::max(st.cloudSeconds, seconds);
    }
    if (!fresh) return false;
    st.nextTick = std::max(st.nextTick, now + kElementTick);
    return React(enemy, Reaction::Acid, now);
}

void QueueProcs(std::uintptr_t enemy, const std::vector<rm::WeaponProc>& procs) {
    for (const rm::WeaponProc& p : procs) {
        QueuedProc q;
        q.enemy = enemy;
        UnitPosition(enemy, q.at);
        q.proc = p;
        g_queuedProcs.push_back(std::move(q));
    }
}

// Shock's lightning (settings element_arcs; 2026-11-11, the user: "couldn't we make our own ray's
// arc"): an arc between two units (their chests, followed as they move; where they were once gone)
// or from the player's gun, drawn over the picture each frame through the game's camera
// (DrawElementArcs), flickering, gone after its time.
struct ElementArc {
    std::uintptr_t from = 0, to = 0;  // 0: the player's gun (from) / the point (to)
    std::uintptr_t gunOwner = 0;      // from 0: the player whose gun it is (M10: either player)
    float a[3] = {}, b[3] = {};
    double born = 0.0, until = 0.0;
    std::uint32_t seed = 0;
};
std::vector<ElementArc> g_arcs;
constexpr float kArcHeight = 120.0f;  // an arc's ends: chest high
constexpr std::size_t kMaxArcs = 48;

void AddArc(std::uintptr_t from, std::uintptr_t to, double now, double seconds) {
    if (!g->settings.elementArcs || g_arcs.size() >= kMaxArcs || to == 0) return;
    ElementArc arc;
    arc.from = from;
    arc.to = to;
    if (!from) arc.gunOwner = engine::LocalPlayer();
    if (from) UnitPosition(from, arc.a);
    UnitPosition(to, arc.b);
    arc.born = now;
    arc.until = now + seconds;
    static std::uint32_t seeds = 0x2545F491u;
    seeds = seeds * 1664525u + 1013904223u;
    arc.seed = seeds;
    g_arcs.push_back(arc);
}

void ApplyProc(const QueuedProc& q, double now) {
    const rm::WeaponProc& p = q.proc;
    const std::string kind(rm::ProcKindName(p.kind));
    int reached = 0;
    bool reacted = false;
    const char* note = "";
    const auto alive = [](std::uintptr_t u) {
        const auto hp = engine::ReadHp(u);
        return hp && hp->hp > 0;
    };
    switch (p.kind) {
        case rm::ProcKind::Burn:
            if (!alive(q.enemy)) {
                note = ", the target already dead";
                break;
            }
            reacted = Ignite(q.enemy, p.dps, p.seconds, now);
            PlayLook(p, q.enemy, now);
            reached = 1;
            break;
        case rm::ProcKind::Poison: {
            if (!alive(q.enemy) || q.enemy == g->boss.unit) {
                note = ", the target already dead";
                break;
            }
            reacted = PoisonEnemy(q.enemy, p.dps, p.seconds, now, p.cloud_dps_fraction, p.cloud_radius);
            PlayLook(p, q.enemy, now);
            reached = 1;
            break;
        }
        case rm::ProcKind::Freeze: {
            if (!alive(q.enemy) || q.enemy == g->boss.unit) {
                note = ", the target already dead";
                break;
            }
            g_elementStatus[q.enemy].frozenUntil = now + p.seconds;
            reacted = React(q.enemy, Reaction::Ice, now, true);
            PlayLook(p, q.enemy, now);
            reached = 1;
            break;
        }
        case rm::ProcKind::Shock:
        case rm::ProcKind::Arc: {
            if (p.kind == rm::ProcKind::Shock && alive(q.enemy)) {
                reacted = React(q.enemy, Reaction::Flash, now);
                PlayLook(p, q.enemy, now);
            }
            int left = p.targets > 0 ? p.targets : 99;
            std::uintptr_t jumpFrom = q.enemy;
            for (const auto& [enemy, d] : EnemiesNear(q.at, static_cast<float>(p.radius), q.enemy)) {
                (void)d;
                if (left-- <= 0) break;
                PlayLook(p, enemy, now);  // the jump to it
                AddArc(jumpFrom, enemy, now, 0.35);
                if (p.kind == rm::ProcKind::Arc) jumpFrom = enemy;  // a chain: each jump from the last
                if (ElementDamage(enemy, p.damage, now, Reaction::Flyoff)) {
                    ++reached;
                    if (alive(enemy)) React(enemy, Reaction::Flash, now);
                }
            }
            break;
        }
        case rm::ProcKind::IgniteAround:
            if (alive(q.enemy)) {
                reacted = Ignite(q.enemy, p.dps, p.seconds, now);
                PlayLook(p, q.enemy, now);
            }
            for (const auto& [enemy, d] : EnemiesNear(q.at, static_cast<float>(p.radius), q.enemy)) {
                (void)d;
                Ignite(enemy, p.dps, p.seconds, now);
                PlayLook(p, enemy, now);
                ++reached;
            }
            break;
        case rm::ProcKind::PoisonCloud:
            // Plague: the cloud of a poisoned enemy that died poisons those around it (no cloud of theirs).
            for (const auto& [enemy, d] : EnemiesNear(q.at, static_cast<float>(p.radius), q.enemy)) {
                (void)d;
                PoisonEnemy(enemy, p.dps, p.seconds, now);
                PlayLook(p, enemy, now);
                ++reached;
            }
            break;
        case rm::ProcKind::Execute: {
            const auto hp = engine::ReadHp(q.enemy);
            if (!hp || hp->hp <= 0 || q.enemy == g->boss.unit) {
                note = ", the target already dead";
                break;
            }
            reacted = React(q.enemy, Reaction::HeadBurst, now, true);  // its head bursts: it dies by it
            g_specialVictims[q.enemy] = now;
            director::Damage(q.enemy, hp->hp);
            reached = 1;
            break;
        }
        case rm::ProcKind::Explode:
        case rm::ProcKind::Shatter: {
            // The victim's head bursts (an explosion); everyone around takes the blast and is thrown.
            if (p.kind == rm::ProcKind::Explode) reacted = React(q.enemy, Reaction::HeadBurst, now, true, true);
            PlayLook(p, q.enemy, now);
            for (const auto& [enemy, d] : EnemiesNear(q.at, static_cast<float>(p.radius), q.enemy)) {
                (void)d;
                const Reaction thrown = p.kind == rm::ProcKind::Explode ? Reaction::Flyoff : Reaction::Yoroke;
                if (ElementDamage(enemy, p.damage, now, Reaction::Flyoff)) {
                    ++reached;
                    if (alive(enemy)) {
                        // Absolute Zero (an evolved Cryo's shatter): those it hits freeze.
                        if (p.freeze_seconds > 0.0) {
                            g_elementStatus[enemy].frozenUntil = now + p.freeze_seconds;
                            React(enemy, Reaction::Ice, now, true);
                        } else {
                            React(enemy, thrown, now, true);
                        }
                    } else if (p.kind == rm::ProcKind::Explode && p.chain > 0) {
                        // Chain Reaction (evolved Explosive): one the blast killed blows up too, next tick.
                        QueuedProc next;
                        next.enemy = enemy;
                        UnitPosition(enemy, next.at);
                        next.proc = p;
                        next.proc.chain = p.chain - 1;
                        next.proc.source = p.source + " (chain)";
                        g_queuedProcs.push_back(std::move(next));
                    }
                }
            }
            break;
        }
        case rm::ProcKind::Overkill: {
            const auto closest = EnemiesNear(q.at, static_cast<float>(p.radius), q.enemy);
            if (!closest.empty() && ElementDamage(closest.front().first, p.damage, now, Reaction::Flyoff)) reached = 1;
            break;
        }
        case rm::ProcKind::Refill: {
            const std::uintptr_t slot = EquippedGunSlot();
            const int max = slot ? ReadOr<std::int32_t>(slot + re5::off::kSlotMax, 0) : 0;
            if (slot && max > 0 && SafeStore<std::int32_t>(slot + re5::off::kSlotCount, max)) reached = 1;
            break;
        }
    }
    if (g_procLogged[kind]++ < 12) {
        RM_LOGI("elements: %s (%s) on %s %08X: %.0f damage, %.0f per second for %.1f s, %.0f range, %d reached%s%s", kind.c_str(),
                p.source.c_str(), engine::ClassName(q.enemy).c_str(), static_cast<unsigned>(q.enemy), p.damage, p.dps, p.seconds,
                p.radius, reached, reacted ? ", its own reaction" : "", note);
    }
}

// A Plague-poisoned enemy died (however): its cloud poisons those around it, once.
void PlagueOnDeath(std::uintptr_t enemy, int maxHp, double now) {
    const auto it = g_elementStatus.find(enemy);
    if (it == g_elementStatus.end()) return;
    ElementStatus& st = it->second;
    if (st.clouded || st.cloudFraction <= 0.0 || st.cloudRadius <= 0.0 || now >= st.poisonUntil + 0.5) return;
    st.clouded = true;
    QueuedProc q;
    q.enemy = enemy;
    UnitPosition(enemy, q.at);
    q.proc.kind = rm::ProcKind::PoisonCloud;
    q.proc.source = "plague";
    q.proc.element = "toxic";
    q.proc.evolved = true;
    q.proc.dps = st.cloudFraction * std::max(1, maxHp);
    q.proc.seconds = std::max(1.0, st.cloudSeconds);
    q.proc.radius = st.cloudRadius;
    g_queuedProcs.push_back(std::move(q));
}

// The flamethrower's elements (the user, 2026-11-11: "electricity has to look more like a ray that
// jumps from enemies, cryo ... after hitting the same enemy for a while it should freeze"). Its
// stream passes no hit to the mod (only its last point does: engine map "The flamethrower"), so
// while it fires (its fuel going down) every enemy in its flame takes its elements here: Shock arcs
// from the nearest one every stream_seconds to those around it, Cryo chills each one and freezes it
// after stream_seconds in the flame, Toxic poisons them, Incendiary sets them burning (Inferno
// spreads it). The flame: enemies within kFlameReach ahead of the player and kFlameHalfAngle of his
// facing, at about his height. The amounts scale from kStreamHitDamage, a hit's worth.
// With an element's look in place of the flame (settings flame_streams, SetFlameStream) the flame's
// hits are gone with it: each one in the flame takes kStreamDps a second here, every kStreamHitEvery,
// scaled as a flamethrower hit (its level, affixes, perks); Shock's stuns it (the flash grenade's
// reaction) as the flame sets it flailing. The flame took a Majini (1000) in about 3.5 s.
constexpr double kStreamDps = 300.0;
constexpr double kStreamHitEvery = 0.2;
std::string g_flameStreamPath;           // the look in place of the flame now ("" the flame)
std::string g_flameStreamCompanion;      // with it in place of the flame's second list (the mod's own effects)
bool g_flameStreamLoops = false;         // a recoloured flame: it keeps going by itself
rm::Element g_flameStreamElement = rm::Element::Incendiary;
// The stream's watch (per player: SeatTurn swaps it with the look above).
struct FlameStreamState {
    double nextLook = 0.0;
    int lastFuel = -1;
    double firingUntil = 0.0, lastTick = 0.0, nextArc = 0.0, nextStreamHit = 0.0, nextGunArc = 0.0;
    bool wasFiring = false;
    int firingLogs = 0;
    std::map<std::uintptr_t, std::pair<double, double>> chill;  // enemy -> seconds in the flame, last seen
};
FlameStreamState g_flameStreamState;

// The run's flamethrower's own element with a look in settings flame_streams that is in memory
// (its evolved one's first); looked at once a second, set for the local player's unit.
void UpdateFlameStreamLook(double now) {
    double& next = g_flameStreamState.nextLook;
    const int seat = g->run->ActiveSeat();
    if (now < next || g_flameUnit == 0 || !g->match.inMatch) {
        // The unit as it is now, every tick (a new match's flamethrower is another unit).
        SetFlameStream(seat, g_flameStreamPath.empty() || !g->match.inMatch ? 0 : g_flameUnit, g_flameStreamPath, g_flameStreamCompanion,
                       g_flameStreamLoops, g_flameStreamLoops ? g->settings.flameStreamScale : 1.0f);
        if (now < next) return;
    }
    next = now + 1.0;
    std::string path;
    rm::Element element = rm::Element::Incendiary;
    if (engine::Caps().flameStream && g->run->HasActiveRun() && g->match.inMatch && g_flameUnit != 0) {
        for (const rm::WeaponInstance& w : g->run->state().weapons) {
            if (w.base_id != "flamethrower") continue;
            for (const rm::Element e : w.elements) {
                const std::string key(rm::ElementName(e));
                auto it = w.element_evolved ? g->settings.flameStreams.find(key + "_evolved") : g->settings.flameStreams.end();
                if (it == g->settings.flameStreams.end()) it = g->settings.flameStreams.find(key);
                if (it == g->settings.flameStreams.end()) continue;
                for (const std::string& one : Alternatives(it->second)) {
                    if (!engine::EffectInMemory(one.c_str())) continue;
                    path = one;
                    break;
                }
                if (path.empty()) continue;
                element = e;
                break;
            }
            if (!path.empty()) break;
        }
    }
    // The mod's recoloured stream keeps going as the game's flame does; its recoloured second list
    // (as the game's flame with wp2104) only with settings flame_stream_companion.
    std::string companion;
    bool loops = false;
    for (const rm::fx::FlameLook& look : rm::fx::kFlameLooks) {
        if (!look.stream || path != rm::fx::FlameEffectPath(look.code, 0)) continue;
        loops = true;
        const std::string second = rm::fx::FlameEffectPath(look.code, 4);
        if (g->settings.flameStreamCompanion && engine::EffectInMemory(second.c_str())) companion = second;
    }
    if (path != g_flameStreamPath) {
        if (path.empty()) {
            RM_LOGI("flamethrower: player %d's: its flame again", seat + 1);
        } else {
            RM_LOGI("flamethrower: player %d's fires %s (%s) in place of its flame", seat + 1, path.c_str(), rm::ElementName(element).data());
        }
    }
    g_flameStreamPath = path;
    g_flameStreamCompanion = companion;
    g_flameStreamLoops = loops;
    g_flameStreamElement = element;
    SetFlameStream(seat, path.empty() ? 0 : g_flameUnit, path, companion, loops, loops ? g->settings.flameStreamScale : 1.0f);
}

// A flamethrower hit's multiplier on `enemy` (the core's, as for the game's own flame hits).
double StreamMultiplier(std::uintptr_t enemy, double now) {
    rm::HitContext ctx;
    ctx.now = now;
    ctx.weapon_id = std::string("flamethrower");
    ctx.is_melee = false;
    ctx.base_damage = kStreamDps * kStreamHitEvery;
    if (const auto hp = engine::ReadHp(enemy)) {
        ctx.target_hp = hp->hp;
        ctx.target_hp_max = hp->max;
    }
    if (const auto me = engine::ReadHp(engine::LocalPlayer()); me && me->max > 0) {  // the player whose flame it is
        ctx.player_hp = me->hp;
        ctx.player_hp_max = me->max;
    }
    ctx.target_frozen = StatusActive(enemy, now, &ElementStatus::frozenUntil);
    ctx.target_poisoned = StatusActive(enemy, now, &ElementStatus::poisonUntil);
    return g->run->ComputeOutgoingMultiplier(ctx).multiplier;  // its procs: the stream's elements below
}

constexpr float kFlameReach = 500.0f;             // 5 m
constexpr float kFlameCosHalfAngle = 0.866f;      // 30 degrees either side
constexpr float kFlameHeight = 250.0f;
constexpr double kStreamHitDamage = 120.0;

void DriveFlameStream(double now) {
    FlameStreamState& st = g_flameStreamState;  // this player's (SeatTurn)
    const int seat = g->run->ActiveSeat();
    int& lastFuel = st.lastFuel;
    double& firingUntil = st.firingUntil;
    double& lastTick = st.lastTick;
    double& nextStreamHit = st.nextStreamHit;
    double& nextArc = st.nextArc;
    bool& wasFiring = st.wasFiring;
    int& firingLogs = st.firingLogs;
    auto& chill = st.chill;
    const double dt = std::clamp(now - lastTick, 0.0, 0.25);
    lastTick = now;
    const auto player = engine::LocalPlayer();
    const std::uintptr_t slot = player && g->match.inMatch && g->run->phase() == rm::Phase::ArenaActive ? EquippedGunSlot() : 0;
    const bool lookInPlace = !g_flameStreamPath.empty();
    if (!slot || ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) != kItemFlamethrower) {
        lastFuel = -1;
        chill.clear();
        if (wasFiring) KeepFlameStreamLook(seat, false);
        wasFiring = false;
        return;
    }
    const int fuel = ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0);
    if (lastFuel >= 0 && fuel < lastFuel) firingUntil = now + 0.35;
    lastFuel = fuel;
    // Its fire with the look in place of the flame (the fuel may not go down without the flame).
    const unsigned long fired = lookInPlace ? FlameStreamFiredAt(seat) : 0;
    if (fired != 0 && GetTickCount() - fired < 350) firingUntil = std::max(firingUntil, now + 0.05);
    const bool firing = now < firingUntil;
    if (firing != wasFiring) {
        if (firingLogs < 8) {
            ++firingLogs;
            RM_LOGI("flamethrower: %s (fuel %d%s)", firing ? "firing" : "stopped", fuel, lookInPlace ? ", the element's look" : "");
        }
        wasFiring = firing;
    }
    if (lookInPlace) KeepFlameStreamLook(seat, firing);
    if (!firing) return;
    const auto elements = g->run->ElementsOf("flamethrower");
    if (elements.empty() && !lookInPlace) return;
    float fx = 0.0f, fz = 0.0f;
    if (!PlayerFacing(player, fx, fz)) return;
    float at[3];
    UnitPosition(player, at);
    std::vector<std::pair<std::uintptr_t, float>> inFlame;
    for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
        if (e.hp <= 0 || e.object == g->boss.unit) continue;
        float p[3];
        UnitPosition(e.object, p);
        const float dx = p[0] - at[0], dz = p[2] - at[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < 1.0f || d > kFlameReach || std::fabs(p[1] - at[1]) > kFlameHeight) continue;
        if ((dx * fx + dz * fz) / d < kFlameCosHalfAngle) continue;
        inFlame.emplace_back(e.object, d);
    }
    std::sort(inFlame.begin(), inFlame.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (auto it = chill.begin(); it != chill.end();) {  // out of the flame a second: the chill is gone
        it = now - it->second.second > 1.0 ? chill.erase(it) : std::next(it);
    }
    if (inFlame.empty()) return;
    // A Shock flamethrower: lightning from the gun to the nearest one in its flame, again and again.
    double& nextGunArc = st.nextGunArc;
    if (lookInPlace && g_flameStreamElement == rm::Element::Shock && now >= nextGunArc) {
        nextGunArc = now + 0.12;
        AddArc(0, inFlame.front().first, now, 0.16);
    }
    if (lookInPlace && now >= nextStreamHit) {
        nextStreamHit = now + kStreamHitEvery;
        for (const auto& [enemy, dist] : inFlame) {
            (void)dist;
            ElementDamage(enemy, kStreamDps * kStreamHitEvery * StreamMultiplier(enemy, now), now, Reaction::Flyoff);
            const auto hp = engine::ReadHp(enemy);
            if (g_flameStreamElement == rm::Element::Shock && hp && hp->hp > 0) React(enemy, Reaction::Flash, now);
        }
    }
    for (const auto& [el, evolved] : elements) {
        const rm::ElementDef* d = g->content->FindElement(el, evolved);
        if (d == nullptr) continue;
        rm::WeaponProc look;
        look.element = std::string(rm::ElementName(el));
        look.evolved = evolved;
        switch (el) {
            case rm::Element::Incendiary:
                look.kind = d->spread_radius > 0.0 ? rm::ProcKind::IgniteAround : rm::ProcKind::Burn;
                for (const auto& [enemy, dist] : inFlame) {
                    (void)dist;
                    Ignite(enemy, kStreamHitDamage * d->dps_fraction, d->seconds, now);
                    PlayLook(look, enemy, now);
                    if (d->spread_radius <= 0.0) continue;
                    float p[3];
                    UnitPosition(enemy, p);
                    for (const auto& [other, od] : EnemiesNear(p, static_cast<float>(d->spread_radius), enemy)) {
                        (void)od;
                        Ignite(other, kStreamHitDamage * d->dps_fraction, d->seconds, now);
                    }
                }
                break;
            case rm::Element::Toxic:
                look.kind = rm::ProcKind::Poison;
                for (const auto& [enemy, dist] : inFlame) {
                    (void)dist;
                    PoisonEnemy(enemy, kStreamHitDamage * d->dps_fraction, d->seconds, now, d->cloud_dps_fraction, d->radius);
                    PlayLook(look, enemy, now);
                }
                break;
            case rm::Element::Cryo:
                for (const auto& [enemy, dist] : inFlame) {
                    (void)dist;
                    auto& [seconds, seen] = chill[enemy];
                    seconds += dt;
                    seen = now;
                    if (d->stream_seconds <= 0.0 || seconds < d->stream_seconds || FrozenNow(enemy, now)) continue;
                    seconds = 0.0;
                    QueuedProc q;
                    q.enemy = enemy;
                    UnitPosition(enemy, q.at);
                    q.proc.kind = rm::ProcKind::Freeze;
                    q.proc.source = d->name + " (flamethrower)";
                    q.proc.element = look.element;
                    q.proc.evolved = evolved;
                    q.proc.seconds = d->seconds;
                    g_queuedProcs.push_back(std::move(q));
                }
                break;
            case rm::Element::Shock:
                if (d->stream_seconds <= 0.0 || now < nextArc) break;
                nextArc = now + d->stream_seconds;
                {
                    QueuedProc q;
                    q.enemy = inFlame.front().first;
                    UnitPosition(q.enemy, q.at);
                    q.proc.kind = rm::ProcKind::Shock;
                    q.proc.source = d->name + " (flamethrower)";
                    q.proc.element = look.element;
                    q.proc.evolved = evolved;
                    q.proc.damage = kStreamHitDamage * d->damage_fraction;
                    q.proc.radius = d->radius;
                    q.proc.targets = d->targets;
                    g_queuedProcs.push_back(std::move(q));
                }
                break;
            case rm::Element::Explosive:
                break;  // its kills blow up as any gun's (the kill's procs)
        }
    }
}

// The effect gallery (settings effect_gallery, or Numpad * in an arena; 2026-11-11, the user: "how to
// turn on that ... do whatever you can to make them be color coded"): every kGalleryStep the next
// effect list in memory (the resource manager's table: engine::LoadedEffectLists) plays on the
// nearest enemy (else on the player), its name in the HUD's top line and the log. Numpad + and -
// step at once, Numpad 0 holds the one shown (it plays again), and Numpad 1 to 5 make it the look
// of an element: Incendiary, Shock, Cryo, Toxic, Explosive (its procs' looks; for Shock, Cryo and
// Toxic also the flamethrower's stream in place of its flame), at once and kept in
// <data>\\element_looks.json for every later game. The effects carry their own colours (an effect
// unit has none to set: uEffectVFR's properties are its coordinates and time rates, and the two
// words a start passes are masks of its parts), so a colour is picked by picking an effect.
constexpr double kGalleryStep = 3.0;
constexpr int kKeyNumpadMultiply = 0x37, kKeyNumpadMinus = 0x4A, kKeyNumpadPlus = 0x4E, kKeyNumpad0 = 0x52;
struct GalleryPick {
    int key;
    const char* element;  // rm::ElementName
    const char* label;
    std::vector<std::string> kinds;
    bool stream;  // also the flamethrower's stream
};
const std::vector<GalleryPick>& GalleryPicks() {
    static const std::vector<GalleryPick> picks = {
        {0x4F, "incendiary", "FIRE", {"burn", "ignite_around"}, false},
        {0x50, "shock", "SHOCK", {"shock", "arc"}, true},
        {0x51, "cryo", "CRYO", {"freeze", "shatter"}, true},
        {0x4B, "toxic", "TOXIC", {"poison", "poison_cloud"}, true},
        {0x4C, "explosive", "EXPLOSIVE", {"explode"}, false},
    };
    return picks;
}

// An effect's name for the HUD line (the game font has no underscore).
std::string GalleryName(const std::string& path) {
    std::string name = Upper(path.substr(path.rfind('\\') + 1));
    std::replace(name.begin(), name.end(), '_', '-');
    return name;
}

// A key's press (down now, not at the last call).
bool KeyPressed(int scancode) {
    static std::map<int, bool> was;
    const bool down = RawKeyDown(scancode);
    bool& before = was[scancode];
    const bool pressed = down && !before;
    before = down;
    return pressed;
}

void DriveEffectGallery(double now) {
    static std::vector<std::string> lists;
    static std::size_t index = 0;
    static double at = 0.0, listedAt = -100.0;
    static std::string shown;
    static bool on = false, started = false, held = false;
    if (!started) {
        started = true;
        on = g->settings.effectGallery;
    }
    const bool inArena = engine::Caps().effects && g->match.inMatch && g->run->phase() == rm::Phase::ArenaActive;
    if (KeyPressed(kKeyNumpadMultiply) && inArena) {
        on = !on;
        g->notice = on ? "EFFECT GALLERY ON" : "EFFECT GALLERY OFF";
        g->noticeUntil = now + 2.0;
        RM_LOGI("effect gallery: %s (Numpad *)", on ? "on" : "off");
        at = now + 1.0;
    }
    const bool step = KeyPressed(kKeyNumpadPlus), back = KeyPressed(kKeyNumpadMinus), hold = KeyPressed(kKeyNumpad0);
    std::vector<const GalleryPick*> picked;
    for (const GalleryPick& p : GalleryPicks()) {
        if (KeyPressed(p.key)) picked.push_back(&p);
    }
    if (!on || !inArena) return;
    if (hold) {
        held = !held;
        g->notice = held ? "EFFECT HELD" : "EFFECT GALLERY GOES ON";
        g->noticeUntil = now + 1.5;
    }
    for (const GalleryPick* p : picked) {
        if (shown.empty()) break;
        for (const std::string& kind : p->kinds) g->settings.elementEffects[kind] = shown;
        if (p->stream) g->settings.flameStreams[p->element] = shown;
        const bool saved = SaveElementLook(g->paths, p->kinds, p->stream ? std::string(p->element) : std::string(), shown);
        g->notice = std::string(p->label) + " LOOK   " + GalleryName(shown) + (saved ? "   SAVED" : "   NOT SAVED");
        g->noticeUntil = now + 3.0;
        RM_LOGI("effect gallery: %s is the %s look%s (%s)", shown.c_str(), p->element, p->stream ? " and its flamethrower stream" : "",
                saved ? "kept in element_looks.json" : "element_looks.json could not be written; this game only");
        at = now + 3.0;
    }
    if (now - listedAt > 30.0) {
        listedAt = now;
        std::vector<std::string> fresh = engine::LoadedEffectLists();
        if (fresh != lists) {
            const auto it = std::find(fresh.begin(), fresh.end(), shown);
            index = it != fresh.end() ? static_cast<std::size_t>(it - fresh.begin()) + 1 : 0;
            lists = std::move(fresh);
            std::string names;
            for (const std::string& l : lists) names += " " + l.substr(l.rfind('\\') + 1);
            RM_LOGI("effect gallery: %zu effect lists in memory:%s", lists.size(), names.c_str());
        }
    }
    if (lists.empty()) return;
    if (back) {
        index = (index + lists.size() - (shown.empty() ? 1 : 2)) % lists.size();
        at = 0.0;
    }
    if (step) at = 0.0;
    if (now < at) return;
    at = now + kGalleryStep;
    const auto player = engine::LocalPlayer();
    if (!player) return;
    float p[3];
    UnitPosition(player, p);
    const auto nearby = EnemiesNear(p, 2000.0f, 0);
    const std::uintptr_t target = nearby.empty() ? player : nearby.front().first;
    if (held && !shown.empty()) {
        engine::PlayEffect(shown.c_str(), target);
        return;
    }
    for (std::size_t tries = 0; tries < lists.size(); ++tries) {
        const std::size_t i = index++ % lists.size();
        const std::string& path = lists[i];
        if (!engine::PlayEffect(path.c_str(), target)) continue;
        shown = path;
        g->notice = "EFFECT " + GalleryName(path) + "   " + std::to_string(i + 1) + " OF " + std::to_string(lists.size());
        g->noticeUntil = now + kGalleryStep;
        RM_LOGI("effect gallery: %s on %s (%zu of %zu)", path.c_str(), engine::ClassName(target).c_str(), i + 1, lists.size());
        return;
    }
    RM_LOGI("effect gallery: none of the %zu lists would start", lists.size());
}

// Each tick: the queued procs, then the burns and poisons (every kElementTick).
void DriveElements(double now) {
    if (!g->match.inMatch || g->run->phase() != rm::Phase::ArenaActive) {
        g_elementStatus.clear();
        g_queuedProcs.clear();
        g_procLogged.clear();
        return;
    }
    std::vector<QueuedProc> queued;
    queued.swap(g_queuedProcs);
    for (const QueuedProc& q : queued) ApplyProc(q, now);
    if (g_elementStatus.empty()) return;
    std::set<std::uintptr_t> living;
    for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
        if (e.hp > 0) living.insert(e.object);
    }
    for (auto it = g_elementStatus.begin(); it != g_elementStatus.end();) {
        ElementStatus& st = it->second;
        const bool burning = now < st.burnUntil, poisoned = now < st.poisonUntil, frozen = now < st.frozenUntil;
        if (!living.count(it->first) || (!burning && !poisoned && !frozen && now - st.reactedAt > kReactionCooldown)) {
            it = g_elementStatus.erase(it);
            continue;
        }
        if ((burning || poisoned) && now >= st.nextTick) {
            st.nextTick = now + kElementTick;
            st.owed += ((burning ? st.burnDps : 0.0) + (poisoned ? st.poisonDps : 0.0)) * kElementTick;
            const double whole = std::floor(st.owed);
            if (whole >= 1.0) {
                st.owed -= whole;
                ElementDamage(it->first, whole, now, burning ? Reaction::Fire : Reaction::Acid);
            }
        }
        ++it;
    }
}

// He turns to face a point at once, the game's way (engine::TurnTo); true when his rotation then
// faces it (within kFacingCone), which is what the game's melee prompt needs.
constexpr float kFacingCone = 0.94f;  // cos 20 degrees

bool FacePoint(std::uintptr_t local, const float at[3]) {
    const float dx = at[0] - ReadOr<float>(local + re5::off::kUnitPosition, 0.0f);
    const float dz = at[2] - ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f);
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return true;
    const float want = std::atan2(dx, dz);
    const bool turned = engine::TurnTo(local, want, 0);
    float fx = 0.0f, fz = 1.0f;
    const bool facing = PlayerFacing(local, fx, fz) && (fx * dx + fz * dz) / len >= kFacingCone;
    static int logged = 0;
    if (logged < 8 || (turned && !facing)) {
        ++logged;
        RM_LOGI("special: turn to %.2f %s; facing %.2f now (%s)", want, turned ? "done" : "refused", std::atan2(fx, fz),
                facing ? "on target" : "off target");
    }
    return facing;
}

// His own dodge plays over the move. A dodge still running after 1.5 s is ended (relax), so a
// dodge without its motion never holds him.
double g_dodgeAt = -1.0;
bool g_dodgeSeen = false;

void StartDodgeAnimation(std::uintptr_t local, const float toward[3]) {
    if (engine::StartDodge(local, toward)) {
        g_dodgeAt = Now();
        g_dodgeSeen = false;
    } else {
        static int refused = 0;
        if (refused++ < 3) RM_LOGI("special: no dodge animation (refused; current action %s)", engine::CurrentActionName(local).c_str());
    }
}

void WatchDodge(double now) {
    if (g_dodgeAt < 0.0) return;
    const auto local = engine::LocalPlayer();
    if (!local) {
        g_dodgeAt = -1.0;
        return;
    }
    const std::string action = engine::CurrentActionName(local);
    const bool dodging = action.rfind("cPlAction_pl_act_avoid", 0) == 0;
    if (dodging && !g_dodgeSeen) {
        g_dodgeSeen = true;
        RM_LOGI("special: dodge running as %s", action.c_str());
    }
    if (!dodging) {
        if (g_dodgeSeen || now - g_dodgeAt > 0.5) {
            if (!g_dodgeSeen) RM_LOGI("special: the dodge never started (current action %s)", action.c_str());
            g_dodgeAt = -1.0;
        }
        return;
    }
    if (now - g_dodgeAt > 1.5) {
        const bool back = engine::StartAction(local, re5::off::kPlActRelax, "cPlAction_pl_act_relax");
        RM_LOGW("special: dodge still running after 1.5 s (%s); back to relax: %s", action.c_str(), back ? "ok" : "refused");
        g_dodgeAt = -1.0;
    }
}

// ---- A computer partner (ROADMAP E22) -------------------------------------------------------------
// The game adds computer players itself: the Port stage's own code adds Josh with the game's
// add-player (slot 2, control 1) and places him at its story position (adapter.log 2026-11-03:
// he stood in wait near the map's origin all arena). The mod adds one the same way, next to the
// player. The add fills the save's player entry of the slot and game work's slot fields; the
// mod keeps copies and puts the save's back a second later (the new player reads its entry
// while it sets itself up) and game work's when the match is over, so nothing of it outlives
// the arena. Its class must be loaded with the stage, or the game ends ("Failed open file").
struct Partner {
    std::uintptr_t unit = 0;
    int slot = -1;
    int generation = -1;  // the match it was added in
    std::vector<std::uint8_t> saveCopy;
    double restoreSaveAt = -1.0;
    std::uint32_t gwBits = 0;
    std::int32_t gwControl = 0, gwSecond = 0, gwArg = 0;
    double addedAt = 0.0;
    int placeTries = 0;     // moves beside the player once the game has placed him
    double nextLogAt = 0.0;
    int logs = 0;
};
Partner g_partner;

// Josh's backup (the user, 2026-11-11: "while the partner is out it should give you your ammo and not
// your partners weapons ammo"): the game drops ammo for the partner's guns too. While the partner the
// mod added is in this match, ammo none of the player's guns takes is picked up as ammo for the gun
// in hand (else for the first gun in the grid that takes any). 0: kept as it is.
std::uint32_t PartnerAmmoSwap(std::uint32_t id) {
    if (!g_partner.unit || g_partner.generation != g->matchGeneration || !g->match.inMatch) return 0;
    static constexpr std::uint32_t kLauncherRounds[] = {0x0206, 0x0207, 0x0208, 0x020E, 0x020F, 0x0210};
    std::set<std::uint32_t> own;
    std::uint32_t first = 0;
    for (const GridCell& c : ReadGrid()) {
        if (!c.record || ItemStackMax(c.id) != 0) continue;  // guns only
        std::vector<std::uint32_t> takes;
        if (c.id == kItemGrenadeLauncher) {
            takes.assign(std::begin(kLauncherRounds), std::end(kLauncherRounds));  // any round it can load
        } else if (c.id != 0x010D) {  // the rocket launcher takes none
            if (const rm::WeaponBaseDef* w = WeaponForItem(c.id)) {
                if (const std::uint32_t a = AmmoItemFor(w->category)) takes.push_back(a);
            }
        }
        for (const std::uint32_t a : takes) {
            own.insert(a);
            if (first == 0) first = a;
        }
    }
    if (own.empty() || own.count(id)) return 0;
    const std::uint32_t held = HeldAmmoItem();
    return held != 0 && own.count(held) ? held : first;
}

constexpr double kPartnerPlaceAfter = 0.5;  // seconds: his setup puts him at the slot's own start first
constexpr float kPartnerBeside = 150.0f;    // he stands this far to the player's side
constexpr double kPartnerLogEvery = 3.0;

// The archive a player class loads from (0x007D5820: uPl04JoshCos1's is uPl04Josh).
std::string PlayerArchive(const std::string& cls) { return cls == "uPl04JoshCos1" ? "uPl04Josh" : cls; }

// Josh's Call for Backup: Sheva in her BSAA outfit (the add's character 1, costume 0). Her class
// loads with the stages of a run whose build has the special (UpdatePreloads).
struct Ally {
    int character;
    int costume;
    const char* cls;
};
constexpr Ally kBackupAlly = {1, 0, "uPl01ShebaNormal"};

// The ally class the run's build calls in, if any: the run's, or before it starts the build of the
// skin chosen in the menu.
const char* PartnerClassForRun() {
    rm::RunController& run = *g->run;
    const bool active = run.HasActiveRun();
    const rm::SkinDef* chosen = active ? nullptr : SelectedSkinDef(run);
    if (!active && chosen == nullptr) return nullptr;
    const rm::SkinBuild& build = active ? run.state().character.build : chosen->build;
    for (int slot = 0; slot < 2; ++slot) {
        if (build.Special(slot).kind == "backup") return kBackupAlly.cls;
    }
    return nullptr;
}

void RestorePartnerGameWork() {
    const auto gw = GameWorkPtr();
    if (!gw || g_partner.slot < 0) return;
    const std::size_t s = static_cast<std::size_t>(g_partner.slot) * 4;
    SafeStore<std::uint32_t>(gw + re5::off::kGameWorkPlayerBits, g_partner.gwBits);
    SafeStore<std::int32_t>(gw + re5::off::kGameWorkPlayerControl + s, g_partner.gwControl);
    SafeStore<std::int32_t>(gw + re5::off::kGameWorkPlayerSecond + s, g_partner.gwSecond);
    SafeStore<std::int32_t>(gw + re5::off::kGameWorkPlayerArg + s, g_partner.gwArg);
}

// Adds `character` in `costume` beside the local player; the unit, or 0 with the reason logged.
std::uintptr_t AddPartner(int character, int costume, const char* why) {
    const auto local = engine::LocalPlayer();
    const auto gw = GameWorkPtr();
    if (!local || !gw || !engine::Caps().partner) {
        RM_LOGW("partner: not added (%s): no player, game work or add-player", why);
        return 0;
    }
    if (g_partner.unit && g_partner.generation == g->matchGeneration) {
        RM_LOGI("partner: not added (%s): one is here already", why);
        return 0;
    }
    const std::string cls = engine::PlayerClassName(character, costume);
    if (cls.empty() || (resources::Available() && !resources::ArchiveLoaded(PlayerArchive(cls)))) {
        RM_LOGW("partner: not added (%s): %s (character %d, costume %d) is not loaded on this stage", why,
                cls.empty() ? "no class" : cls.c_str(), character, costume);
        return 0;
    }
    // A free slot: the Port stage's own Josh takes 2.
    const auto players = ReadOr<std::uintptr_t>(re5::addr::kSPlayerInstance, 0);
    int slot = -1;
    for (int s = 1; s < 4 && slot < 0; ++s) {
        if (players && ReadOr<std::uintptr_t>(players + re5::off::kSPlayerList + static_cast<std::size_t>(s) * 4, 1) == 0) slot = s;
    }
    if (slot < 0) {
        RM_LOGW("partner: not added (%s): no free player slot", why);
        return 0;
    }
    Partner p;
    p.slot = slot;
    p.saveCopy = engine::SavePlayerArea();
    if (p.saveCopy.empty()) {
        RM_LOGW("partner: not added (%s): the save's player entries are not readable", why);
        return 0;
    }
    const std::size_t s4 = static_cast<std::size_t>(slot) * 4;
    p.gwBits = ReadOr<std::uint32_t>(gw + re5::off::kGameWorkPlayerBits, 0);
    p.gwControl = ReadOr<std::int32_t>(gw + re5::off::kGameWorkPlayerControl + s4, 0);
    p.gwSecond = ReadOr<std::int32_t>(gw + re5::off::kGameWorkPlayerSecond + s4, 0);
    p.gwArg = ReadOr<std::int32_t>(gw + re5::off::kGameWorkPlayerArg + s4, 0);
    const std::uintptr_t unit = engine::AddPlayer(slot, character, costume);
    if (!unit) {
        engine::RestorePlayerArea(p.saveCopy);
        RM_LOGW("partner: not added (%s): the game refused %s in slot %d", why, cls.c_str(), slot);
        return 0;
    }
    // Beside the player (as the stage code places its Josh: the position before his first update).
    float fx = 0.0f, fz = 1.0f;
    PlayerFacing(local, fx, fz);
    const float at[3] = {ReadOr<float>(local + re5::off::kUnitPosition, 0.0f) - fz * 120.0f,
                         ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                         ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f) + fx * 120.0f};
    for (int k = 0; k < 3; ++k) SafeStore<float>(unit + re5::off::kUnitPosition + static_cast<std::size_t>(k) * 4, at[k]);
    SafeStore<std::uint32_t>(unit + re5::off::kPlayerAddFlags, ReadOr<std::uint32_t>(unit + re5::off::kPlayerAddFlags, 0) | 1u);
    p.unit = unit;
    p.generation = g->matchGeneration;
    p.addedAt = Now();
    p.restoreSaveAt = Now() + 1.0;
    p.nextLogAt = Now() + kPartnerPlaceAfter;
    g_partner = std::move(p);
    SetPartnerUnit(unit);
    RM_LOGI("partner: %s (character %d, costume %d) added in slot %d at %.0f %.0f %.0f (%s): control %d, hp %d", cls.c_str(), character,
            costume, slot, at[0], at[1], at[2], why, ReadOr<std::int32_t>(unit + re5::off::kPlayerControl, -1),
            engine::ReadHp(unit) ? engine::ReadHp(unit)->hp : -1);
    return unit;
}

// Each tick: once the game has placed him (at the slot's own start, tens of metres away: adapter.log
// 2026-11-03) he is moved beside the player; the save's entries go back once he has set himself
// up; game work's slot fields when his match is over. His state is logged every few seconds.
void DrivePartner(double now) {
    if (g_partner.slot < 0) return;
    const auto local = engine::LocalPlayer();
    const auto hp = g_partner.unit ? engine::ReadHp(g_partner.unit) : std::nullopt;
    if (local && hp && g->match.inMatch && g->matchGeneration == g_partner.generation) {
        float me[3], him[3];
        for (int k = 0; k < 3; ++k) {
            me[k] = ReadOr<float>(local + re5::off::kUnitPosition + static_cast<std::size_t>(k) * 4, 0.0f);
            him[k] = ReadOr<float>(g_partner.unit + re5::off::kUnitPosition + static_cast<std::size_t>(k) * 4, 0.0f);
        }
        const float away = Horizontal(me, him);
        if (g_partner.placeTries < 3 && now - g_partner.addedAt >= kPartnerPlaceAfter && away > 2.0f * kPartnerBeside) {
            ++g_partner.placeTries;
            float fx = 0.0f, fz = 1.0f;
            PlayerFacing(local, fx, fz);
            const float to[3] = {me[0] - fz * kPartnerBeside, me[1], me[2] + fx * kPartnerBeside};
            const int copies = engine::WarpUnit(g_partner.unit, to);
            RM_LOGI("partner: %.1f m away at %.0f %.0f %.0f; moved beside the player (%d copies)", away / 100.0f, him[0], him[1], him[2],
                    copies);
        }
        if (now >= g_partner.nextLogAt && g_partner.logs < 40) {
            g_partner.nextLogAt = now + kPartnerLogEvery;
            ++g_partner.logs;
            const auto mine = engine::ReadHp(local);
            RM_LOGI("partner: %.1f m from the player (player at %.0f %.0f %.0f, hp %d, %s), %s, hp %d/%d", away / 100.0f, me[0], me[1],
                    me[2], mine ? mine->hp : -1, engine::CurrentActionName(local).c_str(), engine::CurrentActionName(g_partner.unit).c_str(),
                    hp->hp, hp->max);
        }
    }
    if (g_partner.restoreSaveAt >= 0.0 && now >= g_partner.restoreSaveAt) {
        g_partner.restoreSaveAt = -1.0;
        const bool ok = engine::RestorePlayerArea(g_partner.saveCopy);
        RM_LOGI("partner: the save's player entries put back: %s", ok ? "ok" : "FAILED");
    }
    if (!g->match.inMatch || g->matchGeneration != g_partner.generation) {
        if (g_partner.restoreSaveAt >= 0.0) engine::RestorePlayerArea(g_partner.saveCopy);
        RestorePartnerGameWork();
        RM_LOGI("partner: match over; game work's slot %d put back", g_partner.slot);
        g_partner = {};
        SetPartnerUnit(0);
    }
}

// ---- Borrowed boss moves -------------------------------------------------------------------------
// A special plays the matching boss's own moves on the player (user playtest 2026-11-03: "the
// ability doesn't get her twin smg out and do the sweep like in the campaign"; "wesker has in the
// campaign animations, try to use them"): the boss Jill's (uEm84) WideFire on the playable Jill,
// the boss Wesker's (uEm80) dash-strike, kick and knee on his. Every player model shares one
// skeleton and these lists' tracks name its bones (checked offline: em84atk uses exactly the bone
// ids of Jill's own pl02action, em80 all of Wesker's pl03action but two). Their archive loads with
// the run's stages (UpdatePreloads); a move looks its list up (never loads it), binds it to a free
// motion list slot of the player's model through the game's own setter, puts the player in a
// hold (the player's test action: no motion of its own, never done by itself) and plays the
// steps on the body layer; after the last one the player is back to relax. While it holds, the
// action's "done" bit stays clear, so the stick does not end it. Anything missing, a list whose
// body tracks do not reach the model's joints, or a hold the game refuses leaves the move as it
// was before (the dodge, or no animation), with a log line saying why.
struct BossMotionSource {
    const char* character;  // the run's character id
    const char* archive;    // loaded with the run's stages
    const char* list;
};
constexpr BossMotionSource kBossMotions[] = {
    {"jill", "uEm84", "pawn\\em\\em84\\motion\\em84atk"},
    {"wesker", "uEm80", "pawn\\em\\em80\\motion\\em80"},
};
const char* const kHoldClass = "cPlAction_pl_act_test";
// A hit the game gives no reaction (its player hit dispatcher 0x00B7B730: the hit record at
// +0x129C, flag 0x40000000 at its +0x3C, with +0x2D7C bit 8 clear) starts setEventAct_wait (0x84),
// which ends itself at once. It cut Twin Fury's sweep twice in the mod's own runs (2026-11-05, no
// damage logged); a move in progress plays on through it.
const char* const kHitBlipClass = "cPlAction_pl_act_setEventAct_wait";
constexpr float kBossMotionBlend = 10.0f;    // frames, the bosses' own blend
// The body's tracks (bone ids under 64: spine, limbs, head) must reach the model's joints; the
// rest are clothing a costume may not have (Midnight Wesker: 141 of the dash's 200 tracks map,
// user playtest 2026-11-03), which the game skips for his own motions too.
constexpr float kBossMotionMinFit = 0.95f;
constexpr double kBossHoldWait = 0.4;        // seconds for the hold to start
constexpr double kBossHoldRetry = 0.05;      // seconds between its requests while it has not
constexpr int kBossBlipLimit = 3;            // no-reaction hits a boss move plays on through
constexpr double kBossMoveLimit = 8.0;       // seconds: a sequence never holds the player longer

struct BossStep {
    int index;    // into the boss's list
    float speed;  // the motion's play rate
};
// The moves (the bosses' own motion ids, all in their first list: cEm84ActWideFire 0x0A ready,
// 0x0D the sweep). The boss Wesker's, from his actions' starts (0x00917D60.. 0x0091BC30): a shift
// (his high-speed move) begins with 0x1C and moves in 0x1B (cEm80ActAtkSH_F, _NeeSt,
// SH_Escape), stops with 0x20 (AtkSH_Stop); 0x24 is the knee out of a shift (AtkSH_Nee); his
// kicks are 0xC8, 0xC9, 0xCA (AtkKick) and 0xDE (AtkLowKick). 0x72 is cEm80ActHiza, a kneel, and
// 0x77 / 0x75 his stagger (Shrink): the lunge played the kneel (user playtest 2026-11-04: "falls
// on his knees"), and the dash 0x1B after he had already moved, which looked like its start.
// The mod's dash is instant, so it plays the shift's stop as he arrives; the lunge his knee.
const std::vector<BossStep>* BossSteps(const std::string& move) {
    static const std::vector<BossStep> sweep = {{0x0A, 2.0f}, {0x0D, 2.0f}};
    static const std::vector<BossStep> dash = {{0x20, 1.5f}};
    static const std::vector<BossStep> knee = {{0x24, 1.6f}};
    static const std::vector<BossStep> kick = {{0xC9, 1.6f}};
    if (move == "dual_smg") return &sweep;
    if (move == "dash") return &dash;
    if (move == "lunge") return &knee;
    if (move == "burst") return &kick;
    return nullptr;
}
// The step of the sweep during which her shots land (dual_smg).
constexpr std::size_t kSweepFireStep = 1;

struct BossMove {
    std::string move;  // empty: none
    std::vector<BossStep> steps;
    std::size_t step = 0;
    int slot = -1;
    std::uintptr_t list = 0;  // what the slot must still hold (the game may give it another list)
    bool playing = false;  // the hold is on and a step plays
    std::string from;      // the player's action when the hold was asked for
    int requests = 0;      // the hold's requests until it ran
    double requestedAt = 0.0;
    const char* retry = "not asked again yet";  // developer trace: the last retry
    double blipAt = 0.0;   // playing: when a no-reaction hit took the hold away (0: it has not)
    int blips = 0;
    double start = 0.0;
    double stepAt = 0.0;
    float lastFrame = 0.0f;
    float origin[3] = {};
};
BossMove g_boss;

const BossMotionSource* BossSourceFor(const std::string& character) {
    for (const BossMotionSource& s : kBossMotions) {
        if (character == s.character) return &s;
    }
    return nullptr;
}

// The boss archive the run's build plays moves from, if any (for the preloads): the run's, or
// before it starts (its first stage loads first) the build of the skin chosen in the menu.
const char* BossArchiveForRun() {
    rm::RunController& run = *g->run;
    const bool active = run.HasActiveRun();
    const rm::SkinDef* chosen = active ? nullptr : SelectedSkinDef(run);
    const BossMotionSource* source = BossSourceFor(active ? run.state().character_id : SelectedCharacterId(run));
    if (source == nullptr || (!active && chosen == nullptr)) return nullptr;
    const rm::SkinBuild& build = active ? run.state().character.build : chosen->build;
    for (int slot = 0; slot < 2; ++slot) {
        if (BossSteps(build.Special(slot).kind) != nullptr) return source->archive;
    }
    return nullptr;
}

bool BossMoveFiring() { return g_boss.playing && g_boss.step == kSweepFireStep && g_boss.move == "dual_smg"; }
bool BossMoveActive() { return !g_boss.move.empty(); }

// Clears the player's "action done" bit (+0x2D7C bit 28).
void ClearActionDone(std::uintptr_t local) {
    const auto flags = ReadOr<std::uint32_t>(local + re5::off::kPlayerActionFlags, 0);
    if (flags & re5::off::kActionDone) SafeStore<std::uint32_t>(local + re5::off::kPlayerActionFlags, flags & ~re5::off::kActionDone);
}

void EndBossMove(std::uintptr_t local, const char* why) {
    if (g_boss.move.empty()) return;
    std::string back = "left to the game";
    if (local && engine::CurrentActionName(local) == kHoldClass) {
        // The hold never ends by itself: relax, else wait, so the player is never left in it.
        if (engine::StartAction(local, re5::off::kPlActRelax, "cPlAction_pl_act_relax")) back = "back to relax";
        else if (engine::StartAction(local, 0, "cPlAction_pl_act_wait")) back = "relax refused, back to wait";
        else back = "relax and wait refused";
    }
    float drift = 0.0f;
    if (local) {
        const float at[3] = {ReadOr<float>(local + re5::off::kUnitPosition, 0.0f), 0.0f, ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f)};
        drift = Horizontal(g_boss.origin, at);
    }
    RM_LOGI("special: boss move %s over (%s) after %.2f s, step %zu of %zu, %.1f m from where it began; %s", g_boss.move.c_str(),
            why, Now() - g_boss.start, std::min(g_boss.step + 1, g_boss.steps.size()), g_boss.steps.size(), drift / 100.0f,
            back.c_str());
    g_boss = {};
}

// Starts the borrowed version of `move` (true), or says why not (false: the caller's own way).
bool StartBossMove(std::uintptr_t local, const std::string& move, double now) {
    const std::vector<BossStep>* steps = BossSteps(move);
    const BossMotionSource* source = BossSourceFor(g->run->state().character_id);
    if (steps == nullptr || source == nullptr || !local) return false;
    auto refuse = [&](const std::string& why) {
        static std::map<std::string, int> said;
        if (said[move + why]++ < 2) RM_LOGI("special: no boss move for %s (%s)", move.c_str(), why.c_str());
        return false;
    };
    if (!engine::Caps().motions) return refuse("motion signatures missing");
    if (resources::Available() && !resources::ArchiveLoaded(source->archive)) return refuse(std::string(source->archive) + " not loaded");
    const std::uintptr_t list = engine::LoadedMotionList(source->list);
    if (!list) return refuse(std::string(source->list) + " not in memory");
    const int slot = engine::BindMotionList(local, list);
    engine::ReleaseResource(list);  // the slot holds its own reference now
    if (slot < 0) return refuse("no free motion list slot");
    for (const BossStep& s : *steps) {
        const auto fit = engine::CheckMotion(local, slot, s.index);
        if (!fit || fit->bodyTracks == 0) return refuse("motion " + std::to_string(s.index) + " missing");
        const float share = static_cast<float>(fit->bodyMapped) / static_cast<float>(fit->bodyTracks);
        static std::set<std::string> logged;
        if (logged.insert(move + std::to_string(s.index)).second) {
            RM_LOGI("special: boss motion %s %02X: %d of %d body tracks and %d of %d in all reach %s's joints, %.0f frames (slot %d)",
                    source->list, s.index, fit->bodyMapped, fit->bodyTracks, fit->mapped, fit->tracks,
                    engine::ClassName(local).c_str(), fit->frames, slot);
        }
        if (share < kBossMotionMinFit) return refuse("motion " + std::to_string(s.index) + " does not fit the model");
    }
    engine::EnsureHoldStartClearsDone(local);
    const std::string from = engine::CurrentActionName(local);
    const auto requestBefore = ReadOr<std::int32_t>(local + re5::off::kCharActionRequest, 0);
    const auto pendingBefore = ReadOr<std::uint8_t>(local + re5::off::kCharActionPending, 0);
    if (!engine::StartAction(local, re5::off::kPlActHold, kHoldClass)) {
        return refuse("the hold was refused (current action " + from + ")");
    }
    if (g->settings.developerMode) {
        RM_LOGI("special: the hold asked for from %s: request %d (pending %d) -> %d, previous %d, pending %d, now %s", from.c_str(),
                requestBefore, pendingBefore, ReadOr<std::int32_t>(local + re5::off::kCharActionRequest, 0),
                ReadOr<std::int32_t>(local + re5::off::kCharActionPrevRequest, 0),
                ReadOr<std::uint8_t>(local + re5::off::kCharActionPending, 0), engine::CurrentActionName(local).c_str());
    }
    // Standing still, her wait action is done (+0x2D7C bit 28) and the game asks for wait again every
    // frame (the mod's own runs, 2026-11-05: request 0, flags 10001000; the hold never started):
    // the bit is cleared until the hold runs.
    ClearActionDone(local);
    EndBossMove(0, "replaced");
    g_boss.move = move;
    g_boss.steps = *steps;
    g_boss.slot = slot;
    g_boss.list = list;
    g_boss.from = from;
    g_boss.requests = 1;
    g_boss.requestedAt = now;
    g_boss.start = now;
    g_boss.stepAt = now;
    for (int k = 0; k < 3; ++k) g_boss.origin[k] = ReadOr<float>(local + re5::off::kUnitPosition + k * 4, 0.0f);
    return true;
}

// Each tick: the hold starts the first step; a step that has ended starts the next; the player
// is back to relax after the last. A hit, a grab or a death (another action) ends it.
void StepBossMove(double now) {
    if (g_boss.move.empty()) return;
    const auto local = engine::LocalPlayer();
    if (!local) {
        g_boss = {};
        return;
    }
    const std::string action = engine::CurrentActionName(local);
    if (g_boss.playing && action == kHoldClass && g_boss.blipAt > 0.0) {
        RM_LOGI("special: the hold is back after a no-reaction hit, %.2f s", now - g_boss.blipAt);
        g_boss.blipAt = 0.0;
    }
    if (g_boss.playing && action != kHoldClass && (action == kHitBlipClass || (g_boss.blipAt > 0.0 && action == "cPlAction_pl_act_wait"))) {
        // A no-reaction hit (kHitBlipClass), or the wait it drops to: the hold is asked for again
        // for a little while and the step plays on (its motion is checked once the hold is back).
        if (g_boss.blipAt <= 0.0) {
            g_boss.blipAt = now;
            g_boss.requestedAt = 0.0;
            ++g_boss.blips;
            RM_LOGI("special: a no-reaction hit (%s) during %s step %zu; the hold is asked for again", action.c_str(),
                    g_boss.move.c_str(), g_boss.step + 1);
        }
        if (now - g_boss.blipAt <= kBossHoldWait && g_boss.blips <= kBossBlipLimit) {
            if (now - g_boss.requestedAt >= kBossHoldRetry && engine::StartAction(local, re5::off::kPlActHold, kHoldClass)) {
                g_boss.requestedAt = now;
            }
            ClearActionDone(local);
            return;
        }
    }
    if (action != kHoldClass) {
        // Until the hold runs, the bit that makes the game ask for wait again stays clear, and while
        // she is still in the action she was in (or the wait the game dropped her to), the hold is
        // asked for again every 0.05 s. The game took the request, started the hold and asked for
        // wait the same frame (2026-11-05: the hold in +0x10E4, nothing pending); the hold's start
        // now clears the done bit itself (engine::EnsureHoldStartClearsDone). Another action (a
        // hit) is never cut.
        if (!g_boss.playing) {
            ClearActionDone(local);
            g_boss.retry = "not asked this tick";
            if (now - g_boss.requestedAt >= kBossHoldRetry) {
                g_boss.requestedAt = now;
                if (action != g_boss.from && action != "cPlAction_pl_act_wait") {
                    g_boss.retry = "not asked again (another action)";
                } else if (engine::StartAction(local, re5::off::kPlActHold, kHoldClass)) {
                    ++g_boss.requests;
                    g_boss.retry = "asked again";
                    ClearActionDone(local);
                } else {
                    g_boss.retry = "asked again, refused";
                }
            }
        }
        if (!g_boss.playing && g->settings.developerMode) {
            // Developer: what stands in the hold's way (it never started in the mod's own runs,
            // 2026-11-05, while it did in the user's).
            // Every tick: a request the step took moves to the previous one and clears pending; one
            // written over before the step leaves both.
            RM_LOGI("special: waiting for the hold: action %s (asked from %s; %s), request %d, previous %d, pending %d, flags %08X, %.3f s",
                    action.c_str(), g_boss.from.c_str(), g_boss.retry, ReadOr<std::int32_t>(local + re5::off::kCharActionRequest, 0),
                    ReadOr<std::int32_t>(local + re5::off::kCharActionPrevRequest, 0),
                    ReadOr<std::uint8_t>(local + re5::off::kCharActionPending, 0),
                    ReadOr<std::uint32_t>(local + re5::off::kPlayerActionFlags, 0), now - g_boss.start);
        }
        if (g_boss.playing || now - g_boss.start > kBossHoldWait) {
            EndBossMove(local, g_boss.playing ? ("interrupted: " + action).c_str()
                                              : ("the hold never started: " + action + ", " + std::to_string(g_boss.requests) + " requests").c_str());
        }
        return;
    }
    if (now - g_boss.start > kBossMoveLimit) {
        EndBossMove(local, "time limit");
        return;
    }
    // The hold is never done by itself; a "done" bit left from the action before would let the
    // stick end it.
    ClearActionDone(local);
    const BossStep& s = g_boss.steps[g_boss.step];
    const unsigned id = static_cast<unsigned>(g_boss.slot) << 8 | static_cast<unsigned>(s.index);
    if (ReadOr<std::uintptr_t>(local + re5::off::kMotionLists + static_cast<std::size_t>(g_boss.slot) * 4, 0) != g_boss.list) {
        EndBossMove(local, "its motion list slot was given another list");
        return;
    }
    if (!g_boss.playing) {
        if (!engine::PlayMotion(local, g_boss.slot, s.index, kBossMotionBlend, s.speed)) {
            EndBossMove(local, "the motion was refused");
            return;
        }
        g_boss.playing = true;
        g_boss.stepAt = now;
        g_boss.lastFrame = 0.0f;
        if (g_boss.requests > 1) {
            RM_LOGI("special: the hold ran after %d requests, %.2f s", g_boss.requests, now - g_boss.start);
        }
        return;
    }
    const auto body = engine::ReadBodyMotion(local);
    if (!body || body->id != static_cast<int>(id)) {
        EndBossMove(local, "another motion took over");
        return;
    }
    // Played once it holds its last frame; a frame going back means it started over: also done.
    const bool wrapped = body->frame + 1.0f < g_boss.lastFrame;
    g_boss.lastFrame = body->frame;
    if (!wrapped && body->frames > 0.0f && body->frame < body->frames - 1.0f) return;
    static int timed = 0;
    if (timed++ < 12) {
        RM_LOGI("special: boss motion %02X done: %.0f frames in %.2f s at speed %.1f", s.index, body->frames, now - g_boss.stepAt, s.speed);
    }
    if (++g_boss.step >= g_boss.steps.size()) {
        EndBossMove(local, "done");
        return;
    }
    const BossStep& next = g_boss.steps[g_boss.step];
    if (!engine::PlayMotion(local, g_boss.slot, next.index, kBossMotionBlend, next.speed)) {
        EndBossMove(local, "the next motion was refused");
        return;
    }
    g_boss.stepAt = now;
    g_boss.lastFrame = 0.0f;
}

void DeniedNotice(double now, const std::string& what, const rm::SpecialState& state) {
    if (now < g_specialDeniedUntil) return;
    g->notice = what + (state.arenas_left > 0 ? "   NOT THIS ARENA" : "   NOT READY");
    g->noticeUntil = now + 1.5;
    g_specialDeniedUntil = now + 1.0;
}

void StartDashMove(double now, const rm::SkinSpecial& sp, std::uintptr_t local, const char* source) {
    rm::RunController& run = *g->run;
    const float dashDistance = static_cast<float>(sp.power > 0.0 ? sp.power : 5.0) * 100.0f;
    const DashPlan plan = PlanDash(local, dashDistance);
    const rm::Status used = run.UseSpecial(now, plan.move == "lunge" ? kLungeCooldown : 1.0, 0);
    if (!used.ok()) {
        DeniedNotice(now, Upper(plan.move), run.Special(now, 0));
        return;
    }
    EndSpecialMove("replaced");
    g_move.kind = sp.kind;
    g_move.move = plan.move;
    g_move.start = now;
    for (int k = 0; k < 3; ++k) {
        g_move.origin[k] = ReadOr<float>(local + re5::off::kUnitPosition + k * 4, 0.0f);
        g_move.end[k] = plan.end[k];
    }
    float toward[3] = {plan.end[0], plan.end[1], plan.end[2]};
    if (plan.move == "lunge") {
        g_move.until = now + kLungeSeconds;
        g_move.victim = plan.target;
        g_move.victimStaggered = plan.staggered;
        for (int k = 0; k < 3; ++k) toward[k] = ReadOr<float>(plan.target + re5::off::kUnitPosition + k * 4, toward[k]);
        RM_LOGI("special: lunge %.1f m (%s) [%s]", plan.distance / 100.0f, plan.why.c_str(), source);
        // He faces it before his forward dodge, so he dashes in the way he looks.
        FacePoint(local, toward);
    } else {
        g_move.until = now + kDashSeconds;
        // Everyone on the line he runs through is hit as he passes.
        std::string results;
        const float len = Horizontal(g_move.origin, g_move.end);
        if (len > 1.0f) {
            const float ux = (g_move.end[0] - g_move.origin[0]) / len, uz = (g_move.end[2] - g_move.origin[2]) / len;
            for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
                if (e.hp <= 0) continue;
                const float ex = ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f) - g_move.origin[0];
                const float ey = ReadOr<float>(e.object + re5::off::kUnitPosition + 4, 0.0f) - g_move.origin[1];
                const float ez = ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f) - g_move.origin[2];
                const float along = ex * ux + ez * uz;
                const float off = std::fabs(ex * uz - ez * ux);
                if (along < 0.0f || along > len + kDashHitRadius || off > kDashHitRadius || std::fabs(ey) > kMaxStep) continue;
                results += " " + StrikeEnemy(e.object, kDashDamage, re5::off::kEm10DmgYoroke, now);
            }
        } else {
            float fx = 0.0f, fz = 1.0f;
            PlayerFacing(local, fx, fz);
            toward[0] = g_move.origin[0] + fx * 300.0f;
            toward[2] = g_move.origin[2] + fz * 300.0f;
        }
        RM_LOGI("special: dash %.1f of %.1f m (%s)%s%s [%s]", plan.distance / 100.0f, dashDistance / 100.0f, plan.why.c_str(),
                results.empty() ? "" : ", hits:", results.c_str(), source);
    }
    // The boss Wesker's dash-strike; without it, his own forward dodge.
    g_move.borrowed = StartBossMove(local, plan.move, now);
    if (!g_move.borrowed) StartDodgeAnimation(local, toward);
    g->notice = Upper(plan.move);
    g->noticeUntil = now + 1.2;
}

void StartBurst(double now, const rm::SkinSpecial& sp, std::uintptr_t local, int slot, const char* source) {
    rm::RunController& run = *g->run;
    const rm::Status used = run.UseSpecial(now, 1.0, slot);
    if (!used.ok()) {
        DeniedNotice(now, Upper(Plain(sp.name)), run.Special(now, slot));
        return;
    }
    g_burstUntil = now + kBurstSeconds;
    const int damage = static_cast<int>(sp.power > 0.0 ? sp.power : kBurstDamage);
    const float py = ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f);
    std::string results;
    for (const auto& [enemy, d] : EnemiesAround(local, kBurstRadius, -1.0f, 0.0f, 1.0f)) {
        (void)d;
        if (std::fabs(ReadOr<float>(enemy + re5::off::kUnitPosition + 4, 0.0f) - py) > kMaxStep) continue;
        results += " " + StrikeEnemy(enemy, damage, re5::off::kEm10DmgFlyoff, now);
    }
    RM_LOGI("special: burst:%s [%s]", results.empty() ? " nobody in reach" : results.c_str(), source);
    // The boss Wesker's kick plays it (none before: user playtest 2026-11-03).
    StartBossMove(local, "burst", now);
    g->notice = Upper(Plain(sp.name));
    g->noticeUntil = now + 1.2;
}

void StartSpecial(double now, int slot, const char* source) {
    rm::RunController& run = *g->run;
    const rm::SpecialState state = run.Special(now, slot);
    if (state.kind.empty()) return;
    const auto local = engine::LocalPlayer();
    if (!local) return;
    const rm::SkinSpecial& sp = run.state().character.build.Special(slot);
    if (sp.kind == "dash") {
        StartDashMove(now, sp, local, source);
        return;
    }
    if (sp.kind == "burst") {
        StartBurst(now, sp, local, slot, source);
        return;
    }
    const rm::Status used = run.UseSpecial(now, 1.0, slot);
    if (!used.ok()) {
        DeniedNotice(now, Upper(Plain(state.name)), state);
        return;
    }
    EndSpecialMove("replaced");
    g_move.kind = sp.kind;
    g_move.start = now;
    for (int k = 0; k < 3; ++k) g_move.origin[k] = ReadOr<float>(local + re5::off::kUnitPosition + k * 4, 0.0f);
    PlayerFacing(local, g_move.dirX, g_move.dirZ);
    if (sp.kind == "dual_smg") {
        g_move.damage = static_cast<int>(sp.power > 0.0 ? sp.power : 90.0);
        g_move.seconds = sp.duration_seconds > 0.0 ? sp.duration_seconds : 1.5;
        g_move.until = now + g_move.seconds;
        g_move.nextPulse = now;
        // The boss Jill's WideFire: she readies, then sweeps; the shots land while she sweeps, for
        // as long as it plays (the move ends with it).
        g_move.borrowed = StartBossMove(local, "dual_smg", now);
        if (g_move.borrowed) g_move.until = now + kBossMoveLimit;
        // Her two SMGs in her hands for it (they fire while the shots land), once the hold runs.
        g_move.twinWanted = g_twinGunsInSpecial;
    } else if (sp.kind == "backup") {
        // Sheva comes as a computer partner for the arena: the game's own partner, who follows,
        // shoots, reloads and heals herself (E22, seen in game 2026-11-03 with a Chris partner).
        // Without her (not loaded, no free slot): covering fire on the nearest enemy instead.
        if (AddPartner(kBackupAlly.character, kBackupAlly.costume, "backup")) {
            g_move = {};
            g->notice = "SHEVA JOINS YOU";
            g->noticeUntil = now + 2.5;
            RM_LOGI("special: backup used: %s joins for the arena [%s]", kBackupAlly.cls, source);
            return;
        }
        g_move.damage = static_cast<int>(sp.power > 0.0 ? sp.power : kBackupDamage);
        g_move.until = now + 1.0e9;
        g_move.nextPulse = now + 0.5;
    }
    g->notice = Upper(Plain(sp.name));
    g->noticeUntil = now + 2.0;
    RM_LOGI("special: %s used at %.0f %.0f %.0f facing %.2f %.2f [%s]", sp.kind.c_str(), g_move.origin[0], g_move.origin[1],
            g_move.origin[2], g_move.dirX, g_move.dirZ, source);
}

void StepSpecial(double now) {
    if (g_move.kind.empty()) return;
    const auto local = engine::LocalPlayer();
    if (!local) {
        EndSpecialMove("no player");
        return;
    }
    if (g_move.kind == "dash") {
        const double seconds = g_move.move == "lunge" ? kLungeSeconds : kDashSeconds;
        const float t = static_cast<float>(std::min(1.0, (now - g_move.start) / seconds));
        if (Horizontal(g_move.origin, g_move.end) > 1.0f) {
            float to[3];
            for (int k = 0; k < 3; ++k) to[k] = g_move.origin[k] + (g_move.end[k] - g_move.origin[k]) * t;
            engine::WarpUnit(local, to);
        }
        if (t < 1.0f) return;
        if (g_move.move == "lunge" && g_move.victim) {
            float at[3];
            for (int k = 0; k < 3; ++k) at[k] = ReadOr<float>(g_move.victim + re5::off::kUnitPosition + k * 4, 0.0f);
            const bool facing = FacePoint(local, at);
            const auto hp = engine::ReadHp(g_move.victim);
            const bool reeling = hp && hp->hp > 0 && Staggered(g_move.victim);
            if (facing && reeling) {
                // In front of it, facing it, while it reels: the game's own melee prompt is up and his
                // own melee move finishes it (his ranged melee: the lunge, then the melee button). The
                // boss move's hold would keep the prompt from him: he is let go.
                EndBossMove(local, "arrived: the game's melee");
                RM_LOGI("special: lunge reached %s (%s), facing it: the game's melee finishes it",
                        engine::ClassName(g_move.victim).c_str(), engine::CurrentActionName(g_move.victim).c_str());
                g->notice = "MELEE";
                g->noticeUntil = now + 1.5;
            } else {
                // Not facing it (the turn did not hold): a staggered one takes a heavy blow instead (a
                // Majini blown away). Else it is left reeling from a head hit, the state the game's
                // melee prompt follows.
                const bool heavy = g_move.victimStaggered && !facing;
                const std::string result = StrikeEnemy(g_move.victim, heavy ? kLungeDamageStaggered : kLungeDamage,
                                                       heavy ? re5::off::kEm10DmgFlyoff : re5::off::kEm10DmgHead, now);
                RM_LOGI("special: lunge strikes %s%s (%s): %s", engine::ClassName(g_move.victim).c_str(),
                        g_move.victimStaggered ? " (was staggered)" : "", facing ? "facing it" : "not facing it", result.c_str());
                // The blow is the boss Wesker's knee, playing since the lunge began.
                if (!heavy && result != "killed" && result != "missed") {
                    g->notice = "MELEE";
                    g->noticeUntil = now + 1.5;
                }
            }
        }
        EndSpecialMove("done");
        return;
    }
    // Her two SMGs: made while the hold was asked for, they could stand in its way.
    if (g_move.twinWanted && !g_move.twinGuns && (!g_move.borrowed || g_boss.playing)) {
        g_move.twinWanted = false;
        g_move.twinGuns = ArmTwinGuns(local, "Twin Fury");
    }
    // Her borrowed sweep: no shots while she readies, and the move ends with the sweep. A sweep
    // cut short before it fired (a hit, the hold refused) still fires, without the animation.
    if (g_move.kind == "dual_smg" && g_move.borrowed) {
        if (!BossMoveActive()) {
            if (g_move.pulses > 0) {
                EndSpecialMove("the sweep is over");
                return;
            }
            g_move.borrowed = false;
            g_move.until = now + g_move.seconds;
            g_move.nextPulse = now;
            RM_LOGI("special: dual_smg fires without the boss move (it ended before the sweep)");
        } else if (!BossMoveFiring()) {
            return;
        }
    }
    if (now >= g_move.until) {
        EndSpecialMove("done");
        return;
    }
    // Her two SMGs fire as fast as the boss's (every few frames) while the shots land.
    if (g_move.kind == "dual_smg" && g_move.twinGuns && now >= g_move.nextTwinShot) {
        g_move.nextTwinShot = now + kTwinShotEvery;
        float tx = g_move.dirX, tz = g_move.dirZ;
        PlayerFacing(local, tx, tz);
        g_move.twinShots += FireTwinGunsOnce(local, tx, tz);
    }
    if (now < g_move.nextPulse) return;
    if (g_move.kind == "dual_smg") {
        g_move.nextPulse = now + kSmgPulse;
        ++g_move.pulses;
        float fx = g_move.dirX, fz = g_move.dirZ;
        PlayerFacing(local, fx, fz);  // she turns while she fires
        // Health alone fell as if a ray hit them, and they dropped once their own action ended
        // (user playtest 2026-11-04). A Majini hit now staggers the first time, and the killing
        // shot blows it away before the damage lands (a dead one takes no action), so it falls
        // there and then.
        for (const auto& [enemy, d] : EnemiesAround(local, kSmgRange, kSmgCone, fx, fz)) {
            (void)d;
            const auto hp = engine::ReadHp(enemy);
            if (!hp || hp->hp <= 0) continue;
            const bool kills = g_move.damage >= hp->hp;
            const bool reacted = g_move.staggered.count(enemy) != 0;
            if (kills) engine::StartAction(enemy, re5::off::kEm10DmgFlyoff, ReactionClass(re5::off::kEm10DmgFlyoff));
            if (!director::Damage(enemy, g_move.damage)) continue;
            ++g_move.hits;
            g_specialVictims[enemy] = now;
            if (!kills && !reacted && engine::StartAction(enemy, re5::off::kEm10DmgYoroke, ReactionClass(re5::off::kEm10DmgYoroke))) {
                g_move.staggered.insert(enemy);
            }
        }
    } else if (g_move.kind == "backup") {
        g_move.nextPulse = now + kBackupPulse;
        ++g_move.pulses;
        auto targets = EnemiesAround(local, kBackupRange, -1.0f, 0.0f, 1.0f);
        if (!targets.empty()) {
            const auto nearest = std::min_element(targets.begin(), targets.end(),
                                                  [](const auto& a, const auto& b) { return a.second < b.second; });
            if (director::Damage(nearest->first, g_move.damage)) {
                ++g_move.hits;
                g_specialVictims[nearest->first] = now;
            }
        }
    }
}

// Chris's Iron Grip (a build's break_grabs): a grab is broken at once. In a grab the player's
// action is the grabber's hold of him (cEm10ActNeckHang_PL, cEm10ActBackHold_PL, ...: the
// grabber at the action's +0xC, the player at +8, 0x00C29370); the hold lets him go itself when
// the grabber leaves its grab (a Majini's action 0x99, the hold's move 0x00C26FB0), so the
// grabber is thrown off with the game's own reaction. Only Majini (uEm10's family) have the
// reactions known; any other grabber is logged and left to the game.
constexpr std::size_t kHoldGrabber = 0x0C;
std::uintptr_t g_grabBroken = 0;  // the hold action handled last (one try per grab)

void DriveGrabBreak(double now) {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || run.phase() != rm::Phase::ArenaActive || (!run.state().character.build.break_grabs && !g->devBreakGrabs)) {
        return;
    }
    const auto local = engine::LocalPlayer();
    if (!local) return;
    const auto action = ReadOr<std::uintptr_t>(local + re5::off::kPlayerAction, 0);
    const std::string name = action ? engine::ClassName(action) : std::string();
    const bool hold = name.size() > 3 && name.compare(name.size() - 3, 3, "_PL") == 0 && name.find("Escape") == std::string::npos;
    if (!hold) {
        g_grabBroken = 0;
        return;
    }
    if (action == g_grabBroken) return;
    g_grabBroken = action;
    const auto grabber = ReadOr<std::uintptr_t>(action + kHoldGrabber, 0);
    const std::string cls = grabber ? engine::ClassName(grabber) : std::string();
    const auto hp = grabber ? engine::ReadHp(grabber) : std::nullopt;
    if (cls.rfind("uEm", 0) != 0 || !hp || hp->hp <= 0) {
        RM_LOGI("iron grip: %s, no grabber found (%s); left to the game", name.c_str(), cls.empty() ? "none" : cls.c_str());
        return;
    }
    // A Uroboros' grab: let go through its own escape once it holds him (kLethalHolds); while it
    // still reaches for him, asked again on the next frame.
    if (cls == "uEm40" || cls == "uEm41") {
        const BossHold* h = LethalHold(engine::CurrentActionName(grabber));
        if (h == nullptr) {
            g_grabBroken = 0;
            return;
        }
        const bool let = ReleaseHold(grabber, *h);
        RM_LOGI("iron grip: %s by %s %s", name.c_str(), cls.c_str(), let ? "let go (its escape)" : "kept its hold (no escape found)");
        if (let) {
            g->notice = "IRON GRIP";
            g->noticeUntil = now + 1.2;
        }
        return;
    }
    const bool thrown = engine::StartAction(grabber, re5::off::kEm10DmgFlyoff, ReactionClass(re5::off::kEm10DmgFlyoff)) ||
                        engine::StartAction(grabber, re5::off::kEm10DmgYoroke, ReactionClass(re5::off::kEm10DmgYoroke));
    RM_LOGI("iron grip: %s by %s %s", name.c_str(), cls.c_str(),
            thrown ? ("thrown off (" + engine::CurrentActionName(grabber) + ")").c_str() : "kept its grab (no known reaction)");
    if (thrown) {
        g->notice = "IRON GRIP";
        g->noticeUntil = now + 1.2;
    }
}

// Game tick: each special's combo starts it during an arena; the move runs its course. While a
// build with specials fights, their bindings are the mod's (hidden from the game).
void DriveSpecial(double now) {
    rm::RunController& run = *g->run;
    const bool arena = run.HasActiveRun() && run.phase() == rm::Phase::ArenaActive && g->match.inMatch;
    const rm::SkinBuild& build = run.state().character.build;
    std::vector<OwnedInput> owned;
    if (arena) {
        for (int slot = 0; slot < 2; ++slot) {
            if (build.Special(slot).Any()) owned.push_back(OwnedFor(slot));
        }
    }
    if (run.ActiveSeat() == 0) {
        SetOwnedInputs(owned);
        if (run.SeatCount() < 2) {
            for (int user = 0; user < 4; ++user) SetOwnedPadInputs(user, {});
        }
    } else {
        std::vector<unsigned short> combos;
        for (const OwnedInput& o : owned) {
            if (o.xinput) combos.push_back(o.xinput);
        }
        SetOwnedPadInputs(PadUserOfSeat(run.ActiveSeat()), combos);
    }
    WatchDodge(now);
    StepBossMove(now);
    if (!arena) {
        EndBossMove(engine::LocalPlayer(), "arena over");
        EndSpecialMove("arena over");
        g_specialVictims.clear();
        for (int slot = 0; slot < 2; ++slot) g_comboHeld[slot] = SpecialComboDown(slot);
        return;
    }
    const auto me = engine::LocalPlayer();
    const std::string blocked = SpecialBlocked(me);
    for (int slot = 0; slot < 2; ++slot) {
        if (!build.Special(slot).Any()) continue;
        const char* source = "?";
        const bool down = SpecialComboDown(slot, &source);
        if (down && !g_comboHeld[slot]) {
            if (blocked.empty()) {
                StartSpecial(now, slot, source);
            } else {
                static double loggedAt = -10.0;
                if (now - loggedAt > 2.0) {
                    loggedAt = now;
                    RM_LOGI("special: slot %d pressed while %s; not used", slot, blocked.c_str());
                }
            }
        }
        g_comboHeld[slot] = down;
    }
    // A move in progress stops when he is grabbed, hit or down (not while it plays its own dodge,
    // a boss move's hold, or a hit the game gives no reaction).
    if (!g_move.kind.empty() && !blocked.empty() && blocked.rfind("cPlAction_pl_act_avoid", 0) != 0 && blocked != kHoldClass &&
        blocked != kHitBlipClass) {
        EndSpecialMove(("interrupted: " + blocked).c_str());
    }
    StepSpecial(now);
    // A dash special's ring shows the move the button would make (checked 10 times a second).
    const rm::SkinSpecial& sp = build.special;
    if (sp.kind == "dash" && g_move.kind.empty() && now - g_armedAt >= 0.1) {
        g_armedAt = now;
        if (const auto local = engine::LocalPlayer()) {
            g_armedMove = PlanDash(local, static_cast<float>(sp.power > 0.0 ? sp.power : 5.0) * 100.0f).move;
        }
    }
}

// A special's HUD ring (bottom left, the 1280x720 canvas mapped like the menu icons; the second
// special to the right of the first): its icon in a ring, a dark sweep over it while it recovers
// with the seconds left, gold when ready, green while it acts; its name and binding under it.
void DrawSpecialWidget(int slot) {
    rm::RunController& run = *g->run;
    const double now = Now();
    const rm::SpecialState st = run.Special(now, slot);
    if (st.kind.empty()) return;
    IDirect3DDevice9* device = overlay::FrameDevice();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    const ImVec2 c(ox + (96.0f + 104.0f * static_cast<float>(slot)) * k, oy + (596.0f + g_drawShiftY) * k);
    const float r = 38.0f * k;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const bool acting = slot == 1 ? now < g_burstUntil : !g_move.kind.empty() && g_move.kind != "dash";
    const bool ready = st.ready;
    // Wesker's dash shows the move the button would make now.
    std::string icon = st.icon, name = Upper(Plain(st.name));
    if (st.kind == "dash" && !g_armedMove.empty()) {
        icon = g_armedMove == "lunge" ? "lunge" : "dash";
        name = Upper(g_armedMove);
    }
    list->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 150), 48);
    if (device) {
        if (const auto texture = icons::Texture(device, icon)) {
            const float half = 26.0f * k;
            const int tint = ready || acting ? 255 : 120;
            list->AddImage(static_cast<ImTextureID>(texture), ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half),
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(tint, tint, tint, 255));
        }
    }
    if (st.cooldown_left > 0.0 && st.cooldown_total > 0.0) {
        const float fraction = static_cast<float>(std::min(1.0, st.cooldown_left / st.cooldown_total));
        const float a0 = -IM_PI * 0.5f;
        list->PathLineTo(c);
        list->PathArcTo(c, r - 2.0f * k, a0, a0 + fraction * 2.0f * IM_PI, 48);
        list->PathFillConvex(IM_COL32(0, 0, 0, 170));
    }
    const float pulse = 0.6f + 0.4f * static_cast<float>(std::sin(now * 4.0));
    const ImU32 ring = acting  ? IM_COL32(120, 230, 120, 255)
                       : ready ? IM_COL32(255, 210, 127, static_cast<int>(255 * pulse))
                               : IM_COL32(150, 150, 150, 200);
    list->AddCircle(c, r, ring, 48, (ready || acting ? 3.0f : 2.0f) * k);
    ImFont* font = ImGui::GetFont();
    auto centred = [&](const std::string& text, float size, float y, ImU32 color) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 at(c.x - sz.x * 0.5f, y);
        list->AddText(font, size, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), text.c_str());
        list->AddText(font, size, at, color, text.c_str());
    };
    std::string middle;
    if (acting) middle = "ACTIVE";
    else if (st.arenas_left > 0) middle = st.arenas_left == 1 ? "NEXT ARENA" : std::to_string(st.arenas_left) + " ARENAS";
    else if (st.cooldown_left > 0.0) middle = std::to_string(static_cast<int>(std::ceil(st.cooldown_left)));
    if (!middle.empty()) centred(middle, (middle.size() > 3 ? 13.0f : 26.0f) * k, c.y - (middle.size() > 3 ? 7.0f : 14.0f) * k, IM_COL32(255, 255, 255, 255));
    centred(name, 15.0f * k, c.y + r + 4.0f * k, ready ? IM_COL32(255, 210, 127, 255) : IM_COL32(220, 220, 220, 230));
    centred(SpecialBindingText(slot), 12.0f * k, c.y + r + 20.0f * k, IM_COL32(200, 200, 200, 200));
}

// The equipped gun's card (M8, user playtest 2026-11-03: no way to tell a weapon's stats): its
// name, rarity, level and what they add, for a few seconds after a switch (bottom right, above
// the game's own weapon panel).
std::string g_cardWeapon;
double g_cardUntil = -1.0;

void WatchEquippedWeapon(double now) {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || run.phase() != rm::Phase::ArenaActive || !g->match.inMatch) {
        g_cardWeapon.clear();
        return;
    }
    const auto id = EquippedWeaponBaseId();
    const std::string base = id ? *id : std::string();
    if (base != g_cardWeapon) {
        g_cardWeapon = base;
        g_cardUntil = base.empty() ? -1.0 : now + 4.0;
    }
}

void DrawWeaponCard() {
    rm::RunController& run = *g->run;
    const double now = Now();
    if (g_cardWeapon.empty() || now > g_cardUntil || !run.HasActiveRun() || !g->match.inMatch || matchui::IsOpen()) return;
    const rm::RunSnapshot s = run.Snapshot(now);
    const rm::WeaponView* w = nullptr;
    for (const rm::WeaponView& v : s.weapons) {
        if (v.base_id == g_cardWeapon) w = &v;
    }
    if (w == nullptr) return;
    std::vector<std::pair<std::string, ImU32>> lines;
    const std::uint32_t rarity = RarityColour(w->rarity);
    const ImU32 head = rarity ? IM_COL32((rarity >> 16) & 0xFF, (rarity >> 8) & 0xFF, rarity & 0xFF, 255) : IM_COL32(240, 240, 240, 255);
    lines.emplace_back(Upper(Plain(w->name)) + "   LV " + std::to_string(w->level), head);
    char text[96];
    std::snprintf(text, sizeof(text), "%s   DAMAGE x%.2f, LEVEL +%d PERCENT", Upper(w->rarity).c_str(), w->base_mult,
                  static_cast<int>(std::lround(w->level_bonus * 100.0)));
    lines.emplace_back(text, IM_COL32(220, 220, 220, 235));
    for (std::size_t i = 0; i < w->affixes.size() && i < 8; ++i) lines.emplace_back(Upper(Plain(w->affixes[i])), IM_COL32(200, 230, 200, 235));
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 15.0f * k, gap = 18.0f * k;
    float width = 0.0f;
    for (const auto& [line, color] : lines) width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str()).x);
    const float right = ox + 1250.0f * k, bottom = oy + (548.0f + g_drawShiftY) * k;
    const float top = bottom - gap * static_cast<float>(lines.size()) - 12.0f * k;
    const float fade = static_cast<float>(std::min(1.0, (g_cardUntil - now) / 0.5));
    list->AddRectFilled(ImVec2(right - width - 20.0f * k, top), ImVec2(right, bottom), IM_COL32(0, 0, 0, static_cast<int>(150 * fade)), 6.0f * k);
    float y = top + 6.0f * k;
    for (const auto& [line, color] : lines) {
        const float w2 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str()).x;
        const ImVec2 at(right - 10.0f * k - w2, y);
        const int alpha = static_cast<int>(((color >> IM_COL32_A_SHIFT) & 0xFF) * fade);
        list->AddText(font, size, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, alpha), line.c_str());
        list->AddText(font, size, at, (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT), line.c_str());
        y += gap;
    }
}

// What enemies do just after the player hits them (the stagger states the lunge looks for): one
// line per class and state in a session.
struct HitSample {
    std::uintptr_t enemy = 0;
    double at = 0.0;
};
std::vector<HitSample> g_hitSamples;
std::set<std::string> g_hitStatesSeen;

void SampleHitStates(double now) {
    for (auto it = g_hitSamples.begin(); it != g_hitSamples.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const std::string state = engine::CurrentActionName(it->enemy);
        const std::string key = engine::ClassName(it->enemy) + " " + state;
        if (!state.empty() && g_hitStatesSeen.size() < 80 && g_hitStatesSeen.insert(key).second) {
            RM_LOGI("enemy states: %s after a hit%s", key.c_str(), Staggered(it->enemy) ? " (a lunge's stagger)" : "");
        }
        it = g_hitSamples.erase(it);
    }
}

// Excella's speed (a run stat, M8): while she walks or runs she covers that much more ground,
// each step checked against walls and the floor like Wesker's moves.
struct SpeedTrack {
    std::uintptr_t player = 0;
    float last[3] = {};
    bool have = false;
};
SpeedTrack g_speed;
constexpr float kSpeedStepMax = 60.0f;   // a longer step between ticks is not walking (a warp)
constexpr float kSpeedExtraMax = 25.0f;  // the most added in one tick
constexpr float kSpeedFloor = 50.0f;     // the floor ahead within half a metre of her own

void ApplyMoveSpeed() {
    rm::RunController& run = *g->run;
    const double bonus = run.HasActiveRun() ? run.state().stats.move_speed : 0.0;
    const auto local = engine::LocalPlayer();
    if (bonus <= 0.0 || !local || run.phase() != rm::Phase::ArenaActive || !g->match.inMatch || !g_move.kind.empty()) {
        g_speed.have = false;
        return;
    }
    float pos[3];
    for (int k = 0; k < 3; ++k) pos[k] = ReadOr<float>(local + re5::off::kUnitPosition + k * 4, 0.0f);
    auto keep = [&](const float at[3]) {
        g_speed.player = local;
        for (int k = 0; k < 3; ++k) g_speed.last[k] = at[k];
        g_speed.have = true;
    };
    if (!g_speed.have || g_speed.player != local) {
        keep(pos);
        return;
    }
    const float dx = pos[0] - g_speed.last[0], dz = pos[2] - g_speed.last[2];
    const float step = std::sqrt(dx * dx + dz * dz);
    const std::string action = engine::CurrentActionName(local);
    const bool moving = action.rfind("cPlAction_pl_act_walk", 0) == 0 || action.rfind("cPlAction_pl_act_run", 0) == 0 ||
                        action.rfind("cPlAction_pl_act_back", 0) == 0;
    if (!moving || step < 1.0f || step > kSpeedStepMax || GamePaused() || ItemScreenOpen()) {
        keep(pos);
        return;
    }
    const float extra = std::min(step * static_cast<float>(bonus), kSpeedExtraMax);
    const float ux = dx / step, uz = dz / step;
    const float a[3] = {pos[0], pos[1] + kChestHeight, pos[2]};
    const float b[3] = {pos[0] + ux * (extra + kWallMargin), pos[1] + kChestHeight, pos[2] + uz * (extra + kWallMargin)};
    const auto wall = engine::CastSegment(a, b);
    float floorY = 0.0f;
    if (!wall || wall->Blocked() || !FloorAt(pos[0] + ux * extra, pos[1], pos[2] + uz * extra, floorY) ||
        std::fabs(floorY - pos[1]) > kSpeedFloor) {
        keep(pos);
        return;
    }
    const float to[3] = {pos[0] + ux * extra, floorY, pos[2] + uz * extra};
    engine::WarpUnit(local, to);
    keep(to);
}

// ---- M10: each player's specials, moves, weapon card and flamethrower --------------------------
// The active seat's are the live globals; the others wait here (SwapLateSeat, from SeatTurn).
struct LateSeat {
    SpecialMove move;
    bool comboHeld[2] = {false, false};
    double specialDeniedUntil = 0.0;
    double burstUntil = -1.0;
    std::string armedMove;
    double armedAt = -1.0;
    std::uintptr_t twinGuns[2] = {0, 0};
    int twinGunsGeneration = -1;
    double dodgeAt = -1.0;
    bool dodgeSeen = false;
    BossMove boss;
    std::uintptr_t grabBroken = 0;
    std::string cardWeapon;
    double cardUntil = -1.0;
    SpeedTrack speed;
    std::uintptr_t flameUnit = 0;
    FlameHideState flameHide;
    RefuelState refuel;
    std::string flameStreamPath, flameStreamCompanion;
    bool flameStreamLoops = false;
    rm::Element flameStreamElement = rm::Element::Incendiary;
    FlameStreamState flameStream;
};
std::array<LateSeat, 2> g_lateSeats;

void SwapLateSeat(int index) {
    if (index < 0 || index >= static_cast<int>(g_lateSeats.size())) return;
    LateSeat& s = g_lateSeats[static_cast<std::size_t>(index)];
    using std::swap;
    swap(g_move, s.move);
    swap(g_comboHeld, s.comboHeld);
    swap(g_specialDeniedUntil, s.specialDeniedUntil);
    swap(g_burstUntil, s.burstUntil);
    swap(g_armedMove, s.armedMove);
    swap(g_armedAt, s.armedAt);
    swap(g_twinGuns, s.twinGuns);
    swap(g_twinGunsGeneration, s.twinGunsGeneration);
    swap(g_dodgeAt, s.dodgeAt);
    swap(g_dodgeSeen, s.dodgeSeen);
    swap(g_boss, s.boss);
    swap(g_grabBroken, s.grabBroken);
    swap(g_cardWeapon, s.cardWeapon);
    swap(g_cardUntil, s.cardUntil);
    swap(g_speed, s.speed);
    swap(g_flameUnit, s.flameUnit);
    swap(g_flameHide, s.flameHide);
    swap(g_refuel, s.refuel);
    swap(g_flameStreamPath, s.flameStreamPath);
    swap(g_flameStreamCompanion, s.flameStreamCompanion);
    swap(g_flameStreamLoops, s.flameStreamLoops);
    swap(g_flameStreamElement, s.flameStreamElement);
    swap(g_flameStreamState, s.flameStream);
}

// M10: whose pad is whose, read in the split-screen lobby: player 2 joins with START on their pad,
// player 1 readies with A (or Enter on the keyboard). A run's pads are those of its first match.
std::array<double, 4> g_padStartAt{-10.0, -10.0, -10.0, -10.0};
std::array<double, 4> g_padReadyAt{-10.0, -10.0, -10.0, -10.0};

void WatchLobbyPads(double now) {
    for (int user = 0; user < 4; ++user) {
        const unsigned short b = RawXInputButtonsOf(user);
        if (b & 0x0010) g_padStartAt[static_cast<std::size_t>(user)] = now;
        if (b & 0x1010) g_padReadyAt[static_cast<std::size_t>(user)] = now;  // A or START
    }
    static int lastJoin = -1, lastReady = -1;
    const bool lobby = (g->coopPlayers == 2 && !g->run->HasActiveRun()) || g->coopLobbyNeeded;
    if (!lobby) {
        lastJoin = lastReady = -1;
        return;
    }
    const auto gw = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    const int join = gw ? ReadOr<std::int32_t>(gw + 0x624, -1) : -1;    // player 2: 0 out, 1 in, 2 ready
    const int ready = gw ? ReadOr<std::int32_t>(gw + 0x620, -1) : -1;   // player 1: 1 in, 2 ready
    const auto latest = [&](const std::array<double, 4>& at, int skip) {
        int best = -1;
        for (int user = 0; user < 4; ++user) {
            if (user == skip || now - at[static_cast<std::size_t>(user)] > 1.5) continue;
            if (best < 0 || at[static_cast<std::size_t>(user)] > at[static_cast<std::size_t>(best)]) best = user;
        }
        return best;
    };
    if (lastJoin == 0 && join == 1) {
        const int user = latest(g_padStartAt, -1);
        if (user >= 0) g->p2Pad = user;
        RM_LOGI("co-op: player 2 joined the lobby with %s", user >= 0 ? ("XInput user " + std::to_string(user)).c_str() : "a pad not seen");
    }
    if (lastReady == 1 && ready == 2) {
        g->p1Pad = latest(g_padReadyAt, g->p2Pad);  // -1: the keyboard
        RM_LOGI("co-op: player 1 readied with %s", g->p1Pad >= 0 ? ("XInput user " + std::to_string(g->p1Pad)).c_str() : "the keyboard");
    }
    lastJoin = join;
    lastReady = ready;
}

// M10: the enemies-left label on player 2's half of the split screen (the game's text routine draws
// only in the top view), under the counter the game draws there, 360 lines lower than the top's.
matchui::Hud g_splitHud;

void DrawSplitHud() {
    if (!g_splitHud.split || matchui::IsOpen()) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const auto text = [&](const std::string& s, float x, float y, float size, ImU32 colour) {
        if (s.empty()) return;
        const ImVec2 sz = font->CalcTextSizeA(size * k, FLT_MAX, 0.0f, s.c_str());
        const ImVec2 at(ox + x * k - sz.x * 0.5f, oy + (y + kSplitHalfLines) * k - sz.y * 0.5f);
        list->AddText(font, size * k, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), s.c_str());
        list->AddText(font, size * k, at, colour, s.c_str());
    };
    // The arena, supplies, keys and notices are the players' together: shown once, at the top of
    // the screen as in solo (the user, 2026-11-18: "if they both have the same ... why couldn't we
    // just have it once like the SP has?"). Only the counter's label stays, under the counter the
    // game itself draws in each half.
    text(g_splitHud.center, 640.0f, 176.0f, 15.0f, IM_COL32(220, 220, 220, 200));
}

void DrawSpecialWidgets() {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || run.phase() != rm::Phase::ArenaActive || !g->match.inMatch || matchui::IsOpen()) return;
    if (run.SeatCount() > 1) {
        // M10: each player's rings on their half of the split screen (player 1 on top).
        ForEachSeatTurn([&] {
            g_drawShiftY = run.ActiveSeat() == 0 ? -kSplitHalfLines : 0.0f;
            DrawSpecialWidget(0);
            DrawSpecialWidget(1);
        });
        g_drawShiftY = 0.0f;
        return;
    }
    DrawSpecialWidget(0);
    DrawSpecialWidget(1);
}

// One line in the game's font during a match; the native screens close when nothing is
// left to choose (run over, match gone).
void UpdateMatchStatus() {
    rm::RunController& run = *g->run;
    const rm::Phase phase = run.phase();
    const bool between = phase == rm::Phase::ArenaResults || phase == rm::Phase::RewardSelection ||
                         phase == rm::Phase::Shop || phase == rm::Phase::RouteSelection ||
                         phase == rm::Phase::RunComplete;
    if (matchui::IsOpen() && (!run.HasActiveRun() || !between || !g->match.inMatch)) matchui::Close();
    matchui::Hud hud;
    int counter = -1;
    if (g->settings.nativeMatchUi && run.HasActiveRun() && g->match.inMatch) {
        // What the HUD shows, read straight from the run (a full Snapshot each tick cost 80 us).
        struct {
            int remaining, arena, arena_count, keys;
            std::int64_t supplies;
            bool keys_needed;
        } s{run.RemainingEnemies(), run.state().arena_index, run.state().arena_count, run.state().keys,
            run.state().supplies.balance(),
            g->content->economy().keys.enabled && !run.state().character.build.chest_keys_exempt};
        if (phase == rm::Phase::ArenaActive) {
            // With the game's clock hidden its big digits count the enemies left; otherwise
            // the number goes into the label.
            const bool digits = engine::Caps().mercHud && !g->settings.mercHud;
            counter = digits ? s.remaining : -1;
            hud.left = "ARENA " + std::to_string(s.arena) +
                       (run.EndlessRun() ? std::string() : " OF " + std::to_string(s.arena_count));  // endless: no last arena
            hud.center = (digits ? std::string() : std::to_string(s.remaining) + " ") +
                         (s.remaining == 1 ? "ENEMY LEFT" : "ENEMIES LEFT");
            // A boss arena (M6): the digits are the boss's health in percent, the label its name.
            const rm::MapDef* map = CurrentMap();
            if (map != nullptr && map->boss) {
                const auto hp = g->boss.unit ? engine::ReadHp(g->boss.unit) : std::nullopt;
                const int percent = hp && hp->max > 0 ? std::clamp((hp->hp * 100 + hp->max - 1) / hp->max, 0, 99)
                                                      : (g->boss.dead ? 0 : 99);
                counter = digits ? percent : -1;
                hud.center = (digits ? std::string() : std::to_string(percent) + " PERCENT ") + Upper(Plain(map->boss->name));
            }
            hud.right = std::to_string(s.supplies) + " SUPPLIES";
            if (s.keys_needed) hud.right += "   " + Upper(Plural(s.keys, "key"));
            if (!g->noticeNext.empty() && Now() >= g->noticeUntil) {
                g->notice = std::move(g->noticeNext);
                g->noticeNext.clear();
                g->noticeUntil = Now() + 4.0;
            }
            if (Now() < g->noticeUntil) hud.notice = g->notice;
        } else if (phase == rm::Phase::RouteSelection && run.state().route_chosen) {
            hud.notice = "NEXT STAGE LOADING";
        }
    }
    hud.split = run.HasActiveRun() && run.SeatCount() > 1 && g->match.inMatch;
    SetMercHudCounter(counter);
    g_splitHud = hud;
    matchui::SetHud(hud);
}

void SettleFinishedRun() {
    rm::RunController& run = *g->run;
    const rm::Phase phase = run.phase();
    if (phase == rm::Phase::RunComplete || phase == rm::Phase::RunFailed) {
        if (matchui::IsOpen()) return;  // the native RUN COMPLETE screen is still up
        // A completed run lets the match end (the game's own time-up path, once); a death is
        // ended by the game itself.
        if (phase == rm::Phase::RunComplete && g->match.inMatch && !g->arenaEndRequested) {
            RequestMercMatchEnd();
            g->arenaEndRequested = true;
            RM_LOGI("run over (%s): requested mercenaries time-up to end the match", std::string(rm::PhaseName(phase)).c_str());
        }
        const auto summary = run.EndRun();
        if (summary.ok()) {
            // Nothing of this run may leak into the next match.
            g->transition = {};
            g->matchUiOpenRequested = false;
            g_enemyTracks.clear();
            autopilot::Disarm("run over");
            RestoreBuildStacks();
            world::SetDropRules({});
            // M7.3: the last match's Mercenaries results are skipped (they would write a record
            // into the game's save) and the game goes back to its Play menu, where the mod menu
            // opens on the run's results.
            autopilot::ReturnToTitle(true);
            if (autopilot::ReturningToTitle()) nativemenu::OpenOnPlayMenu(nativemenu::ScreenId::RunResults);
            host::SetStageOverride(-1);  // plain Mercenaries loads its own stages again
            if (phase == rm::Phase::RunFailed) ClearMercMatchEnd();
            SetStatus("run ended (" + summary->end_reason + "): +" + std::to_string(summary->credits_banked) +
                      " credits banked");
            if (!g->settings.nativeMatchUi) overlay::SetFocused(true);
        } else {
            Report(summary.status(), "end run");
        }
    }
}

// Developer UI demo: walks the real RunController through one arena with
// synthetic kills so every panel can be captured without gameplay. Only runs
// with dev.ui_demo, on a sandbox profile, and labels its kills "demo_".
void DriveUiDemo(double now) {
    rm::RunController& run = *g->run;
    if (g->demoStart < 0) g->demoStart = now;
    const double t = now - g->demoStart;
    auto step = [&](int index, double at, auto&& action) {
        if (g->demoStep == index && t >= at) {
            action();
            ++g->demoStep;
        }
    };
    step(0, 2.0, [&] {
        run.SetArenaPool(RotationMaps(), CurrentMapKey());
        Report(run.StartRun("chris", 42), "demo start run");
        Report(run.BeginArena(now), "demo begin arena");
        overlay::SetFocused(true);
    });
    step(1, 5.0, [&] {
        // Leave three enemies so the HUD shows an arena in progress.
        for (int i = 0; i + 3 < run.RemainingEnemies() + i && run.RemainingEnemies() > 3; ++i) {
            rm::KillEvent kill;
            kill.now = now;
            kill.by_player = true;
            kill.enemy_id = "demo_" + std::to_string(i);
            run.OnEnemyKilled(kill);
        }
        overlay::SetFocused(false);
    });
    step(2, 9.0, [&] {
        for (int i = 0; run.RemainingEnemies() > 0 && i < 500; ++i) {
            rm::KillEvent kill;
            kill.now = now;
            kill.by_player = true;
            kill.enemy_id = "demo_b" + std::to_string(i);
            run.OnEnemyKilled(kill);
        }
    });
    step(3, 13.0, [&] { Report(run.ContinueToRewards(), "demo continue"); });
    step(4, 17.0, [&] { Report(run.ChooseReward(0), "demo choose reward"); });
    step(5, 21.0, [&] { Report(run.LeaveShop(), "demo leave shop"); });
    step(6, 25.0, [&] { Report(run.ChooseRoute(run.RouteChoices().size() > 1 ? 1 : 0), "demo choose route"); });
}

// M7.1: the game's Organize/Buy screen (uIdItemBox00) in a match, created as the match's phase 3
// does (re5_layout.h kFnItemBoxCreate); its archive (ShopResource) must be loaded. The screen
// edits the campaign save, so the save data ([kSaveData], plain in memory) is copied when it opens
// and put back byte for byte when it closes; what it changed is logged first.
std::uintptr_t g_itemBox = 0;
constexpr std::size_t kSaveSnapshotBytes = 0x30000;  // covers slot 0's inventories, storage and stage
std::vector<char> g_saveSnapshot;
std::uintptr_t g_saveSnapshotBase = 0;

bool CopySave(std::uintptr_t base, std::vector<char>& out) {
    out.resize(kSaveSnapshotBytes);
    for (std::size_t at = 0; at < out.size(); at += 0x1000) {
        if (!SafeCopy(out.data() + at, base + at, std::min<std::size_t>(0x1000, out.size() - at))) return false;
    }
    return true;
}

void SnapshotSave() {
    g_saveSnapshotBase = ReadOr<std::uintptr_t>(re5::addr::kSaveData, 0);
    if (!g_saveSnapshotBase || !CopySave(g_saveSnapshotBase, g_saveSnapshot)) {
        g_saveSnapshot.clear();
        RM_LOGE("itembox: could not copy the save data");
    }
}

// Research: the ranges where `now` differs from `from`, with their bytes (old>new) when short.
void LogSaveDiff(const char* label, const std::vector<char>& from, const std::vector<char>& now) {
    std::string ranges;
    int count = 0;
    for (std::size_t i = 0; i < now.size() && i < from.size();) {
        if (now[i] == from[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < now.size() && (now[j] != from[j] || (j + 1 < now.size() && now[j + 1] != from[j + 1]))) ++j;
        if (count++ < 60) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " +%zX:", i);
            ranges += buf;
            for (std::size_t k = i; k < j && k < i + 12; ++k) {
                std::snprintf(buf, sizeof(buf), "%02X>%02X,", static_cast<std::uint8_t>(from[k]), static_cast<std::uint8_t>(now[k]));
                ranges += buf;
            }
        }
        i = j;
    }
    RM_LOGW("itembox: %s: %d ranges:%s", label, count, ranges.c_str());
}
std::vector<char> g_saveSwappedIn;  // developer mode: the save data right after the run went in

// Logs the changed ranges, then writes the copy back.
void RestoreSave() {
    if (g_saveSnapshot.empty() || !g_saveSnapshotBase) return;
    std::vector<char> now;
    if (CopySave(g_saveSnapshotBase, now)) {
        std::string ranges;
        int count = 0;
        for (std::size_t i = 0; i < now.size();) {
            if (now[i] == g_saveSnapshot[i]) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < now.size() && (now[j] != g_saveSnapshot[j] || (j + 1 < now.size() && now[j + 1] != g_saveSnapshot[j + 1]))) ++j;
            if (count++ < 40) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), " +%zX..%zX", i, j);
                ranges += buf;
            }
            i = j;
        }
        RM_LOGW("itembox: the screen changed %d ranges of the save data:%s", count, ranges.c_str());
    }
    bool ok = true;
    for (std::size_t at = 0; at < g_saveSnapshot.size(); at += 0x1000) {
        ok &= SafeWrite(g_saveSnapshotBase + at, g_saveSnapshot.data() + at, std::min<std::size_t>(0x1000, g_saveSnapshot.size() - at));
    }
    RM_LOGW("itembox: save data restored: %s", ok ? "yes" : "NO");
    g_saveSnapshot.clear();
}

std::uintptr_t CreateItemBox() {
    __try {
        const std::uintptr_t obj = reinterpret_cast<std::uintptr_t(__cdecl*)()>(re5::addr::kFnItemBoxCreate)();
        if (!obj) return 0;
        *reinterpret_cast<std::uint32_t*>(obj + 4) |= 0x2000;
        const auto manager = *reinterpret_cast<std::uintptr_t*>(re5::addr::kUnitManager);
        reinterpret_cast<bool(__thiscall*)(std::uintptr_t, int, std::uintptr_t)>(re5::addr::kFnUnitRegister)(manager, 0x1A, obj);
        if (const auto flow = *reinterpret_cast<std::uintptr_t*>(re5::addr::kGameFlow)) {
            *reinterpret_cast<std::uintptr_t*>(flow + re5::off::kGameFlowItemBox) = obj;
        }
        return obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool DeleteItemBox(std::uintptr_t obj) {
    __try {
        if (const auto flow = *reinterpret_cast<std::uintptr_t*>(re5::addr::kGameFlow)) {
            *reinterpret_cast<std::uintptr_t*>(flow + re5::off::kGameFlowItemBox) = 0;
        }
        *reinterpret_cast<std::uint32_t*>(obj + 4) &= ~0x2000u;
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(obj);
        reinterpret_cast<void(__thiscall*)(std::uintptr_t)>(*reinterpret_cast<std::uintptr_t*>(vtable + 0x30))(obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// M7.1: the run inside the screen. While it is open the save data holds the run: Chris's grid is
// the match inventory, Sheva's grid, the storage and the treasures are empty, and the gold is the
// run's supplies (x kGoldPerSupply: the game's prices are in hundreds). When it closes the match
// inventory follows Chris's grid, the supplies follow the gold, and the copy taken before is
// written back (research/re5-engine-map.md "Save data").
constexpr std::size_t kSaveGold = 0x1C0;
// The inventories keep each slot as the match has it (re5_layout.h kSlot*): 0x2C bytes of the
// 0x30-byte inventory slot, the pointer at +0x00 being the screen's.
constexpr std::size_t kSaveGridChris = 0x2344C;  // the inventory block the screen edits
constexpr std::size_t kSaveGridSheva = kSaveGridChris + 0x420;
// The screen fills the block it edits from one of the other two when it opens: all three get the run.
constexpr std::size_t kSaveGridBlocks[] = {0x2344C, 0x249DC, 0x2761C};
constexpr std::size_t kSaveRecordSize = 0x2C;
constexpr std::size_t kSaveRecordItem = re5::off::kSlotItemId, kSaveRecordCount = re5::off::kSlotCount,
                      kSaveRecordMax = re5::off::kSlotMax, kSaveRecordCell = kSlotCell,
                      kSaveRecordUpgrades = re5::off::kSlotUpgrades;
constexpr int kSaveGridRecords = 10;  // cells 0..8 and the knife (9)
// 12-byte records (u16 item, u16 count, ...); a purchase lands in the first (+0x3878), what is
// before it is other data.
constexpr std::size_t kSaveStorageFrom = 0x3878, kSaveStorageTo = 0x3B00;
constexpr std::size_t kSaveTreasuresFrom = 0x3B00, kSaveTreasuresTo = 0x3C00;
constexpr std::int64_t kGoldPerSupply = 100;

struct RunInBox {
    bool active = false;
    bool checked = false;   // the screen's first frames showed the run's grid
    std::int64_t gold = 0;  // what the screen started with
    std::int64_t remainder = 0;  // gold short of a whole supply, kept for the next visit (same run)
    std::string runId;
    // The run's stash: the storage records (12 bytes: u16 item, u16 count, upgrade bytes) the
    // player left there (bought, or moved out of the grid); back in the storage next visit.
    std::vector<std::array<std::uint8_t, 12>> stash;
    std::array<std::uint32_t, kGridCells> cells{};  // the grid written
    std::vector<std::int64_t> restockBefore;  // restocking guns held when the screen opened
    std::int64_t worth = 0;  // what the grid and the stash sold for when the screen opened (SaleWorth)
};
RunInBox g_runInBox;

// What the records (item, count) sell for in supplies (RunController::SaleValue: half the shop price;
// a gun once, whatever its rounds).
std::int64_t SaleWorth(const std::vector<std::pair<std::uint32_t, int>>& records) {
    std::int64_t total = 0;
    for (const auto& [id, count] : records) total += g->run->SaleValue(MakeItemId(RecordItemId(id)), std::max(0, count));
    return total;
}

void AppendStash(std::vector<std::pair<std::uint32_t, int>>& records) {
    for (const auto& r : g_runInBox.stash) records.emplace_back(static_cast<std::uint32_t>(r[0] | (r[1] << 8)), r[2] | (r[3] << 8));
}

bool RunStashHolds(std::uint32_t recordId) {
    if (g_runInBox.runId != g->run->state().run_id) return false;
    for (const auto& r : g_runInBox.stash) {
        if (RecordItemId(static_cast<std::uint32_t>(r[0] | (r[1] << 8))) == recordId) return true;
    }
    return false;
}
bool RunInItemBox() { return g_itemBox != 0 && g_runInBox.active; }

// An item the match cannot take now (a full grid, a gun whose model is not loaded here) goes to
// the run's storage, the one the ORGANIZE screen shows, instead of being lost: the player takes
// it out at a later shop, on any stage of the run. Stacks join an earlier record of the same item
// up to its stack size; a gun's count is its rounds. Returns how many it kept (the storage holds
// 54 records).
constexpr std::size_t kStashRecords = (kSaveStorageTo - kSaveStorageFrom) / 12;

int KeepInRunStash(std::uint32_t id, int count, const char* why) {
    if (id == 0 || count <= 0) return 0;
    if (g_runInBox.runId != g->run->state().run_id) {
        g_runInBox.runId = g->run->state().run_id;
        g_runInBox.remainder = 0;
        g_runInBox.stash.clear();
    }
    const int total = count;
    if (!IsWeaponItem(id)) {
        const int stackMax = std::max(1, ItemStackMax(id));
        for (auto& record : g_runInBox.stash) {
            if (count <= 0) break;
            if ((record[0] | (record[1] << 8)) != static_cast<int>(id)) continue;
            const int have = record[2] | (record[3] << 8);
            const int add = std::min(count, stackMax - have);
            if (add <= 0) continue;
            record[2] = static_cast<std::uint8_t>((have + add) & 0xFF);
            record[3] = static_cast<std::uint8_t>(((have + add) >> 8) & 0xFF);
            count -= add;
        }
    }
    while (count > 0) {
        if (g_runInBox.stash.size() >= kStashRecords) {
            RM_LOGW("inventory: %04X x%d %s; the run's storage is full", id, count, why);
            break;
        }
        const int put = IsWeaponItem(id) ? count : std::min(count, std::max(1, ItemStackMax(id)));
        std::array<std::uint8_t, 12> record{};
        record[0] = static_cast<std::uint8_t>(id & 0xFF);
        record[1] = static_cast<std::uint8_t>(id >> 8);
        record[2] = static_cast<std::uint8_t>(put & 0xFF);
        record[3] = static_cast<std::uint8_t>((put >> 8) & 0xFF);
        g_runInBox.stash.push_back(record);
        count -= put;
    }
    const int kept = total - count;
    if (kept <= 0) return 0;
    RM_LOGW("inventory: %04X x%d %s; kept in the run's storage (ORGANIZE)", id, kept, why);
    g->toasts.Push(GridItemName(id) + " is in the storage (ORGANIZE)");
    g_stashNote = Upper(Plain(GridItemName(id))) + " IN STORAGE";
    g_stashNoteAt = Now();
    if (IsWeaponItem(id)) {
        auto& guns = StashGuns();
        const std::string name = Upper(Plain(GridItemName(id)));
        guns.erase(std::remove(guns.begin(), guns.end(), name), guns.end());
        guns.push_back(name);
    }
    return kept;
}

// ---- The mod's ORGANIZE screen's storage and selling (declared above the menus) ----------------
std::vector<std::pair<std::uint32_t, int>> StashRecords() {
    std::vector<std::pair<std::uint32_t, int>> out;
    if (g_runInBox.runId != g->run->state().run_id) return out;
    AppendStash(out);
    return out;
}

std::vector<OrganizeRow> OrganizeRows(int pick, int stashCount) {
    std::vector<OrganizeRow> rows;
    for (int cell = 0; cell < kGridCells; ++cell) rows.push_back({OrganizeRow::Cell, cell});
    for (int i = 0; i < stashCount; ++i) rows.push_back({OrganizeRow::Stash, i});
    if (pick >= 0 && pick < kGridCells) rows.push_back({OrganizeRow::ToStorage, 0});
    if (pick >= 0) rows.push_back({OrganizeRow::Sell, 0});
    return rows;
}

// A storage record into the active player's grid (wherever there is room).
bool TakeFromStash(int index, std::string& why) {
    if (g_runInBox.runId != g->run->state().run_id || index < 0 || index >= static_cast<int>(g_runInBox.stash.size())) return false;
    auto& record = g_runInBox.stash[static_cast<std::size_t>(index)];
    const auto id = static_cast<std::uint32_t>(record[0] | (record[1] << 8));
    const int count = record[2] | (record[3] << 8);
    const auto local = engine::LocalPlayer();
    // A gun is one record: a second of one in the hands would not be made (and was lost here).
    if (IsWeaponItem(id) && local && FindRecord(local, id)) {
        why = "You have one already";
        return false;
    }
    const int placed = GiveItem("storage", MakeItemId(id), std::max(1, count));
    if (placed <= 0) {
        why = "No room in your grid";
        return false;
    }
    // A gun comes with a full magazine: it gets back the rounds (the fuel) it went in with.
    if (IsWeaponItem(id) && local) {
        if (const auto made = FindRecord(local, id)) SafeStore<std::int32_t>(made + re5::off::kSlotCount, std::max(0, count));
    }
    if (IsWeaponItem(id) || placed >= count) {
        g_runInBox.stash.erase(g_runInBox.stash.begin() + index);
    } else {
        record[2] = static_cast<std::uint8_t>((count - placed) & 0xFF);
        record[3] = static_cast<std::uint8_t>(((count - placed) >> 8) & 0xFF);
    }
    RM_LOGI("organize: player %d took %04X x%d out of the storage", g->run->ActiveSeat() + 1, id, placed);
    return true;
}

// A grid cell of the active player into the run's storage.
bool PutCellInStash(int cell, std::string& why) {
    const auto grid = ReadGrid();
    if (cell < 0 || cell >= kGridCells || !grid[static_cast<std::size_t>(cell)].record) return false;
    const GridCell& c = grid[static_cast<std::size_t>(cell)];
    if (c.id == re5::addr::kItemKnife || c.id == re5::addr::kItemUnarmed) return false;
    const int kept = KeepInRunStash(c.id, std::max(1, c.count), "moved to the storage");
    if (kept <= 0) {
        why = "The storage is full";
        return false;
    }
    StashGunsSeen();  // put there by the player in ORGANIZE: no "IN STORAGE" note for it
    if (IsWeaponItem(c.id) || kept >= c.count) {
        ClearSlot(c.record);
    } else {
        SafeStore<std::int32_t>(c.record + re5::off::kSlotCount, c.count - kept);
    }
    why.clear();
    RM_LOGI("organize: player %d put %04X x%d in the storage", g->run->ActiveSeat() + 1, c.id, kept);
    return true;
}

// What a sale pays: the run's shop's value (half its price), credited to the shared supplies; a
// sold gun leaves the run.
std::int64_t PaySale(std::uint32_t id, int count) {
    const std::int64_t value = g->run->SaleValue(MakeItemId(RecordItemId(id)), std::max(0, count));
    if (IsWeaponItem(id)) Report(g->run->DropWeapon(MakeItemId(RecordItemId(id)), "sold"), "gun sold");
    if (value > 0) Report(g->run->AdjustSupplies(value, "sold"), "sale");
    RM_LOGI("organize: player %d sold %04X x%d for %lld", g->run->ActiveSeat() + 1, id, count, static_cast<long long>(value));
    return value;
}

std::int64_t SellCell(int cell) {
    const auto grid = ReadGrid();
    if (cell < 0 || cell >= kGridCells || !grid[static_cast<std::size_t>(cell)].record) return 0;
    const GridCell c = grid[static_cast<std::size_t>(cell)];
    if (c.id == re5::addr::kItemKnife || c.id == re5::addr::kItemUnarmed) return 0;
    ClearSlot(c.record);
    return std::max<std::int64_t>(1, PaySale(c.id, c.count));
}

std::int64_t SellStash(int index) {
    if (g_runInBox.runId != g->run->state().run_id || index < 0 || index >= static_cast<int>(g_runInBox.stash.size())) return 0;
    const auto record = g_runInBox.stash[static_cast<std::size_t>(index)];
    g_runInBox.stash.erase(g_runInBox.stash.begin() + index);
    return std::max<std::int64_t>(1, PaySale(static_cast<std::uint32_t>(record[0] | (record[1] << 8)), record[2] | (record[3] << 8)));
}

// The adapter's part of a run's resume point (rm::RunController::SaveResumePoint), taken at a
// stage change after the inventory snapshot: what the next arena starts with (the carried items,
// their grid cells, health) plus the run's storage, the gold short of a supply and the guns' levels.
std::string AdapterResumeState() {
    nlohmann::json j;
    nlohmann::json carry = nlohmann::json::array();
    for (const auto& [id, count] : g->transition.carry) carry.push_back({id, count});
    j["carry"] = std::move(carry);
    j["cells"] = g->transition.cells;
    j["counts"] = g->transition.counts;
    j["hp"] = g->transition.hp;
    nlohmann::json stash = nlohmann::json::array();
    for (const auto& record : g_runInBox.stash) {
        std::string hex;
        for (const std::uint8_t b : record) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02X", b);
            hex += buf;
        }
        stash.push_back(hex);
    }
    j["stash"] = std::move(stash);
    j["remainder"] = g_runInBox.runId == g->run->state().run_id ? g_runInBox.remainder : 0;
    nlohmann::json levels = nlohmann::json::object();
    for (const auto& [id, l] : RunLevels()) levels[std::to_string(id)] = {l[0], l[1]};
    j["levels"] = std::move(levels);
    j["level_steps"] = g_weaponLevels.shopSteps;
    // M10: player 2's (a co-op run): their carried inventory and health, and the costume they wore.
    if (g->run->SeatCount() > 1) {
        const AdapterSeat& two = g_adapterSeat == 1 ? AdapterSeat{g->transition.carry, g->transition.cells, g->transition.counts, g->transition.hp, {}}
                                                    : g_adapterSeats[1];
        nlohmann::json carry2 = nlohmann::json::array();
        for (const auto& [id, count] : two.carry) carry2.push_back({id, count});
        j["seat2"] = {{"carry", std::move(carry2)}, {"cells", two.cells}, {"counts", two.counts}, {"hp", two.hp}};
        j["p2_costume"] = g->p2Costume;
    }
    return j.dump();
}

bool RestoreAdapterResumeState(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    try {
        g->transition = {};
        for (const auto& e : j.value("carry", nlohmann::json::array())) {
            g->transition.carry.emplace_back(e.at(0).get<std::uint32_t>(), e.at(1).get<int>());
        }
        if (j.contains("cells")) g->transition.cells = j.at("cells").get<std::array<std::uint32_t, 9>>();
        if (j.contains("counts")) g->transition.counts = j.at("counts").get<std::array<int, 9>>();
        g->transition.hp = j.value("hp", -1);
        g_runInBox.runId = g->run->state().run_id;
        g_runInBox.remainder = j.value("remainder", std::int64_t{0});
        g_runInBox.stash.clear();
        for (const auto& hexValue : j.value("stash", nlohmann::json::array())) {
            const std::string hex = hexValue.get<std::string>();
            std::array<std::uint8_t, 12> record{};
            for (std::size_t i = 0; i < record.size() && i * 2 + 1 < hex.size(); ++i) {
                record[i] = static_cast<std::uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
            }
            g_runInBox.stash.push_back(record);
        }
        RunLevels().clear();
        const nlohmann::json levels = j.value("levels", nlohmann::json::object());  // kept: items() of a temporary dangles
        for (const auto& [key, l] : levels.items()) {
            RunLevels()[static_cast<std::uint32_t>(std::stoul(key))] = {l.at(0).get<std::uint32_t>(), l.at(1).get<std::uint32_t>()};
        }
        g_weaponLevels.shopSteps.clear();
        if (j.contains("level_steps")) g_weaponLevels.shopSteps = j.at("level_steps").get<std::map<std::uint32_t, int>>();
        // M10: player 2's (the adapter's seat 0 is active here: no SeatTurn outside a tick).
        g_adapterSeats[1] = {};
        if (j.contains("seat2")) {
            const nlohmann::json& s = j.at("seat2");
            AdapterSeat& two = g_adapterSeats[1];
            for (const auto& e : s.value("carry", nlohmann::json::array())) two.carry.emplace_back(e.at(0).get<std::uint32_t>(), e.at(1).get<int>());
            if (s.contains("cells")) two.cells = s.at("cells").get<std::array<std::uint32_t, 9>>();
            if (s.contains("counts")) two.counts = s.at("counts").get<std::array<int, 9>>();
            two.hp = s.value("hp", -1);
            RM_LOGI("resume: player 2: %zu carried records, health %d", two.carry.size(), two.hp);
        }
        g->p2Costume = j.value("p2_costume", -1);
    } catch (const std::exception& e) {
        RM_LOGW("resume: the adapter's state could not be read (%s)", e.what());
        return false;
    }
    RM_LOGI("resume: %zu carried records, %zu in the storage, health %d", g->transition.carry.size(), g_runInBox.stash.size(),
            g->transition.hp);
    return true;
}

void LogRunMap();

// CONTINUE RUN: the profile's run again, before the arena it was saved before. The game's flow
// then starts that arena's match on the run map's next map, and TryBeginArena begins it with the
// carried inventory as after any stage change.
bool ContinueRun() {
    rm::RunController& run = *g->run;
    const auto resumed = run.ResumeRun();
    if (!resumed.ok()) {
        RM_LOGW("resume: %s", resumed.status().message.c_str());
        g->toasts.Push("The run cannot go on: " + resumed.status().message);
        return false;
    }
    if (!RestoreAdapterResumeState(*resumed)) RM_LOGW("resume: going on without the carried inventory");
    g->coopLobbyNeeded = run.SeatCount() > 1 && engine::Caps().splitScreen;
    if (g->coopLobbyNeeded) {
        g->p1Pad = -2;  // learned again in the lobby (WatchLobbyPads)
        g->p2Pad = -1;
    }
    const int costume = static_cast<int>(std::max<std::int64_t>(0, run.state().character.costume_index));
    const int planned = run.PlannedMapKey();
    const int stage = planned >= 0 ? planned : PickMap(-1);
    if (g->settings.directStart && autopilot::Available() && !autopilot::Armed()) ArmMap(stage, costume);
    g->transition.pending = true;
    g->transition.generation = g->matchGeneration;
    g->transition.startedAt = Now();
    LogRunMap();
    RM_LOGI("resume: run %s goes on at arena %d on stage %d (costume slot %d)", run.state().run_id.c_str(),
            run.state().arena_index + 1, stage, costume);
    return true;
}


void WriteSaveGrid(std::uintptr_t block, const std::array<GridCell, kGridCells>* grid) {
    for (int r = 0; r < kSaveGridRecords; ++r) {
        const auto rec = block + static_cast<std::size_t>(r) * kSaveRecordSize;
        if (ReadOr<std::int32_t>(rec + kSaveRecordCell, -1) == 9) continue;  // the knife stays
        const GridCell* c = grid != nullptr && r < kGridCells ? &(*grid)[static_cast<std::size_t>(r)] : nullptr;
        const bool item = c != nullptr && c->record != 0;
        SafeStore<std::uint32_t>(rec + kSaveRecordItem, item ? c->id : 0);
        SafeStore<std::int32_t>(rec + kSaveRecordCount, item ? c->count : 0);
        SafeStore<std::int32_t>(rec + kSaveRecordMax, item ? ItemStackMax(c->id) : 0);
        SafeStore<std::int32_t>(rec + kSaveRecordCell, r);
        // The rest of the campaign's slot (two words before the cell, the equipped mark) goes;
        // the weapon keeps the levels it has in the match.
        for (const std::size_t field : {std::size_t{0x10}, std::size_t{0x14}, re5::off::kSlotEquipped}) {
            SafeStore<std::int32_t>(rec + field, 0);
        }
        for (const std::size_t word : {std::size_t{0}, std::size_t{4}}) {
            SafeStore<std::uint32_t>(rec + kSaveRecordUpgrades + word, item ? ReadOr<std::uint32_t>(c->record + re5::off::kSlotUpgrades + word, 0) : 0);
        }
    }
}

// The run goes into the save data (after its copy is taken).
void PutRunInSave(std::uintptr_t save) {
    const auto grid = ReadGrid();
    for (const std::size_t block : kSaveGridBlocks) {
        WriteSaveGrid(save + block, &grid);
        WriteSaveGrid(save + block + 0x420, nullptr);  // Sheva
    }
    for (int c = 0; c < kGridCells; ++c) {
        g_runInBox.cells[static_cast<std::size_t>(c)] = grid[static_cast<std::size_t>(c)].record ? grid[static_cast<std::size_t>(c)].id : 0;
    }
    g_runInBox.checked = false;
    for (std::size_t at = kSaveStorageFrom; at < kSaveStorageTo; ++at) SafeStore<std::uint8_t>(save + at, 0);
    for (std::size_t at = kSaveTreasuresFrom; at < kSaveTreasuresTo; ++at) SafeStore<std::uint8_t>(save + at, 0);
    if (g_runInBox.runId != g->run->state().run_id) {
        g_runInBox.runId = g->run->state().run_id;
        g_runInBox.remainder = 0;
        g_runInBox.stash.clear();
    }
    // The run's stash in the storage (as many records as fit).
    std::size_t at = kSaveStorageFrom;
    for (const auto& record : g_runInBox.stash) {
        if (at + 12 > kSaveStorageTo) break;
        SafeWrite(save + at, record.data(), record.size());
        at += 12;
    }
    g_runInBox.restockBefore.clear();
    for (const std::int64_t item : g->run->RestockWeaponItems()) {
        const std::uint32_t record = RecordItemId(static_cast<std::uint32_t>(item));
        const bool inGrid = std::any_of(grid.begin(), grid.end(), [record](const GridCell& c) { return c.record && c.id == record; });
        if (inGrid || RunStashHolds(record)) g_runInBox.restockBefore.push_back(item);
    }
    g_runInBox.remainder = 0;  // a sale pays the run's own worth (TakeRunFromSave), never part of the gold
    g_runInBox.gold = g->run->Snapshot(Now()).supplies * kGoldPerSupply;
    SafeStore<std::uint32_t>(save + kSaveGold, static_cast<std::uint32_t>(g_runInBox.gold));
    {
        std::vector<std::pair<std::uint32_t, int>> records;
        for (const GridCell& c : grid) {
            if (c.record) records.emplace_back(c.id, c.count);
        }
        AppendStash(records);
        g_runInBox.worth = SaleWorth(records);
    }
    g_runInBox.active = true;
    std::string text;
    for (int c = 0; c < kGridCells; ++c) {
        if (!grid[static_cast<std::size_t>(c)].record) continue;
        char buf[24];
        std::snprintf(buf, sizeof(buf), " %d:%04Xx%d", c, grid[static_cast<std::size_t>(c)].id, grid[static_cast<std::size_t>(c)].count);
        text += buf;
    }
    RM_LOGI("itembox: the run is in the screen (gold %lld; grid%s)", static_cast<long long>(g_runInBox.gold), text.c_str());
}

// The screen closed: the match inventory follows Chris's grid, the supplies follow the gold
// (before the save data is restored).
void TakeRunFromSave(std::uintptr_t save) {
    if (!g_runInBox.active) return;
    g_runInBox.active = false;
    if (!g_runInBox.checked) {
        // The screen never showed the run: what it holds is not the run's (campaign items).
        RM_LOGE("itembox: the screen did not show the run's grid; its result is ignored");
        return;
    }
    g->transition.carry.clear();
    g->transition.cells = {};
    g->transition.counts = {};
    // What is not in the nine cells (the knife, unarmed) stays as it is.
    if (const auto player = engine::LocalPlayer()) {
        const auto inv = player + re5::off::kPlayerInventory;
        for (int i = 0; i < re5::off::kInventorySlots; ++i) {
            const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
            if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
            const int cell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
            if (cell >= 0 && cell < kGridCells) continue;
            g->transition.carry.emplace_back(SlotItemId(slot), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0));
        }
    }
    std::string text;
    for (int r = 0; r < kSaveGridRecords; ++r) {
        const auto rec = save + kSaveGridChris + static_cast<std::size_t>(r) * kSaveRecordSize;
        const auto id = ReadOr<std::uint32_t>(rec + kSaveRecordItem, 0);
        const int count = ReadOr<std::int32_t>(rec + kSaveRecordCount, 0);
        const int cell = ReadOr<std::int32_t>(rec + kSaveRecordCell, -1);
        if (id == 0 || cell < 0 || cell >= kGridCells || (id != re5::addr::kItemKnife && count <= 0 && ItemStackMax(id) > 0)) continue;
        g->transition.carry.emplace_back(id, count);
        g->transition.cells[static_cast<std::size_t>(cell)] = id;
        g->transition.counts[static_cast<std::size_t>(cell)] = count;
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %d:%04Xx%d", cell, id, count);
        text += buf;
        if (ItemStackMax(id) == 0) {  // a weapon: its levels as the screen left them
            WeaponLevels levels = SlotLevels(rec);
            // Out of the storage with none (a gun the stage change could not place goes there
            // without its levels): the run's levels for it stay, they never go down in a run.
            const auto known = RunLevels().find(id);
            if (levels == WeaponLevels{} && known != RunLevels().end()) levels = known->second;
            RunLevels()[id] = levels;
            std::snprintf(buf, sizeof(buf), "[%08X %08X]", levels[0], levels[1]);
            text += buf;
        }
    }
    RM_LOGI("itembox: the run's grid from the screen:%s", text.c_str());
    // The storage becomes the run's stash.
    g_runInBox.stash.clear();
    std::string stashed;
    for (std::size_t at = kSaveStorageFrom; at + 12 <= kSaveStorageTo; at += 12) {
        std::array<std::uint8_t, 12> record{};
        if (!SafeCopy(record.data(), save + at, record.size())) continue;
        const std::uint16_t id = static_cast<std::uint16_t>(record[0] | (record[1] << 8));
        if (id == 0) continue;
        g_runInBox.stash.push_back(record);
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %04X x%u @+%zX [%02X%02X%02X%02X %02X%02X%02X%02X]", id, static_cast<unsigned>(record[2] | (record[3] << 8)),
                      at, record[4], record[5], record[6], record[7], record[8], record[9], record[10], record[11]);
        stashed += buf;
    }
    // Items given to the partner ("Dar"): a solo run has none, so they go to the stash too.
    for (int r = 0; r < kSaveGridRecords; ++r) {
        const auto rec = save + kSaveGridSheva + static_cast<std::size_t>(r) * kSaveRecordSize;
        const auto id = ReadOr<std::uint32_t>(rec + kSaveRecordItem, 0);
        const int count = ReadOr<std::int32_t>(rec + kSaveRecordCount, 0);
        if (id == 0 || id == re5::addr::kItemKnife || ReadOr<std::int32_t>(rec + kSaveRecordCell, -1) >= kGridCells) continue;
        std::array<std::uint8_t, 12> record{};
        record[0] = static_cast<std::uint8_t>(id & 0xFF);
        record[1] = static_cast<std::uint8_t>(id >> 8);
        record[2] = static_cast<std::uint8_t>(count & 0xFF);
        record[3] = static_cast<std::uint8_t>((count >> 8) & 0xFF);
        g_runInBox.stash.push_back(record);
        char buf[40];
        std::snprintf(buf, sizeof(buf), " %04X x%d (from the partner)", id, count);
        stashed += buf;
    }
    RM_LOGI("itembox: stash (%zu):%s", g_runInBox.stash.size(), stashed.c_str());
    // A restocking gun that went in the screen was sold: given up for good, or it would come back
    // to be sold again every arena.
    for (const std::int64_t item : g_runInBox.restockBefore) {
        const std::uint32_t record = RecordItemId(static_cast<std::uint32_t>(item));
        const auto& cells = g->transition.cells;
        const bool kept = std::any_of(cells.begin(), cells.end(), [record](std::uint32_t id) { return id != 0 && RecordItemId(id) == record; }) ||
                          RunStashHolds(record);
        if (kept) continue;
        Report(g->run->DropWeapon(item, "sold"), "restocking gun sold");
        RM_LOGI("restock: %04X left in the screen (sold); it no longer comes back", static_cast<unsigned>(item));
    }
    g_runInBox.restockBefore.clear();
    // A sale pays what the run's shop says the item is worth (RunController::SaleValue, half its price),
    // never the game's gold: the game paid 11200 gold (112 supplies) for the run's free M92F, and more
    // for a gun than the shop had asked (the user, 2026-11-11: a friend's marathon bought every gun and
    // upgrade with supplies to spare). The pay is what the grid and the storage lost in worth, at most
    // what the game paid, so moving or combining items pays nothing. (Before the layout clears the cells.)
    std::vector<std::pair<std::uint32_t, int>> after;
    for (std::size_t c = 0; c < g->transition.cells.size(); ++c) {
        if (g->transition.cells[c] != 0) after.emplace_back(g->transition.cells[c], g->transition.counts[c]);
    }
    AppendStash(after);
    const std::int64_t worth = SaleWorth(after);
    // What the match cannot take (a gun whose model is not loaded here) goes to the stash.
    ApplyCarriedInventory();
    const auto gold = static_cast<std::int64_t>(ReadOr<std::uint32_t>(save + kSaveGold, 0));
    std::int64_t delta = 0;
    if (gold > g_runInBox.gold) {
        delta = std::clamp<std::int64_t>(g_runInBox.worth - worth, 0, (gold - g_runInBox.gold) / kGoldPerSupply);
    } else if (gold < g_runInBox.gold) {
        delta = gold / kGoldPerSupply - g->run->Snapshot(Now()).supplies;  // spent there (its Buy and Upgrade are off)
    }
    if (delta != 0) Report(g->run->AdjustSupplies(delta, "native_shop"), "native shop supplies");
    RM_LOGI("itembox: gold %lld -> %lld, worth %lld -> %lld: supplies %+lld", static_cast<long long>(g_runInBox.gold),
            static_cast<long long>(gold), static_cast<long long>(g_runInBox.worth), static_cast<long long>(worth),
            static_cast<long long>(delta));
    if (delta > 0) g->toasts.Push("Sold for " + std::to_string(delta) + (delta == 1 ? " supply" : " supplies") + " (half the shop's price)");
}

void OpenItemBox(const std::vector<std::pair<std::size_t, std::uint32_t>>& pokes = {}, bool withRun = false) {
    if (!engine::Caps().itemBox) {
        RM_LOGW("itembox: signatures missing");
    } else if (g_itemBox) {
        RM_LOGW("itembox: already open (%08X)", static_cast<unsigned>(g_itemBox));
    } else if (!resources::ArchiveLoaded("ShopResource")) {
        RM_LOGW("itembox: ShopResource is not loaded (dev uipreload ShopResource, then a new match)");
    } else {
        SnapshotSave();
        if (g_saveSnapshot.empty()) return;  // no screen that could change the save without its copy
        // Research: values written into the save data after the copy (so the restore undoes them).
        for (const auto& [offset, value] : pokes) {
            if (offset + 4 <= kSaveSnapshotBytes) SafeStore(g_saveSnapshotBase + offset, value);
            RM_LOGW("itembox: save +%zX = %u", offset, value);
        }
        if (withRun && g->run->HasActiveRun()) {
            PutRunInSave(g_saveSnapshotBase);
            // The first grid is the character's (its id is the short name the game uses: chris,
            // wesker), the second (a partner's) stays empty.
            std::string name = g->run->state().character_id;
            if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            if (!name.empty() && !nativemenu::SetItemBoxLabels(name, "")) RM_LOGW("itembox: grid labels unchanged");
        }
        if (!g->settings.developerMode || !CopySave(g_saveSnapshotBase, g_saveSwappedIn)) g_saveSwappedIn.clear();
        g_itemBox = CreateItemBox();
        RM_LOGW("itembox: created %08X", static_cast<unsigned>(g_itemBox));
        if (!g_itemBox) {  // no screen: the run leaves the save data at once
            g_runInBox.active = false;
            RestoreSave();
            nativemenu::RestoreItemBoxLabels();
        }
    }
}

// The SHOP's organize row opens the game's screen when it can here: its signatures, its archive
// loaded with this stage, and a run between arenas.
bool NativeItemBoxReady() {
    return g->settings.nativeItemBox && engine::Caps().itemBox && g->match.inMatch && g->run->phase() == rm::Phase::Shop &&
           !g_itemBox && resources::ArchiveLoaded("ShopResource");
}

// Every frame: once the screen reports it is done (obj+0x3C), it goes, as in phase 3.
void PollItemBoxNow();

// M10: the screen opens with, and gives back, the grid of the player whose shop asked for it.
void PollItemBox() {
    SeatTurn turn(g->itemBoxSeat);
    PollItemBoxNow();
}

void PollItemBoxNow() {
    static double openedAt = 0.0;
    static bool paused = false;
    if (g->itemBoxRequested && !matchui::IsOpen()) {
        g->itemBoxRequested = false;
        OpenItemBox({}, true);
        if (!g_itemBox) {
            g->shopUiRequested = true;  // it did not open: back to the shop
        } else {
            // The world stands still while it is open, as under the mod's own screens.
            matchui::SetWorldPause(true);
            paused = true;
            openedAt = Now();
        }
    }
    // With the run in the screen, Buy and Upgrade are the SHOP's and its guns have pictures (user
    // playtest 2026-11-03); set before the screen's first frame, cleared the tick after it goes.
    itemboxguard::SetGuard(g_itemBox != 0 && g_runInBox.active);
    if (!g_itemBox) return;
    // The world stays paused while it is up: coming back to the game window cleared the pause, and
    // the player walked about under the screen (the user, 2026-11-11: "if I tab out ... I can move").
    if (paused) matchui::HoldWorldPause("the Organize screen");
    // Never stuck in it: after 90 s the screen goes (the save data is restored all the same).
    if (paused && Now() - openedAt > 90.0 && ReadOr<std::uint8_t>(g_itemBox + 0x3C, 0) == 0) {
        RM_LOGE("itembox: no answer after 90 s; closing it");
        DeleteItemBox(g_itemBox);
        g_itemBox = 0;
        g_runInBox.active = false;
        RestoreSave();
        nativemenu::RestoreItemBoxLabels();
        matchui::SetWorldPause(false);
        paused = false;
        if (g->run->phase() == rm::Phase::Shop) g->shopUiRequested = true;
        return;
    }
    if (!g->match.inMatch) {
        g_itemBox = 0;  // gone with the match
        g_runInBox.active = false;
        RestoreSave();
        nativemenu::RestoreItemBoxLabels();
        return;
    }
    // Once, while it is open: does the block the screen edits hold the run's grid?
    if (g_runInBox.active && !g_runInBox.checked && g_saveSnapshotBase && ReadOr<std::uint8_t>(g_itemBox + 0x3C, 0) == 0) {
        static int frames = 0;
        if (++frames >= 30) {
            frames = 0;
            bool same = true;
            for (int c = 0; c < kGridCells && same; ++c) {
                const auto rec = g_saveSnapshotBase + kSaveGridChris + static_cast<std::size_t>(c) * kSaveRecordSize;
                same = ReadOr<std::uint32_t>(rec + kSaveRecordItem, 0) == g_runInBox.cells[static_cast<std::size_t>(c)];
            }
            g_runInBox.checked = same;
            RM_LOGW("itembox: the screen shows %s", same ? "the run's grid" : "another grid (not the run's)");
            if (!same) g_runInBox.active = false;
        }
    }
    if (ReadOr<std::uint8_t>(g_itemBox + 0x3C, 0) == 0) return;
    RM_LOGW("itembox: closed (+0x3E %u, +0x5B6C %u, +0x5B6D %u); deleted: %s", ReadOr<std::uint8_t>(g_itemBox + 0x3E, 0),
            ReadOr<std::uint8_t>(g_itemBox + 0x5B6C, 0), ReadOr<std::uint8_t>(g_itemBox + 0x5B6D, 0),
            DeleteItemBox(g_itemBox) ? "yes" : "no");
    g_itemBox = 0;
    if (std::vector<char> now; !g_saveSwappedIn.empty() && CopySave(g_saveSnapshotBase, now)) {
        LogSaveDiff("the screen's own changes", g_saveSwappedIn, now);
    }
    TakeRunFromSave(g_saveSnapshotBase);
    RestoreSave();
    nativemenu::RestoreItemBoxLabels();
    // Unpaused here, before the SHOP screen (opened after this tick) asks for its own pause.
    if (paused) {
        matchui::SetWorldPause(false);
        paused = false;
    }
    if (g->run->phase() == rm::Phase::Shop) g->shopUiRequested = true;
}

// E16 research (developer commands, research/re5-engine-map.md "Twin guns"): "twinguns" puts the
// boss Jill's two SMGs in the player's hands (ArmTwinGuns), "twinguns off" deletes them; "twinfire
// [n] [at] [attack=<hex>]" fires each n times along her facing or at the nearest enemy and follows
// the shots, "twinstream <s> [attack=<hex>]" fires both every 0.1 s for a while.
std::vector<std::uintptr_t> g_twinShots;
double g_twinTraceUntil = 0.0;

void TwinGunsCommand(const std::string& command) {
    const auto local = engine::LocalPlayer();
    if (command == "twinguns off") {
        DisarmTwinGuns("dev");
        return;
    }
    if (!engine::Caps().twinGuns || !local) {
        RM_LOGW("dev twinguns: %s", local ? "signatures missing" : "no player");
        return;
    }
    if (command == "twinguns") {
        RM_LOGW("dev twinguns: %s", ArmTwinGuns(local, "dev") ? "in her hands" : "not made (see the line above)");
        return;
    }
    // "twinfire [n] [at]": from in front of her hands along her facing, or with "at" from 0.8 m in
    // front of the nearest enemy at it (does a shot hurt one at all?); the hit log names who scored.
    int shots = 1;
    std::sscanf(command.c_str() + 8, "%d", &shots);
    shots = std::clamp(shots, 1, 20);
    const bool at = command.find(" at") != std::string::npos;
    float fx = 0.0f, fz = 1.0f;
    PlayerFacing(local, fx, fz);
    float base[3] = {ReadOr<float>(local + re5::off::kUnitPosition, 0.0f), ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                     ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f)};
    float ahead = 120.0f;
    if (at) {
        std::uintptr_t nearest = 0;
        float best = 2500.0f;
        for (const engine::EnemyState& e : engine::ReadEnemyStates()) {
            if (e.hp <= 0) continue;
            const float ex = ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f) - base[0];
            const float ez = ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f) - base[2];
            const float d = std::sqrt(ex * ex + ez * ez);
            if (d < best && d > 1.0f) {
                best = d;
                nearest = e.object;
                fx = ex / d;
                fz = ez / d;
            }
        }
        if (!nearest) {
            RM_LOGW("dev twinfire: no enemy within 25 m");
            return;
        }
        ahead = best - 80.0f;
        RM_LOGW("dev twinfire: at %s %08X, %.1f m away (hp %d)", engine::ClassName(nearest).c_str(), static_cast<unsigned>(nearest),
                best / 100.0f, engine::ReadHp(nearest) ? engine::ReadHp(nearest)->hp : -1);
    }
    const float dir[3] = {fx, 0.0f, fz};
    world::WatchCreated("uWp86");
    int fired = 0;
    for (int n = 0; n < 2; ++n) {
        if (!g_twinGuns[n]) continue;
        const float side = n == 0 ? 25.0f : -25.0f;  // right and left of her
        const float muzzle[3] = {base[0] + fx * ahead + fz * side, base[1] + 130.0f, base[2] + fz * ahead - fx * side};
        const std::uintptr_t gun = g_twinGuns[n];
        RM_LOGW("dev twinfire: gun %d at %.0f %.0f %.0f; +0x90 %.2f %.2f %.2f +0xA0 %.2f %.2f %.2f +0xB0 %.2f %.2f %.2f +0xC0 %.2f %.2f %.2f",
                n, ReadOr<float>(gun + 0x30, 0.0f), ReadOr<float>(gun + 0x34, 0.0f), ReadOr<float>(gun + 0x38, 0.0f),
                ReadOr<float>(gun + 0x90, 0.0f), ReadOr<float>(gun + 0x94, 0.0f), ReadOr<float>(gun + 0x98, 0.0f),
                ReadOr<float>(gun + 0xA0, 0.0f), ReadOr<float>(gun + 0xA4, 0.0f), ReadOr<float>(gun + 0xA8, 0.0f),
                ReadOr<float>(gun + 0xB0, 0.0f), ReadOr<float>(gun + 0xB4, 0.0f), ReadOr<float>(gun + 0xB8, 0.0f),
                ReadOr<float>(gun + 0xC0, 0.0f), ReadOr<float>(gun + 0xC4, 0.0f), ReadOr<float>(gun + 0xC8, 0.0f));
        for (int s = 0; s < shots; ++s) fired += engine::FireTwinGun(gun, muzzle, dir) ? 1 : 0;
    }
    // The shots made (the uWp86 units sItem created meanwhile) are followed for a moment.
    g_twinShots = world::TakeCreated();
    world::WatchCreated("");
    // "attack=<hex>": the shots' attack id (+0x1CA0, read every frame by the shell's update
    // 0x00BB9570; the boss's gun gives 0x190003, the shell factories 0x10018, 0x10021, 0x16000E ...).
    if (const auto pos = command.find("attack="); pos != std::string::npos) {
        const auto attack = static_cast<std::uint32_t>(std::strtoul(command.c_str() + pos + 7, nullptr, 16));
        for (const std::uintptr_t s : g_twinShots) SafeStore<std::uint32_t>(s + 0x1CA0, attack);
        RM_LOGW("dev twinfire: the shots' attack id set to %X", attack);
    }
    g_twinTraceUntil = Now() + 1.5;
    RM_LOGW("dev twinfire %d: %d shots fired (guns %08X %08X) facing %.2f %.2f; %zu shot units found", shots, fired,
            static_cast<unsigned>(g_twinGuns[0]), static_cast<unsigned>(g_twinGuns[1]), fx, fz, g_twinShots.size());
}

// "twinstream <seconds> [attack=<hex>]": both guns fire every 0.1 s along her facing for that long
// (to see the shots: flashes, tracers), the shots' attack id set (FFFFFFFF: none, the shell's
// update skips its attack, so they hurt nobody).
double g_twinStreamUntil = 0.0;
double g_twinStreamNext = 0.0;
std::uint32_t g_twinStreamAttack = 0xFFFFFFFFu;
int g_twinStreamShots = 0;

void StreamTwinShots(double now) {
    const auto local = engine::LocalPlayer();
    if (now > g_twinStreamUntil || now < g_twinStreamNext || !local) {
        if (g_twinStreamShots > 0 && now > g_twinStreamUntil) {
            RM_LOGW("dev twinstream: %d shots fired, attack %X", g_twinStreamShots, g_twinStreamAttack);
            g_twinStreamShots = 0;
        }
        return;
    }
    g_twinStreamNext = now + 0.1;
    float fx = 0.0f, fz = 1.0f;
    PlayerFacing(local, fx, fz);
    const float dir[3] = {fx, 0.0f, fz};
    world::WatchCreated("uWp86");
    for (int n = 0; n < 2; ++n) {
        if (!g_twinGuns[n]) continue;
        const float side = n == 0 ? 25.0f : -25.0f;
        const float muzzle[3] = {ReadOr<float>(local + re5::off::kUnitPosition, 0.0f) + fx * 60.0f + fz * side,
                                 ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f) + 130.0f,
                                 ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f) + fz * 60.0f - fx * side};
        g_twinStreamShots += engine::FireTwinGun(g_twinGuns[n], muzzle, dir) ? 1 : 0;
    }
    for (const std::uintptr_t s : world::TakeCreated()) SafeStore<std::uint32_t>(s + 0x1CA0, g_twinStreamAttack);
    world::WatchCreated("");
}

// Each tick after a twinfire: where its shots are, how they move (velocity in their own space at
// +0x1CE0, in the world at +0x1CF0, their time factor +0xFD4) and their attack ids (+0x1CA0/4).
void TraceTwinShots(double now) {
    StreamTwinShots(now);
    static double nextAt = 0.0;
    if (g_twinShots.empty() || now > g_twinTraceUntil || now < nextAt) return;
    nextAt = now + 0.15;
    for (std::size_t i = 0; i < g_twinShots.size() && i < 4; ++i) {
        const std::uintptr_t s = g_twinShots[i];
        if (engine::ClassName(s) != "uWp86") {
            RM_LOGW("twin shot %zu %08X: gone", i, static_cast<unsigned>(s));
            continue;
        }
        RM_LOGW("twin shot %zu %08X at %.0f %.0f %.0f; local v %.1f %.1f %.1f, world v %.1f %.1f %.1f, time %.2f, attack %X %X, owner %08X",
                i, static_cast<unsigned>(s), ReadOr<float>(s + 0x30, 0.0f), ReadOr<float>(s + 0x34, 0.0f), ReadOr<float>(s + 0x38, 0.0f),
                ReadOr<float>(s + 0x1CE0, 0.0f), ReadOr<float>(s + 0x1CE4, 0.0f), ReadOr<float>(s + 0x1CE8, 0.0f),
                ReadOr<float>(s + 0x1CF0, 0.0f), ReadOr<float>(s + 0x1CF4, 0.0f), ReadOr<float>(s + 0x1CF8, 0.0f),
                ReadOr<float>(s + 0xFD4, 0.0f), ReadOr<std::uint32_t>(s + 0x1CA0, 0), ReadOr<std::uint32_t>(s + 0x1CA4, 0),
                ReadOr<std::uint32_t>(s + 0xFDC, 0));
    }
}

// Developer mode: where the mod's time goes, by section (logged every 10 s, the largest first).
struct ProfileAccum {
    double us = 0.0;
    long calls = 0;
};
std::map<std::string, ProfileAccum> g_profile;
double g_profileLoggedAt = 0.0;

class ProfileScope {
public:
    explicit ProfileScope(const char* name) : name_(g->settings.developerMode ? name : nullptr) {
        if (name_) QueryPerformanceCounter(&start_);
    }
    ~ProfileScope() {
        if (!name_) return;
        static const double usPerTick = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return 1.0e6 / static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        ProfileAccum& a = g_profile[name_];
        a.us += static_cast<double>(end.QuadPart - start_.QuadPart) * usPerTick;
        ++a.calls;
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    const char* name_;
    LARGE_INTEGER start_{};
};

void LogProfile(double now) {
    if (!g->settings.developerMode || now - g_profileLoggedAt < 10.0) return;
    const double span = now - g_profileLoggedAt;
    g_profileLoggedAt = now;
    std::vector<std::pair<std::string, ProfileAccum>> rows(g_profile.begin(), g_profile.end());
    g_profile.clear();
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
    std::string line;
    for (std::size_t i = 0; i < rows.size() && i < 10; ++i) {
        char part[96];
        std::snprintf(part, sizeof(part), " | %s %.0f us/s (%.1f us x %ld)", rows[i].first.c_str(), rows[i].second.us / span,
                      rows[i].second.us / std::max(1L, rows[i].second.calls), rows[i].second.calls);
        line += part;
    }
    if (!line.empty()) RM_LOGI("profile:%s", line.c_str());
}

// Developer automation: <data>\dev.command holds one command per file drop.
// Only in developer mode; every command is logged.
void PollDevCommand(double now) {
    static double lastCheck = 0.0;
    if (!g->settings.developerMode || now - lastCheck < 0.5) return;
    lastCheck = now;
    const auto file = g->paths.dataDir / L"dev.command";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return;
    std::string command;
    {
        std::ifstream in(file);
        std::getline(in, command);
    }
    std::filesystem::remove(file, ec);
    rm::RunController& run = *g->run;
    // "seat <n> <command>": the command as seat n's player (M10: their run seat, unit and the
    // adapter's state for them, as in their SeatTurn).
    std::optional<SeatTurn> seatTurn;
    if (command.rfind("seat ", 0) == 0 && command.size() > 7 && run.HasActiveRun()) {
        const int seat = command[5] - '0';
        command = command.substr(7);
        seatTurn.emplace(seat);
        RM_LOGW("dev: as seat %d: %s", seat, command.c_str());
    }
    if (command == "force_complete" && run.phase() == rm::Phase::ArenaActive) {
        const int remaining = run.RemainingEnemies();
        for (int i = 0; i < remaining; ++i) {
            rm::KillEvent kill;
            kill.now = now;
            kill.by_player = true;
            kill.enemy_id = "devcmd_" + std::to_string(i) + "_" + std::to_string(static_cast<long long>(now * 1000));
            run.OnEnemyKilled(kill);
        }
        RM_LOGW("dev command force_complete: %d synthetic kills", remaining);
    } else if (command.rfind("poke ", 0) == 0 || command.rfind("peek ", 0) == 0) {
        // "poke <hex address> <hex dword>" / "peek <hex address>": raw dword access for
        // reverse-engineering experiments (developer mode only, logged).
        std::istringstream words(command.substr(5));
        std::string addressText, valueText;
        words >> addressText >> valueText;
        const auto address = static_cast<std::uintptr_t>(std::strtoul(addressText.c_str(), nullptr, 16));
        if (command[1] == 'o' && !valueText.empty()) {
            const auto value = static_cast<std::uint32_t>(std::strtoul(valueText.c_str(), nullptr, 16));
            RM_LOGW("dev poke %08X = %08X -> %s", static_cast<unsigned>(address), value,
                    SafeStore(address, value) ? "ok" : "failed");
        } else {
            std::uint32_t value = 0;
            const bool ok = SafeRead(address, value);
            RM_LOGW("dev peek %08X = %08X (%s)", static_cast<unsigned>(address), value, ok ? "ok" : "unreadable");
        }
    } else if (command.rfind("dump ", 0) == 0) {
        // "dump <hex address> <dword count>": logs dwords and the object's DTI class.
        std::istringstream words(command.substr(5));
        std::string addressText;
        int count = 16;
        words >> addressText >> count;
        const auto address = static_cast<std::uintptr_t>(std::strtoul(addressText.c_str(), nullptr, 16));
        RM_LOGW("dev dump %08X class=%s", static_cast<unsigned>(address), engine::ClassName(address).c_str());
        for (int i = 0; i < count && i < 256; i += 4) {
            std::uint32_t v[4]{};
            for (int k = 0; k < 4; ++k) SafeRead(address + (i + k) * 4, v[k]);
            RM_LOGW("  +%03X: %08X %08X %08X %08X", i * 4, v[0], v[1], v[2], v[3]);
        }
    } else if (command.rfind("find ", 0) == 0) {
        // "find <hex bytes>": scans committed readable memory for a byte pattern.
        std::vector<unsigned char> pattern;
        std::istringstream words(command.substr(5));
        std::string byte;
        while (words >> byte) pattern.push_back(static_cast<unsigned char>(std::strtoul(byte.c_str(), nullptr, 16)));
        int hits = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (std::uintptr_t a = 0x10000; !pattern.empty() && a < 0x7FFE0000 && hits < 24; a += mbi.RegionSize) {
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_WRITECOPY;
            if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD)) continue;
            // Fixed-size chunks (overlapping by the pattern length): regions can be
            // hundreds of MB, more than a 32-bit process can allocate at once.
            constexpr std::size_t kChunk = 1 << 20;
            static std::vector<unsigned char> buffer(kChunk + 256);
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            for (std::size_t offset = 0; offset < mbi.RegionSize && hits < 24; offset += kChunk) {
                const std::size_t size = std::min<std::size_t>(mbi.RegionSize - offset, kChunk + pattern.size() - 1);
                if (size < pattern.size() || !SafeCopy(buffer.data(), base + offset, size)) continue;
                for (std::size_t i = 0; i + pattern.size() <= size && hits < 24; ++i) {
                    if (buffer[i] == pattern[0] && std::memcmp(buffer.data() + i, pattern.data(), pattern.size()) == 0) {
                        RM_LOGW("dev find hit %08X", static_cast<unsigned>(base + offset + i));
                        ++hits;
                    }
                }
            }
        }
        RM_LOGW("dev find: %d hits for %zu bytes", hits, pattern.size());
    } else if (command.rfind("patch_ret ", 0) == 0 || command.rfind("unpatch ", 0) == 0) {
        // "patch_ret <hex addr>" makes a function return at once (its first byte
        // is kept and put back by "unpatch <hex addr>"): finds which draw routine
        // paints what. Developer mode only, logged.
        // "patch_ret <addr> [bytes]" writes `ret <bytes>` (3 bytes; plain ret with 0).
        static std::vector<std::pair<std::uintptr_t, std::array<unsigned char, 3>>> saved;
        const bool patch = command[0] == 'p';
        std::istringstream words(command.substr(patch ? 10 : 8));
        std::string addressText;
        unsigned popBytes = 0;
        words >> addressText >> popBytes;
        const auto address = static_cast<std::uintptr_t>(std::strtoul(addressText.c_str(), nullptr, 16));
        auto* p = reinterpret_cast<unsigned char*>(address);
        DWORD old = 0;
        if (address < re5::kImageBase || !VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old)) {
            RM_LOGW("dev %s %08X: not writable", patch ? "patch_ret" : "unpatch", static_cast<unsigned>(address));
        } else {
            if (patch) {
                saved.emplace_back(address, std::array<unsigned char, 3>{p[0], p[1], p[2]});
                if (popBytes == 0) {
                    p[0] = 0xC3;
                } else {
                    p[0] = 0xC2;
                    p[1] = static_cast<unsigned char>(popBytes & 0xFF);
                    p[2] = static_cast<unsigned char>(popBytes >> 8);
                }
            } else {
                for (auto it = saved.begin(); it != saved.end(); ++it) {
                    if (it->first != address) continue;
                    std::memcpy(p, it->second.data(), 3);
                    saved.erase(it);
                    break;
                }
            }
            VirtualProtect(p, 3, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 3);
            RM_LOGW("dev %s %08X -> %02X %02X %02X", patch ? "patch_ret" : "unpatch", static_cast<unsigned>(address), p[0], p[1], p[2]);
        }
    } else if (command.rfind("snap ", 0) == 0 || command == "diff") {
        // "snap <hex addr|gw> <bytes>" copies a block (gw = game work); "diff" logs
        // the dwords that changed since, then keeps the new values. Finds the
        // fields a menu screen writes when the player makes a choice.
        static std::uintptr_t snapAddress = 0;
        static std::vector<std::uint32_t> snapData;
        if (command[0] == 's') {
            std::istringstream words(command.substr(5));
            std::string addressText, sizeText;
            words >> addressText >> sizeText;
            std::uintptr_t address = 0;
            if (addressText == "gw") {
                address = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
            } else {
                address = static_cast<std::uintptr_t>(std::strtoul(addressText.c_str(), nullptr, 16));
            }
            const auto bytes = std::min<std::size_t>(std::strtoul(sizeText.c_str(), nullptr, 0), 1 << 20);
            snapData.assign(bytes / 4, 0);
            snapAddress = address;
            const bool ok = address && !snapData.empty() && SafeCopy(snapData.data(), address, snapData.size() * 4);
            if (!ok) snapData.clear();
            RM_LOGW("dev snap %08X %zu bytes -> %s", static_cast<unsigned>(address), bytes, ok ? "ok" : "failed");
        } else {
            std::vector<std::uint32_t> current(snapData.size());
            if (snapData.empty() || !SafeCopy(current.data(), snapAddress, current.size() * 4)) {
                RM_LOGW("dev diff: no snapshot or unreadable");
            } else {
                int changes = 0;
                for (std::size_t i = 0; i < current.size(); ++i) {
                    if (current[i] == snapData[i]) continue;
                    if (changes < 120) {
                        RM_LOGW("dev diff +%05X: %08X -> %08X", static_cast<unsigned>(i * 4), snapData[i], current[i]);
                    }
                    ++changes;
                }
                RM_LOGW("dev diff %08X: %d dwords changed", static_cast<unsigned>(snapAddress), changes);
                snapData = std::move(current);
            }
        }
    } else if (command.rfind("scene ", 0) == 0) {
        // "scene <hex id> [hex arg]": a scene request through the scene manager, as the
        // title-flow script makes it (0xB = Mercenaries menu, 0xD = Mercenaries match).
        std::istringstream words(command.substr(6));
        std::string idText, argText;
        words >> idText >> argText;
        const auto scene = ReadOr<std::uintptr_t>(re5::addr::kSceneManager, 0);
        const auto id = static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 16));
        const auto arg = static_cast<std::uint32_t>(std::strtoul(argText.c_str(), nullptr, 16));
        const bool ok = scene && SafeStore(scene + re5::off::kSceneRequest, id) &&
                        SafeStore(scene + re5::off::kSceneRequestArg, arg);
        RM_LOGW("dev scene request %X arg %X -> %s", id, arg, ok ? "written" : "failed");
    } else if (command.rfind("pause ", 0) == 0) {
        matchui::SetWorldPause(command[6] == '1');
    } else if (command.rfind("buy ", 0) == 0 && run.phase() == rm::Phase::Shop) {
        // "buy <kind>": purchases the first shop offer of that kind (heal, reroll, upgrade, weapon, item).
        const std::string kind = command.substr(4);
        const rm::RunSnapshot s = run.Snapshot(now);
        bool found = false;
        for (std::size_t i = 0; i < s.shop_offers.size() && !found; ++i) {
            if (s.shop_offers[i].kind != kind || s.shop_offers[i].purchased) continue;
            found = true;
            const rm::Status st = run.Purchase(i);
            RM_LOGW("dev buy %s (%s) -> %s", kind.c_str(), s.shop_offers[i].title.c_str(), st.ok() ? "ok" : st.message.c_str());
        }
        if (!found) RM_LOGW("dev buy %s: no such offer", kind.c_str());
    } else if (command.rfind("placechest", 0) == 0) {
        // "placechest [distance] [seconds]": a time-bonus statue (chest) in front of the player.
        float distance = 250.0f;
        int seconds = 30;
        std::istringstream(command.size() > 10 ? command.substr(11) : std::string()) >> distance >> seconds;
        const auto player = engine::LocalPlayer();
        if (player) {
            const float qx = ReadOr<float>(player + re5::off::kUnitRotation, 0.0f), qy = ReadOr<float>(player + re5::off::kUnitRotation + 4, 0.0f);
            const float qz = ReadOr<float>(player + re5::off::kUnitRotation + 8, 0.0f), qw = ReadOr<float>(player + re5::off::kUnitRotation + 12, 1.0f);
            // +Z forward rotated by the player's quaternion.
            const float fx = 2.0f * (qx * qz + qw * qy), fz = 1.0f - 2.0f * (qx * qx + qy * qy);
            const float len = std::max(0.001f, std::sqrt(fx * fx + fz * fz));
            const float pos[3] = {ReadOr<float>(player + re5::off::kUnitPosition, 0.0f) + fx / len * distance,
                                  ReadOr<float>(player + re5::off::kUnitPosition + 4, 0.0f),
                                  ReadOr<float>(player + re5::off::kUnitPosition + 8, 0.0f) + fz / len * distance};
            world::PlaceChest(pos, seconds);
        }
    } else if (command == "setdump") {
        // Every set record of the stage, and where the player stands.
        if (const auto local = engine::LocalPlayer()) {
            RM_LOGW("setdump: player at %.1f %.1f %.1f", ReadOr<float>(local + re5::off::kUnitPosition, 0.0f),
                    ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f), ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f));
        }
        world::DumpSetRecords();
    } else if (command == "fataltest") {
        // The game's own fatal error handler (shows its message box and exits the game).
        using FatalFn = void(__cdecl*)(void* context, const char* format, ...);
        RM_LOGW("dev fataltest: calling the game's fatal error handler");
        reinterpret_cast<FatalFn>(re5::addr::kFnGameFatalError)(reinterpret_cast<void*>(0x011AF634), "dev fataltest %d", 42);
    } else if (command.rfind("nextmap ", 0) == 0) {
        // "nextmap <key>": the run's next map pick (Mercenaries index 0..7 or a campaign stage number).
        g_forcedNextMap = std::atoi(command.c_str() + 8);
        RM_LOGW("dev next map %d (rotation of %zu maps)", g_forcedNextMap, RotationMaps().size());
    } else if (command.rfind("stage ", 0) == 0) {
        // "stage <n>": the next Mercenaries matches load stage number n (campaign s1xx..s6xx,
        // Mercenaries 400..407); "stage -1" goes back to the chosen Mercenaries stage.
        host::SetStageOverride(std::atoi(command.c_str() + 6));
        RM_LOGW("dev stage override %d (saved stage now %d)", host::StageOverride(), host::SavedStage());
    } else if (command.rfind("cell ", 0) == 0) {
        // "cell <from> <to>": moves an inventory grid cell (1..9) like ORGANIZE does.
        int from = 0, to = 0;
        std::istringstream(command.substr(5)) >> from >> to;
        RM_LOGW("dev cell %d -> %d: %s", from, to, SwapGridCells(from - 1, to - 1) ? "moved" : "nothing moved");
    } else if (command.rfind("supplies ", 0) == 0) {
        // "supplies <n>": credits n supplies to the active run.
        Report(run.DevGrantSupplies(std::atoll(command.c_str() + 9)), "dev supplies");
    } else if (command == "chest" && run.phase() == rm::Phase::ArenaActive) {
        SimulateTimeBonusPickup();  // goes through the same path as a real pickup
        RM_LOGW("dev chest: pickup simulated");
    } else if (command.rfind("layoutclasses", 0) == 0) {
        // "layoutclasses uOm08f0 ...": the filed campaign layouts also build these classes (next loads).
        std::istringstream words(command.substr(13));
        std::vector<std::string> names;
        for (std::string w; words >> w;) names.push_back(w);
        world::SetLayoutClasses(names);
        RM_LOGW("dev layoutclasses: %zu classes (next loads)", names.size());
    } else if (command.rfind("hidemodels", 0) == 0) {
        // "hidemodels om020eL om020f ...": set records named so are not built on the next loads
        // ("hidemodels map" or "hidemodels" alone: none). Research only: a model's collision can
        // stay in the stage (the Spencer Mansion's doors still block with their models gone).
        std::istringstream words(command.substr(10));
        std::vector<std::string> names;
        for (std::string w; words >> w;) names.push_back(w);
        if (names.size() == 1 && names[0] == "map") names.clear();
        if (names.empty()) {
            g_devHiddenModels.reset();
        } else {
            g_devHiddenModels = names;
        }
        world::SetHiddenModels(names);
        RM_LOGW("dev hidemodels: %zu names (next loads)", names.size());
    } else if (command.rfind("spawnat ", 0) == 0) {
        // "spawnat <x> <y> <z> <class> [weapon hex]": one unit at a map position (does a spawn point
        // reach the player?).
        std::istringstream words(command.substr(8));
        float at[3] = {};
        std::string cls, weaponText;
        words >> at[0] >> at[1] >> at[2] >> cls >> weaponText;
        if (cls.empty() || (resources::Available() && !resources::ArchiveLoaded(cls))) {
            RM_LOGW("dev spawnat: no class, or %s is not loaded on this stage", cls.c_str());
            return;
        }
        director::SpawnSpec spec;
        spec.unitClass = cls;
        std::memcpy(spec.pos, at, sizeof(spec.pos));
        spec.weapon = weaponText.empty() ? -1 : static_cast<int>(std::strtoul(weaponText.c_str(), nullptr, 16));
        const std::uintptr_t unit = director::Spawn(spec);
        RM_LOGW("dev spawnat %s at %.0f %.0f %.0f -> %08X", cls.c_str(), at[0], at[1], at[2], static_cast<unsigned>(unit));
    } else if (command.rfind("spawn ", 0) == 0) {
        // "spawn <class> [weapon hex] [count] [distance] [field=value ...]": e.g. "spawn uEm10 70100 3"
        // next to the player; the fields are a map profile's (maps.json "fields": "attribute=32768").
        std::istringstream words(command.substr(6));
        std::string cls, weaponText;
        int count = 1;
        float distance = 150.0f;
        words >> cls >> weaponText >> count >> distance;
        std::vector<std::pair<std::string, double>> fields;
        for (std::string token; words >> token;) {
            const auto eq = token.find('=');
            if (eq != std::string::npos && eq > 0) fields.emplace_back(token.substr(0, eq), std::atof(token.c_str() + eq + 1));
        }
        const int weapon = weaponText.empty() ? -1 : static_cast<int>(std::strtoul(weaponText.c_str(), nullptr, 16));
        // A class whose archive this stage never loaded makes the game look for its model on disk
        // and stop with "Failed open file ... emXXXX_low.mod" (as the director, never spawn it).
        if (resources::Available() && !resources::ArchiveLoaded(cls)) {
            RM_LOGW("dev spawn %s refused: its archive is not loaded on this stage (dev preload, then a new stage)", cls.c_str());
            return;
        }
        const auto player = engine::LocalPlayer();
        const float pos[3] = {ReadOr<float>(player + re5::off::kUnitPosition, 0.0f), ReadOr<float>(player + re5::off::kUnitPosition + 4, 0.0f),
                              ReadOr<float>(player + re5::off::kUnitPosition + 8, 0.0f)};
        // In front of the player (uCoord quaternion, +Z forward); along +X when the rotation is unreadable.
        const float qx = ReadOr<float>(player + re5::off::kUnitRotation, 0.0f), qy = ReadOr<float>(player + re5::off::kUnitRotation + 4, 0.0f);
        const float qz = ReadOr<float>(player + re5::off::kUnitRotation + 8, 0.0f), qw = ReadOr<float>(player + re5::off::kUnitRotation + 12, 0.0f);
        const float norm = qx * qx + qy * qy + qz * qz + qw * qw;
        float fx = 1.0f, fz = 0.0f;
        if (norm > 0.9f && norm < 1.1f) {
            fx = 2.0f * (qx * qz + qw * qy);
            fz = 1.0f - 2.0f * (qx * qx + qy * qy);
            const float len = std::sqrt(fx * fx + fz * fz);
            if (len > 0.01f) {
                fx /= len;
                fz /= len;
            }
        }
        RM_LOGW("dev spawn: player rotation %.3f %.3f %.3f %.3f (norm %.3f) -> forward %.2f %.2f", qx, qy, qz, qw, norm, fx, fz);
        for (int i = 0; i < std::clamp(count, 1, 20); ++i) {
            const float side = 80.0f * static_cast<float>(i);  // further units step sideways
            const float spawnAt[3] = {pos[0] + fx * distance - fz * side, pos[1], pos[2] + fz * distance + fx * side};
            director::SpawnSpec spec;
            spec.unitClass = cls;
            std::memcpy(spec.pos, spawnAt, sizeof(spec.pos));
            spec.facing = std::atan2(-fx, -fz);  // facing the player
            spec.weapon = weapon;
            spec.fields = fields;
            const std::uintptr_t unit = player ? director::Spawn(spec) : 0;
            RM_LOGW("dev spawn %s (weapon %X) at %.0f %.0f %.0f -> %08X %s", cls.c_str(), weapon, spawnAt[0], spawnAt[1], spawnAt[2],
                    static_cast<unsigned>(unit), unit ? engine::ClassName(unit).c_str() : "failed");
        }
    } else if (command == "special" || command.rfind("special ", 0) == 0) {
        // "special [slot]": the build's special without any key or button (input ruled out).
        int slot = 0;
        std::sscanf(command.c_str() + 7, "%d", &slot);
        StartSpecial(now, std::clamp(slot, 0, 1), "dev");
        RM_LOGW("dev special %d: asked", slot);
    } else if (command == "twinguns special off" || command == "twinguns special on") {
        g_twinGunsInSpecial = command == "twinguns special on";
        RM_LOGW("dev twinguns: Twin Fury %s them", g_twinGunsInSpecial ? "uses" : "does without");
    } else if (command == "twinguns" || command == "twinguns off" || command.rfind("twinfire", 0) == 0) {
        TwinGunsCommand(command);
    } else if (command.rfind("twinstream", 0) == 0) {
        double seconds = 3.0;
        std::sscanf(command.c_str() + 10, "%lf", &seconds);
        g_twinStreamAttack = 0xFFFFFFFFu;
        if (const auto pos = command.find("attack="); pos != std::string::npos) {
            g_twinStreamAttack = static_cast<std::uint32_t>(std::strtoul(command.c_str() + pos + 7, nullptr, 16));
        }
        g_twinStreamUntil = now + std::clamp(seconds, 0.5, 10.0);
        g_twinStreamNext = now;
        RM_LOGW("dev twinstream %.1f s, attack %X", seconds, g_twinStreamAttack);
    } else if (command.rfind("partnerhp ", 0) == 0) {
        // "partnerhp <hp>": the partner's health (what does her death do to a run's match?).
        const int hp = std::atoi(command.c_str() + 10);
        const bool ok = g_partner.unit && SafeStore<std::int16_t>(g_partner.unit + re5::off::kHp, static_cast<std::int16_t>(hp));
        RM_LOGW("dev partnerhp %d: %s", hp, ok ? "ok" : "no partner");
    } else if (command == "partner" || command.rfind("partner ", 0) == 0) {
        // "partner <character> <costume>": a computer player beside the local one (E22 research).
        int character = 0, costume = 0;
        std::sscanf(command.c_str() + 7, "%d %d", &character, &costume);
        const std::uintptr_t unit = AddPartner(character, costume, "dev");
        RM_LOGW("dev partner %d %d -> %08X", character, costume, static_cast<unsigned>(unit));
    } else if (command.rfind("attachprobe ", 0) == 0) {
        // "attachprobe on|off": the joint and parent changes of what is attached to the local player.
        const bool on = command.substr(12) == "on";
        const bool ok = SetAttachProbe(on ? engine::LocalPlayer() : 0);
        RM_LOGW("dev attachprobe %s%s", on ? "on" : "off", ok ? "" : " (no hook: developer mode and the signature)");
    } else if (command.rfind("hitprobe ", 0) == 0) {
        // "hitprobe <class>|off": the enemy hit check's calls for the first enemy of the class
        // with a hit pending, logged with what it reads and answers.
        const std::string cls = command.substr(9);
        const std::uintptr_t unit = cls == "off" ? 0 : FirstEnemyOf(cls);
        SetHitProbe(unit);
        RM_LOGW("dev hitprobe %s -> %08X%s", cls.c_str(), static_cast<unsigned>(unit), engine::Caps().hitCheckProbe ? "" : " (no probe: signature)");
    } else if (command.rfind("coopwatch ", 0) == 0) {
        g_coopWatch = command.substr(10) == "on";
        g_coopGw.clear();
        g_coopPlayersLast.clear();
        g_coopViewsLast.clear();
        g_coopPadsLast.clear();
        g_coopNext = 0.0;
        RM_LOGW("dev coopwatch %s", g_coopWatch ? "on" : "off");
    } else if (command.rfind("pad ", 0) == 0) {
        // "pad <user> on|off", "pad <user> <A+B..> [ms]" (LT/RT pull a trigger), "pad <user> stick|rstick <x> <y> [ms]".
        std::istringstream words(command.substr(4));
        int user = -1;
        std::string what;
        words >> user >> what;
        if (user < 0 || user > 3) {
            RM_LOGW("dev pad: user 0..3");
        } else if (what == "on" || what == "off") {
            VirtualPad v;
            v.on = what == "on";
            SetVirtualPad(user, v);
            g_padRelease[user] = 0.0;
            RM_LOGW("dev pad %d %s", user, what.c_str());
        } else if (what == "stick" || what == "rstick") {
            int x = 0, y = 0, ms = 0;
            words >> x >> y >> ms;
            VirtualPad v = GetVirtualPad(user);
            v.on = true;
            (what == "stick" ? v.lx : v.rx) = static_cast<short>(std::clamp(x, -32768, 32767));
            (what == "stick" ? v.ly : v.ry) = static_cast<short>(std::clamp(y, -32768, 32767));
            SetVirtualPad(user, v);
            if (ms > 0) {
                g_padRelease[user] = now + ms / 1000.0;
                g_padReleaseSticks[user] = true;
            }
            RM_LOGW("dev pad %d %s %d %d for %d ms", user, what.c_str(), x, y, ms);
        } else {
            int ms = 150;
            words >> ms;
            int triggers = 0;
            const unsigned short bits = PadButtonBits(what, triggers);
            VirtualPad v = GetVirtualPad(user);
            v.on = true;
            v.buttons = static_cast<unsigned short>(v.buttons | bits);
            if (triggers & 1) v.lt = 255;
            if (triggers & 2) v.rt = 255;
            SetVirtualPad(user, v);
            g_padRelease[user] = now + std::max(ms, 30) / 1000.0;
            g_padReleaseButtons[user] = static_cast<unsigned short>(g_padReleaseButtons[user] | bits);
            RM_LOGW("dev pad %d press %s (%04X) for %d ms", user, what.c_str(), bits, ms);
        }
    } else if (command == "mercenaries solo" || command == "mercenaries duo" || command == "mercenaries split") {
        // MODE SELECT's rows: Solo 0, Duo (online) 1, Split screen 2.
        autopilot::SetDevMercenariesMode(command == "mercenaries split" ? 2 : command == "mercenaries duo" ? 1 : 0);
        nativemenu::DecideMercenariesOnPlayMenu();
        RM_LOGW("dev %s: the game's Mercenaries menu at the next Play menu (no run)", command.c_str());
    } else if (command.rfind("seathp ", 0) == 0) {
        // "seathp <seat> <hp>": a co-op player's health (M10 tests: a player down).
        int seat = 0, hp = 0;
        std::sscanf(command.c_str() + 7, "%d %d", &seat, &hp);
        const auto unit = engine::HumanPlayerInSlot(seat);
        RM_LOGW("dev seathp %d %d: %s", seat, hp, unit && engine::WriteHp(unit, hp) ? "ok" : "no such player");
    } else if (command == "organize" && run.phase() == rm::Phase::Shop) {
        // "organize": the SHOP's ORGANIZE INVENTORY row for the player whose turn it is (M10 tests):
        // the game's screen in solo, the mod's grid screen in co-op.
        g->itemBoxSeat = UiSeat(run);
        StashGunsSeen();
        matchui::Close();
        if (run.SeatCount() < 2) {
            g->itemBoxRequested = true;
        } else {
            g->organizePick = -1;
            g->organizeUiRequested = true;  // opened after the tick: the screen builds under the mutex itself
        }
        RM_LOGW("dev organize: player %d's grid", g->itemBoxSeat + 1);
    } else if (command.rfind("mortal ", 0) == 0) {
        int seat = 0;
        char what[8] = {};
        std::sscanf(command.c_str() + 7, "%d %7s", &seat, what);
        if (seat >= 0 && seat < 8) {
            if (std::string(what) == "on") g_devMortalSeats |= 1u << seat; else g_devMortalSeats &= ~(1u << seat);
        }
        RM_LOGW("dev mortal %d %s (mask %X)", seat, what, g_devMortalSeats);
    } else if (command.rfind("coop ", 0) == 0) {
        // "coop on [skin]" / "coop off": PLAYERS 2 (player 2's skin) or 1, as the Play menu sets them.
        std::istringstream words(command.substr(5));
        std::string what, skin;
        words >> what >> skin;
        g->coopPlayers = what == "on" ? 2 : 1;
        if (!skin.empty()) g->p2Skin = skin;
        RM_LOGW("dev coop: players %d, player 2 skin '%s'", g->coopPlayers, g->p2Skin.c_str());
    } else if (command == "saveguard on" || command == "saveguard off") {
        g_devSaveGuard = command == "saveguard on";
        RM_LOGW("dev %s", command.c_str());
    } else if (command.rfind("screenwatch ", 0) == 0) {
        g_screenWatch = command.substr(12) == "on";
        g_screenWatchLast.clear();
        RM_LOGW("dev screenwatch %s", g_screenWatch ? "on" : "off");
    } else if (command.rfind("areacams ", 0) == 0) {
        // "areacams auto|game|off": the camera zones of campaign areas (auto: off while a run
        // plays one; game: the game's own; off: always off).
        const std::string mode = command.substr(9);
        areacams::SetMode(mode == "game" ? areacams::Mode::Game : mode == "off" ? areacams::Mode::Off : areacams::Mode::Auto);
        RM_LOGW("dev areacams %s (now %s)", areacams::ModeName(areacams::GetMode()), areacams::Off() ? "off" : "the game's own");
    } else if (command.rfind("warp ", 0) == 0) {
        // "warp x y z": writes the local player's position (does a start position stick?).
        float pos[3] = {};
        const int n = std::sscanf(command.c_str() + 5, "%f %f %f", &pos[0], &pos[1], &pos[2]);
        const int written = n == 3 ? engine::WarpUnit(engine::LocalPlayer(), pos) : 0;
        RM_LOGW("dev warp %.0f %.0f %.0f -> %d copies written", pos[0], pos[1], pos[2], written);
    } else if (command == "plactions") {
        // Research (M8, E19): every pointer in the local player object (0x4000 bytes) to an
        // object whose class is a player action (cPlAction...), to find the current action.
        const auto local = engine::LocalPlayer();
        int found = 0;
        for (std::size_t off = 0; local && off < 0x4000; off += 4) {
            const auto p = ReadOr<std::uintptr_t>(local + off, 0);
            if (p < 0x00400000 || p > 0x7FFF0000 || (p & 3)) continue;
            const std::string name = engine::ClassName(p);
            if (name.rfind("cPl", 0) != 0 && name.rfind("cWp", 0) != 0) continue;
            RM_LOGW("dev plactions +%04zX -> %08X %s", off, static_cast<unsigned>(p), name.c_str());
            if (++found >= 64) break;
        }
        RM_LOGW("dev plactions: %d pointers", found);
    } else if (command == "where") {
        // The player's position and every other copy of it in the player object (the movement
        // code keeps its own: a warp must write them all).
        const auto local = engine::LocalPlayer();
        const float x = ReadOr<float>(local + re5::off::kUnitPosition, 0.0f);
        const float y = ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f);
        const float z = ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f);
        std::string copies;
        for (std::size_t off = 0; local && off < 0x4000; off += 4) {
            const float cx = ReadOr<float>(local + off, 0.0f), cy = ReadOr<float>(local + off + 4, 0.0f), cz = ReadOr<float>(local + off + 8, 0.0f);
            if (std::fabs(cx - x) < 1.0f && std::fabs(cy - y) < 1.0f && std::fabs(cz - z) < 1.0f) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), " +%zX", off);
                copies += buf;
            }
        }
        RM_LOGW("dev where: player at %.1f %.1f %.1f; copies at%s", x, y, z, copies.c_str());
    } else if (command.rfind("uipreload ", 0) == 0) {
        // "uipreload ShopResource ...": screen archives (loader list 1) for the following loads.
        resources::Extras extras;
        std::istringstream in(command.substr(10));
        for (std::string word; in >> word;) extras.uiArchives.push_back(word);
        RM_LOGW("dev uipreload: %zu screen archives for the next loads", extras.uiArchives.size());
        resources::SetDevExtras(std::move(extras));
    } else if (command.rfind("itembox", 0) == 0) {
        // "itembox [poke <hex offset> <decimal value> ...]": opens the screen; pokes go into the
        // save data after its copy is taken (undone on close).
        std::vector<std::pair<std::size_t, std::uint32_t>> pokes;
        std::istringstream in(command.substr(7));
        std::string word, offset, value;
        bool withRun = false;
        while (in >> word) {
            if (word == "poke" && in >> offset >> value) {
                pokes.emplace_back(std::strtoul(offset.c_str(), nullptr, 16), static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)));
            } else if (word == "run") {
                withRun = true;  // "itembox run": the run's inventory and supplies in the screen
            }
        }
        OpenItemBox(pokes, withRun);
    } else if (command.rfind("savedump", 0) == 0) {
        // "savedump [hex bytes]": the game's save data ([kSaveData], plain in memory; the file is
        // obfuscated) to <data>\savedump.bin, for offline study (M7.1: what the item box shows).
        const auto base = ReadOr<std::uintptr_t>(re5::addr::kSaveData, 0);
        std::size_t size = command.size() > 9 ? std::strtoul(command.c_str() + 9, nullptr, 16) : 0x30000;
        size = std::min<std::size_t>(size, 0x100000);
        std::vector<char> buffer(size);
        std::size_t copied = 0;
        while (base && copied < size && SafeCopy(buffer.data() + copied, base + copied, std::min<std::size_t>(0x1000, size - copied))) {
            copied += std::min<std::size_t>(0x1000, size - copied);
        }
        std::ofstream out(g->paths.dataDir / L"savedump.bin", std::ios::binary);
        out.write(buffer.data(), static_cast<std::streamsize>(copied));
        RM_LOGW("dev savedump: %zu bytes from %08X (slot %d)", copied, static_cast<unsigned>(base),
                base ? ReadOr<std::int32_t>(base + re5::off::kSaveSlot, -1) : -1);
    } else if (command.rfind("enemyhp ", 0) == 0) {
        // "enemyhp <n>": every live enemy's health down to n (a boss finished by gunfire in tests).
        const int hp = std::atoi(command.c_str() + 8);
        int changed = 0;
        for (const auto& e : engine::ReadEnemyStates()) {
            if (e.hp > 0 && hp > 0 && engine::WriteHp(e.object, hp)) ++changed;
        }
        RM_LOGW("dev enemyhp %d: %d enemies", hp, changed);
    } else if (command == "enemies") {
        const auto states = engine::ReadEnemyStates();
        int alive = 0;
        for (const auto& e : states) alive += e.hp > 0 ? 1 : 0;
        RM_LOGW("dev enemies: %zu listed, %d alive, %d native spawns refused", states.size(), alive, director::TakeBlockedAttempts());
        for (const auto& e : states) {
            if (e.hp <= 0) continue;
            RM_LOGW("  %08X %s hp %d/%d at %.0f %.0f %.0f", static_cast<unsigned>(e.object), engine::ClassName(e.object).c_str(), e.hp, e.max,
                    ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f), ReadOr<float>(e.object + re5::off::kUnitPosition + 4, 0.0f),
                    ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f));
        }
    } else if (command.rfind("watch ", 0) == 0) {
        // "watch <class>|off": that enemy's action and health on every change.
        g_watch = {};
        if (command.substr(6) != "off") g_watch.cls = command.substr(6);
        RM_LOGW("dev watch %s", g_watch.cls.empty() ? "off" : g_watch.cls.c_str());
    } else if (command.rfind("openlog ", 0) == 0) {
        // "openlog on|off": every .arc the game opens, logged (a mod's files: when they load).
        fileredirect::SetLogOpens(command.substr(8) == "on");
        RM_LOGW("dev openlog %s", command.substr(8).c_str());
    } else if (command == "redirect clear") {
        fileredirect::SetRedirects({});
        g_devRedirects = false;
        g_modRedirectSkin = "-";
        RM_LOGW("dev redirect: none (mod skins set them again)");
    } else if (command.rfind("redirect ", 0) == 0) {
        // "redirect <path tail> <file>": the game's opens of a path ending in the tail read the file
        // instead ("redirect Image\Archive\uPl05ExcellaNormal.arc G:\mods\x.arc"; the file's path
        // may hold spaces, the tail may not).
        const std::string rest = command.substr(9);
        const auto space = rest.find(' ');
        auto redirects = fileredirect::Redirects();
        if (space != std::string::npos) {
            const std::string tail = rest.substr(0, space), target = rest.substr(space + 1);
            const std::filesystem::path targetPath(target);
            const bool exists = std::filesystem::is_regular_file(targetPath);
            redirects.push_back({std::wstring(tail.begin(), tail.end()), targetPath.wstring()});
            fileredirect::SetRedirects(redirects);
            g_devRedirects = true;  // mod skins leave them alone until "redirect clear"
            RM_LOGW("dev redirect %s -> %s%s (%zu in all)", tail.c_str(), target.c_str(), exists ? "" : " (NO SUCH FILE)", redirects.size());
        }
    } else if (command.rfind("wpwatch ", 0) == 0) {
        g_wpWatch = command.substr(8) == "on";
        g_wpWatchLast.clear();
        RM_LOGW("dev wpwatch %s", g_wpWatch ? "on" : "off");
    } else if (command.rfind("watchplayer ", 0) == 0) {
        g_watchPlayer = command.substr(12) == "on";
        g_watchPlayerAction.clear();
        RM_LOGW("dev watchplayer %s", g_watchPlayer ? "on" : "off");
    } else if (command.rfind("actions ", 0) == 0) {
        // "actions <class>": the action table of the first enemy of that class.
        const std::uintptr_t unit = FirstEnemyOf(command.substr(8));
        const auto names = engine::ActionNames(unit);
        RM_LOGW("dev actions %s %08X: %zu, now %d", command.substr(8).c_str(), static_cast<unsigned>(unit), names.size(),
                engine::CurrentActionId(unit));
        std::string line;
        for (std::size_t id = 0; id < names.size(); ++id) {
            if (names[id].empty()) continue;
            line += (line.empty() ? "" : ", ") + std::to_string(id) + " " + names[id];
            if (line.size() > 160) {
                RM_LOGW("  %s", line.c_str());
                line.clear();
            }
        }
        if (!line.empty()) RM_LOGW("  %s", line.c_str());
    } else if (command.rfind("bring ", 0) == 0) {
        // "bring <class> <distance> [side] [pin seconds] [away]": the first enemy of the class moved
        // that far straight ahead of the player as he faces now (aiming turns him to the camera),
        // `side` units to his right (negative: left): the aiming camera looks past his shoulder,
        // beside his facing. "away": while pinned it faces the way the player does (its back to him).
        std::istringstream args(command.substr(6));
        std::string cls, away;
        float distance = 500.0f, side = 0.0f, pin = 0.0f;
        args >> cls >> distance >> side >> pin >> away;
        const std::uintptr_t unit = FirstEnemyOf(cls);
        const auto player = engine::LocalPlayer();
        if (unit && player) {
            const float qx = ReadOr<float>(player + re5::off::kUnitRotation, 0.0f), qy = ReadOr<float>(player + re5::off::kUnitRotation + 4, 0.0f);
            const float qz = ReadOr<float>(player + re5::off::kUnitRotation + 8, 0.0f), qw = ReadOr<float>(player + re5::off::kUnitRotation + 12, 0.0f);
            float fx = 2.0f * (qx * qz + qw * qy), fz = 1.0f - 2.0f * (qx * qx + qy * qy);
            const float len = std::sqrt(fx * fx + fz * fz);
            if (len > 0.01f) {
                fx /= len;
                fz /= len;
            }
            // Right of the facing (fx, fz) on the ground plane: (-fz, fx) in this game's frame (a
            // guess the hit sweep tells: negative sides are the other way).
            const float rx = -fz, rz = fx;
            const float dest[3] = {ReadOr<float>(player + re5::off::kUnitPosition, 0.0f) + fx * distance + rx * side,
                                   ReadOr<float>(player + re5::off::kUnitPosition + 4, 0.0f),
                                   ReadOr<float>(player + re5::off::kUnitPosition + 8, 0.0f) + fz * distance + rz * side};
            const int copies = engine::WarpUnit(unit, dest);
            g_pin = {};
            if (pin > 0.0f) {
                g_pin.unit = unit;
                for (int k = 0; k < 3; ++k) g_pin.dest[k] = dest[k];
                g_pin.until = now + pin;
                g_pin.turn = away == "away";
                g_pin.yaw = std::atan2(fx, fz);
                if (g_pin.turn) engine::TurnTo(unit, g_pin.yaw, 0);
            }
            RM_LOGW("dev bring %s %.0f side %.0f: to %.0f %.0f %.0f (%d copies)%s", cls.c_str(), distance, side, dest[0], dest[1],
                    dest[2], copies, pin > 0.0f ? ", pinned" : "");
        } else {
            RM_LOGW("dev bring %s: no such enemy or no player", cls.c_str());
        }
    } else if (command.rfind("target ", 0) == 0) {
        // "target <class>": the first enemy of the class gets the player as its target (a boss
        // spawned outside its scenario may have none and stand still: the Uroboros did).
        const std::uintptr_t unit = FirstEnemyOf(command.substr(7));
        const auto local = engine::LocalPlayer();
        const auto before = unit ? ReadOr<std::uintptr_t>(unit + re5::off::kEnemyTarget, 0) : 0;
        const bool set = unit && local && SafeStore<std::uintptr_t>(unit + re5::off::kEnemyTarget, local);
        RM_LOGW("dev target %s %08X: was %08X, %s", command.substr(7).c_str(), static_cast<unsigned>(unit),
                static_cast<unsigned>(before), set ? "the player now" : "not set");
    } else if (command.rfind("act ", 0) == 0) {
        // "act <class> <id>": starts that action of the first enemy of the class (StartAction's checks).
        const auto space = command.find(' ', 4);
        const std::string cls = command.substr(4, space == std::string::npos ? std::string::npos : space - 4);
        const int id = space == std::string::npos ? -1 : std::atoi(command.c_str() + space + 1);
        const std::uintptr_t unit = FirstEnemyOf(cls);
        const auto names = engine::ActionNames(unit);
        const std::string name = id >= 0 && id < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(id)] : std::string();
        const bool started = !name.empty() && engine::StartAction(unit, id, name.c_str(), true);
        RM_LOGW("dev act %s %d (%s): %s", cls.c_str(), id, name.c_str(), started ? "started" : "refused");
    } else if (command.rfind("gun ", 0) == 0 && run.HasActiveRun()) {
        // "gun <base|held> <rarity> [elements|-] [effects|-]": the run's gun of that base becomes
        // this one (comma lists of element names and unique affix ids; no regular bonuses).
        std::istringstream words(command.substr(4));
        std::string base, rarity, elements, effects;
        words >> base >> rarity >> elements >> effects;
        if (base == "held") base = EquippedWeaponBaseId().value_or(std::string());
        rm::WeaponInstance w = rm::MakeStartingWeapon(base, std::string());
        w.rarity = rm::ParseRarity(rarity).value_or(rm::Rarity::Common);
        const auto split = [](const std::string& list) {
            std::vector<std::string> out;
            std::istringstream parts(list);
            for (std::string part; std::getline(parts, part, ',');) {
                if (!part.empty() && part != "-") out.push_back(part);
            }
            return out;
        };
        for (const std::string& e : split(elements)) {
            if (const auto el = rm::ParseElement(e)) w.elements.push_back(*el);
        }
        for (const std::string& id : split(effects)) {
            if (const rm::UniqueAffixDef* u = g->content->FindUnique(id)) {
                w.affixes.push_back(rm::Affix{u->kind, u->value, u->threshold, u->id});
            }
        }
        for (const auto& existing : run.state().weapons) {
            if (existing.base_id == base) w.instance_id = existing.instance_id;
        }
        const rm::Status s = run.DevGrantWeapon(w);
        RM_LOGW("dev gun %s %s: %zu elements, %zu effects: %s", base.c_str(), rm::RarityName(w.rarity).data(), w.elements.size(),
                w.affixes.size(), s.ok() ? "held" : s.message.c_str());
    } else if (command.rfind("fx ", 0) == 0 || command == "gallery") {
        // "fx <path>": that effect list on the nearest enemy (if it is in memory); "gallery": the
        // effect gallery on or off (settings effect_gallery).
        if (command == "gallery") {
            g->settings.effectGallery = !g->settings.effectGallery;
            RM_LOGW("dev gallery: %s", g->settings.effectGallery ? "on" : "off");
        } else {
            const std::string path = command.substr(3);
            const auto player = engine::LocalPlayer();
            float p[3] = {};
            if (player) UnitPosition(player, p);
            const auto nearby = player ? EnemiesNear(p, 2000.0f, 0) : std::vector<std::pair<std::uintptr_t, float>>{};
            const bool played = !nearby.empty() && engine::PlayEffect(path.c_str(), nearby.front().first);
            RM_LOGW("dev fx %s: %s", path.c_str(), nearby.empty() ? "no enemy within 20 m" : played ? "played" : "not in memory");
        }
    } else if (command.rfind("effect ", 0) == 0) {
        // "effect <path> [load]": is that effect list cached (and with `load`, load it from its
        // file now; the reference is kept for the session).
        std::istringstream words(command.substr(7));
        std::string path, mode;
        words >> path >> mode;
        const bool cached = [&] {
            const std::uintptr_t r = engine::EffectList(path.c_str(), false);
            if (r) engine::ReleaseResource(r);
            return r != 0;
        }();
        const std::uintptr_t loaded = !cached && mode == "load" ? engine::EffectList(path.c_str(), true) : 0;
        RM_LOGW("dev effect %s: %s%s", path.c_str(), cached ? "cached" : "not cached",
                mode == "load" && !cached ? (loaded ? ", loaded now" : ", NOT loaded") : "");
    } else if (command.rfind("refine", 0) == 0 && run.HasActiveRun()) {
        // "refine [base]": that gun (else the one in hand) one tier up, as a reward would.
        std::string base = command.size() > 7 ? command.substr(7) : EquippedWeaponBaseId().value_or(std::string());
        const rm::Status s = run.DevRefineWeapon(base.empty() ? std::nullopt : std::optional<std::string>(base));
        std::string after;
        for (const auto& w : run.state().weapons) {
            if (w.base_id == base) after = std::string(rm::RarityName(w.rarity)) + ", " + std::to_string(w.elements.size()) + " elements";
        }
        RM_LOGW("dev refine %s: %s; now %s", base.c_str(), s.ok() ? "done" : s.message.c_str(), after.c_str());
    } else if (command.rfind("mag ", 0) == 0) {
        // "mag <n>": the rounds in the gun in hand (Last Word's last round).
        const std::uintptr_t slot = EquippedGunSlot();
        const int n = std::atoi(command.c_str() + 4);
        RM_LOGW("dev mag %d: %s", n, slot && SafeStore<std::int32_t>(slot + re5::off::kSlotCount, n) ? "set" : "no gun in hand");
    } else if (command.rfind("outgoing ", 0) == 0) {
        // "outgoing <x>": every player hit's damage times x (developer; adapter.json dev.outgoing_multiplier).
        g->settings.devOutgoingMultiplier = static_cast<float>(std::atof(command.c_str() + 9));
        RM_LOGW("dev outgoing x%.2f", g->settings.devOutgoingMultiplier);
    } else if (command.rfind("grabs ", 0) == 0) {
        // "grabs break|game": every Majini grab broken at once (as Iron Grip), or left to the game.
        g->devBreakGrabs = command.substr(6) == "break";
        RM_LOGW("dev grabs %s", g->devBreakGrabs ? "broken" : "left to the game");
    } else if (command == "abandon" && run.HasActiveRun()) {
        // Ends the run as the overlay's "Abandon run" does; the match goes on as plain Mercenaries.
        Report(run.AbandonRun(now), "dev abandon run");
    } else if (command == "killall") {
        // Real deaths (not the run's synthetic "kill <n>"): forces the game to refill its waves.
        int killed = 0;
        for (const auto& e : engine::ReadEnemyStates()) killed += e.hp > 0 && director::Kill(e.object) ? 1 : 0;
        RM_LOGW("dev killall: %d enemies killed", killed);
    } else if (command == "watchers") {
        // The rescue's sight test (AnyoneMaySee): each camera, then each director enemy, its
        // distance to the nearest standing player and whether a player may see it.
        const Watchers w = ReadWatchers();
        for (const auto& v : w.views) {
            RM_LOGW("dev watchers: view %d eye %.0f %.0f %.0f target %.0f %.0f %.0f fov %.1f aspect %.2f", v.viewport, v.eye[0], v.eye[1],
                    v.eye[2], v.target[0], v.target[1], v.target[2], v.fov, v.aspect);
        }
        for (const auto& [unit, id] : g->directorUnits) {
            const auto hp = engine::ReadHp(unit);
            if (!hp || hp->hp <= 0) continue;
            float pos[3];
            for (int k = 0; k < 3; ++k) pos[k] = ReadOr<float>(unit + re5::off::kUnitPosition + k * 4, 0.0f);
            const float body[3] = {pos[0], pos[1] + kBodyHeight, pos[2]};
            bool inView = false;
            for (const auto& v : w.views) inView = inView || InCameraView(v, body);
            RM_LOGW("dev watchers: %s %s %.1f m from the nearest player, %s, %s", id.c_str(), engine::ClassName(unit).c_str(),
                    w.players.empty() ? -1.0f : NearestWatcher(w, pos).second / 100.0f, inView ? "in a camera's view" : "out of view",
                    AnyoneMaySee(w, pos) ? "SEEN" : "unseen");
        }
    } else if (command.rfind("facept ", 0) == 0) {
        // "facept <x> <z>": the local player turns to a world point (a ladder, a door).
        float tx = 0.0f, tz = 0.0f;
        const auto local = engine::LocalPlayer();
        if (local && std::sscanf(command.c_str() + 7, "%f %f", &tx, &tz) == 2) {
            const float yaw = std::atan2(tx - ReadOr<float>(local + re5::off::kUnitPosition, 0.0f),
                                         tz - ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f));
            const float q[4] = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            for (int i = 0; i < 4; ++i) SafeStore<float>(local + re5::off::kUnitRotation + i * 4, q[i]);
            RM_LOGW("dev facept %.0f %.0f: %.0f degrees", tx, tz, yaw * 57.29578f);
        }
    } else if (command.rfind("face ", 0) == 0) {
        // "face <class>": the local player turns to the first live enemy of the class (screenshots:
        // aiming then puts the camera over the player's shoulder on it).
        const std::string cls = command.substr(5);
        const auto local = engine::LocalPlayer();
        bool done = false;
        for (const auto& e : engine::ReadEnemyStates()) {
            if (done || e.hp <= 0 || !local || engine::ClassName(e.object) != cls) continue;
            const float dx = ReadOr<float>(e.object + re5::off::kUnitPosition, 0.0f) - ReadOr<float>(local + re5::off::kUnitPosition, 0.0f);
            const float dz = ReadOr<float>(e.object + re5::off::kUnitPosition + 8, 0.0f) - ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f);
            const float yaw = std::atan2(dx, dz);  // forward = (sin, cos), as the spawn code reads it
            const float q[4] = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            for (int i = 0; i < 4; ++i) SafeStore<float>(local + re5::off::kUnitRotation + i * 4, q[i]);
            RM_LOGW("dev face %s: %.0f degrees, %.1f m away", cls.c_str(), yaw * 57.29578f, std::sqrt(dx * dx + dz * dz) / 100.0f);
            done = true;
        }
        if (!done) RM_LOGW("dev face %s: no such live enemy", cls.c_str());
    } else if (command == "killone") {
        // One real death: the first live enemy (to bring an arena down to its last enemy).
        for (const auto& e : engine::ReadEnemyStates()) {
            if (e.hp > 0 && director::Kill(e.object)) {
                RM_LOGW("dev killone: %s killed, %d left", engine::ClassName(e.object).c_str(), run.RemainingEnemies() - 1);
                break;
            }
        }
    } else if (command.rfind("survey ", 0) == 0) {
        director::SetSurvey(command.substr(7) == "on");
        world::SetSurvey(command.substr(7) == "on");
        const auto work = engine::ReadGameWork();
        RM_LOGW("dev survey %s (stage %d)", command.substr(7).c_str(), work ? work->stage : -1);
    } else if (command.rfind("nativespawns ", 0) == 0) {
        // "nativespawns off|on": the game's own set spawns (Mercenaries waves, scripts).
        director::BlockNativeSpawns(command.substr(13) == "off");
        RM_LOGW("dev native spawns %s", director::NativeSpawnsBlocked() ? "blocked" : "allowed");
    } else if (command == "timeup") {
        RequestMercMatchEnd();
        RM_LOGW("dev timeup requested");
    } else if (command.rfind("merchud ", 0) == 0) {
        // "merchud on|off": the game's clock and score panels during a run.
        g->settings.mercHud = command.substr(8) == "on";
        RM_LOGW("dev merc hud %s", g->settings.mercHud ? "shown" : "hidden");
    } else if (command.rfind("preload ", 0) == 0) {
        // "preload uEm11 011A 70160 ...": class archives (with the enemies), item ids in hex
        // (with the player's items) and enemy weapon ids (category<<16 | id, with the enemies)
        // for every following Mercenaries load, on top of the run's; "preload clear".
        resources::Extras extras;
        std::istringstream in(command.substr(8));
        for (std::string word; in >> word;) {
            if (word == "clear") continue;
            const auto value = std::strtoul(word.c_str(), nullptr, 16);
            if (word.rfind("u", 0) == 0) extras.enemyClasses.push_back(word);
            else if (value > 0xFFFF) extras.enemyWeapons.push_back(static_cast<std::uint32_t>(value));
            else extras.playerItems.push_back(static_cast<std::uint16_t>(value));
        }
        RM_LOGW("dev preload: %zu classes, %zu enemy weapons, %zu player items for the next loads", extras.enemyClasses.size(),
                extras.enemyWeapons.size(), extras.playerItems.size());
        resources::SetDevExtras(std::move(extras));
    } else if (command.rfind("quiet ", 0) == 0) {
        // "quiet on|off": the director stops refilling and the game's own spawns stay off, so
        // after a killall the arena is empty (screenshots, weapon tests).
        g->devQuiet = command.substr(6) == "on";
        RM_LOGW("dev quiet %s", g->devQuiet ? "on" : "off");
    } else if (command == "loadlists") {
        resources::LogLists();
    } else if (command.rfind("archive ", 0) == 0) {
        const std::string name = command.substr(8);
        RM_LOGW("dev archive %s: %s", name.c_str(), resources::ArchiveLoaded(name) ? "loaded" : "not loaded");
    } else if (command.rfind("merckills ", 0) == 0) {
        // "merckills N": sets the game's Mercenaries kill counter (its cap ends a match).
        const auto work = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
        const int kills = std::atoi(command.c_str() + 10);
        if (work) SafeStore(work + re5::off::kMercKills, static_cast<std::int32_t>(kills));
        RM_LOGW("dev mercenaries kill counter set to %d", kills);
    } else if (command == "die" && run.phase() == rm::Phase::ArenaActive) {
        Report(run.OnPlayerDied(now), "dev die");
    } else if (command == "itemnames") {
        // Names of the item records (categories 1..3) from the mes_item message file, to map ids.
        const auto sMessage = ReadOr<std::uintptr_t>(re5::addr::kSMessageInstance, 0);
        std::uintptr_t table = 0;
        int count = 0;
        for (std::size_t off = 0; sMessage && off < 0x12000 && !table; off += 4) {
            const auto r = ReadOr<std::uintptr_t>(sMessage + off, 0);
            if (!r || ReadOr<std::uint32_t>(r + re5::off::kRMessageTypeCrc, 0) != 0x10C460E6u) continue;
            const std::string path = ReadCString(r + re5::off::kRMessagePath, 64);
            RM_LOGW("dev itemnames: message slot +%zX = %s", off, path.c_str());
            if (path.find("mes_item") == std::string::npos) continue;
            const auto data = ReadOr<std::uintptr_t>(r + re5::off::kRMessageData, 0);
            table = ReadOr<std::uintptr_t>(r + re5::off::kRMessageTable, 0);
            count = data ? ReadOr<std::uint16_t>(data + re5::off::kMsg2Count, 0) : 0;
        }
        if (!table) {
            RM_LOGW("dev itemnames: mes_item not loaded");
        } else {
            const auto glyph = [](std::uint16_t code) -> char {
                if (code >= 0x38 && code < 0x38 + 52 && (code % 2 == 0)) return static_cast<char>('A' + (code - 0x38) / 2);
                if (code >= 0x6C && code < 0x6C + 52 && (code % 2 == 0)) return static_cast<char>('a' + (code - 0x6C) / 2);
                if (code <= 18 && code % 2 == 0) return static_cast<char>('0' + code / 2);
                if (code == 0x1C) return ' ';
                if (code == 0x24) return ',';
                if (code == 0x26) return '.';
                if (code == 0x2E) return '-';
                return '?';
            };
            for (int cat = 1; cat <= 3; ++cat) {
                const auto base = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + cat * 4, 0);
                for (int idx = 0; base && idx < 0x40; ++idx) {
                    const int msg = ReadOr<std::int32_t>(base + idx * 0x48 + 0x40, 0);
                    if (msg <= 0 || msg >= count) continue;
                    const auto entry = ReadOr<std::uintptr_t>(table + msg * 4, 0);
                    std::string name;
                    for (int k = 0; entry && k < 48; ++k) {
                        const auto code = ReadOr<std::uint16_t>(entry + k * 4, 0);
                        const auto width = ReadOr<std::uint16_t>(entry + k * 4 + 2, 0);
                        if (code == 0 && width == 0x0401) break;
                        if (code == 0 && width == 0x0403) { name += ' '; continue; }
                        name += glyph(code);
                    }
                    RM_LOGW("dev itemnames: %04X msg %d = %s", (cat << 8) | idx, msg, name.c_str());
                }
            }
        }
    } else if (command.rfind("sethp ", 0) == 0) {
        // "sethp <hp>": writes the local player's health (to test heals and the carry-over).
        const auto local = engine::LocalPlayer();
        const int hp = std::atoi(command.c_str() + 6);
        const bool ok = local && hp > 0 && engine::WriteHp(local, hp);
        RM_LOGW("dev sethp %d -> %s", hp, ok ? "ok" : "failed");
    } else if (command.rfind("fadein", 0) == 0) {
        // "fadein [frames]": the game's fader back to the scene.
        const int frames = std::max(1, std::atoi(command.c_str() + 6));
        const auto fader = ReadOr<std::uintptr_t>(re5::addr::kFadeInstance, 0);
        const auto vtable = fader ? ReadOr<std::uintptr_t>(fader, 0) : 0;
        const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + re5::off::kFadeVtableIn, 0) : 0;
        if (fn >= re5::kImageBase) {
            using FadeFn = void(__thiscall*)(std::uintptr_t self, float alpha, int frames);
            reinterpret_cast<FadeFn>(fn)(fader, 1.0f, frames);
        }
        RM_LOGW("dev fadein %d frames -> %s", frames, fn >= re5::kImageBase ? "called" : "no fader");
    } else if (command == "quitmatch") {
        // "quitmatch": the game's own request to leave a running match for the menus (app
        // request 0x10000: main state 2 turns it into its exit state 0xC), as a quit from the
        // pause menu leaves it (test of a run's suspend, scripts/test_resume.sh).
        const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
        if (!app || !engine::Caps().appRequest) {
            RM_LOGW("dev quitmatch: no application object or its request function");
        } else {
            using RequestFn = void(__thiscall*)(std::uintptr_t app, unsigned flags);
            reinterpret_cast<RequestFn>(re5::addr::kFnAppRequest)(app, 0x10000);
            RM_LOGW("dev quitmatch: the match is asked to leave for the menus");
        }
    } else if (command == "gomenu") {
        // "gomenu": leave the match for the Mercenaries menu scene the way the results
        // screen does (FUN_00794F30 on the game-flow object); the title-flow script then
        // runs again, and an armed autopilot answers it with the next stage.
        const auto flow = ReadOr<std::uintptr_t>(re5::addr::kGameFlow, 0);
        if (flow) {
            using ToMenuFn = void(__fastcall*)(std::uintptr_t self, void*);
            reinterpret_cast<ToMenuFn>(re5::addr::kFnFlowToMercMenu)(flow, nullptr);
            RM_LOGW("dev gomenu: flow %08X +1304 %08X +131C %02X", static_cast<unsigned>(flow),
                    ReadOr<std::uint32_t>(flow + 0x1304, 0), ReadOr<std::uint8_t>(flow + 0x131C, 0));
        } else {
            RM_LOGW("dev gomenu: no game-flow object");
        }
    } else if (command == "rogue") {
        // Opens the mod menu by itself as soon as the Play menu waits for input, so test scripts
        // never press keys on the Play menu's own rows (a stray Enter on row 0 starts the campaign,
        // and leaving it saves the game).
        nativemenu::OpenOnPlayMenu(nativemenu::ScreenId::Main);
        RM_LOGW("dev rogue: the mod menu opens at the next Play menu");
    } else if (command == "rogue skins" || command == "rogue characters") {
        // "rogue skins|characters": the mod menu opens on that screen at the next Play menu (the
        // selected character's skins: dev select first).
        nativemenu::OpenOnPlayMenu(command == "rogue skins" ? nativemenu::ScreenId::Skins : nativemenu::ScreenId::Characters);
        RM_LOGW("dev %s: opens at the next Play menu", command.c_str());
    } else if (command.rfind("select ", 0) == 0 && !run.HasActiveRun()) {
        // "select <skin id>": the character and skin the next START RUN plays, as the CHARACTERS
        // and SKINS screens choose them.
        const std::string skinId = command.substr(7);
        const auto characters = run.Characters();
        bool found = false;
        for (std::size_t i = 0; i < characters.size() && !found; ++i) {
            for (const auto& s : characters[i].skins) {
                if (s.id != skinId) continue;
                g->selectedCharacter = static_cast<int>(i);
                g->selectedSkin = s.id;
                found = true;
                break;
            }
        }
        RM_LOGW("dev select %s -> %s", skinId.c_str(), found ? "ok" : "unknown skin");
    } else if (command.rfind("arm ", 0) == 0) {
        // "arm <stage> <costume slot>": the next START RUN uses this plan.
        std::istringstream words(command.substr(4));
        autopilot::Plan plan;
        words >> plan.stage >> plan.costume;
        autopilot::Arm(plan);
        g->currentStage = plan.stage;  // the director's map follows the armed stage
    } else if (command == "dumpinv") {
        // The local player's inventory slots (item id, count, max).
        const auto player = engine::LocalPlayer();
        const auto inv = player ? player + re5::off::kPlayerInventory : 0;
        for (int i = 0; inv && i < re5::off::kInventorySlots; ++i) {
            const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
            if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
            std::uint32_t raw[12]{};
            for (int k = 0; k < 12; ++k) SafeRead(slot + k * 4, raw[k]);
            RM_LOGW("dev inv slot %2d @%08X: item %04X count %d max %d | %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                    i, static_cast<unsigned>(slot), ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0),
                    ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0), ReadOr<std::int32_t>(slot + re5::off::kSlotMax, 0),
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
        }
        if (!inv) RM_LOGW("dev inv: no local player");
    } else if (command.rfind("give ", 0) == 0 || command.rfind("ammo ", 0) == 0) {
        // "give <hex item> <count>" tops up an existing stack through the game's
        // own inventory helper; "ammo <hex item> <count>" uses the ammo one.
        std::istringstream words(command.substr(5));
        std::string idText;
        int count = 1;
        words >> idText >> count;
        const auto item = static_cast<int>(std::strtoul(idText.c_str(), nullptr, 16));
        const auto player = engine::LocalPlayer();
        if (!engine::Caps().inventory || !player) {
            RM_LOGW("dev %s: inventory not available", command.c_str());
        } else {
            using InventoryFn = int(__thiscall*)(std::uintptr_t inventory, int item, int count);
            const auto fn = reinterpret_cast<InventoryFn>(command[0] == 'g' ? re5::addr::kFnInventoryAddToStacks
                                                                            : re5::addr::kFnInventoryAddAmmo);
            const int left = fn(player + re5::off::kPlayerInventory, item, count);
            RM_LOGW("dev %s item %04X x%d -> %d not placed", command[0] == 'g' ? "give" : "ammo", item, count, left);
        }
    } else if (command.rfind("equip ", 0) == 0) {
        // "equip <hex item>": the hands take that item's grid cell (the game's own change of weapon).
        const auto local = engine::LocalPlayer();
        const auto id = static_cast<std::uint32_t>(std::strtoul(command.c_str() + 6, nullptr, 16));
        const auto record = local ? FindRecord(local, id) : 0;
        const int cell = record ? ReadOr<std::int32_t>(record + kSlotCell, -1) : -1;
        const bool ok = cell >= 0 && cell < kGridCells && engine::Caps().equipCell &&
                        SafeStore<std::int32_t>(local + re5::off::kPlayerWeaponCell, cell) && CallEquipCell(local, cell);
        RM_LOGW("dev equip %04X: cell %d, %s", id, cell + 1, ok ? "in hand" : "not equipped");
    } else if (command.rfind("obtain ", 0) == 0) {
        // "obtain <hex item> [count]": as a shop purchase or a reward hands it over (GiveOrStash:
        // the grid, a gun in a grid cell, what does not fit to the run's storage).
        std::istringstream words(command.substr(7));
        std::string idText;
        int count = 1;
        words >> idText >> count;
        const auto item = static_cast<std::int64_t>(std::strtoul(idText.c_str(), nullptr, 16));
        RM_LOGW("dev obtain %04X x%d -> %d", static_cast<unsigned>(item), count, GiveOrStash("dev obtain", item, count));
    } else if (command.rfind("create ", 0) == 0) {
        // "create <hex item> <count> [position] [hex flags]": a new inventory slot through the
        // game's own creator, as PickUpItem calls it (flags 0xD = find a free slot).
        std::istringstream words(command.substr(7));
        std::string idText, flagsText;
        int count = 1;
        int position = 0;
        words >> idText >> count >> position >> flagsText;
        const int flags = flagsText.empty() ? 0xD : static_cast<int>(std::strtoul(flagsText.c_str(), nullptr, 16));
        const auto item = static_cast<int>(std::strtoul(idText.c_str(), nullptr, 16));
        const auto player = engine::LocalPlayer();
        if (!engine::Caps().inventory || !player) {
            RM_LOGW("dev create: inventory not available");
        } else {
            using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t inventory, int item, int count, int a, int b,
                                                         int c, int flags, int a1, int a2, int a3, int a4, int a5,
                                                         int a6, int a7, int a8, int a9, std::uintptr_t player,
                                                         int position, int zero);
            const auto fn = reinterpret_cast<CreateFn>(re5::addr::kFnInventoryCreate);
            const auto slot = fn(player + re5::off::kPlayerInventory, item, count, -1, -1, -1, flags, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, player, position, 0);
            RM_LOGW("dev create item %04X x%d position %d flags %X -> slot %08X", item, count, position, flags,
                    static_cast<unsigned>(slot));
        }
    } else if (command.rfind("notify ", 0) == 0) {
        // "notify <hex item> <count>": the game's own "item acquired" notice.
        std::istringstream words(command.substr(7));
        std::string idText;
        int count = 1;
        words >> idText >> count;
        const auto item = static_cast<int>(std::strtoul(idText.c_str(), nullptr, 16));
        const auto game = ReadMercTimer().object;
        if (!engine::Caps().inventory || !game) {
            RM_LOGW("dev notify: game object or signatures not available");
        } else {
            using NotifyFn = void(__thiscall*)(std::uintptr_t game, int item, int count);
            reinterpret_cast<NotifyFn>(re5::addr::kFnGameNotifyItem)(game, item, count);
            RM_LOGW("dev notify item %04X x%d", item, count);
        }
    } else if (command.rfind("pokeb ", 0) == 0) {
        // "pokeb <hex addr> <hex byte>"
        std::istringstream words(command.substr(6));
        std::string addressText, valueText;
        words >> addressText >> valueText;
        const auto address = static_cast<std::uintptr_t>(std::strtoul(addressText.c_str(), nullptr, 16));
        const auto value = static_cast<std::uint8_t>(std::strtoul(valueText.c_str(), nullptr, 16));
        RM_LOGW("dev pokeb %08X = %02X (%s)", static_cast<unsigned>(address), value, SafeStore(address, value) ? "ok" : "failed");
    } else if (command == "menu_snapshot") {
        menuprobe::Snapshot();
    } else if (command == "menu_diff") {
        menuprobe::Diff();
    } else if (command == "menu_list") {
        RM_LOGW("live menu screens: %s", menuprobe::Describe().c_str());
    } else if (command.rfind("unlock ", 0) == 0 && !run.HasActiveRun()) {
        // "unlock perk <id>" / "unlock weapon <id>": the same purchase path as the Unlocks panel.
        std::istringstream words(command.substr(7));
        std::string kind, id;
        words >> kind >> id;
        const auto receipt = run.PurchaseUnlock(kind == "weapon" ? rm::UnlockKind::Weapon : rm::UnlockKind::Perk, id);
        RM_LOGW("dev command %s -> %s", command.c_str(), receipt.ok() ? "ok" : receipt.status().message.c_str());
    } else if (command.rfind("grant ", 0) == 0 && run.HasActiveRun()) {
        // "grant <perk id>": adds the perk to the active run (eligibility still applies).
        const rm::Status status = run.DevGrantPerk(command.substr(6));
        RM_LOGW("dev command %s -> %s", command.c_str(), status.ok() ? "ok" : status.message.c_str());
    } else if (command.rfind("kill ", 0) == 0 && run.phase() == rm::Phase::ArenaActive) {
        // "kill <n>": n synthetic player kills, to exercise kill-triggered perks without fighting.
        const int wanted = std::clamp(std::atoi(command.c_str() + 5), 1, 500);
        const int count = std::min(wanted, run.RemainingEnemies());
        for (int i = 0; i < count; ++i) {
            rm::KillEvent kill;
            kill.now = now;
            kill.by_player = true;
            kill.enemy_id = "devkill_" + std::to_string(i) + "_" + std::to_string(static_cast<long long>(now * 1000));
            run.OnEnemyKilled(kill);
        }
        RM_LOGW("dev command kill: %d synthetic kills (%d remain)", count, run.RemainingEnemies());
    } else {
        RM_LOGW("dev command '%s' ignored in phase %s", command.c_str(), std::string(rm::PhaseName(run.phase())).c_str());
    }
}

class RogueSink final : public HookSink {
public:
    float OutgoingMultiplier(const OutgoingHit& hit) override {
        return Guarded("OutgoingMultiplier", 1.0f, [&] { return Outgoing(hit); });
    }
    void EnemyHitDone(const EnemyHitResult& r) override {
        Guarded("EnemyHitDone", 0, [&] { EnemyHit(r); return 0; });
    }
    IncomingDecision Incoming(const IncomingHit& hit) override {
        return Guarded("Incoming", IncomingDecision{}, [&] { return IncomingImpl(hit); });
    }
    void PlayerHitDone(const PlayerHitResult& r) override {
        Guarded("PlayerHitDone", 0, [&] { PlayerHit(r); return 0; });
    }
    void GameTick() override {
        Guarded("GameTick", 0, [&] { Tick(); return 0; });
    }
    bool BlockGameInput() override { return overlay::IsFocused(); }
    bool HoldGameButtons() override { return g_holdButtons.load(std::memory_order_relaxed); }

private:
    float Outgoing(const OutgoingHit& hit) {
        if (!hit.attackerIsLocal) return 1.0f;
        std::lock_guard lock(g->mutex);
        SeatTurn turn(SeatTurn::OfUnit(hit.attacker));
        rm::HitContext ctx;
        ctx.now = Now();
        ctx.weapon_id = EquippedWeaponBaseId();
        ctx.is_melee = engine::InMeleeAction(hit.attacker);
        ctx.base_damage = hit.damage;
        ctx.target_hp = hit.targetHp;
        ctx.target_hp_max = hit.targetMaxHp;
        if (g->match.localMaxHp > 0) {
            ctx.player_hp = g->match.localHp;
            ctx.player_hp_max = g->match.localMaxHp;
        }
        // The rarity remake: the target's statuses, the boss, the rounds left (Last Word).
        ctx.target_is_boss = hit.enemy != 0 && hit.enemy == g->boss.unit;
        ctx.target_frozen = StatusActive(hit.enemy, ctx.now, &ElementStatus::frozenUntil);
        ctx.target_poisoned = StatusActive(hit.enemy, ctx.now, &ElementStatus::poisonUntil);
        if (const std::uintptr_t slot = EquippedGunSlot(); slot && !*ctx.is_melee) {
            ctx.magazine_left = ReadOr<std::int32_t>(slot + re5::off::kSlotCount, -1);
        }
        const auto result = g->run->ComputeOutgoingMultiplier(ctx);
        if (!result.procs.empty()) QueueProcs(hit.enemy, result.procs);
        // Which gun's hits carry which element, once a match each (2026-11-11, the user: "once the
        // flamethrower got an element my pistol also got it").
        {
            static int generation = -1;
            static std::set<std::string> told;
            if (generation != g->matchGeneration) {
                generation = g->matchGeneration;
                told.clear();
            }
            for (const rm::WeaponProc& p : result.procs) {
                const std::string gun = ctx.weapon_id ? *ctx.weapon_id : std::string("-");
                if (!p.element.empty() && told.insert(gun + "/" + p.element).second) {
                    RM_LOGI("elements: a %s hit carries %s (%s)", gun.c_str(), p.element.c_str(), p.source.c_str());
                }
            }
        }
        static int magazineLogged = 0;
        if (g->settings.probeHits && ctx.magazine_left && magazineLogged++ < 40) {
            RM_LOGI("hit: %s, magazine %d left, x%.2f, %zu procs%s", ctx.weapon_id ? ctx.weapon_id->c_str() : "-", *ctx.magazine_left,
                    result.multiplier, result.procs.size(), ctx.target_frozen ? ", frozen" : "");
        }
        if (g->settings.probeHits && g->run->phase() == rm::Phase::ArenaActive) {
            RM_LOGD("outgoing core=%.4f crit=%d perk=%+.3f streak=%+.3f build=%+.3f weapon=%+.3f (%s%s) target %d/%d dev=%.2f",
                    result.multiplier, result.crit, result.breakdown.perk_bonus, result.breakdown.kill_streak_bonus,
                    result.breakdown.build_bonus, result.breakdown.weapon_bonus, ctx.weapon_id ? ctx.weapon_id->c_str() : "-",
                    *ctx.is_melee ? ", melee" : "", hit.targetHp, hit.targetMaxHp, g->settings.devOutgoingMultiplier);
            // Research (M8, E19): the player's pointers to action objects at the hit, to tell a
            // melee hit (cPlAction_pl_act_attack_*) from a shot.
            std::string actions;
            const auto local = engine::LocalPlayer();
            for (std::size_t off = 0; local && off < 0x4000; off += 4) {
                const auto p = ReadOr<std::uintptr_t>(local + off, 0);
                if (p < 0x00400000 || p > 0x7FFF0000 || (p & 3)) continue;
                const std::string name = engine::ClassName(p);
                if (name.rfind("cPlAction", 0) != 0) continue;
                char part[96];
                std::snprintf(part, sizeof(part), " +%zX %s", off, name.c_str() + 9);
                actions += part;
            }
            RM_LOGD("outgoing actions:%s", actions.c_str());
        }
        return static_cast<float>(result.multiplier) * g->settings.devOutgoingMultiplier;
    }

    void EnemyHit(const EnemyHitResult& r) {
        if (g->settings.probeHits || r.killed) {
            // The attacker's class when it is not the player: grenades, mines and barrels are
            // the player's too in a solo run, and these lines tell which classes they are.
            const std::string by = r.attackerIsLocal ? std::string() : " by=" + (r.attacker ? engine::ClassName(r.attacker) : std::string("-"));
            RM_LOGI("enemy_hit enemy=%08X class=%s player=%d local=%d hp %d->%d/%d raw=%d applied=%d killed=%d%s",
                    static_cast<unsigned>(r.enemy), engine::ClassName(r.enemy).c_str(), r.attackerIsPlayer,
                    r.attackerIsLocal, r.hpBefore, r.hpAfter, r.maxHp, r.rawDamage, r.appliedDamage, r.killed, by.c_str());
        }
        if (!r.killed) {
            if (r.attackerIsLocal) {
                std::lock_guard lock(g->mutex);
                if (g_hitSamples.size() < 64) g_hitSamples.push_back({r.enemy, Now() + 0.25});
                g_specialVictims[r.enemy] = Now();  // its death soon after is the player's (fire, a fall)
            }
            return;
        }
        std::lock_guard lock(g->mutex);
        SeatTurn turn(r.attackerIsLocal ? SeatTurn::OfUnit(r.attacker) : 0);
        rm::KillEvent kill;
        kill.now = Now();
        kill.by_player = r.attackerIsLocal;
        if (r.attackerIsLocal) {
            kill.weapon_id = EquippedWeaponBaseId();
            kill.was_melee = engine::InMeleeAction(r.attacker);
        }
        kill.target_is_boss = r.enemy == g->boss.unit;  // before EnemyKillId forgets the boss
        kill.target_hp_max = r.maxHp;
        kill.overkill = std::max(0, r.appliedDamage - r.hpBefore);
        kill.target_frozen = StatusActive(r.enemy, kill.now, &ElementStatus::frozenUntil);
        PlagueOnDeath(r.enemy, r.maxHp, kill.now);
        kill.enemy_id = EnemyKillId(r.enemy, r.maxHp);
        kill.enemy_class = engine::ClassName(r.enemy);
        const auto outcome = g->run->OnEnemyKilled(kill);
        if (!outcome.procs.empty()) QueueProcs(r.enemy, outcome.procs);
        if (outcome.counted) {
            RM_LOGI("kill counted: +%lld supplies, %d remaining%s", static_cast<long long>(outcome.supplies_awarded),
                    outcome.remaining, kill.was_melee.value_or(false) ? " (melee)" : "");
        }
        BuildKillNotice(outcome, kill.now);
    }

    // M8: a build's heals (Rebecca): the core scales the game's heal and may start buffs.
    int HealAmount(std::uintptr_t player, int amount) override {
        (void)player;
        return Guarded("heal", amount, [&] {
            std::lock_guard lock(g->mutex);
            if (!g->run->HasActiveRun()) return amount;
            SeatTurn turn(SeatTurn::OfUnit(player));
            const auto hp = engine::ReadHp(engine::LocalPlayer());
            rm::HealContext ctx;
            ctx.now = Now();
            ctx.amount = amount;
            ctx.hp = hp ? hp->hp : 0;
            ctx.hp_max = hp ? hp->max : 0;
            const rm::HealOutcome out = g->run->OnPlayerHealed(ctx);
            const int applied = static_cast<int>(std::lround(out.amount));
            if (applied != amount || out.high_tier) {
                RM_LOGI("build heal: %d -> %d%s", amount, applied, out.high_tier ? " (big heal: buffs)" : "");
            }
            if (out.high_tier) {
                const auto& build = g->run->state().character.build;
                g->notice = Upper(Plain(build.name.empty() ? std::string("Big heal") : build.name)) + "   UNTOUCHABLE " +
                            std::to_string(static_cast<int>(out.invulnerable_seconds)) + " SEC";
                g->noticeUntil = ctx.now + 3.0;
            }
            return applied;
        });
    }

    IncomingDecision IncomingImpl(const IncomingHit& hit) {
        std::lock_guard lock(g->mutex);
        SeatTurn turn(SeatTurn::OfUnit(hit.player));
        rm::IncomingContext ctx;
        ctx.now = Now();
        ctx.amount = hit.damage;
        ctx.hp = hit.hp;
        ctx.hp_max = hit.maxHp;
        const auto result = g->run->ComputeIncoming(ctx);
        IncomingDecision decision;
        const bool invulnerable = g->settings.devInvulnerable && (g_devMortalSeats & (1u << turn.seat())) == 0;
        decision.multiplier = invulnerable
                                  ? 0.0f
                                  : static_cast<float>(result.multiplier) * g->settings.devIncomingMultiplier;
        if (g->settings.probeHits && g->run->phase() == rm::Phase::ArenaActive) {
            RM_LOGD("incoming core=%.4f (raw %.4f) prevent_death=%d dev=%.2f", result.multiplier,
                    result.raw_multiplier, result.prevent_death, g->settings.devIncomingMultiplier);
        }
        decision.preventDeath = result.prevent_death;
        decision.leaveHp = std::max(1, static_cast<int>(std::lround(result.leave_hp)));
        return decision;
    }

    void PlayerHit(const PlayerHitResult& r) {
        RM_LOGD("player_hit raw=%d applied=%d hp %d->%d/%d", r.rawDamage, r.appliedDamage, r.hpBefore, r.hpAfter,
                r.maxHp);
        if (r.appliedDamage <= 0) return;  // absorbed (developer invulnerability)
        std::lock_guard lock(g->mutex);
        SeatTurn turn(SeatTurn::OfUnit(r.player));
        rm::DamageEvent damage;
        damage.now = Now();
        damage.amount = std::max(0, r.hpBefore - r.hpAfter);
        damage.hp_before = r.hpBefore;
        damage.hp_after = r.hpAfter;
        damage.hp_max = r.maxHp;
        g->run->OnPlayerDamaged(damage);
    }

    void Tick() {
        probe::OnGameTick();
        if (g->settings.devMenuProbe) menuprobe::OnGameTick();
        TickLocked();
        if (g->matchUiOpenRequested) {
            g->matchUiOpenRequested = false;
            matchui::Open(nativemenu::ScreenId::MatchResults);  // builds the screen under the mutex itself
        }
        if (g->shopUiRequested) {
            g->shopUiRequested = false;
            matchui::Open(nativemenu::ScreenId::MatchShop);
        }
        if (g->organizeUiRequested) {
            g->organizeUiRequested = false;
            matchui::Open(nativemenu::ScreenId::MatchOrganize);
        }
    }

    void TickLocked() {
        std::lock_guard lock(g->mutex);
        const double now = Now();
        { ProfileScope p("tick:ObserveMatch"); ObserveMatch(now); }
        if (g->match.inMatch && g->run->HasActiveRun() && g_startMovedGeneration != g->matchGeneration &&
            MoveToMapStart("match start")) {
            g_startMovedGeneration = g->matchGeneration;
        }
        nativemenu::OnGameTick();
        autopilot::OnGameTick();
        if (g->settings.uiDemo) {
            DriveUiDemo(now);
        } else {
            { ProfileScope p("tick:HandleNativeMenu"); HandleNativeMenu(now); }
            { ProfileScope p("tick:WatchArenaMatch"); WatchArenaMatch(now); }
            { ProfileScope p("tick:TryBeginArena"); TryBeginArena(now); }
        }
        { ProfileScope p("tick:UpdateInventoryHold"); UpdateInventoryHold(now); }
        { ProfileScope p("tick:SkipMercIntro"); SkipMercIntro(g->run->HasActiveRun()); }  // a run's match starts without the announcer's intro
        { ProfileScope p("tick:DriveDirector"); DriveDirector(now); }
        { ProfileScope p("tick:DriveBossFall"); DriveBossFall(now); }
        { ProfileScope p("tick:TameBossKills"); TameBossKills(); }
        { ProfileScope p("tick:ReleaseBossHolds"); ReleaseBossHolds(now); }
        { ProfileScope p("tick:WatchEventCamera"); WatchEventCamera(now); }
        { ProfileScope p("tick:DriveArena"); DriveArena(now); }
        { ProfileScope p("tick:DetectEnemyDeaths"); DetectEnemyDeaths(now); }
        { ProfileScope p("tick:OpenChests"); OpenChests(now); }
        // M10: player 1's pad is the mod's primary one; each player's own turn below.
        SetPrimaryPadUser(g->run->HasActiveRun() && g->run->SeatCount() > 1 ? PadUserOfSeat(0) : 0);
        WatchLobbyPads(now);
        { ProfileScope playersScope("tick:players");
        ForEachSeatTurn([&] {
            ApplyHeals();
            DriveSpecial(now);
            DriveGrabBreak(now);
            WatchEquippedWeapon(now);
            ApplyMoveSpeed();
            RefuelFlamethrower(now);
            HideHolsteredFlamethrower(now);
            UpdateFlameStreamLook(now);
            DriveFlameStream(now);
        }); }
        { ProfileScope p("tick:DriveElements"); DriveElements(now); }
        { ProfileScope p("tick:DriveEffectGallery"); DriveEffectGallery(now); }
        { ProfileScope p("tick:UpdateModRedirects"); UpdateModRedirects(); }
        { ProfileScope p("tick:TraceTwinShots"); TraceTwinShots(now); }
        { ProfileScope p("tick:DrivePartner"); DrivePartner(now); }
        { ProfileScope p("tick:SampleHitStates"); SampleHitStates(now); }
        // A run's campaign area keeps the player camera: its camera zones and scripted zone
        // cameras are off (user playtest 2026-11-08, the Volcano).
        areacams::SetRunWantsOff(g->run->HasActiveRun() && CurrentMapKey() >= kFirstCampaignStage);
        stagefix::SetInRun(g->run->HasActiveRun());
        // The run's boss takes gunfire where its own check would refuse it (Uroboros Wesker).
        SetBossTakesGunfire(g->boss.unit);
        // A match a run played in stays a run's match until it is gone, so quitting it writes no
        // save even if the run is settled first.
        static bool runMatch = false;
        if (!g->match.inMatch) runMatch = false;
        else if (g->run->HasActiveRun()) runMatch = true;
        autopilot::SetRunMatch(runMatch);
        // The game's save file is never written in a run's match nor for a while after it (a
        // pause menu quit wrote it with no quit state on the way); a run waiting at the menus
        // holds nothing back (the campaign saves as ever).
        static double runMatchSeenAt = -1.0e9;
        if (runMatch) runMatchSeenAt = now;
        autopilot::SetSaveGuard(g_devSaveGuard || now - runMatchSeenAt < kSaveGuardAfterMatch);
        LogProfile(now);
        SetCoopDeathHold(g->run->HasActiveRun() && g->run->SeatCount() > 1);
        ForEachSeatTurn([&] {
            if (g->run->phase() == rm::Phase::ArenaActive && g->match.inMatch) ApplyBuildHealth();  // gains mid-arena
            // Items earned by perks, rewards and the shop wait until the player is in a match.
            if (g->match.inMatch && engine::LocalPlayer() && g->run->phase() != rm::Phase::Idle && ReadMercTimer().recent) {
                ApplyItemGrants();
                DriveWeaponLevels(now);
            }
        });
        { ProfileScope p("tick:DriveScope"); ForEachSeatTurn([&] { DriveScope(now); }); ApplyScopes(); }
        if (g->settings.developerMode) ProbeScene();
        PollDevCommand(now);
        { ProfileScope p("tick:PollItemBox"); PollItemBox(); }
        { ProfileScope p("tick:SettleFinishedRun"); SettleFinishedRun(); }
        { ProfileScope p("tick:UpdateTimerPolicy"); UpdateTimerPolicy(); }
        { ProfileScope p("tick:UpdatePreloads"); UpdatePreloads(); }
        { ProfileScope p("tick:PlaceArenaChests"); PlaceArenaChests(); }
        { ProfileScope p("tick:UpdateMatchStatus"); UpdateMatchStatus(); }
        const rm::Phase phase = g->run->phase();
        if (phase != g->lastPhase) {
            RM_LOGI("phase %s -> %s", std::string(rm::PhaseName(g->lastPhase)).c_str(),
                    std::string(rm::PhaseName(phase)).c_str());
            if (phase == rm::Phase::ArenaResults) {
                if (g->settings.nativeMatchUi && g->match.inMatch && matchui::IsOpen() == false) {
                    g->matchUiOpenRequested = true;  // opened after the tick releases the mutex
                } else if (!g->settings.nativeMatchUi) {
                    overlay::SetFocused(true);
                }
            }
            if (phase == rm::Phase::ArenaActive) overlay::SetFocused(false);
            g->lastPhase = phase;
        }
    }
};

RogueSink g_sink;

// --- UI (render thread; same thread as the game tick on this build) -----------------

ImVec4 RarityColor(const std::string& rarity) {
    if (rarity == "uncommon") return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    if (rarity == "rare") return ImVec4(0.40f, 0.65f, 1.00f, 1.0f);
    if (rarity == "epic") return ImVec4(0.75f, 0.45f, 1.00f, 1.0f);
    if (rarity == "legendary") return ImVec4(1.00f, 0.65f, 0.15f, 1.0f);
    if (rarity == "exotic") return ImVec4(0.30f, 0.94f, 0.90f, 1.0f);
    return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
}

// Menu hotkeys pressed this frame (overlay::TakeHotkeys bits).
unsigned g_hotkeys = 0;
bool DigitPressed(std::size_t index) { return index < 9 && (g_hotkeys & (1u << (index + 1))) != 0; }
bool EnterPressed() { return (g_hotkeys & overlay::kHotkeyEnter) != 0; }

// Draws offer cards; returns the clicked index or -1. Digit N picks card N.
int OfferCards(const std::vector<rm::OfferView>& offers, const char* buttonLabel, bool showPrice) {
    int clicked = -1;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        const auto& o = offers[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginGroup();
        ImGui::TextColored(RarityColor(o.rarity), "%s", o.title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("[%s%s%s]", o.kind.c_str(), o.rarity.empty() ? "" : ", ", o.rarity.c_str());
        ImGui::TextWrapped("%s", o.detail.c_str());
        if (showPrice) {
            ImGui::Text("Price: %lld supplies", static_cast<long long>(o.price));
        }
        ImGui::BeginDisabled(o.purchased);
        char label[64];
        std::snprintf(label, sizeof(label), "[%zu] %s", i + 1, o.purchased ? "Bought" : buttonLabel);
        if (ImGui::Button(label) || (!o.purchased && DigitPressed(i))) clicked = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }
    return clicked;
}

void DrawHud(const rm::RunSnapshot& s) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##rm_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (s.run_active) {
        ImGui::Text("ROGUE MERCENARIES  %s", s.character_id.c_str());
        ImGui::Text("Arena %d/%d  %s", s.arena, s.arena_count, s.phase_name.c_str());
        if (s.phase == rm::Phase::ArenaActive) {
            ImGui::Text("Enemies left: %d  (%d/%d)", s.remaining, s.kills, s.budget);
        }
        ImGui::Text("Supplies: %lld   Credits: +%lld", static_cast<long long>(s.supplies),
                    static_cast<long long>(s.credits_pending));
        if (s.kill_streak_bonus > 0.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Streak +%.0f%% (%d)", s.kill_streak_bonus * 100.0,
                               s.kill_streak_kills);
        }
        if (!s.loadout.empty()) ImGui::TextWrapped("Loadout: %s", s.loadout.c_str());
        if (s.cheat_death_remaining > 0) ImGui::Text("Second wind: %d", s.cheat_death_remaining);
        if (!s.perks.empty()) {
            std::string line = "Perks:";
            for (const auto& p : s.perks) {
                line += " " + p.name + (p.stacks > 1 ? " x" + std::to_string(p.stacks) : "");
            }
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (s.phase != rm::Phase::ArenaActive) {
            ImGui::TextDisabled("F7: open run menu");
        }
    } else if (g->rogueModePending) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1.0f), "ROGUE MERCENARIES");
        ImGui::TextDisabled(autopilot::Armed() ? "Starting: Solo, random stage, your character"
                                               : "Solo, any stage, your character - the run starts with the match");
    } else if (!(g->settings.developerMode && g->settings.devHideLabel)) {
        ImGui::TextDisabled(g->settings.everythingUnlocked ? "Rogue Mercenaries (everything unlocked) - Play menu or F7" : "Rogue Mercenaries - Play menu or F7");
    }
    for (const auto& toast : g->toasts.Active()) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1.0f), "%s", toast.text.c_str());
    }
    ImGui::End();
}

// Permanent progression: perks and weapons that need Credits before they can
// appear in runs. Only content the adapter can actually support is listed.
void DrawUnlocks() {
    rm::RunController& run = *g->run;
    const rm::Profile& profile = run.profile();
    const auto& caps = run.capabilities();
    struct Entry {
        rm::UnlockKind kind;
        std::string id;
        std::string name;
        std::string detail;
        std::int64_t cost;
    };
    std::vector<Entry> entries;
    for (const auto& perk : run.content().perks()) {
        if (!perk.enabled || perk.unlock_cost_credits <= 0 || profile.unlocked_perks.count(perk.id) ||
            !caps.HasAll(perk.requires_capabilities)) {
            continue;
        }
        entries.push_back({rm::UnlockKind::Perk, perk.id, perk.name, perk.description, perk.unlock_cost_credits});
    }
    for (const auto& weapon : run.content().weapons()) {
        if (!weapon.enabled || weapon.unlock_cost_credits <= 0 || profile.unlocked_weapons.count(weapon.id)) continue;
        entries.push_back({rm::UnlockKind::Weapon, weapon.id, weapon.name, "weapon", weapon.unlock_cost_credits});
    }
    if (entries.empty()) return;
    if (!ImGui::CollapsingHeader("Unlocks (Credits)")) return;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(profile.credits.balance() < e.cost);
        if (ImGui::SmallButton(("Unlock " + std::to_string(e.cost)).c_str())) {
            const auto receipt = run.PurchaseUnlock(e.kind, e.id);
            Report(receipt.ok() ? rm::Status::Success() : receipt.status(), ("unlock " + e.id).c_str());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextWrapped("%s - %s", e.name.c_str(), e.detail.c_str());
        ImGui::PopID();
    }
}

void DrawIdle(const rm::RunSnapshot& s) {
    rm::RunController& run = *g->run;
    ImGui::Text("Credits: %lld", static_cast<long long>(s.credits));
    if (const auto& summary = run.last_summary()) {
        ImGui::Text("Last run %s: %s, %d/%d arenas, %lld kills, +%lld credits", summary->run_id.c_str(),
                    summary->end_reason.c_str(), summary->arenas_cleared, summary->arena_count,
                    static_cast<long long>(summary->total_kills), static_cast<long long>(summary->credits_banked));
    }
    ImGui::SeparatorText("Character");
    const auto characters = run.Characters();
    if (!g->match.localCharacter.empty()) {
        for (std::size_t i = 0; i < characters.size(); ++i) {
            if (characters[i].id == g->match.localCharacter) g->selectedCharacter = static_cast<int>(i);
        }
        ImGui::TextDisabled("In game: %s", g->match.localCharacter.c_str());
    }
    for (std::size_t i = 0; i < characters.size(); ++i) {
        const auto& c = characters[i];
        ImGui::PushID(static_cast<int>(i));
        const bool available = c.gate == rm::CharacterGate::Selectable;
        ImGui::BeginDisabled(!available);
        char label[96];
        std::snprintf(label, sizeof(label), "[%zu] %s", i + 1, c.name.c_str());
        ImGui::RadioButton(label, &g->selectedCharacter, static_cast<int>(i));
        if (available && DigitPressed(i)) g->selectedCharacter = static_cast<int>(i);
        ImGui::EndDisabled();
        if (c.gate == rm::CharacterGate::Locked) {
            ImGui::SameLine();
            if (ImGui::SmallButton(("Unlock (" + std::to_string(c.unlock_cost_credits) + ")").c_str())) {
                const auto receipt = run.PurchaseUnlock(rm::UnlockKind::Character, c.id);
                Report(receipt.ok() ? rm::Status::Success() : receipt.status(), "unlock character");
            }
        } else if (!available) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not available yet)");
        }
        ImGui::PopID();
    }
    DrawUnlocks();
    ImGui::SeparatorText("Run");
    ImGui::TextWrapped(
        "Easiest: choose ROGUE MERCENARIES in the game's Play menu - the run starts with your first match. "
        "Or start a run here, then pick the same character in The Mercenaries. Each match is one arena: defeat "
        "the required enemies, pick a reward, shop and choose the next route.");
    static char seedText[24] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("Seed (optional)", "random", seedText, sizeof(seedText),
                             ImGuiInputTextFlags_CharsDecimal);
    if ((ImGui::Button("[Enter] Start run") || EnterPressed()) &&
        g->selectedCharacter < static_cast<int>(characters.size())) {
        std::uint64_t seed = std::strtoull(seedText, nullptr, 10);
        if (seed == 0) {
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            seed = static_cast<std::uint64_t>(counter.QuadPart) ^ 0x9E3779B97F4A7C15ull;
        }
        run.SetArenaPool(RotationMaps(), CurrentMapKey());
        Report(run.StartRun(characters[g->selectedCharacter].id, seed), "start run");
        for (const auto& perk : g->settings.devGrantPerks) {
            Report(run.DevGrantPerk(perk), ("dev grant " + perk).c_str());
        }
        if (run.HasActiveRun()) overlay::SetFocused(false);
    }
}

void DrawRunPanel(const rm::RunSnapshot& s) {
    rm::RunController& run = *g->run;
    ImGui::Text("%s  seed %llu  character %s", s.run_id.c_str(), static_cast<unsigned long long>(run.state().seed),
                s.character_id.c_str());
    ImGui::Text("Arena %d/%d  phase %s  route %s", s.arena, s.arena_count, s.phase_name.c_str(), s.route_name.c_str());
    ImGui::Text("Supplies %lld  pending credits %lld  rerolls %d", static_cast<long long>(s.supplies),
                static_cast<long long>(s.credits_pending), s.reroll_tokens);

    switch (s.phase) {
        case rm::Phase::Idle:
        case rm::Phase::ArenaActive:
            if (s.phase == rm::Phase::Idle) {
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                                   g->match.inMatch ? "Starting..." : "Start a Mercenaries match to begin arena 1.");
                // Lets testers without The Mercenaries unlocked exercise the
                // combat hooks in story mode (no countdown there).
                if (g->settings.developerMode && ImGui::Button("Begin arena now (dev, any mode)")) {
                    Report(run.BeginArena(Now()), "begin arena");
                    if (run.phase() == rm::Phase::ArenaActive) overlay::SetFocused(false);
                }
            } else {
                ImGui::Text("Enemies left: %d (%d/%d)", s.remaining, s.kills, s.budget);
            }
            break;
        case rm::Phase::ArenaResults:
            if (s.last_result) {
                const auto& r = *s.last_result;
                ImGui::SeparatorText("Arena cleared");
                ImGui::Text("Rating %s   time %.1f s   kills %d   damage taken %.0f%s", std::string(rm::RatingName(r.rating)).c_str(),
                            r.time_seconds, r.kills, r.damage_taken, r.no_damage ? "  (no damage!)" : "");
                ImGui::Text("Supplies +%lld   credits +%lld", static_cast<long long>(r.supplies_awarded),
                            static_cast<long long>(r.credits_accrued));
            }
            if (ImGui::Button("[Enter] Continue to rewards") || EnterPressed()) {
                Report(run.ContinueToRewards(), "continue");
            }
            break;
        case rm::Phase::RewardSelection: {
            ImGui::SeparatorText("Choose one reward");
            const int pick = OfferCards(s.reward_offers, "Take", false);
            if (pick >= 0) Report(run.ChooseReward(static_cast<std::size_t>(pick)), "choose reward");
            if (s.reroll_tokens > 0 && ImGui::Button("Reroll offer")) Report(run.RerollReward(), "reroll");
            break;
        }
        case rm::Phase::Shop: {
            ImGui::SeparatorText("Shop");
            const int pick = OfferCards(s.shop_offers, "Buy", true);
            if (pick >= 0) Report(run.Purchase(static_cast<std::size_t>(pick)), "purchase");
            if (ImGui::Button("[Enter] Leave shop") || EnterPressed()) Report(run.LeaveShop(), "leave shop");
            break;
        }
        case rm::Phase::RouteSelection: {
            if (!run.state().route_chosen) {
                ImGui::SeparatorText("Choose the next route");
                const int pick = OfferCards(s.route_offers, "Take route", false);
                if (pick >= 0) {
                    const rm::Status status = run.ChooseRoute(static_cast<std::size_t>(pick));
                    Report(status, "choose route");
                    if (status.ok()) {
                        // Hand input back so the player can press Retry.
                        overlay::SetFocused(false);
                        g->toasts.Push("Press Retry to start arena " + std::to_string(s.arena + 1));
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                                   "Route set. Press Retry (or start a new match) to begin arena %d.", s.arena + 1);
                if (g->settings.developerMode && ImGui::Button("Begin arena now (dev)")) {
                    Report(run.BeginArena(Now()), "begin arena");
                }
            }
            break;
        }
        default:
            break;
    }

    if (!s.perks.empty()) {
        ImGui::SeparatorText("Perks");
        for (const auto& p : s.perks) {
            ImGui::TextColored(RarityColor(p.rarity), "%s", p.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s x%d", p.category.c_str(), p.stacks);
        }
    }
    if (!s.weapons.empty()) {
        ImGui::SeparatorText("Weapons");
        for (const auto& w : s.weapons) {
            ImGui::TextColored(RarityColor(w.rarity), "%s Lv%d", w.name.c_str(), w.level);
            for (const auto& a : w.affixes) ImGui::BulletText("%s", a.c_str());
        }
    }
    ImGui::Separator();
    // Two clicks within 3 s: a stray click must never end a run.
    static double armedAt = -10.0;
    const bool armed = Now() - armedAt < 3.0;
    if (ImGui::Button(armed ? "Click again to abandon the run" : "Abandon run")) {
        if (armed) {
            Report(run.AbandonRun(Now()), "abandon run");
            armedAt = -10.0;
        } else {
            armedAt = Now();
        }
    }
}

void DrawDeveloper(const rm::RunSnapshot& s) {
    if (!ImGui::CollapsingHeader("Developer")) return;
    probe::DrawDeveloperPanel();
    ImGui::Text("capabilities: %s", g->run->capabilities().ToString().c_str());
    ImGui::Text("match: in=%d mode=%d stage=%d timer=%.1f local=%s hp=%d/%d", g->match.inMatch, g->match.mode,
                g->match.stage, g->match.timer, g->match.localClass.c_str(), g->match.localHp, g->match.localMaxHp);
    if (s.phase == rm::Phase::ArenaActive && ImGui::Button("Force arena complete")) {
        // Feeds synthetic player kills so the normal completion path runs.
        for (int i = 0; i < s.remaining; ++i) {
            rm::KillEvent kill;
            kill.now = Now();
            kill.by_player = true;
            kill.enemy_id = "dev_" + std::to_string(i) + "_" + std::to_string(static_cast<long long>(Now() * 1000));
            g->run->OnEnemyKilled(kill);
        }
    }
    if (ImGui::Button("Export diagnostics")) {
        const auto zip = g->paths.dataDir / L"rogue-mercenaries-diagnostics.zip";
        std::string summary = std::string("mod ") + kModVersion + " adapter " + kAdapterVersion +
                              "\ncapabilities " + g->run->capabilities().ToString() + "\n";
        const auto report = rm::ExportDiagnostics(zip, g->paths.logDir, g->paths.configDir, {}, summary);
        g->diagnosticsResult = report.status.ok() ? ("written " + rm::PathText(zip)) : report.status.message;
    }
    if (!g->diagnosticsResult.empty()) ImGui::TextWrapped("%s", g->diagnosticsResult.c_str());
}

}  // namespace

void ModInit(const Paths& paths, const Settings& settings) {
    g = new ModState();
    g->paths = paths;
    g->settings = settings;
    if (settings.customEffects) effectspack::Start(paths);

    auto loaded = rm::Content::LoadFromDirectory(paths.configDir);
    icons::SetDirectory(paths.configDir / L"icons");
    portraits::SetGameDirectory(paths.gameDir);
    LoadJoypadMapping();
    for (const auto& w : loaded.warnings) RM_LOGW("content: %s", w.c_str());
    for (const auto& e : loaded.errors) RM_LOGE("content: %s", e.c_str());
    g->contentErrors = loaded.errors;

    rm::JsonlOptions jsonl;
    jsonl.directory = paths.logDir;
    jsonl.max_level = settings.logLevel == "debug" ? rm::EventLevel::Debug : rm::EventLevel::Info;
    g->jsonl = std::make_unique<rm::JsonlSink>(jsonl);
    g->fanout = std::make_unique<rm::FanoutSink>();
    g->fanout->AddSink(g->jsonl.get());
    g->fanout->AddSink(&g->toasts);

    if (!loaded.ok()) {
        RM_LOGE("content invalid; roguelite disabled (diagnostics only)");
        return;
    }
    g->content = loaded.content;
    auto profilePath = paths.dataDir / std::string(rm::ProfileFileName(settings.everythingUnlocked));
    if (settings.uiDemo) {
        // Fresh throwaway profile; the player's real profile is never touched.
        const auto sandbox = paths.dataDir / L"dev-sandbox";
        std::error_code ec;
        std::filesystem::remove_all(sandbox, ec);
        std::filesystem::create_directories(sandbox, ec);
        profilePath = sandbox / L"profile.json";
        RM_LOGW("dev: UI demo mode on sandbox profile %s", rm::PathText(profilePath).c_str());
    }
    g->store = std::make_unique<rm::ProfileStore>(profilePath, g->fanout.get());
    auto profile = g->store->Load();
    if (settings.everythingUnlocked) {
        rm::UnlockEverything(profile.profile, *g->content);
        RM_LOGI("profile: everything unlocked, on its own file (%s); the normal progress is not used", rm::PathText(profilePath).c_str());
    }
    RM_LOGI("profile loaded from %s (credits=%lld)", std::string(rm::ProfileSourceName(profile.source)).c_str(),
            static_cast<long long>(profile.profile.credits.balance()));
    rm::RunControllerOptions options;
    options.sink = g->fanout.get();
    options.store = g->store.get();
    g->run = std::make_unique<rm::RunController>(g->content, std::move(profile.profile), options);
    if (g->run->CanResumeRun()) {
        // A run with a point to go on from waits for CONTINUE RUN (START RUN settles it instead).
        RM_LOGI("interrupted run %s kept: CONTINUE RUN goes on with it", g->run->interrupted_run()->run_id.c_str());
    } else if (g->run->interrupted_run()) {
        const auto recovered = g->run->RecoverInterruptedRun();
        RM_LOGI("interrupted run settled: %s", recovered.ok() ? recovered->run_id.c_str() : "failed");
    }
}

void ModCapabilitiesReady() {
    if (!g || !g->run) return;
    std::lock_guard lock(g->mutex);
    g->run->SetCapabilities(VerifiedCapabilities());
    g->run->SetInventory(&g_inventoryBridge);
    RM_LOGI("core capabilities: %s", g->run->capabilities().ToString().c_str());
    ScanModSkins();
}

void ModShutdown() {
    if (g && g->jsonl) g->jsonl->Flush();
}

HookSink* ModSink() { return g && g->run ? static_cast<HookSink*>(&g_sink) : nullptr; }

void DrawDisabled() {
    ImGui::Begin("Rogue Mercenaries - disabled", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("The mod stopped itself after an internal error; the game is unmodified now.");
    ImGui::TextWrapped("%s", g->disabledReason.c_str());
    ImGui::TextDisabled("See %s", rm::PathText((g->paths.logDir / L"adapter.log")).c_str());
    ImGui::End();
}

// The icons of the Play menu's mod rows, left of each label: drawn over the finished frame in the
// label's colour and alpha. The game fits its 1280x720 canvas into the back buffer at one scale,
// centred (3440x1440: x 2.0 from 440, rows centred on x 1720).
// A label's width in layout units is scale x (kGlyphUnit x its glyph widths + kGlyphSpacing per
// glyph): fitted to the perk lists at 1920x1080 (cursor rows exact, the others about 4 % wide).
// Labels are centred on their x and y.
constexpr float kGlyphUnit = 0.1907f;
constexpr float kGlyphSpacing = 5.64f;
constexpr float kIconSize = 34.0f;  // layout units at scale 1
constexpr float kIconGap = 10.0f;

void DrawMenuIcons() {
    IDirect3DDevice9* device = overlay::FrameDevice();
    const auto rows = nativemenu::VisibleRowIcons();
    if (!device || rows.empty()) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    for (const nativemenu::RowIcon& r : rows) {
        const auto texture = icons::Texture(device, r.icon);
        if (!texture) continue;
        const float half = (static_cast<float>(r.textWidth) * kGlyphUnit + static_cast<float>(r.glyphs) * kGlyphSpacing) * r.scale * 0.5f;
        const float size = kIconSize * r.scale;
        const float right = static_cast<float>(r.x) - half - kIconGap * r.scale;
        const float top = static_cast<float>(r.y) - size * 0.5f;
        const std::uint32_t a = r.argb >> 24, red = (r.argb >> 16) & 0xFF, green = (r.argb >> 8) & 0xFF, blue = r.argb & 0xFF;
        list->AddImage(static_cast<ImTextureID>(texture), ImVec2(ox + (right - size) * k, oy + top * k), ImVec2(ox + right * k, oy + (top + size) * k),
                       ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(red, green, blue, a));
    }
}

// The bust that goes with the cursor row, on the left of the Play menu as on the game's own
// character select: same canvas mapping as the icons, the cut-off bottom faded out.
constexpr float kPortraitLeft = -40.0f;  // canvas units
constexpr float kPortraitTop = 96.0f;
constexpr float kPortraitHeight = 440.0f;
constexpr float kPortraitFade = 0.3f;    // of the height, at the bottom

void DrawMenuPortrait() {
    IDirect3DDevice9* device = overlay::FrameDevice();
    const int costume = nativemenu::CursorPortrait();
    if (!device || costume < 0) return;
    const portraits::Portrait p = portraits::Get(device, costume);
    if (!p.texture || p.height <= 0) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    const float w = kPortraitHeight * static_cast<float>(p.width) / static_cast<float>(p.height);
    const float x0 = ox + kPortraitLeft * k, x1 = ox + (kPortraitLeft + w) * k;
    const float y0 = oy + kPortraitTop * k, y2 = oy + (kPortraitTop + kPortraitHeight) * k;
    const float y1 = y0 + (y2 - y0) * (1.0f - kPortraitFade);
    const float v1 = 1.0f - kPortraitFade;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    list->PushTextureID(static_cast<ImTextureID>(p.texture));
    list->PrimReserve(12, 8);
    const ImU32 solid = IM_COL32(255, 255, 255, 255), clear = IM_COL32(255, 255, 255, 0);
    // Two quads: the upper part opaque, the lower part from opaque to clear.
    const struct { float ya, yb, va, vb; ImU32 ca, cb; } parts[] = {{y0, y1, 0.0f, v1, solid, solid}, {y1, y2, v1, 1.0f, solid, clear}};
    for (const auto& q : parts) {
        const auto base = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
        list->PrimWriteVtx(ImVec2(x0, q.ya), ImVec2(0.0f, q.va), q.ca);
        list->PrimWriteVtx(ImVec2(x1, q.ya), ImVec2(1.0f, q.va), q.ca);
        list->PrimWriteVtx(ImVec2(x1, q.yb), ImVec2(1.0f, q.vb), q.cb);
        list->PrimWriteVtx(ImVec2(x0, q.yb), ImVec2(0.0f, q.vb), q.cb);
        for (const int i : {0, 1, 2, 0, 2, 3}) list->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
    }
    list->PopTextureID();
}

void DrawUnguarded();

void ModDraw() {
    if (!g) return;
    if (g->disabled.load()) {
        DrawDisabled();
        return;
    }
    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);
    const bool ok = Guarded("ModDraw", false, [] {
        DrawUnguarded();
        return true;
    });
    if (!ok) {
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
    }
}

// Each arc over the picture: its two ends where the game's camera shows them (rm::screenfx, the
// game's own maths), a jagged path between them that changes every 50 ms, a wide faint glow and a
// thin bright core, fading out at its end. Once a match the camera is logged as read.
void DrawElementArcsIn(const engine::CameraView& cam, float x0, float y0, float width, float height, double now);

// The arcs in every view: one in solo, each player's on their half of a split screen (M10).
void DrawElementArcs() {
    if (g_arcs.empty()) return;
    const double now = Now();
    std::erase_if(g_arcs, [now](const ElementArc& a) { return now >= a.until; });
    if (g_arcs.empty() || !g->match.inMatch) {
        g_arcs.clear();
        return;
    }
    static int loggedGeneration = -1;
    const auto views = engine::ReadCameraViews();
    if (views.empty()) {
        if (loggedGeneration != g->matchGeneration) {
            loggedGeneration = g->matchGeneration;
            RM_LOGW("arcs: no camera to draw them with (sCamera's viewports); not drawn");
        }
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    for (const engine::CameraView& cam : views) {
        // A viewport's region (x0 y0 x1 y1 in screen pixels; none: the whole screen).
        const bool region = cam.region[2] > cam.region[0] && cam.region[3] > cam.region[1];
        const float x0 = region ? static_cast<float>(cam.region[0]) : 0.0f;
        const float y0 = region ? static_cast<float>(cam.region[1]) : 0.0f;
        const float w = region ? static_cast<float>(cam.region[2] - cam.region[0]) : display.x;
        const float h = region ? static_cast<float>(cam.region[3] - cam.region[1]) : display.y;
        if (views.size() > 1 && !region) continue;
        list->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), true);
        DrawElementArcsIn(cam, x0, y0, w, h, now);
        list->PopClipRect();
    }
    if (loggedGeneration != g->matchGeneration) {
        loggedGeneration = g->matchGeneration;
        RM_LOGI("arcs: drawn in %zu view(s)", views.size());
    }
}

void DrawElementArcsIn(const engine::CameraView& cam, float x0, float y0, float width, float height, double now) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    static int loggedViews = 0;
    if (loggedViews < 4) {
        ++loggedViews;
        RM_LOGI("arcs: camera %s %08X (viewport %d, region %d %d %d %d): eye %.0f %.0f %.0f, target %.0f %.0f %.0f, up %.2f %.2f %.2f, "
                "fov %.1f, aspect %.3f, near %.1f, far %.0f; screen %.0f x %.0f",
                engine::ClassName(cam.camera).c_str(), static_cast<unsigned>(cam.camera), cam.viewport, cam.region[0], cam.region[1],
                cam.region[2], cam.region[3], cam.eye[0], cam.eye[1], cam.eye[2], cam.target[0], cam.target[1], cam.target[2], cam.up[0],
                cam.up[1], cam.up[2], cam.fov, cam.aspect, cam.nearPlane, cam.farPlane, display.x, display.y);
    }
    const rm::screenfx::Mat4 viewProj =
        rm::screenfx::Multiply(rm::screenfx::LookAtRH(cam.eye, cam.target, cam.up),
                               rm::screenfx::PerspectiveFovRH(cam.fov, cam.aspect, cam.nearPlane, cam.farPlane));
    const auto player = engine::LocalPlayer();
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    static int drawnLogs = 0;
    for (const ElementArc& arc : g_arcs) {
        float a[3], b[3];
        if (arc.from) {
            if (engine::ReadHp(arc.from)) UnitPosition(arc.from, a);
            else std::copy(std::begin(arc.a), std::end(arc.a), a);
            a[1] += kArcHeight;
        } else {
            // The player's gun: chest high, half a metre ahead.
            const std::uintptr_t owner = arc.gunOwner ? arc.gunOwner : player;
            if (!owner) continue;
            UnitPosition(owner, a);
            float fx = 0.0f, fz = 1.0f;
            PlayerFacing(owner, fx, fz);
            a[0] += fx * 50.0f;
            a[2] += fz * 50.0f;
            a[1] += kArcHeight + 10.0f;
        }
        if (engine::ReadHp(arc.to)) UnitPosition(arc.to, b);
        else std::copy(std::begin(arc.b), std::end(arc.b), b);
        b[1] += kArcHeight;
        auto pa = rm::screenfx::Project(viewProj, a, width, height);
        auto pb = rm::screenfx::Project(viewProj, b, width, height);
        if (!pa || !pb) continue;
        pa->x += x0;
        pa->y += y0;
        pb->x += x0;
        pb->y += y0;
        if (drawnLogs < 3) {
            ++drawnLogs;
            RM_LOGI("arcs: %s -> %s at %.0f %.0f -> %.0f %.0f on the screen", arc.from ? engine::ClassName(arc.from).c_str() : "the gun",
                    engine::ClassName(arc.to).c_str(), pa->x, pa->y, pb->x, pb->y);
        }
        const float life = static_cast<float>((arc.until - now) / std::max(0.01, arc.until - arc.born));
        const auto alpha = [life](float a0) { return static_cast<int>(std::clamp(a0 * std::min(1.0f, life * 2.0f), 0.0f, 255.0f)); };
        const std::uint32_t flicker = static_cast<std::uint32_t>(now * 20.0);  // a new shape every 50 ms
        const float length = std::hypot(pb->x - pa->x, pb->y - pa->y);
        const int segments = std::clamp(static_cast<int>(length / 18.0f), 4, 24);
        const float closest = std::min(pa->depth, pb->depth), thick = std::clamp(900.0f / std::max(closest, 1.0f), 1.5f, 6.0f);
        const auto path = rm::screenfx::BoltPath(pa->x, pa->y, pb->x, pb->y, segments, 0.12f, arc.seed ^ (flicker * 2654435761u));
        std::vector<ImVec2> points;
        points.reserve(path.size());
        for (const auto& [x, y] : path) points.emplace_back(x, y);
        list->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(120, 90, 255, alpha(70.0f)), 0, thick * 4.0f);
        list->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(170, 150, 255, alpha(140.0f)), 0, thick * 1.8f);
        list->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(235, 235, 255, alpha(255.0f)), 0, thick * 0.6f);
        // A short fork from a third of the way along.
        const std::size_t at = points.size() / 3;
        if (at > 0 && at + 1 < points.size()) {
            const ImVec2 o = points[at];
            const float dx = points[at + 1].x - o.x, dy = points[at + 1].y - o.y;
            const auto fork = rm::screenfx::BoltPath(o.x, o.y, o.x + dx * 2.5f - dy * 1.2f, o.y + dy * 2.5f + dx * 1.2f, 4, 0.2f,
                                                     arc.seed ^ flicker ^ 0x5bd1e995u);
            std::vector<ImVec2> branch;
            for (const auto& [x, y] : fork) branch.emplace_back(x, y);
            list->AddPolyline(branch.data(), static_cast<int>(branch.size()), IM_COL32(170, 150, 255, alpha(110.0f)), 0, thick * 0.9f);
        }
    }
}

void DrawUnguarded() {
    if (!g->run) {
        ImGui::Begin("Rogue Mercenaries - disabled");
        ImGui::TextUnformatted("Content failed to load:");
        for (const auto& e : g->contentErrors) ImGui::BulletText("%s", e.c_str());
        ImGui::End();
        return;
    }
    DrawMenuPortrait();
    DrawMenuIcons();
    std::lock_guard lock(g->mutex);
    { ProfileScope p("draw:DrawElementArcs"); DrawElementArcs(); }
    { ProfileScope p("draw:DrawSplitHud"); DrawSplitHud(); }
    { ProfileScope p("draw:DrawSpecialWidgets"); DrawSpecialWidgets(); }
    if (g->run->SeatCount() > 1) {
        ForEachSeatTurn([] {  // M10: each player's card on their half
            g_drawShiftY = g->run->ActiveSeat() == 0 ? -kSplitHalfLines : 0.0f;
            DrawWeaponCard();
        });
        g_drawShiftY = 0.0f;
    } else {
        DrawWeaponCard();
    }
    ProfileScope snapScope("draw:snapshot");
    // The run's snapshot only when the overlay draws with it: its own HUD (no native one) or its window.
    const bool overlayHud = !(g->settings.nativeMatchUi && g->match.inMatch && g->run->HasActiveRun());
    if (!overlayHud && !overlay::IsFocused()) return;
    const rm::RunSnapshot snapshot = g->run->Snapshot(Now());
    if (overlayHud) DrawHud(snapshot);
    if (!overlay::IsFocused()) return;
    g_hotkeys = overlay::TakeHotkeys();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Re-centred every frame: the window auto-sizes as panels change.
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(720.0f, vp->Size.y * 0.85f));
    ImGui::Begin("Rogue Mercenaries", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    if (snapshot.run_active) {
        DrawRunPanel(snapshot);
    } else {
        DrawIdle(snapshot);
    }
    if (!g->lastStatus.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", g->lastStatus.c_str());
    }
    if (g->settings.developerMode) DrawDeveloper(snapshot);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Keys: 1-9 pick, Enter confirms, arrows/Space navigate, F7 closes. The game gets no input "
                       "while this menu is open.");
    ImGui::PopStyleColor();
    ImGui::End();
}

}  // namespace rmre5
