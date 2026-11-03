#include "settings.h"

#include <windows.h>

#include <shlobj.h>

#include <fstream>

#include "nlohmann/json.hpp"

namespace rmre5 {

Paths ResolvePaths(void* module) {
    Paths paths;
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(static_cast<HMODULE>(module), buffer, MAX_PATH);
    paths.gameDir = std::filesystem::path(buffer).parent_path();
    paths.modDir = paths.gameDir / L"RogueMercenaries";
    paths.configDir = paths.modDir / L"configs";

    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        paths.dataDir = std::filesystem::path(local) / L"RogueMercenaries";
        CoTaskMemFree(local);
    } else {
        paths.dataDir = paths.modDir / L"data";
    }
    paths.logDir = paths.dataDir / L"logs";
    return paths;
}

Settings LoadSettings(const Paths& paths, std::string* error) {
    Settings settings;
    const auto file = paths.modDir / L"adapter.json";
    std::ifstream in(file);
    if (!in) {
        if (error) {
            *error = "adapter.json not found; using defaults";
        }
        return settings;
    }
    try {
        const auto json = nlohmann::json::parse(in);
        settings.logLevel = json.value("log_level", settings.logLevel);
        settings.overlayKey = json.value("overlay_key_vk", settings.overlayKey);
        settings.developerMode = json.value("developer_mode", settings.developerMode);
        settings.timerPolicy = json.value("timer_policy", settings.timerPolicy);
        settings.timerHoldSeconds = json.value("timer_hold_seconds", settings.timerHoldSeconds);
        settings.mercHud = json.value("merc_hud", settings.mercHud);
        settings.endMatchOnClear = json.value("end_match_on_clear", settings.endMatchOnClear);
        settings.stagePerArena = json.value("stage_per_arena", settings.stagePerArena);
        settings.campaignArenas = json.value("campaign_arenas", settings.campaignArenas);
        settings.nativeItemBox = json.value("native_item_box", settings.nativeItemBox);
        settings.nativeMatchUi = json.value("native_match_ui", settings.nativeMatchUi);
        settings.director = json.value("director", settings.director);
        settings.nativeMenu = json.value("native_menu", settings.nativeMenu);
        settings.directStart = json.value("direct_start", settings.directStart);
        settings.stageCount = json.value("stage_count", settings.stageCount);
        settings.specialKey = json.value("special_key_scancode", settings.specialKey);
        if (json.contains("special_pad") && json["special_pad"].is_array()) {
            settings.specialPad.clear();
            for (const auto& b : json["special_pad"]) {
                if (b.is_string()) settings.specialPad.push_back(b.get<std::string>());
            }
        }
        settings.special2Key = json.value("special2_key_scancode", settings.special2Key);
        if (json.contains("special2_pad") && json["special2_pad"].is_array()) {
            settings.special2Pad.clear();
            for (const auto& b : json["special2_pad"]) {
                if (b.is_string()) settings.special2Pad.push_back(b.get<std::string>());
            }
        }
        if (json.contains("dev")) {
            const auto& dev = json["dev"];
            settings.probeIntervalMs = dev.value("probe_interval_ms", settings.probeIntervalMs);
            settings.probeHits = dev.value("probe_hits", settings.probeHits);
            settings.muteAudio = dev.value("mute_audio", settings.muteAudio);
            settings.disableInputHooks = dev.value("disable_input_hooks", settings.disableInputHooks);
            settings.disableD3d9Hooks = dev.value("disable_d3d9_hooks", settings.disableD3d9Hooks);
            settings.disableCombatHooks = dev.value("disable_combat_hooks", settings.disableCombatHooks);
            settings.disableOverlayDraw = dev.value("disable_overlay_draw", settings.disableOverlayDraw);
            settings.uiDemo = dev.value("ui_demo", settings.uiDemo);
            settings.devInvulnerable = dev.value("invulnerable", settings.devInvulnerable);
            settings.devOutgoingMultiplier = dev.value("outgoing_multiplier", settings.devOutgoingMultiplier);
            settings.devIncomingMultiplier = dev.value("incoming_multiplier", settings.devIncomingMultiplier);
            settings.devGrantPerks = dev.value("grant_perks", settings.devGrantPerks);
            settings.devMenuProbe = dev.value("menu_probe", settings.devMenuProbe);
        }
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("adapter.json invalid: ") + e.what();
        }
        return Settings{};
    }
    return settings;
}

}  // namespace rmre5
