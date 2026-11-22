#pragma once

// adapter.json (next to the mod's configs in <game>\RogueMercenaries\).
// Missing or invalid files fall back to these defaults and log why.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rmre5 {

struct Paths {
    std::filesystem::path gameDir;     // folder containing re5dx9.exe and this DLL
    std::filesystem::path modDir;      // <game>\RogueMercenaries
    std::filesystem::path configDir;   // <game>\RogueMercenaries\configs
    std::filesystem::path dataDir;     // %LOCALAPPDATA%\RogueMercenaries
    std::filesystem::path logDir;      // <data>\logs
};

struct Settings {
    std::string logLevel = "info";
    int overlayKey = 0x76;  // VK_F7
    bool developerMode = false;
    // Diagnostics: log engine state every N ms (0 = off) and every hit/kill.
    int probeIntervalMs = 0;
    bool probeHits = false;
    // Silences this process' audio session; for unattended test launches.
    bool muteAudio = false;
    // Component switches for bisecting startup problems.
    bool disableInputHooks = false;
    bool disableD3d9Hooks = false;
    bool disableCombatHooks = false;
    bool disableOverlayDraw = false;
    // Scripted run with synthetic kills on a sandbox profile, for UI captures.
    bool uiDemo = false;
    // Test aids for automated play: no damage taken, extra outgoing damage.
    bool devInvulnerable = false;
    bool devHideLabel = false;  // dev "hide_label": no status line at the top right (screenshots for the mod page)
    float devOutgoingMultiplier = 1.0f;
    float devIncomingMultiplier = 1.0f;
    // Perks granted (under normal rules) right after a run starts.
    std::vector<std::string> devGrantPerks;
    // Hooks every menu screen (uId*) to log which ones are live.
    bool devMenuProbe = false;
    // Test aids for a PC unlike the developer's: the game told it owns no Extra Content, and the
    // save's The Mercenaries row closed.
    bool devNoExtraContent = false;
    bool devMercenariesLocked = false;
    // Adds ROGUE MERCENARIES to the game's Play menu.
    bool nativeMenu = true;
    // START RUN drives the game's setup screens itself: Solo, a random stage
    // among the first stageCount, and the selected character's costume slot.
    bool directStart = true;
    int stageCount = 8;
    // Mercenaries timer handling while an arena is active:
    //   "hold"  keeps the countdown at or above timerHoldSeconds
    //   "game"  leaves the game's own countdown alone
    std::string timerPolicy = "hold";
    float timerHoldSeconds = 90.0f;
    // The game's Mercenaries clock and score panels during a run (off: only the mod's HUD).
    bool mercHud = false;
    // End the Mercenaries match (timer -> 0) once the arena's kill budget is met.
    bool endMatchOnClear = false;  // true: the game's time-up ends the match after every arena
    // Each route leads to a new random stage: the match is ended by the game's time-up, its
    // results screen is accepted behind a black fade, the title-flow script restarts the
    // match on the next stage and the inventory is carried over. false: one map per run.
    bool stagePerArena = true;
    // Runs also pick the campaign areas listed in maps.json (stage numbers 100 and up), loaded
    // in place of a Mercenaries stage. false: Mercenaries stages only.
    bool campaignArenas = true;
    // The SHOP's organize row opens the game's own Organize/Buy screen with the run in it (the
    // campaign save data is copied before and restored after). false: the mod's grid screen.
    bool nativeItemBox = true;
    // Results, rewards, shop and route as native in-match screens (world paused); false keeps
    // the overlay windows for them.
    bool nativeMatchUi = true;
    // Arenas on stages listed in maps.json are run by the mod's director: the game's own
    // spawns stay off for the whole match and the run spawns exactly the enemies it counts.
    bool director = true;
    // A build's special move (M8): the keyboard key (DirectInput scancode, default Q) and the
    // controller buttons pressed together (names: A B X Y LB RB BACK START LS RS; XInput pads,
    // and DirectInput pads through the game's config.ini [JOYPAD] numbers).
    int specialKey = 0x10;
    std::vector<std::string> specialPad = {"LB", "RB"};
    // A build's second special (Wesker's burst): X, and both sticks pressed together.
    int special2Key = 0x2D;
    std::vector<std::string> special2Pad = {"LS", "RS"};
    // Sniper rifles aim over the shoulder like any gun in a run; the scope is one button away while
    // aiming: this key (DirectInput scancode, default Z) or the right stick's click.
    bool sniperNormalAim = true;
    // The everything-unlocked package: everything open, on a profile file of its own; the normal
    // progress is not read or written (rm::ProfileFileName, scripts/package_release.ps1 -Unlocked).
    bool everythingUnlocked = false;
    // The map editor (F8 in an arena, MAP EDITOR in the mod menu): adapter.json "map_editor".
    bool mapEditor = false;
    // The tester (characters.json tester_only: a Gatling and a rocket launcher that never run out, for
    // trying bosses and maps): listed only with adapter.json "test_character": true.
    bool testCharacter = false;
    // The info panel (the user, 2026-11-22: "a small top right panel with cool information with slick
    // icons and stuff from data that we don't see in the organic hud"): adapter.json "info_panel".
    bool infoPanel = true;
    int scopeKey = 0x2C;
    // The elements' looks (2026-11-11): an effect list started on each enemy an element's proc
    // reaches, by proc kind ("burn", "ignite_around", "poison", "poison_cloud", "freeze", "shock",
    // "arc", "explode", "shatter"; "<kind>_evolved" for an evolved element's own), played only when
    // the list is in memory (engine::PlayEffect); "a|b": the first of them in memory. The defaults
    // are colour coded (the mod's own effects, rm/effects.h: the flamethrower's small flame orange
    // for fire, cyan for Cryo, green for Toxic; the stun rod's sparks for Shock), each with the
    // game's own list after it.
    std::map<std::string, std::string> elementEffects = {
        {"shock", "effect\\efl\\wp\\wp22\\wp2200|effect\\efl\\wp\\wp21\\wp21e3"},
        {"arc", "effect\\efl\\wp\\wp22\\wp2200|effect\\efl\\wp\\wp21\\wp21e3"},
        {"burn", "effect\\efl\\wp\\wp21\\wp21f3|effect\\efl\\wp\\wp21\\wp2103"},
        {"ignite_around", "effect\\efl\\wp\\wp21\\wp21f3|effect\\efl\\wp\\wp21\\wp2103"},
        {"freeze", "effect\\efl\\wp\\wp21\\wp21c3"},
        {"shatter", "effect\\efl\\wp\\wp21\\wp21c3"},
        {"poison", "effect\\efl\\wp\\wp21\\wp21d3"},
        {"poison_cloud", "effect\\efl\\wp\\wp21\\wp21d3"},
    };
    // Shock's arcs drawn as lightning between the enemies it jumps to, and from a Shock flamethrower
    // to the enemy in its flame (over the picture, where the game's camera shows them).
    bool elementArcs = true;
    // Each arena of a run plays another track: the Mercenaries theme and the battle tracks take turns
    // (music.h; 2026-11-22, the user: "cycle through the games battle themes").
    bool musicRotation = true;
    // The mod's own effects (rm/effects.h, effects_pack.h): <data>\\generated\\rmEffects.arc built from
    // the game's archives at start and loaded with a run's. Off: the game's own looks only.
    bool customEffects = true;
    // The flamethrower's stream by its own element (2026-11-11, the user: "flamethrower electric doesn't
    // do what I said, it still does flame"): the effect list it fires at its nozzle in place of its
    // flame ("shock", "cryo", "toxic", "explosive", "incendiary"; "<element>_evolved" for the evolved
    // one's own), only when in memory; "a|b": the first in memory. Without one, its flame. The
    // defaults: its own flame recoloured (the mod's own effects: violet for Shock, cyan for Cryo,
    // green for Toxic), Shock's falling back to the stun rod's sparks.
    // The recoloured stream's size (its effect's scale) and whether the flame's second list (wp2104's
    // copy: a light or a glow) comes with it (2026-11-11, the user: "can you make the particles way
    // lesser?").
    float flameStreamScale = 0.6f;
    bool flameStreamCompanion = false;
    std::map<std::string, std::string> flameStreams = {
        {"shock", "effect\\efl\\wp\\wp21\\wp21e0|effect\\efl\\wp\\wp22\\wp2200"},
        {"cryo", "effect\\efl\\wp\\wp21\\wp21c0"},
        {"toxic", "effect\\efl\\wp\\wp21\\wp21d0"},
    };
    // The effect gallery (this, or Numpad * in an arena): every few seconds the next effect
    // list in memory plays on the nearest enemy, its name in the HUD line, to pick the elements' looks.
    bool effectGallery = false;
};

Paths ResolvePaths(void* module);
Settings LoadSettings(const Paths& paths, std::string* error);
/// The elements' looks picked in the effect gallery are kept in <data>\\element_looks.json
/// ({"element_effects": {kind: path}, "flame_streams": {element: path}}), read over adapter.json's
/// by LoadSettings. Saves `path` for those kinds (and for that element's flamethrower stream when
/// `streamElement` is not empty); false when the file could not be written.
bool SaveElementLook(const Paths& paths, const std::vector<std::string>& kinds, const std::string& streamElement,
                     const std::string& path);

}  // namespace rmre5
