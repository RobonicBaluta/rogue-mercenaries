// Roguelite mod for RE5: drives rm::RunController from engine hooks and draws
// the run UI. Arenas map onto Mercenaries matches: a match starting while the
// run waits for its next arena begins that arena; reaching the kill budget
// completes it and (optionally) ends the match through the game's own timer.

#include <windows.h>

#include <algorithm>
#include <map>
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
#include "imgui_internal.h"
#include "log.h"
#include "memory.h"
#include "menu_probe.h"
#include "autopilot.h"
#include "native_menu.h"
#include "match_ui.h"
#include "mod.h"
#include "overlay.h"
#include "probe.h"
#include "re5_layout.h"
#include "version.h"

#include "rm/content.h"
#include "rm/diagnostics.h"
#include "rm/profile.h"
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
    bool matchUiOpenRequested = false;  // native results screen to open once the tick's lock is released
    int matchGeneration = 0;         // +1 on every match detected
    int currentStage = -1;           // stage the autopilot armed for the current match
    struct StageTransition {
        bool pending = false;
        int generation = 0;     // matchGeneration when the match end was requested
        double startedAt = 0.0;
        std::vector<std::pair<std::uint32_t, int>> carry;  // (item id, count) per slot record
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
    if (caps.inventory) out.Add(rm::cap::kInventory);
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
        if (m.inMatch) ++g->matchGeneration;
    }
}

// --- native mod menu content -------------------------------------------------

std::string Plain(const std::string& s);
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
        if (c == '%') out += " percent";
        else if (c == '+' && digitNext) out += "plus ";
        else if (c == '-' && digitNext) out += "minus ";
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
                screen.items = {
                    {"START RUN", "Solo Mercenaries on any stage. Pick your character,\nthe run starts with the match."},
                    {"CHARACTERS", "Choose who you play. Locked characters show\nhow to unlock them."},
                    {"PERKS", "Permanent perks. Locked ones are bought with credits."},
                    {"CREDITS", "Your credits, lifetime records and weapon unlocks."},
                };
                break;
            case ScreenId::Characters: {
                screen.header = "CHARACTERS";
                const auto characters = run.Characters();
                for (std::size_t i = 0; i < characters.size(); ++i) {
                    const auto& c = characters[i];
                    Item item;
                    item.label = Upper(c.name);
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
                const std::string character = SelectedCharacterId(run);
                for (rm::PerkCategory c : kSlotCategories) {
                    Item item;
                    const rm::PerkDef* equipped = character.empty() ? nullptr : run.EquippedPerk(character, c);
                    item.label = CategoryTag(c) + (equipped ? Upper(Plain(equipped->name)) : std::string("NONE"));
                    item.help = equipped ? Plain(equipped->description) + "\nPress to change it."
                                         : "Nothing chosen for this slot.\nPress to choose.";
                    screen.items.push_back(std::move(item));
                }
                break;
            }
            case ScreenId::PerkList: {
                const std::string character = SelectedCharacterId(run);
                screen.header = Upper(std::string(rm::PerkCategoryName(g->perkCategory))) + "   " + Upper(CreditsLine());
                screen.empty_help = "No perk of this kind for this character yet.";
                const rm::PerkDef* equipped = character.empty() ? nullptr : run.EquippedPerk(character, g->perkCategory);
                for (const rm::PerkDef* perk : MenuPerks(run, g->perkCategory)) {
                    Item item;
                    item.label = Upper(Plain(perk->name));
                    const bool locked = perk->unlock_cost_credits > 0 && !profile.unlocked_perks.count(perk->id);
                    const bool selected = equipped != nullptr && equipped->id == perk->id;
                    if (selected) item.label += "   SELECTED";
                    item.locked = locked;
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
                    if (!o.purchased && o.price > s.supplies) item.locked = true;
                    screen.items.push_back(std::move(item));
                }
                Item leave;
                leave.label = "LEAVE SHOP";
                leave.help = "On to the next route.";
                screen.items.push_back(std::move(leave));
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
                for (const auto& o : s.route_offers) {
                    Item item;
                    item.label = Upper(Plain(o.title));
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
                for (const auto& s : c.skins) {
                    Item item;
                    item.label = Upper(s.name);
                    if (s.gate == rm::CharacterGate::Selectable) {
                        const bool selected = current != nullptr && current->id == s.id;
                        if (selected) item.label += "   SELECTED";
                        item.help = selected ? "Your skin for the next run." : "Press to wear this skin.";
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
                    {Upper(CreditsLine()), "Earned by finishing runs. Spent on perks,\nweapons and characters."},
                    {Upper(Plural(stats.credits_earned, "credit")) + " EARNED", "All credits earned by your runs so far."},
                    {Upper(Plural(stats.runs_completed, "run")) + " COMPLETED", Plural(stats.runs_failed, "run") + " ended early."},
                    {Upper(Plural(stats.total_kills, "kill")), "Enemies killed in all runs."},
                    {"BEST RUN " + Upper(Plural(stats.best_arena, "arena")), "Most arenas cleared in one run."},
                    {Upper(Plural(stats.s_rank_arenas, "S rating")), Plural(stats.no_damage_arenas, "arena") +
                                                                       " cleared without taking damage."},
                };
                for (const auto& weapon : run.content().weapons()) {
                    if (!weapon.enabled || weapon.unlock_cost_credits <= 0) continue;
                    Item item;
                    const bool owned = profile.unlocked_weapons.count(weapon.id) > 0;
                    item.label = Upper(weapon.name) + (owned ? "   UNLOCKED" : "");
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
                        LARGE_INTEGER counter;
                        QueryPerformanceCounter(&counter);
                        const int stages = std::max(1, g->settings.stageCount);
                        autopilot::Arm({(g->currentStage = static_cast<int>(static_cast<std::uint64_t>(counter.QuadPart) % stages)),
                                        static_cast<int>(costume), stages});
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
                // "Leave shop" row or cancel.
                const rm::Status st = run.LeaveShop();
                Report(st, "leave shop");
                if (!st.ok()) { d.outcome = Outcome::Denied; break; }
                d.outcome = Outcome::Push;
                d.next = ScreenId::MatchRoute;
                break;
            }
            case ScreenId::MatchRoute: {
                if (index < 0) break;
                const rm::Status st = run.ChooseRoute(static_cast<std::size_t>(index));
                Report(st, "choose route");
                d.outcome = st.ok() ? Outcome::Close : Outcome::Denied;
                break;
            }
            case ScreenId::MatchRunOver: {
                if (index < 0) break;
                d.outcome = Outcome::Close;  // the tick then banks the run and lets the match end
                break;
            }
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
    const rm::Status status =
        run.StartRun(g->match.localCharacter, seed, SkinForRun(run.Characters(), g->match.localCharacter));
    Report(status, ("start run (" + g->match.localCharacter + ")").c_str());
    if (!status.ok()) {
        g->toasts.Push("Rogue Mercenaries: " + status.message);
        return;
    }
    for (const auto& perk : g->settings.devGrantPerks) {
        Report(run.DevGrantPerk(perk), ("dev grant " + perk).c_str());
    }
    (void)now;
}

// Puts `count` of game item `id` into the local player's inventory through the
// game's own helpers (verified: AddToStacks tops up a stack, Create makes a
// new slot the way PickUpItem does). Returns false when nothing could be placed.
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

// Every run starts with the pistol only: the character's own Mercenaries loadout is cut down
// to the pistol, its ammo and the knife (records invalidated, which frees their grid slots).
void StripToPistol() {
    const auto player = engine::LocalPlayer();
    if (!engine::Caps().inventory || !player) return;
    std::int64_t pistol = re5::addr::kItemM92F;
    if (const rm::WeaponBaseDef* w = g->content->FindWeapon(g->run->state().loadout.weapon_id)) {
        if (w->game_item >= 0) pistol = w->game_item;
    }
    const auto inv = player + re5::off::kPlayerInventory;
    bool havePistol = false;
    int removed = 0;
    for (int i = 0; i < re5::off::kInventorySlots; ++i) {
        const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
        if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
        const auto id = ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0);
        if (id == static_cast<std::uint32_t>(pistol)) havePistol = true;
        const bool keep = id == re5::addr::kItemUnarmed || id == re5::addr::kItemKnife ||
                          id == re5::addr::kItemHandgunAmmo || id == static_cast<std::uint32_t>(pistol);
        if (keep) continue;
        SafeStore<std::int32_t>(slot + re5::off::kSlotCount, 0);
        SafeStore<std::uint8_t>(slot + re5::off::kSlotValid, 0);
        ++removed;
    }
    if (!havePistol) {
        // Magazine size from the item record (dword 2); 1 if the table cannot be read.
        const auto table = ReadOr<std::uintptr_t>(re5::addr::kItemTableRoot + ((pistol >> 8) & 0xFF) * 4, 0);
        const int magazine = table ? std::max(1, ReadOr<std::int32_t>(table + (pistol & 0xFF) * 0x48 + 8, 1)) : 1;
        GiveItem("pistol", pistol, magazine);
    }
    RM_LOGI("inventory: stripped to the pistol %04X (%d record(s) removed%s)", static_cast<unsigned>(pistol), removed,
            havePistol ? "" : ", pistol created");
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
        SafeStore<std::int32_t>(slot + re5::off::kSlotCount, 0);
        SafeStore<std::uint8_t>(slot + re5::off::kSlotValid, 0);
        ++removed;
    }
    return removed;
}

// Before the match ends for the next stage: remember every item and count.
void SnapshotInventory() {
    g->transition.carry = ReadInventory();
    std::string text;
    for (const auto& [id, count] : g->transition.carry) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s%04X x%d", text.empty() ? "" : ", ", id, count);
        text += buf;
    }
    RM_LOGI("stage change: inventory snapshot (%zu records): %s", g->transition.carry.size(), text.c_str());
}

// In the new match: make the inventory match the snapshot. The game recreates the
// character's own loadout, so stacks are topped up or shrunk and extra items re-created;
// items that were used up are set to 0.
void ApplyCarriedInventory() {
    if (!engine::Caps().inventory || !engine::LocalPlayer()) return;
    std::map<std::uint32_t, int> wanted;
    for (const auto& [id, count] : g->transition.carry) wanted[id] += count;
    std::map<std::uint32_t, int> have;
    for (const auto& [id, count] : ReadInventory()) have[id] += count;
    for (const auto& [id, total] : wanted) {
        const int current = have.count(id) ? have[id] : 0;
        if (total > current) {
            GiveItem("carried", id, total - current);
        } else if (total < current) {
            if (total <= 0) RemoveItemRecords(id); else ShrinkItem(id, total);
            RM_LOGI("carried: item %04X shrunk %d -> %d", id, current, total);
        }
    }
    for (const auto& [id, current] : have) {
        if (wanted.count(id) == 0) {
            RemoveItemRecords(id);
            RM_LOGI("carried: item %04X was used up, removed", id);
        }
    }
    g->transition.carry.clear();
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

// A different random stage for the next arena (session-locked stages are skipped by Arm()).
int NextStage() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const int stages = std::max(1, g->settings.stageCount);
    int stage = static_cast<int>(static_cast<std::uint64_t>(counter.QuadPart) % stages);
    if (stages > 1 && stage == g->currentStage) stage = (stage + 1) % stages;
    return stage;
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
    const int stage = NextStage();
    g->transition.pending = true;
    g->transition.generation = g->matchGeneration;
    g->transition.startedAt = now;
    RM_LOGI("stage change: stage %d -> %d (costume slot %d), ending the match", g->currentStage, stage, costume);
    g->currentStage = stage;
    autopilot::Arm({stage, costume, std::max(1, g->settings.stageCount)});
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
    if (status.ok() && waitingFirst) ApplyLoadoutToInventory();
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

// The countdown floor only applies while an arena is being fought.
void UpdateTimerPolicy() {
    // The whole run (arenas, rewards, shop, route choice) happens inside one match, so the
    // countdown is held for as long as the run is alive; it is released when the run ends.
    const rm::Phase phase = g->run->phase();
    const bool alive = g->run->HasActiveRun() && phase != rm::Phase::RunComplete && phase != rm::Phase::RunFailed;
    const bool hold = g->settings.timerPolicy == "hold" && alive && !g->transition.pending;
    SetMercTimerFloor(hold ? g->settings.timerHoldSeconds : -1.0f);
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

// One line in the game's font during a match; the native screens close when nothing is
// left to choose (run over, match gone).
void UpdateMatchStatus() {
    rm::RunController& run = *g->run;
    const rm::Phase phase = run.phase();
    const bool between = phase == rm::Phase::ArenaResults || phase == rm::Phase::RewardSelection ||
                         phase == rm::Phase::Shop || phase == rm::Phase::RouteSelection ||
                         phase == rm::Phase::RunComplete;
    if (matchui::IsOpen() && (!run.HasActiveRun() || !between || !g->match.inMatch)) matchui::Close();
    std::string line;
    if (g->settings.nativeMatchUi && run.HasActiveRun() && g->match.inMatch) {
        const rm::RunSnapshot s = run.Snapshot(Now());
        if (phase == rm::Phase::ArenaActive) {
            line = "ARENA " + std::to_string(s.arena) + " OF " + std::to_string(s.arena_count) + "   " +
                   std::to_string(s.remaining) + (s.remaining == 1 ? " ENEMY LEFT   " : " ENEMIES LEFT   ") +
                   std::to_string(s.supplies) + " SUPPLIES";
        } else if (phase == rm::Phase::RouteSelection && run.state().route_chosen) {
            line = "NEXT STAGE LOADING";
        }
    }
    matchui::SetStatus(line);
}

void SettleFinishedRun() {
    rm::RunController& run = *g->run;
    const rm::Phase phase = run.phase();
    if (phase == rm::Phase::RunComplete || phase == rm::Phase::RunFailed) {
        if (matchui::IsOpen()) return;  // the native RUN COMPLETE screen is still up
        // The run is over: let the match end (the game's own time-up path, once).
        if (g->match.inMatch && !g->arenaEndRequested) {
            RequestMercMatchEnd();
            g->arenaEndRequested = true;
            RM_LOGI("run over (%s): requested mercenaries time-up to end the match", std::string(rm::PhaseName(phase)).c_str());
        }
        const auto summary = run.EndRun();
        if (summary.ok()) {
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
    } else if (command.rfind("arm ", 0) == 0) {
        // "arm <stage> <costume slot>": the next START RUN uses this plan.
        std::istringstream words(command.substr(4));
        autopilot::Plan plan;
        words >> plan.stage >> plan.costume;
        autopilot::Arm(plan);
    } else if (command == "dumpinv") {
        // The local player's inventory slots (item id, count, max).
        const auto player = engine::LocalPlayer();
        const auto inv = player ? player + re5::off::kPlayerInventory : 0;
        for (int i = 0; inv && i < re5::off::kInventorySlots; ++i) {
            const auto slot = inv + re5::off::kInventoryFirstSlot + i * re5::off::kInventorySlotSize;
            if (ReadOr<std::uint8_t>(slot + re5::off::kSlotValid, 0) != 1) continue;
            std::uint32_t raw[12]{};
            for (int k = 0; k < 12; ++k) SafeRead(slot + k * 4, raw[k]);
            RM_LOGW("dev inv slot %2d: item %04X count %d max %d | %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                    i, ReadOr<std::uint32_t>(slot + re5::off::kSlotItemId, 0),
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
        ctx.base_damage = hit.damage;
        ctx.target_hp = hit.targetHp;
        ctx.target_hp_max = hit.targetMaxHp;
        if (g->match.localMaxHp > 0) {
            ctx.player_hp = g->match.localHp;
            ctx.player_hp_max = g->match.localMaxHp;
        }
        const auto result = g->run->ComputeOutgoingMultiplier(ctx);
        if (g->settings.probeHits && g->run->phase() == rm::Phase::ArenaActive) {
            RM_LOGD("outgoing core=%.4f crit=%d perk=%+.3f streak=%+.3f weapon=%+.3f target %d/%d dev=%.2f",
                    result.multiplier, result.crit, result.breakdown.perk_bonus, result.breakdown.kill_streak_bonus,
                    result.breakdown.weapon_bonus, hit.targetHp, hit.targetMaxHp, g->settings.devOutgoingMultiplier);
        }
        return static_cast<float>(result.multiplier) * g->settings.devOutgoingMultiplier;
    }

    void EnemyHit(const EnemyHitResult& r) {
        if (g->settings.probeHits || r.killed) {
            RM_LOGI("enemy_hit enemy=%08X class=%s player=%d local=%d hp %d->%d/%d raw=%d applied=%d killed=%d",
                    static_cast<unsigned>(r.enemy), engine::ClassName(r.enemy).c_str(), r.attackerIsPlayer,
                    r.attackerIsLocal, r.hpBefore, r.hpAfter, r.maxHp, r.rawDamage, r.appliedDamage, r.killed);
        }
        if (!r.killed) return;
        std::lock_guard lock(g->mutex);
        rm::KillEvent kill;
        kill.now = Now();
        kill.by_player = r.attackerIsLocal;
        char id[32];
        std::snprintf(id, sizeof(id), "em_%08X_%d", static_cast<unsigned>(r.enemy), r.maxHp);
        kill.enemy_id = id;
        const auto outcome = g->run->OnEnemyKilled(kill);
        if (outcome.counted) {
            RM_LOGI("kill counted: +%lld supplies, %d remaining", static_cast<long long>(outcome.supplies_awarded),
                    outcome.remaining);
        }
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
    }

    void TickLocked() {
        std::lock_guard lock(g->mutex);
        const double now = Now();
        ObserveMatch(now);
        nativemenu::OnGameTick();
        autopilot::OnGameTick();
        if (g->settings.uiDemo) {
            DriveUiDemo(now);
        } else {
            HandleNativeMenu(now);
            TryBeginArena(now);
        }
        DriveArena(now);
        ApplyHeals();
        // Items earned by perks, rewards and the shop wait until the player is in a match.
        if (g->match.inMatch && engine::LocalPlayer() && g->run->phase() != rm::Phase::Idle && ReadMercTimer().recent) {
            ApplyItemGrants();
        }
        if (g->settings.developerMode) ProbeScene();
        PollDevCommand(now);
        SettleFinishedRun();
        UpdateTimerPolicy();
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
    std::lock_guard lock(g->mutex);
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
