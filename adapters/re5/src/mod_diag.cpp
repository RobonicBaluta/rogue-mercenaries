// Diagnostic mod used until the roguelite core is linked: logs engine events,
// never changes gameplay (all multipliers are 1).

#include "imgui.h"
#include "log.h"
#include "mod.h"
#include "overlay.h"
#include "probe.h"

namespace rmre5 {
namespace {

Settings g_settings;

class DiagnosticSink final : public HookSink {
public:
    float OutgoingMultiplier(const OutgoingHit&) override { return 1.0f; }

    void EnemyHitDone(const EnemyHitResult& r) override {
        if (g_settings.probeHits || r.killed) {
            RM_LOGI("probe enemy_hit enemy=%08X attacker=%08X player=%d local=%d hp %d->%d/%d raw=%d applied=%d killed=%d",
                    static_cast<unsigned>(r.enemy), static_cast<unsigned>(r.attacker), r.attackerIsPlayer,
                    r.attackerIsLocal, r.hpBefore, r.hpAfter, r.maxHp, r.rawDamage, r.appliedDamage, r.killed);
        }
    }

    IncomingDecision Incoming(const IncomingHit&) override { return {}; }

    void PlayerHitDone(const PlayerHitResult& r) override {
        RM_LOGI("probe player_hit player=%08X raw=%d applied=%d hp %d->%d/%d", static_cast<unsigned>(r.player),
                r.rawDamage, r.appliedDamage, r.hpBefore, r.hpAfter, r.maxHp);
    }

    void GameTick() override { probe::OnGameTick(); }
    bool BlockGameInput() override { return overlay::IsFocused(); }
};

DiagnosticSink g_sink;

}  // namespace

void ModInit(const Paths&, const Settings& settings) { g_settings = settings; }
void ModCapabilitiesReady() {}
void ModShutdown() {}
HookSink* ModSink() { return &g_sink; }

void ModDraw() {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("Rogue Mercenaries (diagnostic)", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    probe::DrawDeveloperPanel();
    ImGui::End();
}

}  // namespace rmre5
