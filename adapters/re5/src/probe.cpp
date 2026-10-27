#include "probe.h"

#include <windows.h>

#include <mutex>
#include <string>

#include "engine.h"
#include "hooks.h"
#include "imgui.h"
#include "log.h"

namespace rmre5::probe {
namespace {

int g_intervalMs = 0;
DWORD g_lastProbe = 0;

struct Observed {
    int mode = -999;
    int stage = -999;
    int playerCount = -1;
    std::uintptr_t players[4]{};
    int controls[4]{-9, -9, -9, -9};
};
Observed g_seen;

// Copy of the last game-thread snapshot for the render thread.
std::mutex g_viewMutex;
engine::PlayersSnapshot g_viewPlayers;
engine::EnemiesSnapshot g_viewEnemies;
std::optional<engine::GameWork> g_viewWork;

void LogPlayers(const engine::PlayersSnapshot& players) {
    RM_LOGI("probe players count=%d local_slot=%d", players.count, players.localSlot);
    for (const auto& p : players.players) {
        if (p.object) {
            RM_LOGI("probe   slot=%d obj=%08X class=%s number=%d control=%d hp=%d/%d", p.slot,
                    static_cast<unsigned>(p.object), p.className.c_str(), p.number, p.control, p.hp.hp, p.hp.max);
        }
    }
}

}  // namespace

void Configure(const Settings& settings) { g_intervalMs = settings.probeIntervalMs; }

void OnGameTick() {
    const auto players = engine::ReadPlayers();
    const auto work = engine::ReadGameWork();
    const auto enemies = engine::ReadEnemies();

    // Change-triggered facts are always logged; they are rare.
    const int mode = work ? work->mode : -1;
    const int stage = work ? work->stage : -1;
    if (mode != g_seen.mode || stage != g_seen.stage) {
        RM_LOGI("probe game work mode %d -> %d, stage %d -> %d (obj=%08X)", g_seen.mode, mode, g_seen.stage, stage,
                work ? static_cast<unsigned>(work->object) : 0u);
        g_seen.mode = mode;
        g_seen.stage = stage;
    }
    bool playersChanged = players.count != g_seen.playerCount;
    for (int i = 0; i < 4; ++i) {
        playersChanged |= players.players[i].object != g_seen.players[i] ||
                          (players.players[i].object && players.players[i].control != g_seen.controls[i]);
    }
    if (playersChanged) {
        LogPlayers(players);
        g_seen.playerCount = players.count;
        for (int i = 0; i < 4; ++i) {
            g_seen.players[i] = players.players[i].object;
            g_seen.controls[i] = players.players[i].control;
        }
    }

    const DWORD now = GetTickCount();
    if (g_intervalMs > 0 && now - g_lastProbe >= static_cast<DWORD>(g_intervalMs)) {
        g_lastProbe = now;
        const auto& status = Status();
        RM_LOGI("probe tick=%llu presents=%llu enemy_hits=%llu player_hits=%llu enemies listed=%d alive=%d "
                "mode=%d stage=%d merc_timer=%.2f merc_score=%d merc_kills=%d countdown=%.2f state=%d",
                status.ticks, status.presents, status.enemyHits, status.playerHits, enemies.listed, enemies.alive,
                mode, stage, work ? work->mercTimer : 0.0f, work ? work->mercScore : 0, work ? work->mercKills : 0,
                ReadMercTimer().countdown, ReadMercTimer().state);
        if (players.count > 0) {
            LogPlayers(players);
        }
    }

    std::lock_guard lock(g_viewMutex);
    g_viewPlayers = players;
    g_viewEnemies = enemies;
    g_viewWork = work;
}

void DrawDeveloperPanel() {
    engine::PlayersSnapshot players;
    engine::EnemiesSnapshot enemies;
    std::optional<engine::GameWork> work;
    {
        std::lock_guard lock(g_viewMutex);
        players = g_viewPlayers;
        enemies = g_viewEnemies;
        work = g_viewWork;
    }
    const auto& caps = engine::Caps();
    const auto& status = Status();
    ImGui::Text("build=%s combat=%s input=%s d3d9=%s", caps.buildIdentity ? "ok" : "NO", status.combat ? "ok" : "off",
                status.input ? "ok" : "off", status.d3d9 ? "ok" : "off");
    ImGui::Text("ticks=%llu presents=%llu hits(enemy/player)=%llu/%llu", status.ticks, status.presents,
                status.enemyHits, status.playerHits);
    if (work) {
        ImGui::Text("mode=%d stage=%d merc timer=%.1f score=%d", work->mode, work->stage, work->mercTimer,
                    work->mercScore);
    } else {
        ImGui::TextUnformatted("game work: unavailable");
    }
    ImGui::Text("players=%d local slot=%d enemies listed=%d alive=%d", players.count, players.localSlot,
                enemies.listed, enemies.alive);
    for (const auto& p : players.players) {
        if (p.object) {
            ImGui::BulletText("[%d] %s #%d ctl=%d hp %d/%d", p.slot, p.className.c_str(), p.number, p.control, p.hp.hp,
                              p.hp.max);
        }
    }
}

}  // namespace rmre5::probe
