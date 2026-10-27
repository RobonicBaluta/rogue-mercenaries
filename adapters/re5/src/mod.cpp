// Roguelite mod for RE5: drives rm::RunController from engine hooks and draws
// the run UI. Arenas map onto Mercenaries matches: a match starting while the
// run waits for its next arena begins that arena; reaching the kill budget
// completes it and (optionally) ends the match through the game's own timer.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "engine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log.h"
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
    bool autoOpenedForPhase = false;
    rm::Phase lastPhase = rm::Phase::Idle;
    std::string lastStatus;
    std::uint64_t seedInput = 0;
    int selectedCharacter = 0;
    std::string diagnosticsResult;
    // Set when any mod code threw; the game keeps running with vanilla rules.
    std::atomic<bool> disabled{false};
    std::string disabledReason;
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
    }
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
    const rm::Status status = run.BeginArena(now);
    Report(status, "begin arena");
    g->arenaEndRequested = false;
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
    const bool hold = g->settings.timerPolicy == "hold" && g->run->phase() == rm::Phase::ArenaActive;
    SetMercTimerFloor(hold ? g->settings.timerHoldSeconds : -1.0f);
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

void SettleFinishedRun() {
    rm::RunController& run = *g->run;
    const rm::Phase phase = run.phase();
    if (phase == rm::Phase::RunComplete || phase == rm::Phase::RunFailed) {
        const auto summary = run.EndRun();
        if (summary.ok()) {
            SetStatus("run ended (" + summary->end_reason + "): +" + std::to_string(summary->credits_banked) +
                      " credits banked");
            overlay::SetFocused(true);
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
        RM_LOGI("player_hit raw=%d applied=%d hp %d->%d/%d", r.rawDamage, r.appliedDamage, r.hpBefore, r.hpAfter,
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
        std::lock_guard lock(g->mutex);
        const double now = Now();
        ObserveMatch(now);
        if (g->settings.uiDemo) {
            DriveUiDemo(now);
        } else {
            TryBeginArena(now);
        }
        DriveArena(now);
        ApplyHeals();
        PollDevCommand(now);
        SettleFinishedRun();
        UpdateTimerPolicy();
        const rm::Phase phase = g->run->phase();
        if (phase != g->lastPhase) {
            RM_LOGI("phase %s -> %s", std::string(rm::PhaseName(g->lastPhase)).c_str(),
                    std::string(rm::PhaseName(phase)).c_str());
            if (phase == rm::Phase::ArenaResults) overlay::SetFocused(true);
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
    } else {
        ImGui::TextDisabled("Rogue Mercenaries - F7 to start a run");
    }
    for (const auto& toast : g->toasts.Active()) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1.0f), "%s", toast.text.c_str());
    }
    ImGui::End();
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
        ImGui::RadioButton(c.name.c_str(), &g->selectedCharacter, static_cast<int>(i));
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
    ImGui::SeparatorText("Run");
    ImGui::TextWrapped(
        "Pick the same character in The Mercenaries, then start a match. Each match is one arena: defeat the "
        "required enemies, pick a reward, shop, choose the next route and press Retry.");
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
    DrawHud(snapshot);
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
