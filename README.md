# Rogue Mercenaries

A single-player roguelite mod for **Resident Evil 5 Gold Edition (Steam, build 11465250, `re5dx9.exe` 1.0.0.129)**. Each run is three arenas, Mercenaries stages and campaign areas picked on a branching run map, and then a boss, with no game screens in between. Between arenas you take a reward (items, a weapon or supplies), shop with temporary Supplies and choose the next arena; perks are per-character traits unlocked only in the mod's menu, and a run banks permanent Credits for unlocks.

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
   - **START RUN**: Solo, a random arena (one of the eight Mercenaries stages or one of the campaign areas in `configs/maps.json`) and your character and skin are handed to the game's own menu script, so no setup screen appears; the screen fades to black and the match loads about 1.5 s later. The run starts with it.
   - **CHARACTERS**: pick who you play. All eight characters and every skin are available for now (the challenge unlocks come back once the roster is settled). The game's own character select must also have them unlocked. Choosing a character opens its **SKINS**: a skin changes the archetype a little (the weapons a run can start with, the starting perk, extra character skills), and most skins have their own challenge.
   - **PERKS**: three slots for the selected character, one per kind. *Style* defines how the character plays (Chris restocks a spray per arena, Sheva heals on kills); *Supply* is what a run starts with (grenade pouch, medkit, a shotgun for Chris); *Support* helps in bad situations (Second Wind, Last Stand below 30% health). Each slot opens its list, every perk with its icon: pick one (it shows SELECTED), press it again to clear the slot, and buy locked ones with Credits. The style slot falls back to the character's own trait. Skins carry the small percentage tweaks.
   - **CREDITS**: your balance, lifetime records and weapon unlocks.
   Esc goes back one screen; locked entries are red, like the game's own disabled rows.
3. Every run starts with one gun: the game's Mercenaries loadout is cut down to the weapon in your hands, its ammo and the knife when the first arena starts (for most costumes that is the pistol; Sheva keeps her VZ61), and your Supply perks then add their items and weapons (a weapon arrives with a full magazine). Everything else comes from rewards and the shop, which can offer any gun of `configs/weapons.json`: during a run every stage loads all of their models along with yours (about 20 MB), and what you buy comes with you to the next stage. The game's grid has nine slots; the number keys 1 to 4 pick its first four cells. (`configs/loadouts.json` still holds the item catalog and an optional point-budget roll, off by default.)
   *Without the menu row:* go to **The Mercenaries → Solo**, press **F7**, select the same character and press **Enter** (Start run). Set `"direct_start": false` in `adapter.json` to go through the game's own setup screens yourself.
4. The run starts with the match. There is no clock and no score: the Mercenaries countdown, score and combo panels stay off screen for the whole run, and the game can no longer end the match by itself (the countdown is held underneath and its 150-kill limit is off), so the kill target decides the arena. In the clock's place the same big orange digits count the *enemies left*; the arena number is on the left and your supplies on the right, in the game's font. (`"merc_hud": true` in `adapter.json` shows the game's clock and score again, and the count moves into the label.) The mod spawns the arena's enemies itself (`configs/maps.json`: the spawn points and enemy types each stage uses in the original game): the game's own waves stay off for the whole run, up to 12 enemies are alive at once, and the arena ends when exactly its kill target has died, however they died. Enemies are spawned the way the game's own Mercenaries spawns them (weapons, plagas and looks as surveyed per stage) and already know where you are, so they come for you from the start. Each stage's special enemies (the big ones the game keeps one of at a time) join once a quarter of the arena is dead: one at a time, one more per route tier above the first. The first enemies come 3 s after an arena starts. (`"director": false` in `adapter.json` brings back the game's own waves.)
   The fourth and last arena is a boss, chosen in the last NEXT ROUTE: the Uroboros in the pump rooms or the giant Uroboros in the capsule dome, alone, with its health in the big digits (in percent) and its name under them; it dies to gunfire (the Uroboros' glowing weak spots take more) and the run is complete. Campaign areas always start you at the same spot (the game would pick one of the area's entrances at random).
   Arenas are also campaign areas (Kijuju's streets and slums, the marsh village, the port, caves and ruins, the facility, the freighter, the volcano, Spencer's mansion): the area loads in place of a Mercenaries stage, starting where the story enters it but without its cutscenes or story enemies, and the director spawns a similar stage's Mercenaries enemies at the area's own AI waypoints around you. `"campaign_arenas": false` keeps runs on the Mercenaries stages.
5. When the target is reached the world pauses and the mod's own screens appear in the game's menu font (no overlay):
   - **ARENA N CLEARED** shows the rating, time, kills, supplies and credits; **Enter** continues;
   - **CHOOSE A REWARD** lists the offers (items go straight into your inventory; if nothing fits you get the item's value in supplies); a reroll row appears when you hold a token;
   - **SHOP** lists heal, reroll token, an upgrade for a weapon you hold, a weapon you don't have yet, and items (grenades, sprays, herbs, ammo bundles) with their prices, coloured by rarity; bought weapons and items go straight into your inventory (a purchase that does not fit is refunded). **ORGANIZE INVENTORY** opens the game's own Organize/Buy screen with the run in it: your nine cells as the first grid, under your character's name (the second grid, a partner's, stays empty), your supplies as money (100 per supply) and the run's storage. Move items, sell them (the money comes back as supplies), buy the guns and ammo your campaign has unlocked (they land in the storage and stay there for the run) and upgrade a gun's firepower, reload speed, capacity or critical rate; an upgrade takes effect in the match and stays on that gun for the rest of the run. **Esc** then **Enter** ("A punto") returns to the SHOP. Your campaign save is not changed: its data is copied before the screen opens and written back when it closes. Where the screen cannot load, the row shows the mod's own nine cells instead (pick one, then another, to swap them; `"native_item_box": false` in `adapter.json` always uses them). The layout is kept on the next stage. The last row leaves;
   - **NEXT ROUTE** lists the arenas the run map leads to next (before the last one: the bosses, e.g. `UROBOROS   BOSS`), with their tier (tier 2 in amber, tier 3 in red); the line below names the route (enemy count, rewards, a curse when it carries one) and the arenas that choice leads on to, so a pick also decides what can follow. The pick ends the match, skips the game's results screen and restarts in that arena with your inventory and health carried over; the top line names the arena and route as it begins (set `"stage_per_arena": false` in `adapter.json` to stay on one map).
   Arrow keys move, **Enter** confirms. Each arena places three **chests**: the clock statues Mercenaries uses for time bonuses, at spots where the game itself puts them (the stage's own statues stay away during a run). Shoot one or press the action key at it; it gives supplies and often an item (ammo, grenades, herbs, a spray) or, rarely, a gun; the line at the top shows what you got. (`"native_match_ui": false` brings the overlay windows back.)
6. After arena 3 a **RUN COMPLETE** screen shows the credits you banked; leaving it ends the match through the game's own screens. Character perks (for example Chris's *Pocket Grenades*: a hand grenade every four kills) trigger during the arenas; a grant that does not fit shows "inventory full".

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
1. **Feel and balance:** play one normal run (default content: 20–38 kills per arena) with Chris. Say whether the kill targets and the reward pacing feel right.
2. **Sheva:** start a run as Sheva in Mercenaries and clear one arena.
3. **Story mode (optional):** start a chapter, then F7 → Start run → **Begin arena now (dev, any mode)**. Requires `developer_mode`, e.g. install with `adapter.test.json`. Do a few kills, and check that only *your* kills count, not Sheva's.

Install with `adapter.test.json` to get the developer section and detailed logs. Afterwards send F7 → Developer → **Export diagnostics** (`%LOCALAPPDATA%\RogueMercenaries\rogue-mercenaries-diagnostics.zip`) and a sentence on how it felt.

## Developer automation

`scripts\smoke.ps1` runs an unattended check. It installs the mod, launches the game through Steam (muted, in the background), captures frames from inside the game, then closes and uninstalls. `scripts\game_config.ps1 -Mode test|restore` switches the game to a small windowed mode and back. See [TESTING.md](TESTING.md).

## Project layout

```
core/        portable roguelite rules (no game code)      tests/       Catch2 tests
configs/     perks, weapons, characters, routes, economy,  simulation/  rm_sim (10k-run balance)
             icons (scripts/make_icons.py draws them)
adapters/re5 dinput8.dll: hooks, overlay, mod glue        scripts/     build/deploy/smoke/RE tools
research/    capability ledger, engine map, sim reports    third_party/ MinHook, Dear ImGui, nlohmann/json, Catch2
```
