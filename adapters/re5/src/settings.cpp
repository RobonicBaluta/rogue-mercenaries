#include "settings.h"

#include <windows.h>

#include <shlobj.h>

#include <fstream>

#include "nlohmann/json.hpp"

namespace rmre5 {

namespace {

// "kind or element" -> path entries of `json[key]` into `into` ("" removes one).
void MergeLooks(const nlohmann::json& json, const char* key, std::map<std::string, std::string>& into) {
    if (!json.contains(key) || !json[key].is_object()) return;
    for (const auto& [name, path] : json[key].items()) {
        if (!path.is_string()) continue;
        if (path.get<std::string>().empty()) {
            into.erase(name);
        } else {
            into[name] = path.get<std::string>();
        }
    }
}

Settings LoadAdapterSettings(const Paths& paths, std::string* error);

}  // namespace

Settings LoadSettings(const Paths& paths, std::string* error) {
    Settings settings = LoadAdapterSettings(paths, error);
    std::ifstream in(paths.dataDir / L"element_looks.json");
    if (in) {
        try {
            const auto json = nlohmann::json::parse(in);
            MergeLooks(json, "element_effects", settings.elementEffects);
            MergeLooks(json, "flame_streams", settings.flameStreams);
        } catch (const std::exception&) {
            // A broken file: the defaults and adapter.json's looks.
        }
    }
    return settings;
}

bool SaveElementLook(const Paths& paths, const std::vector<std::string>& kinds, const std::string& streamElement,
                     const std::string& path) {
    const auto file = paths.dataDir / L"element_looks.json";
    nlohmann::json json = nlohmann::json::object();
    {
        std::ifstream in(file);
        if (in) {
            try {
                json = nlohmann::json::parse(in);
            } catch (const std::exception&) {
                json = nlohmann::json::object();
            }
        }
    }
    if (!json.is_object()) json = nlohmann::json::object();
    for (const std::string& kind : kinds) json["element_effects"][kind] = path;
    if (!streamElement.empty()) json["flame_streams"][streamElement] = path;
    std::error_code ec;
    std::filesystem::create_directories(paths.dataDir, ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << json.dump(2) << "\n";
    return static_cast<bool>(out);
}

Paths ResolvePaths(void* module) {
    Paths paths;
    wchar_t buffer[MAX_PATH];
    // The game's folder is the executable's: the mod's DLL may be the game's dinput8.dll or a
    // plugin another loader loaded from a subfolder (plugins\, scripts\; 2026-11-18, a player:
    // the RE5 input fixes need their own dinput8.dll).
    (void)module;
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
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

namespace {

Settings LoadAdapterSettings(const Paths& paths, std::string* error) {
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
        settings.sniperNormalAim = json.value("sniper_normal_aim", settings.sniperNormalAim);
        settings.everythingUnlocked = json.value("everything_unlocked", settings.everythingUnlocked);
        settings.scopeKey = json.value("scope_key_scancode", settings.scopeKey);
        settings.effectGallery = json.value("effect_gallery", settings.effectGallery);
        settings.customEffects = json.value("custom_effects", settings.customEffects);
        settings.elementArcs = json.value("element_arcs", settings.elementArcs);
        settings.flameStreamScale = json.value("flame_stream_scale", settings.flameStreamScale);
        settings.flameStreamCompanion = json.value("flame_stream_companion", settings.flameStreamCompanion);
        MergeLooks(json, "element_effects", settings.elementEffects);  // "": no look for that kind
        MergeLooks(json, "flame_streams", settings.flameStreams);      // "": the flame for that element
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
            settings.devNoExtraContent = dev.value("no_extra_content", settings.devNoExtraContent);
            settings.devMercenariesLocked = dev.value("mercenaries_locked", settings.devMercenariesLocked);
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

}  // namespace

}  // namespace rmre5
