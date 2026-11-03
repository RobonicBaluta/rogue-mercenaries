// Roguelite mod for RE5: drives rm::RunController from engine hooks and draws
// the run UI. Arenas map onto Mercenaries matches: a match starting while the
// run waits for its next arena begins that arena; reaching the kill budget
// completes it and (optionally) ends the match through the game's own timer.

#include <windows.h>
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

#include "engine.h"
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
#include "rm/profile.h"
#include "rm/rosters.h"
#include "rm/run_controller.h"
#include "rm/telemetry.h"

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
    } boss;
    struct StageTransition {
        bool pending = false;
        int generation = 0;     // matchGeneration when the match end was requested
        double startedAt = 0.0;
        std::vector<std::pair<std::uint32_t, int>> carry;  // (item id, count) per slot record
        std::array<std::uint32_t, 9> cells{};             // item id per grid cell (ORGANIZE layout)
        int hp = -1;            // player health at the transition (-1 = unknown)
    } transition;
    bool autoOpenedForPhase = false;
    rm::Phase lastPhase = rm::Phase::Idle;
    std::string lastStatus;
    std::uint64_t seedInput = 0;
    int selectedCharacter = 0;
    std::string selectedSkin;  // skin id for the selected character (empty = default)
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
        grid[static_cast<std::size_t>(cell)] = GridCell{slot, ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0),
                                                        ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0)};
    }
    return grid;
}

std::uintptr_t ItemRecord(std::uint32_t id) {
    const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((id >> 8) & 0xFF) * 4, 0);
    return table ? table + (id & 0xFF) * 0x48 : 0;
}

int ItemStackMax(std::uint32_t id) {
    const auto record = ItemRecord(id);
    if (!record || ReadOr<std::int32_t>(record + 0x30, 0) == 3) return 0;  // weapons do not stack
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
        if (!record || ReadOr<std::int32_t>(record + 0x30, 0) == 3) return;
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
};
RunWeaponLevels g_weaponLevels;

std::map<std::uint32_t, WeaponLevels>& RunLevels() {
    if (g_weaponLevels.runId != g->run->state().run_id) {
        g_weaponLevels.runId = g->run->state().run_id;
        g_weaponLevels.byItem.clear();
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

void ApplyWeaponLevels() {
    if (!g->run->HasActiveRun()) return;
    auto& levels = RunLevels();
    for (const GridCell& c : ReadGrid()) {
        const auto it = c.record ? levels.find(c.id) : levels.end();
        if (it == levels.end() || SlotLevels(c.record) == it->second) continue;
        SafeStore(c.record + re5::off::kSlotUpgrades, it->second[0]);
        SafeStore(c.record + re5::off::kSlotUpgrades + 4, it->second[1]);
        RM_LOGI("upgrades: %04X levels %08X %08X", c.id, it->second[0], it->second[1]);
    }
}

// Name of an inventory item for the game font: the run's weapons and catalog first.
std::string GridItemName(std::uint32_t id) {
    for (const rm::WeaponBaseDef& w : g->content->weapons()) {
        if (w.game_item == static_cast<std::int64_t>(id)) return w.name;
    }
    for (const rm::EquipmentDef& e : g->content->loadouts().equipment) {
        if (e.game_item == static_cast<std::int64_t>(id)) return e.name;
    }
    switch (id) {
        case 0x0101: case 0x010E: case 0x0123: case 0x0124: return "Knife";
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

// A random map of the rotation other than `avoid`; never a boss arena (M6: those end a run's
// map).
int PickMap(int avoid) {
    if (g_forcedNextMap >= 0) return std::exchange(g_forcedNextMap, -1);
    std::vector<int> keys = RotationMaps();
    std::erase_if(keys, [](int key) {
        const rm::MapDef* m = g->content->FindMapByStage(key);
        return m != nullptr && m->boss.has_value();
    });
    if (keys.size() > 1) std::erase(keys, avoid);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return keys[static_cast<std::size_t>(static_cast<std::uint64_t>(counter.QuadPart) % keys.size())];
}

// Arms the autopilot for the next match on map `key`: a Mercenaries stage directly, a campaign
// area through Mercenaries stage 0 with the host loading its stage number instead.
void ArmMap(int key, int costume) {
    const bool campaign = key >= kFirstCampaignStage;
    g->currentStage = key;
    host::SetStageOverride(campaign ? key : -1);
    autopilot::Arm({campaign ? 0 : key, costume, std::max(1, g->settings.stageCount)});
}

// The director map of the stage being played. maps.json keys Mercenaries maps by their stage
// index (0..7) and campaign areas by their stage number (100 and up), which the host loads in
// place of the chosen Mercenaries stage (M5).
int CurrentMapKey() { return host::StageOverride() >= 0 ? host::StageOverride() : g->currentStage; }
const rm::MapDef* CurrentMap() { return g->content->FindMapByStage(CurrentMapKey()); }

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
    return true;
}

// Rarity colours (0 = common: the game's own colour).
std::uint32_t RarityColour(std::string_view rarity) {
    if (rarity == "uncommon") return 0xFF7CDC7Cu;
    if (rarity == "rare") return 0xFF5CA8FFu;
    if (rarity == "epic") return 0xFFC884FFu;
    if (rarity == "legendary") return 0xFFFFB040u;
    return 0;
}

// Help line for a reward or shop offer in the game font (letters, digits, space, comma, period).
std::string OfferDetail(const rm::OfferView& o) {
    if (!o.detail.empty()) return Plain(o.detail);
    if (o.kind == "heal") return "Restores half of your health.";
    if (o.kind == "upgrade") return "One more damage level for this weapon.";
    if (o.kind == "weapon") return "A new weapon for the run.";
    if (o.kind == "supplies") return "Supplies to spend in the shop.";
    if (o.kind == "reroll") return "Reroll the next reward offer once.";
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

// The costume slot a character would wear: its chosen skin if it is the selected character,
// else its default.
int CostumeOf(const std::vector<rm::CharacterAvailability>& characters, std::size_t index) {
    const auto& c = characters[index];
    const rm::SkinAvailability* skin = static_cast<int>(index) == g->selectedCharacter ? SelectedSkin(c) : nullptr;
    return static_cast<int>(skin && skin->costume_index >= 0 ? skin->costume_index : c.costume_index);
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
    screen.portrait = costume;
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
        add(std::to_string(s.arenas_cleared) + " OF " + std::to_string(s.arena_count) + " ARENAS CLEARED",
            std::string(s.end_reason == "died" ? "Killed" : "Ended") + " in arena " + std::to_string(reached) +
                (s.final_arena.empty() ? "." : ", " + Plain(s.final_arena) + "."),
            "last_stand");
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

// Screens of the native menu, built from the run controller. Called on the
// game's menu thread, so everything happens under the mod mutex.
class MenuModel final : public nativemenu::Model {
public:
    nativemenu::Screen Build(nativemenu::ScreenId id) override {
        std::lock_guard lock(g->mutex);
        return Guarded("menu build", nativemenu::Screen{}, [&] { return BuildLocked(id); });
    }

    nativemenu::Decision Decide(nativemenu::ScreenId id, int index) override {
        std::lock_guard lock(g->mutex);
        return Guarded("menu decide", nativemenu::Decision{}, [&] { return DecideLocked(id, index); });
    }

private:
    using Item = nativemenu::Item;
    using Outcome = nativemenu::Outcome;
    using ScreenId = nativemenu::ScreenId;

    std::string CreditsLine() const { return Plural(g->run->profile().credits.balance(), "credit"); }

    nativemenu::Screen BuildLocked(ScreenId id) {
        rm::RunController& run = *g->run;
        const rm::Profile& profile = run.profile();
        nativemenu::Screen screen;
        switch (id) {
            case ScreenId::Main:
                screen.header = "ROGUE MERCENARIES   " + Upper(CreditsLine());
                screen.portrait = SelectedCostume(run);
                screen.items = {
                    {"START RUN", "Solo Mercenaries on any stage. Pick your character,\nthe run starts with the match.",
                     false, 0, "crosshair"},
                    {"CHARACTERS", "Choose who you play. Locked characters show\nhow to unlock them.", false, 0, "person"},
                    {"PERKS", "Permanent perks. Locked ones are bought with credits.", false, 0, "medal"},
                    {"CREDITS", "Your credits, lifetime records and weapon unlocks.", false, 0, "credits"},
                };
                break;
            case ScreenId::RunResults:
                screen = RunResultsScreen(run);
                break;
            case ScreenId::Characters: {
                screen.header = "CHARACTERS";
                const auto characters = run.Characters();
                for (std::size_t i = 0; i < characters.size(); ++i) {
                    const auto& c = characters[i];
                    Item item;
                    item.label = Upper(c.name);
                    item.portrait = CostumeOf(characters, i);
                    if (c.gate == rm::CharacterGate::Selectable) {
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
                screen.portrait = SelectedCostume(run);
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
                screen.portrait = SelectedCostume(run);
                const rm::PerkDef* equipped = character.empty() ? nullptr : run.EquippedPerk(character, g->perkCategory);
                for (const rm::PerkDef* perk : MenuPerks(run, g->perkCategory)) {
                    Item item;
                    item.label = Upper(Plain(perk->name));
                    const bool locked = perk->unlock_cost_credits > 0 && !profile.unlocked_perks.count(perk->id);
                    const bool selected = equipped != nullptr && equipped->id == perk->id;
                    if (selected) item.label += "   SELECTED";
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
            case ScreenId::MatchRewards: {
                const rm::RunSnapshot s = run.Snapshot(Now());
                screen.header = "CHOOSE A REWARD";
                screen.empty_help = "Nothing to choose. Press to continue.";
                for (const auto& o : s.reward_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title));
                    item.help = OfferDetail(o);
                    item.color = RarityColour(o.rarity);
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
                screen.header = "SHOP   " + std::to_string(s.supplies) + " SUPPLIES";
                for (const auto& o : s.shop_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title)) + (o.purchased ? "   BOUGHT" : "");
                    item.help = OfferDetail(o) + "\nPrice " + std::to_string(o.price) + " supplies.";
                    item.color = RarityColour(o.rarity);
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
                organize.help = NativeItemBoxReady() ? "Arrange, combine and sell items in the game screen.\nBuying and upgrades stay in this SHOP."
                                                     : "Rearrange the nine cells of your item grid.";
                screen.items.push_back(std::move(organize));
                Item leave;
                leave.label = "LEAVE SHOP";
                leave.help = "On to the next route.";
                screen.items.push_back(std::move(leave));
                break;
            }
            case ScreenId::MatchOrganize: {
                const auto grid = ReadGrid();
                const int pick = g->organizePick;
                const bool picked = pick >= 0 && pick < kGridCells && grid[static_cast<std::size_t>(pick)].record;
                screen.header = picked ? "MOVE " + Upper(Plain(GridItemName(grid[static_cast<std::size_t>(pick)].id))) + " TO"
                                       : "ORGANIZE   PICK AN ITEM";
                for (int cell = 0; cell < kGridCells; ++cell) {
                    const GridCell& c = grid[static_cast<std::size_t>(cell)];
                    Item item;
                    item.label = std::to_string(cell + 1) + "   " +
                                 (c.record ? Upper(Plain(GridItemName(c.id))) + (c.count > 1 ? "  " + std::to_string(c.count) : "")
                                           : std::string("EMPTY"));
                    if (picked && cell == pick) {
                        item.label += "   PICKED";
                        item.color = 0xFFFFD27Fu;
                        item.help = "Press again to put it back.";
                    } else if (picked) {
                        item.help = c.record ? "Press to swap the two items." : "Press to move it here.";
                    } else {
                        item.help = c.record ? "Press to pick it up." : "An empty cell.";
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
                        item.label += " - " + Upper(Plain(skinDef->build.name));
                        item.icon = skinDef->build.icon;
                    }
                    std::string summary = skinDef != nullptr ? skinDef->build.summary : std::string();
                    if (summary.empty() && skinDef != nullptr && !skinDef->starting_passive.empty()) {
                        if (const rm::PerkDef* trait = run.content().FindPerk(skinDef->starting_passive)) summary = trait->description;
                    }
                    if (s.gate == rm::CharacterGate::Selectable) {
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
            case ScreenId::Main:
                if (index == 0) {
                    if (g->settings.directStart && autopilot::Available() && !autopilot::Armed()) {
                        const auto characters = run.Characters();
                        std::int64_t costume = 0;
                        if (g->selectedCharacter >= 0 && g->selectedCharacter < static_cast<int>(characters.size())) {
                            const auto& c = characters[g->selectedCharacter];
                            const rm::SkinAvailability* skin = SelectedSkin(c);
                            costume = std::max<std::int64_t>(skin && skin->costume_index >= 0 ? skin->costume_index : c.costume_index, 0);
                        }
                        ArmMap(PickMap(-1), static_cast<int>(costume));
                    }
                    d.outcome = Outcome::StartRun;
                } else {
                    d.outcome = Outcome::Push;
                    d.next = index == 1 ? ScreenId::Characters : index == 2 ? ScreenId::Perks : ScreenId::Credits;
                }
                break;
            case ScreenId::Characters: {
                const auto characters = run.Characters();
                if (index < 0 || index >= static_cast<int>(characters.size())) break;
                const auto& c = characters[index];
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
                d.next = ScreenId::MatchRewards;
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
                    d.outcome = Outcome::Push;
                    d.next = run.phase() == rm::Phase::Shop ? ScreenId::MatchShop : ScreenId::MatchRunOver;
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
                    d.outcome = st.ok() ? Outcome::Refresh : Outcome::Denied;
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
                    // grid screen otherwise.
                    if (NativeItemBoxReady()) {
                        g->itemBoxRequested = true;
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
                d.outcome = Outcome::Push;
                d.next = ScreenId::MatchRoute;
                break;
            }
            case ScreenId::MatchOrganize: {
                const auto grid = ReadGrid();
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
                if (index >= kGridCells) break;
                if (pick < 0) {
                    if (!grid[static_cast<std::size_t>(index)].record) {
                        d.outcome = Outcome::Denied;
                        break;
                    }
                    pick = index;
                } else if (pick == index) {
                    pick = -1;
                } else {
                    SwapGridCells(pick, index);
                    pick = -1;
                }
                d.outcome = Outcome::Refresh;
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
                if (skins[index].gate == rm::CharacterGate::Selectable) {
                    g->selectedSkin = skins[index].id;
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

void HandleNativeMenu(double now) {
    nativemenu::SetModel(&g_menuModel);
    matchui::SetModel(&g_menuModel);
    if (nativemenu::TakeChosen()) {
        g->rogueModePending = true;
        g->toasts.Push("Rogue Mercenaries: choose Solo, a stage and Chris or Sheva");
    }
    if (!g->rogueModePending) return;
    rm::RunController& run = *g->run;
    if (nativemenu::MenuAtInput() && run.phase() == rm::Phase::Idle && run.state().arena_index == 0) {
        g->rogueModePending = false;  // backed out to the Play menu
        autopilot::Disarm("back at the Play menu");
        host::SetStageOverride(-1);
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
    const rm::Status status =
        run.StartRun(g->match.localCharacter, seed, SkinForRun(run.Characters(), g->match.localCharacter));
    Report(status, ("start run (" + g->match.localCharacter + ")").c_str());
    if (!status.ok()) {
        g->toasts.Push("Rogue Mercenaries: " + status.message);
        return;
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
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        if ((id >> 8) != 1) continue;
        for (const rm::WeaponBaseDef& w : g->content->weapons()) {
            if (w.game_item == static_cast<std::int64_t>(id)) return w.id;
        }
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
        }
        return id;
    }
    char id[32];
    std::snprintf(id, sizeof(id), "em_%08X_%d", static_cast<unsigned>(enemy), maxHp);
    return id;
}

// Enemies a build special hurt (M8: Twin Fury, Backup) and when: their deaths are the player's.
std::map<std::uintptr_t, double> g_specialVictims;
constexpr double kSpecialKillCredit = 3.0;
void BuildKillNotice(const rm::KillOutcome& outcome, double now);

void ReportEnemyDeath(std::uintptr_t enemy, int maxHp, double now, const char* how) {
    rm::KillEvent kill;
    kill.now = now;
    const auto victim = g_specialVictims.find(enemy);
    kill.by_player = victim != g_specialVictims.end() && now - victim->second < kSpecialKillCredit;
    if (kill.by_player) {
        kill.was_melee = false;
        kill.enemy_class = engine::ClassName(enemy);
    }
    kill.enemy_id = EnemyKillId(enemy, maxHp);
    const rm::KillOutcome out = g->run->OnEnemyKilled(kill);
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
bool WeaponModelLoaded(std::uint32_t id) {
    if (g->loadedWeaponItems.count(id)) return true;
    return resources::Available() && (id >> 8) == 1 && resources::WeaponLoaded(static_cast<std::uint16_t>(id));
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

// Returns the units placed (stacked or in new slots); 0 when nothing fit or no player.
int GiveItem(const char* source, std::int64_t gameItem, int count) {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player || gameItem < 0 || count <= 0) return 0;
    const auto inventory = player + re5::off::kPlayerInventory;
    const int id = static_cast<int>(gameItem);
    std::uintptr_t stack = 0;
    for (int i = 0; i < re5::off::kInventorySlots && !stack; ++i) {
        const auto slot = inventory + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) == 1 &&
            ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) == static_cast<std::uint32_t>(id)) {
            stack = slot;
        }
    }
    using AddFn = int(__thiscall*)(std::uintptr_t inventory, int item, int count);
    using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t inventory, int item, int count, int a, int b, int c,
                                                 int flags, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                                 int a8, int a9, std::uintptr_t player, int position, int zero);
    const auto addToStacks = reinterpret_cast<AddFn>(re5::addr::kFnInventoryAddToStacks);
    const auto create = reinterpret_cast<CreateFn>(re5::addr::kFnInventoryCreate);
    // Weapons (item kind 3) do not stack: one record, loaded with a full magazine (the
    // record's count is the ammo in the gun).
    const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((id >> 8) & 0xFF) * 4, 0);
    const auto record = table ? table + (id & 0xFF) * 0x48 : 0;
    if (record && ReadOr<std::int32_t>(record + 0x30, 0) == 3) {
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
        const int magazine = std::max(1, ReadOr<std::int32_t>(record + 8, 1));
        const auto made = create(inventory, id, magazine, -1, -1, -1, 0xD, 0, 0, 0, 0, 0, 0, 0, 0, 0, player, 0, 0);
        RM_LOGI("%s: weapon %04X created with %d rounds -> %s", source, id, magazine, made ? "ok" : "no slot");
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
        default: return re5::addr::kItemHandgunAmmo;
    }
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

void ClearSlot(std::uintptr_t slot) {
    if (engine::Caps().slotClear && ClearSlotNative(slot)) return;
    SafeStore<std::int32_t>(slot + re5::off::kSlotCount, 0);
    SafeStore<std::uint8_t>(slot + re5::off::kSlotValid, 0);
}

// Every run starts with the pistol only: the character's own Mercenaries loadout is cut down
// to the pistol, its ammo and the knife (records invalidated, which frees their grid slots).
void StripToPistol() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return;
    const auto inv = player + re5::off::kPlayerInventory;
    // The gun in the player's hands stays whatever it is (removing the equipped weapon's
    // record hangs the game: S.T.A.R.S. Chris and his Samurai Edge); every other weapon,
    // ammo (except handgun rounds), grenade and spray goes.
    std::uint32_t equipped = 0;
    std::string loaded;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        if (ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) == 1) equipped = id;
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
    if (const rm::WeaponBaseDef* w = g->content->FindWeapon(g->run->state().loadout.weapon_id)) {
        if (w->game_item >= 0) pistol = w->game_item;
    }
    std::uint32_t ammo = re5::addr::kItemHandgunAmmo;
    if (equipped != 0 && (equipped >> 8) == 1) {
        pistol = equipped;  // the hands win over the roll
        for (const rm::WeaponBaseDef& w : g->content->weapons()) {
            if (w.game_item != static_cast<std::int64_t>(equipped)) continue;
            ammo = AmmoItemFor(w.category);
            const rm::Status swapped = g->run->SwapStartingWeapon(w.id);
            if (!swapped.ok()) RM_LOGW("inventory: starting weapon swap to %s failed: %s", w.id.c_str(), swapped.message.c_str());
            break;
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
        const bool knife = id == re5::addr::kItemKnife || id == 0x010E || id == 0x0123 || id == 0x0124;  // every character's knife
        const bool keep = isEquipped || id == re5::addr::kItemUnarmed || knife || id == ammo ||
                          id == static_cast<std::uint32_t>(pistol);
        if (keep) continue;
        ClearSlot(slot);
        ++removed;
    }
    if (!havePistol && equipped == 0) {
        // Nothing in hand and no pistol: give the rolled one (magazine from the item record).
        const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((pistol >> 8) & 0xFF) * 4, 0);
        const int magazine = table ? std::max(1, ReadOr<std::int32_t>(table + (pistol & 0xFF) * 0x48 + 8, 1)) : 1;
        GiveItem("pistol", pistol, magazine);
    }
    RM_LOGI("inventory: stripped to the pistol %04X (equipped %04X, %d record(s) removed%s)", static_cast<unsigned>(pistol),
            equipped, removed, havePistol || equipped ? "" : ", pistol created");
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
        out.emplace_back(ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0));
    }
    return out;
}

// Sets the total count of `id` across its slot records to `total` by lowering the last
// records first (0 = used up). Returns the amount removed.
int ShrinkItem(std::uint32_t id, int total) {
    const auto player = engine::LocalPlayer();
    if (!player) return 0;
    const auto inv = player + re5::off::kPlayerInventory;
    int current = 0;
    std::vector<std::uintptr_t> records;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) != id) continue;
        records.push_back(slot);
        current += ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0);
    }
    int excess = current - total;
    const int removed = std::max(0, excess);
    for (auto it = records.rbegin(); it != records.rend() && excess > 0; ++it) {
        const int count = ReadOr<std::int32_t>(*it + re5::off::kSlotCount, 0);
        const int take = std::min(count, excess);
        SafeStore<std::int32_t>(*it + re5::off::kSlotCount, count - take);
        excess -= take;
    }
    return removed;
}

// Invalidates every record of `id` (the game's grid slot is freed). Returns the records removed.
int RemoveItemRecords(std::uint32_t id) {
    const auto player = engine::LocalPlayer();
    if (!player) return 0;
    const auto inv = player + re5::off::kPlayerInventory;
    int removed = 0;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) != id) continue;
        if (ReadOr<std::int32_t>(slot + re5::off::kSlotEquipped, 0) == 1) continue;  // never the gun in hand
        ClearSlot(slot);
        ++removed;
    }
    return removed;
}

bool IsWeaponItem(std::uint32_t id) {
    const auto record = ItemRecord(id);
    return record && ReadOr<std::int32_t>(record + 0x30, 0) == 3;
}

// The rounds in a held gun (its record's count).
void SetWeaponRounds(std::uint32_t id, int rounds) {
    const auto player = engine::LocalPlayer();
    if (!player) return;
    const auto inv = player + re5::off::kPlayerInventory;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        if (ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0) != id) continue;
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
    for (std::size_t c = 0; c < grid.size(); ++c) g->transition.cells[c] = grid[c].record ? grid[c].id : 0;
    RecordWeaponLevels();
    std::string text;
    for (const auto& [id, count] : g->transition.carry) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s%04X x%d", text.empty() ? "" : ", ", id, count);
        text += buf;
    }
    RM_LOGI("stage change: inventory snapshot (%zu records): %s", g->transition.carry.size(), text.c_str());
}

void KeepInRunStash(std::uint32_t id, int count, const char* why);

// In the new match: make the inventory match the snapshot. The game recreates the
// character's own loadout: its items the run no longer has go first and stacks are lowered,
// so their cells are free before the run's items come back (with them still in the grid, a
// full run lost its herbs and ammo, user playtest 2026-11-03); stacks are then topped up and
// extra items re-created. What still does not fit is kept in the run's storage (ORGANIZE).
// A gun's count is the ammo in it: a gun is there or not, and its rounds follow the snapshot
// (an empty gun stays, empty; one held already is never copied into the storage).
void ApplyCarriedInventory() {
    if (!engine::Caps().inventory || !engine::LocalPlayer()) return;
    RefreshPlaceableWeapons();
    std::map<std::uint32_t, int> wanted;
    for (const auto& [id, count] : g->transition.carry) wanted[id] += count;
    std::map<std::uint32_t, int> have;
    for (const auto& [id, count] : ReadInventory()) have[id] += count;
    for (const auto& [id, current] : have) {
        if (wanted.count(id) == 0) {
            RemoveItemRecords(id);
            RM_LOGI("carried: item %04X was used up, removed", id);
        }
    }
    for (const auto& [id, total] : wanted) {
        const int current = have.count(id) ? have[id] : 0;
        if (IsWeaponItem(id) || total >= current) continue;
        if (total <= 0) RemoveItemRecords(id); else ShrinkItem(id, total);
        RM_LOGI("carried: item %04X shrunk %d -> %d", id, current, total);
    }
    for (const auto& [id, total] : wanted) {
        if (id == 0) continue;
        if (IsWeaponItem(id)) {
            const bool held = have.count(id) != 0 || GiveItem("carried", id, 1) > 0;
            if (!held) KeepInRunStash(id, std::max(0, total), "could not be created after the stage change");
            if (held) SetWeaponRounds(id, std::max(0, total));
            continue;
        }
        const int current = have.count(id) ? have[id] : 0;
        if (total > current) {
            const int missing = (total - current) - GiveItem("carried", id, total - current);
            if (missing > 0) KeepInRunStash(id, missing, "did not fit after the stage change");
        }
    }
    g->transition.carry.clear();
    // Put every item back into the cell it had (the player's ORGANIZE layout).
    for (int target = 0; target < kGridCells; ++target) {
        const std::uint32_t id = g->transition.cells[static_cast<std::size_t>(target)];
        if (id == 0) continue;
        const auto grid = ReadGrid();
        if (grid[static_cast<std::size_t>(target)].record && grid[static_cast<std::size_t>(target)].id == id) continue;
        for (int from = 0; from < kGridCells; ++from) {
            if (from == target || !grid[static_cast<std::size_t>(from)].record || grid[static_cast<std::size_t>(from)].id != id) continue;
            // Cells before `target` that already hold their own item stay put.
            if (from < target && g->transition.cells[static_cast<std::size_t>(from)] == id) continue;
            SwapGridCells(from, target);
            break;
        }
    }
    g->transition.cells = {};
    ApplyWeaponLevels();
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
    SnapshotInventory();
    g->transition.hp = -1;
    if (const auto local = engine::LocalPlayer()) {
        if (const auto hp = engine::ReadHp(local)) g->transition.hp = hp->hp;
    }
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

void ApplyItemGrants() {
    for (const rm::ItemGrant& grant : g->run->ConsumeItemGrants()) {
        const bool fromPerk = grant.source != "reward" && grant.source != "shop";
        const std::string label = (fromPerk ? "perk " : "") + grant.source;
        if (GiveItem(label.c_str(), grant.game_item, grant.count) == 0) {
            g->toasts.Push(label + ": inventory full");
        }
    }
}

// Rewards and shop purchases place their item at once; the core fails the purchase
// (and refunds it) when nothing fits.
class InventoryBridge final : public rm::InventoryPort {
public:
    int Give(std::int64_t gameItem, int count, std::string_view source) override {
        if (!g->match.inMatch) return 0;
        return GiveItem(std::string(source).c_str(), gameItem, count);
    }
};
InventoryBridge g_inventoryBridge;

constexpr float kStartTolerance = 1000.0f;  // engine units (10 m) from the map's start
constexpr float kStartLevel = 40.0f;        // or another level there (s204: the boat under the ledge)

// The skin build's maximum health (M8, Barry): once per match, scaled from the maximum the match
// started with; health is then full (a stage change restores the carried health after this).
// The maximum the match started with, and what the build made of it (one per match).
struct BuildHealthState {
    int generation = -1;
    int baseMax = 0;
    int appliedMax = 0;
};
BuildHealthState g_buildHealth;

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
void ArenaBegan(double now) {
    g->arenaBeganAt = now;
    g->boss = {};
    ApplyBuildHealth();
    ApplyBuildStacks();
    world::SetDropReplacement(static_cast<int>(g->run->state().character.build.drops_as));
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
    if (line.empty()) return;
    g->notice = line;
    g->noticeUntil = now + 5.0;
    RM_LOGI("arena %d: %s (stage %d, enemy threat %d%s)", s.arena, line.c_str(), CurrentMapKey(), g->run->ArenaThreatLevel(),
            guestLog.c_str());
}

void TryBeginArena(double now) {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || !g->match.inMatch) return;
    const rm::Phase phase = run.phase();
    const bool waitingFirst = phase == rm::Phase::Idle && run.state().arena_index == 0;
    const bool waitingNext = phase == rm::Phase::RouteSelection && run.state().route_chosen;
    if (!waitingFirst && !waitingNext) return;
    if (!g->match.localCharacter.empty() && g->match.localCharacter != run.state().character_id) {
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
            const rm::Status status = run.BeginArena(now);
            Report(status, "begin arena");
            g->arenaEndRequested = false;
            if (status.ok()) {
                ArenaBegan(now);
                ApplyCarriedInventory();
                RestoreCarriedHp();
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
    if (status.ok() && waitingFirst) ApplyLoadoutToInventory();
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

// Enemies that cannot reach the player (behind a door the story opens, on a ledge, a boss at a
// door) are moved: one far from the player that has not moved 1.5 m in 12 s (8 s for a boss), and
// the last few when nothing has died for a while and they are far. The director gives each one
// another point within the player's reach and drops the point it was stuck at (user playtest
// 2026-11-03: Spencer's mansion, the Uroboros at a door).
struct StuckTrack {
    float anchor[3] = {};
    double anchorAt = 0.0;
    double lastRescue = -1.0e9;
};
std::unordered_map<std::uintptr_t, StuckTrack> g_stuckTracks;
double g_stuckCheckAt = 0.0;
int g_stuckRemaining = -1;
double g_stuckRemainingSince = 0.0;
constexpr double kStuckCheckEvery = 0.5;
constexpr double kStuckSeconds = 12.0;
constexpr double kBossStuckSeconds = 8.0;
constexpr float kStuckMove = 150.0f;           // moved less than this ...
constexpr float kStuckFar = 900.0f;            // ... while farther than this from the player
constexpr int kLastEnemies = 3;
constexpr double kLastEnemiesQuiet = 25.0;     // no kill for this long ...
constexpr float kLastEnemiesFar = 1500.0f;     // ... and this far: pulled in

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
    for (const auto& [unit, id] : g->directorUnits) {
        const auto hp = engine::ReadHp(unit);
        if (!hp || hp->hp <= 0) continue;
        float pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = ReadOr<float>(unit + re5::off::kUnitPosition + k * 4, 0.0f);
        auto [it, fresh] = g_stuckTracks.try_emplace(unit);
        StuckTrack& t = it->second;
        const float moved = std::sqrt(std::pow(pos[0] - t.anchor[0], 2.0f) + std::pow(pos[1] - t.anchor[1], 2.0f) +
                                      std::pow(pos[2] - t.anchor[2], 2.0f));
        if (fresh || moved > kStuckMove) {
            for (int k = 0; k < 3; ++k) t.anchor[k] = pos[k];
            t.anchorAt = now;
            if (!lastFew) continue;
        }
        const float away = static_cast<float>(std::sqrt(std::pow(pos[0] - (*player)[0], 2.0) + std::pow(pos[2] - (*player)[2], 2.0)));
        const bool boss = unit == g->boss.unit;
        const bool stuck = away > kStuckFar && now - t.anchorAt > (boss ? kBossStuckSeconds : kStuckSeconds);
        const bool straggler = lastFew && away > kLastEnemiesFar;
        if ((!stuck && !straggler) || now - t.lastRescue < 15.0) continue;
        const auto to = run.DirectorRelocate(id, player);
        if (!to) continue;
        const float dest[3] = {static_cast<float>((*to)[0]), static_cast<float>((*to)[1]), static_cast<float>((*to)[2])};
        const int copies = engine::WarpUnit(unit, dest);
        RM_LOGI("director: %s %s %.0f s at %.0f %.0f %.0f, %.0f m from the player; moved to %.0f %.0f %.0f (%d copies)",
                engine::ClassName(unit).c_str(), straggler && !stuck ? "one of the last, quiet" : "stuck", now - t.anchorAt,
                pos[0], pos[1], pos[2], away / 100.0f, dest[0], dest[1], dest[2], copies);
        for (int k = 0; k < 3; ++k) t.anchor[k] = dest[k];
        t.anchorAt = now;
        t.lastRescue = now;
    }
}

// ROADMAP M1: on a stage listed in maps.json the run owns the arena. The game's own spawns
// stay off for the whole match (also between arenas), and the core's director decides what
// to spawn where; the adapter spawns it and reports which unit became which ticket.
void DriveDirector(double now) {
    rm::RunController& run = *g->run;
    const bool directorStage = g->match.inMatch && run.HasActiveRun() && run.capabilities().Has(rm::cap::kDirector) &&
                               CurrentMap() != nullptr;
    director::BlockNativeSpawns(directorStage || g->devQuiet);
    run.SetMapStage(g->match.inMatch ? CurrentMapKey() : -1);
    if (!g->match.inMatch) {
        g->directorUnits.clear();
        g->boss = {};
        g_stuckTracks.clear();
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
    if (g->arenaBeganAt >= 0.0 && now - g->arenaBeganAt < grace) {
        if (bossArena && run.director() != nullptr && run.director()->alive() + run.director()->pending() == 0) {
            const int left = static_cast<int>(std::ceil(grace - (now - g->arenaBeganAt)));
            g->notice = Upper(Plain(arenaMap->boss->name)) + " IS COMING   " + std::to_string(left);
            g->noticeUntil = now + 0.3;
        }
        return;
    }
    std::optional<std::array<double, 3>> player;
    if (const auto local = engine::LocalPlayer()) {
        player = std::array<double, 3>{ReadOr<float>(local + re5::off::kUnitPosition, 0.0f),
                                       ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                                       ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f)};
    }
    for (const rm::SpawnRequest& r : run.DirectorTick(now, player)) {
        director::SpawnSpec spec;
        spec.unitClass = r.unit_class;
        for (int i = 0; i < 3; ++i) spec.pos[i] = static_cast<float>(r.pos[i]);
        if (player) spec.facing = static_cast<float>(std::atan2((*player)[0] - r.pos[0], (*player)[2] - r.pos[2]));
        spec.weapon = static_cast<int>(r.arm);
        spec.fields = r.fields;
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
}

void DriveArena(double now) {
    rm::RunController& run = *g->run;
    if (run.phase() != rm::Phase::ArenaActive) return;

    // Player death: HP held at zero (Mercenaries solo has no revive).
    if (g->match.zeroHpSince >= 0 && now - g->match.zeroHpSince >= 2.0) {
        Report(run.OnPlayerDied(now), "player died");
        return;
    }
    if (run.IsArenaComplete()) {
        const auto result = run.CompleteArena(now);
        if (result.ok()) {
            SetStatus("arena cleared in " + std::to_string(static_cast<int>(result->time_seconds)) + " s");
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

const char* BossArchiveForRun();  // the specials' borrowed boss moves, below

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
    static int lastStage = -2;
    static bool lastRun = false;
    static std::vector<std::string> lastGuests;
    static const char* lastBoss = nullptr;
    if (runLoads == lastRun && CurrentMapKey() == lastStage && guests == lastGuests && bossArchive == lastBoss) return;
    lastRun = runLoads;
    lastStage = CurrentMapKey();
    lastGuests = guests;
    lastBoss = bossArchive;
    resources::Extras extras;
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
        // The game's Organize/Buy screen (M7.1).
        if (g->settings.nativeItemBox && engine::Caps().itemBox) extras.uiArchives.push_back("ShopResource");
        const rm::MapDef* own = CurrentMap();
        const std::optional<rm::MapDef> map =
            own == nullptr ? std::nullopt : planned ? g->run->ArenaMap(CurrentMapKey()) : std::optional<rm::MapDef>(*own);
        if (map) {
            extras.enemyArchives = map->preload;
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
        const auto chest = run.OpenChest(now);
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
    if (RawKeyDown(SpecialKey(slot))) {
        if (source) *source = "keyboard";
        return true;
    }
    const auto& names = SpecialPad(slot);
    if (names.empty()) return false;
    const unsigned short xbuttons = RawXInputButtons();
    bool xinput = true, dinput = true;
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
};
SpecialMove g_move;
bool g_comboHeld[2] = {false, false};
double g_specialDeniedUntil = 0.0;
double g_burstUntil = -1.0;  // the burst's ring shows it acting until then

constexpr double kSmgPulse = 0.15;
constexpr float kSmgRange = 1400.0f;     // 14 m
constexpr float kSmgCone = 0.766f;       // cos 40 degrees
constexpr double kBackupPulse = 1.2;
constexpr float kBackupRange = 1800.0f;  // 18 m
constexpr int kBackupDamage = 150;

void EndSpecialMove(const char* why) {
    if (g_move.kind.empty()) return;
    RM_LOGI("special: %s%s%s over (%s): %d pulses, %d hits", g_move.kind.c_str(), g_move.move.empty() ? "" : " ",
            g_move.move.c_str(), why, g_move.pulses, g_move.hits);
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

// ---- Borrowed boss moves -------------------------------------------------------------------------
// A special plays the matching boss's own moves on the player (user playtest 2026-11-03: "the
// ability doesn't get her twin smg out and do the sweep like in the campaign"; "wesker has in the
// campaign animations, try to use them"): the boss Jill's (uEm84) WideFire on the playable Jill,
// the boss Wesker's (uEm80) dash-strike, kick and knee on his. Every player model shares one
// skeleton and these lists' tracks name its bones (checked offline: em84atk uses exactly the bone
// ids of Jill's own pl02action, em80 all of Wesker's pl03action but two). Their archive loads with
// the run's stages (UpdatePreloads); a move looks its list up (never loads it), binds it to a free
// motion list slot of the player's model through the game's own setter, puts the player in the
// scripted scenes' hold (event wait: it sets no motion of its own) and plays the steps on the body
// layer; after the last one the player is back to relax. Anything missing, a list that does not
// fit the model, or a hold the game refuses leaves the move as it was before (the dodge, or no
// animation), with a log line saying why.
struct BossMotionSource {
    const char* character;  // the run's character id
    const char* archive;    // loaded with the run's stages
    const char* list;
};
constexpr BossMotionSource kBossMotions[] = {
    {"jill", "uEm84", "pawn\\em\\em84\\motion\\em84atk"},
    {"wesker", "uEm80", "pawn\\em\\em80\\motion\\em80"},
};
const char* const kEventWaitClass = "cPlAction_pl_act_setEventAct_wait";
constexpr float kBossMotionBlend = 10.0f;    // frames, the bosses' own blend
constexpr float kBossMotionMinFit = 0.9f;    // share of a motion's bone tracks that must reach a joint
constexpr double kBossHoldWait = 0.4;        // seconds for the hold to start
constexpr double kBossMoveLimit = 8.0;       // seconds: a sequence never holds the player longer

struct BossStep {
    int index;    // into the boss's list
    float speed;  // the motion's play rate
};
// The moves (the bosses' own motion ids: cEm84ActWideFire 0x10A ready, 0x10D the sweep;
// cEm80ActAtkSH_F 0x1B; cEm80ActAtkKick 0xC9; cEm80ActHiza 0x72; all in their first list).
const std::vector<BossStep>* BossSteps(const std::string& move) {
    static const std::vector<BossStep> sweep = {{0x0A, 2.0f}, {0x0D, 2.0f}};
    static const std::vector<BossStep> dash = {{0x1B, 2.0f}};
    static const std::vector<BossStep> kick = {{0xC9, 1.6f}};
    static const std::vector<BossStep> knee = {{0x72, 1.6f}};
    if (move == "dual_smg") return &sweep;
    if (move == "dash" || move == "lunge") return &dash;
    if (move == "burst") return &kick;
    if (move == "knee") return &knee;
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

void EndBossMove(std::uintptr_t local, const char* why) {
    if (g_boss.move.empty()) return;
    std::string back = "left to the game";
    if (local && engine::CurrentActionName(local) == kEventWaitClass) {
        back = engine::StartAction(local, re5::off::kPlActRelax, "cPlAction_pl_act_relax") ? "back to relax" : "relax refused";
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
        if (!fit || fit->tracks == 0) return refuse("motion " + std::to_string(s.index) + " missing");
        const float share = static_cast<float>(fit->mapped) / static_cast<float>(fit->tracks);
        static std::set<std::string> logged;
        if (logged.insert(move + std::to_string(s.index)).second) {
            RM_LOGI("special: boss motion %s %02X: %d of %d bone tracks reach %s's joints, %.0f frames (slot %d)", source->list,
                    s.index, fit->mapped, fit->tracks, engine::ClassName(local).c_str(), fit->frames, slot);
        }
        if (share < kBossMotionMinFit) return refuse("motion " + std::to_string(s.index) + " does not fit the model");
    }
    if (!engine::StartAction(local, re5::off::kPlActEventWait, kEventWaitClass)) {
        return refuse("the hold was refused (current action " + engine::CurrentActionName(local) + ")");
    }
    EndBossMove(0, "replaced");
    g_boss.move = move;
    g_boss.steps = *steps;
    g_boss.slot = slot;
    g_boss.list = list;
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
    if (action != kEventWaitClass) {
        if (g_boss.playing || now - g_boss.start > kBossHoldWait) {
            EndBossMove(local, g_boss.playing ? ("interrupted: " + action).c_str() : ("the hold never started: " + action).c_str());
        }
        return;
    }
    if (now - g_boss.start > kBossMoveLimit) {
        EndBossMove(local, "time limit");
        return;
    }
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
        g_move.until = now + (sp.duration_seconds > 0.0 ? sp.duration_seconds : 1.5);
        g_move.nextPulse = now;
        // The boss Jill's WideFire: she readies, then sweeps; the shots land while she sweeps, for
        // as long as it plays (the move ends with it).
        g_move.borrowed = StartBossMove(local, "dual_smg", now);
        if (g_move.borrowed) g_move.until = now + kBossMoveLimit;
    } else if (sp.kind == "backup") {
        // Stand-in until a real computer ally can be spawned (ROADMAP E22): covering fire on the
        // nearest enemy for the rest of the arena.
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
                // The blow is the boss Wesker's knee, in the same hold as the dash-strike.
                if (g_boss.move == "lunge") {
                    g_boss.move = "knee";
                    g_boss.steps = *BossSteps("knee");
                    g_boss.step = 0;
                    g_boss.playing = false;
                }
                if (!heavy && result != "killed" && result != "missed") {
                    g->notice = "MELEE";
                    g->noticeUntil = now + 1.5;
                }
            }
        }
        EndSpecialMove("done");
        return;
    }
    // Her borrowed sweep: no shots while she readies, and the move ends with the sweep.
    if (g_move.kind == "dual_smg" && g_move.borrowed) {
        if (!BossMoveActive()) {
            EndSpecialMove("the sweep is over");
            return;
        }
        if (!BossMoveFiring()) return;
    }
    if (now >= g_move.until) {
        EndSpecialMove("done");
        return;
    }
    if (now < g_move.nextPulse) return;
    if (g_move.kind == "dual_smg") {
        g_move.nextPulse = now + kSmgPulse;
        ++g_move.pulses;
        float fx = g_move.dirX, fz = g_move.dirZ;
        PlayerFacing(local, fx, fz);  // she turns while she fires
        for (const auto& [enemy, d] : EnemiesAround(local, kSmgRange, kSmgCone, fx, fz)) {
            (void)d;
            if (!director::Damage(enemy, g_move.damage)) continue;
            ++g_move.hits;
            g_specialVictims[enemy] = now;
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
    SetOwnedInputs(owned);
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
    // A move in progress stops when he is grabbed, hit or down (not while it plays its own dodge
    // or a boss move's hold).
    if (!g_move.kind.empty() && !blocked.empty() && blocked.rfind("cPlAction_pl_act_avoid", 0) != 0 && blocked != kEventWaitClass) {
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
    const ImVec2 c(ox + (96.0f + 104.0f * static_cast<float>(slot)) * k, oy + 596.0f * k);
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
    for (std::size_t i = 0; i < w->affixes.size() && i < 4; ++i) lines.emplace_back(Upper(Plain(w->affixes[i])), IM_COL32(200, 230, 200, 235));
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::min(display.x / 1280.0f, display.y / 720.0f);
    const float ox = (display.x - 1280.0f * k) * 0.5f;
    const float oy = (display.y - 720.0f * k) * 0.5f;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 15.0f * k, gap = 18.0f * k;
    float width = 0.0f;
    for (const auto& [line, color] : lines) width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str()).x);
    const float right = ox + 1250.0f * k, bottom = oy + 548.0f * k;
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

void DrawSpecialWidgets() {
    rm::RunController& run = *g->run;
    if (!run.HasActiveRun() || run.phase() != rm::Phase::ArenaActive || !g->match.inMatch || matchui::IsOpen()) return;
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
        const rm::RunSnapshot s = run.Snapshot(Now());
        if (phase == rm::Phase::ArenaActive) {
            // With the game's clock hidden its big digits count the enemies left; otherwise
            // the number goes into the label.
            const bool digits = engine::Caps().mercHud && !g->settings.mercHud;
            counter = digits ? s.remaining : -1;
            hud.left = "ARENA " + std::to_string(s.arena) + " OF " + std::to_string(s.arena_count);
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
    SetMercHudCounter(counter);
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
            world::SetDropReplacement(-1);
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
};
RunInBox g_runInBox;

// An item the match cannot take now (a full grid, a gun whose model is not loaded here) goes to
// the run's storage, the one the ORGANIZE screen shows, instead of being lost.
void KeepInRunStash(std::uint32_t id, int count, const char* why) {
    if (id == 0 || count <= 0) return;
    if (g_runInBox.runId != g->run->state().run_id) {
        g_runInBox.runId = g->run->state().run_id;
        g_runInBox.remainder = 0;
        g_runInBox.stash.clear();
    }
    std::array<std::uint8_t, 12> record{};
    record[0] = static_cast<std::uint8_t>(id & 0xFF);
    record[1] = static_cast<std::uint8_t>(id >> 8);
    record[2] = static_cast<std::uint8_t>(count & 0xFF);
    record[3] = static_cast<std::uint8_t>((count >> 8) & 0xFF);
    g_runInBox.stash.push_back(record);
    RM_LOGW("inventory: %04X x%d %s; kept in the run's storage (ORGANIZE)", id, count, why);
    g->toasts.Push(GridItemName(id) + " is in the storage (ORGANIZE)");
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
    g_runInBox.gold = g->run->Snapshot(Now()).supplies * kGoldPerSupply + g_runInBox.remainder;
    SafeStore<std::uint32_t>(save + kSaveGold, static_cast<std::uint32_t>(g_runInBox.gold));
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
    // What is not in the nine cells (the knife, unarmed) stays as it is.
    if (const auto player = engine::LocalPlayer()) {
        const auto inv = player + re5::off::kPlayerInventory;
        for (int i = 0; i < re5::off::kInventorySlots; ++i) {
            const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
            if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
            const int cell = ReadOr<std::int32_t>(slot + kSlotCell, -1);
            if (cell >= 0 && cell < kGridCells) continue;
            g->transition.carry.emplace_back(ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0), ReadOr<std::int32_t>(slot + re5::off::kSlotCount, 0));
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
    // What the match cannot take (a gun whose model is not loaded here) goes to the stash.
    ApplyCarriedInventory();
    // Supplies follow the gold; what is short of a whole supply waits for the next visit.
    const auto gold = static_cast<std::int64_t>(ReadOr<std::uint32_t>(save + kSaveGold, 0));
    const std::int64_t supplies = g->run->Snapshot(Now()).supplies;
    const std::int64_t delta = gold / kGoldPerSupply - supplies;
    if (delta != 0) Report(g->run->AdjustSupplies(delta, "native_shop"), "native shop supplies");
    g_runInBox.remainder = gold % kGoldPerSupply;
    RM_LOGI("itembox: gold %lld -> %lld: supplies %+lld (%lld gold kept)", static_cast<long long>(g_runInBox.gold),
            static_cast<long long>(gold), static_cast<long long>(delta), static_cast<long long>(g_runInBox.remainder));
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
void PollItemBox() {
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
    } else if (command.rfind("spawn ", 0) == 0) {
        // "spawn <class> [weapon hex] [count]": e.g. "spawn uEm10 70100 3" next to the player.
        std::istringstream words(command.substr(6));
        std::string cls, weaponText;
        int count = 1;
        float distance = 150.0f;
        words >> cls >> weaponText >> count >> distance;
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
            const std::uintptr_t unit = player ? director::Spawn(spec) : 0;
            RM_LOGW("dev spawn %s (weapon %X) at %.0f %.0f %.0f -> %08X %s", cls.c_str(), weapon, spawnAt[0], spawnAt[1], spawnAt[2],
                    static_cast<unsigned>(unit), unit ? engine::ClassName(unit).c_str() : "failed");
        }
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
    } else if (command == "abandon" && run.HasActiveRun()) {
        // Ends the run as the overlay's "Abandon run" does; the match goes on as plain Mercenaries.
        Report(run.AbandonRun(now), "dev abandon run");
    } else if (command == "killall") {
        // Real deaths (not the run's synthetic "kill <n>"): forces the game to refill its waves.
        int killed = 0;
        for (const auto& e : engine::ReadEnemyStates()) killed += e.hp > 0 && director::Kill(e.object) ? 1 : 0;
        RM_LOGW("dev killall: %d enemies killed", killed);
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

private:
    float Outgoing(const OutgoingHit& hit) {
        if (!hit.attackerIsLocal) return 1.0f;
        std::lock_guard lock(g->mutex);
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
        const auto result = g->run->ComputeOutgoingMultiplier(ctx);
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
            }
            return;
        }
        std::lock_guard lock(g->mutex);
        rm::KillEvent kill;
        kill.now = Now();
        kill.by_player = r.attackerIsLocal;
        if (r.attackerIsLocal) {
            kill.weapon_id = EquippedWeaponBaseId();
            kill.was_melee = engine::InMeleeAction(r.attacker);
        }
        kill.enemy_id = EnemyKillId(r.enemy, r.maxHp);
        kill.enemy_class = engine::ClassName(r.enemy);
        const auto outcome = g->run->OnEnemyKilled(kill);
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
        rm::IncomingContext ctx;
        ctx.now = Now();
        ctx.amount = hit.damage;
        ctx.hp = hit.hp;
        ctx.hp_max = hit.maxHp;
        const auto result = g->run->ComputeIncoming(ctx);
        IncomingDecision decision;
        decision.multiplier = g->settings.devInvulnerable
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
    }

    void TickLocked() {
        std::lock_guard lock(g->mutex);
        const double now = Now();
        ObserveMatch(now);
        if (g->match.inMatch && g->run->HasActiveRun() && g_startMovedGeneration != g->matchGeneration &&
            MoveToMapStart("match start")) {
            g_startMovedGeneration = g->matchGeneration;
        }
        nativemenu::OnGameTick();
        autopilot::OnGameTick();
        if (g->settings.uiDemo) {
            DriveUiDemo(now);
        } else {
            HandleNativeMenu(now);
            TryBeginArena(now);
        }
        DriveDirector(now);
        DriveArena(now);
        DetectEnemyDeaths(now);
        OpenChests(now);
        ApplyHeals();
        DriveSpecial(now);
        WatchEquippedWeapon(now);
        SampleHitStates(now);
        ApplyMoveSpeed();
        // A match a run played in stays a run's match until it is gone, so quitting it writes no
        // save even if the run is settled first.
        static bool runMatch = false;
        if (!g->match.inMatch) runMatch = false;
        else if (g->run->HasActiveRun()) runMatch = true;
        autopilot::SetRunMatch(runMatch);
        if (g->run->phase() == rm::Phase::ArenaActive && g->match.inMatch) ApplyBuildHealth();  // gains mid-arena
        // Items earned by perks, rewards and the shop wait until the player is in a match.
        if (g->match.inMatch && engine::LocalPlayer() && g->run->phase() != rm::Phase::Idle && ReadMercTimer().recent) {
            ApplyItemGrants();
        }
        if (g->settings.developerMode) ProbeScene();
        PollDevCommand(now);
        PollItemBox();
        SettleFinishedRun();
        UpdateTimerPolicy();
        UpdatePreloads();
        PlaceArenaChests();
        UpdateMatchStatus();
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
    } else {
        ImGui::TextDisabled("Rogue Mercenaries - Play menu or F7");
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
        g->diagnosticsResult = report.status.ok() ? ("written " + zip.string()) : report.status.message;
    }
    if (!g->diagnosticsResult.empty()) ImGui::TextWrapped("%s", g->diagnosticsResult.c_str());
}

}  // namespace

void ModInit(const Paths& paths, const Settings& settings) {
    g = new ModState();
    g->paths = paths;
    g->settings = settings;

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
    auto profilePath = paths.dataDir / L"profile.json";
    if (settings.uiDemo) {
        // Fresh throwaway profile; the player's real profile is never touched.
        const auto sandbox = paths.dataDir / L"dev-sandbox";
        std::error_code ec;
        std::filesystem::remove_all(sandbox, ec);
        std::filesystem::create_directories(sandbox, ec);
        profilePath = sandbox / L"profile.json";
        RM_LOGW("dev: UI demo mode on sandbox profile %s", profilePath.string().c_str());
    }
    g->store = std::make_unique<rm::ProfileStore>(profilePath, g->fanout.get());
    auto profile = g->store->Load();
    RM_LOGI("profile loaded from %s (credits=%lld)", std::string(rm::ProfileSourceName(profile.source)).c_str(),
            static_cast<long long>(profile.profile.credits.balance()));
    rm::RunControllerOptions options;
    options.sink = g->fanout.get();
    options.store = g->store.get();
    g->run = std::make_unique<rm::RunController>(g->content, std::move(profile.profile), options);
    if (g->run->interrupted_run()) {
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
}

void ModShutdown() {
    if (g && g->jsonl) g->jsonl->Flush();
}

HookSink* ModSink() { return g && g->run ? static_cast<HookSink*>(&g_sink) : nullptr; }

void DrawDisabled() {
    ImGui::Begin("Rogue Mercenaries - disabled", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("The mod stopped itself after an internal error; the game is unmodified now.");
    ImGui::TextWrapped("%s", g->disabledReason.c_str());
    ImGui::TextDisabled("See %s", (g->paths.logDir / L"adapter.log").string().c_str());
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
    DrawSpecialWidgets();
    DrawWeaponCard();
    const rm::RunSnapshot snapshot = g->run->Snapshot(Now());
    if (!(g->settings.nativeMatchUi && g->match.inMatch && g->run->HasActiveRun())) DrawHud(snapshot);
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
