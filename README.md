# Rogue Mercenaries

A single-player roguelite mod for **Resident Evil 5 Gold Edition (Steam, build 11465250, `re5dx9.exe` 1.0.0.129)**. Each run is three arenas on three random Mercenaries stages, with no game screens in between. Between arenas you take a reward (items, a weapon or supplies), shop with temporary Supplies and choose a route; perks are per-character traits unlocked only in the mod's menu, and a run banks permanent Credits for unlocks.

## Status (2026-10-27)

| Area | State |
|---|---|
| Portable core (rules, economy, saves, telemetry) | 63 test cases pass on x64 and Win32; the 10,000-run simulation passes all invariants ([research/sim](research/sim/summary-2026-10-27.md)) |
| RE5 loader (`dinput8.dll` proxy) | Verified in game: loads, checks 13 engine signatures, exits cleanly |
| Combat hooks | Verified in game in The Mercenaries:<br>• real kills are attributed to the player<br>• incoming damage is modified (perk −5%)<br>• outgoing damage is modified (Sharpshooter stacks + weapon upgrade)<br>• death ends the run |
| Arena loop | Verified in game:<br>• each match is an arena<br>• the mod ends the match through the game's countdown when the kill target is met<br>• three arenas, then credits are banked and still there after a game restart |
| Overlay | HUD and run menu; digit/Enter hotkeys and mouse |
| Not yet tested | Story mode with the AI partner; Sheva and other characters in game; balance with real players |

Details:
- [research/capabilities.md](research/capabilities.md): what is verified and how.
- [research/re5-engine-map.md](research/re5-engine-map.md): engine addresses.
- [TASKS.md](TASKS.md): checklist with evidence.

## Build

Requires Visual Studio 2022 Build Tools (C++ x86/x64, Windows SDK) and Python 3 for one test.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1
```

This builds and tests x64 and Win32 and produces `build\win32\adapters\re5\Release\dinput8.dll`.

## Install / uninstall

The installer refuses to overwrite a `dinput8.dll` it did not install and records every file it creates. Close the game first.

```powershell
# install (adapter.test.json = developer menu + detailed logs, audio on)
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Mode install -AdapterConfig configs\adapter.test.json
# remove exactly what was installed; also checks re5dx9.exe is unchanged
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Mode uninstall
```

Installed files: `<game>\dinput8.dll` and `<game>\RogueMercenaries\` (`configs\*.json` and `adapter.json`). Your mod save and logs live in `%LOCALAPPDATA%\RogueMercenaries\`. The game's own save is never touched.

## Playing

1. Launch RE5 from Steam. Top right shows *Rogue Mercenaries – Play menu or F7*.
2. In the game's **Play** menu choose **ROGUE MERCENARIES**, the last row (Up from the first row gets there). It opens the mod's own menu, drawn by the game itself:
   - **START RUN**: Solo, a random stage (any of the eight) and your character and skin are handed to the game's own menu script, so no setup screen appears; the screen fades to black and the match loads about 1.5 s later. The run starts with it.
   - **CHARACTERS**: pick who you play. Only Chris and Sheva start unlocked; every other character shows the challenge that unlocks them (for example *Complete a run as Chris* unlocks Jill). The game's own character select must also have them unlocked. Choosing a character opens its **SKINS**: a skin changes the archetype a little (the weapons a run can start with, the starting perk, extra character skills), and most skins have their own challenge.
   - **PERKS**: every perk with its description; locked ones are bought with Credits.
   - **CREDITS**: your balance, lifetime records and weapon unlocks.
   Esc goes back one screen; locked entries are red, like the game's own disabled rows.
3. Every run rolls a starting loadout from a point budget (`configs/loadouts.json`): one of the skin's weapons, then equipment while points last; unspent points become supplies. The HUD shows the roll. Grenades and sprays are placed in your real inventory when the first arena starts (on top of the character's own loadout; the game's grid has nine slots, so the roll adds at most two kinds of item); herbs are tracked by the run only until their item ids are confirmed, and the weapon pick still only sets the damage class.
   *Without the menu row:* go to **The Mercenaries → Solo**, press **F7**, select the same character and press **Enter** (Start run). Set `"direct_start": false` in `adapter.json` to go through the game's own setup screens yourself.
4. The run starts with the match. The HUD shows *Enemies left*. The Mercenaries countdown is held at 90 s, so the kill target decides the arena.
5. When the target is reached the match keeps running and the run menu opens (the countdown stays held):
   - **Enter** — continue;
   - **1–3** — take a reward (items go straight into your inventory; if nothing fits you get the item's value in supplies);
   - digits buy in the shop (items, a weapon, an upgrade, a heal), **Enter** leaves it; a purchase that does not fit is refunded;
   - a digit picks the next route; the mod then ends the match, skips the game's results screen and restarts on another random stage with your inventory carried over (set `"stage_per_arena": false` in `adapter.json` to stay on one map and start the next arena at once).
6. After arena 3 the run ends, its Credits are banked and the game's own results screen closes the match. Character perks (for example Chris's *Pocket Grenades*: a hand grenade every four kills) trigger during the arenas; a grant that does not fit shows "inventory full".

While the menu is open the game receives no input. The mouse works too, and so do the arrow keys with Space.

About the Play menu row:
- it appears only with Latin-alphabet game languages; with Japanese, Chinese or Russian text use F7;
- reach it with the keyboard or a gamepad, because mouse hover does not select it;
- set `"native_menu": false` in `adapter.json` to turn it off.

## What still needs a human

Already verified in game by automation (2026-10-27):
- the combat hooks (T1);
- the three-arena loop (T2);
- death, unlocks, diagnostics export and crash recovery.

Automated aiming is blind, so these still need a person:
1. **Feel and balance:** play one normal run (default content: 20–38 kills per arena) with Chris. Say whether the kill targets, the 90 s countdown hold and the reward pacing feel right.
2. **Sheva:** start a run as Sheva in Mercenaries and clear one arena.
3. **Story mode (optional):** start a chapter, then F7 → Start run → **Begin arena now (dev, any mode)**. Requires `developer_mode`, e.g. install with `adapter.test.json`. Do a few kills, and check that only *your* kills count, not Sheva's.

Install with `adapter.test.json` to get the developer section and detailed logs. Afterwards send F7 → Developer → **Export diagnostics** (`%LOCALAPPDATA%\RogueMercenaries\rogue-mercenaries-diagnostics.zip`) and a sentence on how it felt.

## Developer automation

`scripts\smoke.ps1` runs an unattended check. It installs the mod, launches the game through Steam (muted, in the background), captures frames from inside the game, then closes and uninstalls. `scripts\game_config.ps1 -Mode test|restore` switches the game to a small windowed mode and back. See [TESTING.md](TESTING.md).

## Project layout

```
core/        portable roguelite rules (no game code)      tests/       Catch2 tests
configs/     perks, weapons, characters, routes, economy   simulation/  rm_sim (10k-run balance)
adapters/re5 dinput8.dll: hooks, overlay, mod glue        scripts/     build/deploy/smoke/RE tools
research/    capability ledger, engine map, sim reports    third_party/ MinHook, Dear ImGui, nlohmann/json, Catch2
```
