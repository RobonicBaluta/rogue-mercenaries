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

### Tester package (alpha)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1
powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1
```

This writes `dist\RogueMercenaries-<version>.zip` (the version is `kModVersion` in `adapters\re5\src\version.h`;
the script refuses a DLL of another version or a developer `adapter.json`). The ZIP holds one folder: `Install.bat`,
`Uninstall.bat`, `README.txt` (the tester's guide, from `installer\README.txt`), the scripts in `installer\`, the
files they install (`files\`), licenses and `SHA256SUMS.txt`; no game files, profile or logs. `Install.bat` finds
RE5 through Steam's library folders, checks `re5dx9.exe` against the supported build (asks before installing on
another), copies the game's save to `%LOCALAPPDATA%\RogueMercenaries\save-backups`, keeps another mod's
`dinput8.dll` in `RogueMercenaries\backup`, and records every file in `RogueMercenaries\install.json`. Running it again
replaces an earlier install. `Uninstall.bat` (in the package or in the game's `RogueMercenaries` folder) removes exactly
those files, puts the other `dinput8.dll` back, and asks before deleting progress and logs. A tester's progress starts
empty: the mod's profile lives in `%LOCALAPPDATA%`, which the package never ships or touches.

`-Manual` writes `RogueMercenaries-<version>[-unlocked]-manual.zip` with no script (a mod site's scan flagged the
installer's): the ZIP's root is the game folder (`dinput8.dll`, `RogueMercenaries\`) and its README gives by-hand steps.
`-Plugin` writes `...-plugin.zip`, the same with the DLL as `plugins\RogueMercenaries.dll`, for a game whose input fixes
come with their own `dinput8.dll` plugin loader (a player, 2026-11-18: "it's practically impossible to play RE5 without
these input fixes"). The DLL runs the mod however it is loaded (the game's `dinput8.dll`, a loader's `plugins` folder, an
ASI loader's `.asi`): it finds the game folder from the executable, says in its log how it was loaded, and only one copy
runs if two are present. `tools/plugin_loader` (built as `build\win32\tools\plugin_loader\Release\dinput8.dll`, never
shipped) is a test loader that does what those loaders are documented to do (`plugins\*.dll`, `*.asi`).

## Playing

1. Launch RE5 from Steam. Top right shows *Rogue Mercenaries – Play menu or F7*.
2. In the game's **Play** menu choose **ROGUE MERCENARIES**, the last row (Up from the first row gets there). It opens the mod's own menu, drawn by the game itself:
   - **START RUN**: Solo, a random arena (one of the eight Mercenaries stages or one of the campaign areas in `configs/maps.json`) and your character and skin are handed to the game's own menu script, so no setup screen appears; the screen fades to black and the match loads about 1.5 s later. The run starts with it. Once a longer run is open (see **The ladder** under step 4), START RUN first asks for the **RUN LENGTH**: each length with the boss it ends on; a locked one is red and says which boss opens it.
   - **CHARACTERS**: pick who you play. You start with Chris and Sheva in their BSAA outfits; everyone and everything else is earned (see **Unlocks** below), whatever the game's own Mercenaries unlocks: the run starts the match with the costume directly. A locked row is red and says what opens it and how far along you are (`2 of 5 done`); RUN RESULTS names what a run unlocked. The character under the cursor is shown on the left, with the bust portrait the game's own character select uses (read from your game's files at runtime). Choosing a character opens its **SKINS**, each skin with its own portrait: most skins are a *build*, a strong rule with a cost (see **Skin builds** below), named in the row with its icon; the line below says what it does.
   - **PERKS**: three slots for the selected character, one per kind. *Style* defines how the character plays (Chris restocks a spray per arena, Sheva heals on kills); *Supply* is what a run starts with (grenade pouch, medkit, a shotgun for Chris); *Support* helps in bad situations (Second Wind, Last Stand below 30% health). Each slot opens its list, every perk with its icon: pick one (it shows SELECTED), press it again to clear the slot, and buy locked ones with Credits. The style slot falls back to the character's own trait. The fourth row, **SKIN**, names the selected skin's build and opens SKINS.
   - **CREDITS**: your balance, lifetime records, the last run's results and weapon unlocks. Every RE5 gun is in the pool; the M93R (150), stun rod (100), grenade launcher (350, its explosive rounds then turn up in the shop and rewards), rocket launcher (400, one rocket) and flamethrower (300) are bought here first (added 2026-11-04, not yet played). The flamethrower
     holds 100 fuel, which comes back by itself (2.5 a second once you stop firing for 1.5 s; there are no pumps here); held on
     an enemy it sets Majini burning (they die within seconds), keeps the Chainsaw Majini staggered, and burns the bigger
     ones and the dogs down more slowly. Its tank is only on your back while it is in your hands.
   Every row of the main screen, CREDITS and the run's results has its icon next to it.
   Esc goes back one screen; locked entries are red, like the game's own disabled rows.
3. Every run starts with one gun: the game's Mercenaries loadout is cut down to the weapon in your hands, its ammo and the knife when the first arena starts (for most costumes that is the pistol; Sheva keeps her VZ61), and your Supply perks then add their items and weapons (a weapon arrives with a full magazine and is one of the run's guns like any other: its level and stats, upgrades, refines and the build's elements). Everything else comes from rewards and the shop, which can offer any gun of `configs/weapons.json`: during a run every stage loads all of their models along with yours (about 20 MB), and what you buy comes with you to the next stage. The game's grid has nine slots; the number keys 1 to 4 pick its first four cells. (`configs/loadouts.json` still holds the item catalog and an optional point-budget roll, off by default.)
   *Without the menu row:* go to **The Mercenaries → Solo**, press **F7**, select the same character and press **Enter** (Start run). Set `"direct_start": false` in `adapter.json` to go through the game's own setup screens yourself.
4. The run starts with the match. There is no clock and no score: the Mercenaries countdown, score and combo panels stay off screen for the whole run, and the game can no longer end the match by itself (the countdown is held underneath and its 150-kill limit is off), so the kill target decides the arena. In the clock's place the same big orange digits count the *enemies left*; the arena number is on the left and your supplies on the right, in the game's font. Since 0.2.6 a solo run also has a small **ARENA INTEL** panel in the top right corner: the time (against the arena's par time), kills, damage dealt (and per second), damage taken, supplies, keys, credits, the kill chain, in a boss arena the boss's health and phase, and the rating so far; the supplies and keys move from the top line into it (`"info_panel": false` in `adapter.json` turns it off). (`"merc_hud": true` in `adapter.json` shows the game's clock and score again, and the count moves into the label.) The mod spawns the arena's enemies itself (`configs/maps.json`: the spawn points and enemy types each stage uses in the original game): the game's own waves stay off for the whole run, up to 12 enemies are alive at once, and the arena ends when exactly its kill target has died, however they died. Enemies are spawned the way the game's own Mercenaries spawns them (weapons, plagas and looks as surveyed per stage) and already know where you are, so they come for you from the start. Enemies get harder as the run goes on: the first arena has only Majini with bare hands or small blades, at most 6 at once and no special enemies; later arenas add axes, torches, bows and handguns, then dynamite, stun rods, shotguns and the big special enemies (a Licker, the Executioner), and the hardest routes machine guns, rocket launchers, gatlings and the chainsaw, up to 12 at once (`economy.json` `enemy_threat`; the arena line in `adapter.log` names its level). Each stage's special enemies join once a quarter of the arena is dead: one at a time from threat level 3, one more at level 4. From the second arena on, each arena also brings *guests*: enemies of another stage, planned when the run starts (`economy.json` `run_rosters`): one more kind of Majini and, from threat level 3, another stage's special enemy, never more than five kinds in one arena, and preferably kinds you have not met yet on the way there. Only those load with the stage. NEXT ROUTE says where they come from when the help line has room, and the arena's name on screen is followed by "WITH ENEMIES FROM ...". The first enemies come 3 s after an arena starts. (`"director": false` in `adapter.json` brings back the game's own waves.)
   **The ladder** (`economy.json` `ladder`): the first run is 4 arenas and ends on the Uroboros. Beat him and the next short run ends on a choice in the last NEXT ROUTE: the Uroboros again, U-8 or Popokarimu. Beat U-8 or Popokarimu and the 8-arena run opens: arena 4 is the 4-arena run's boss (your pick), arena 8 Uroboros Mkono, then Uroboros Mkono, Uroboros Aheri or Jill (at 1.3 times their health). Beat Aheri or Jill and the 12-arena run opens: bosses on arenas 4, 8 and 12, the last Uroboros Wesker on the Volcano (at 1.5 times his health). A 12-arena run that took the alternate boss on arena 4 and on arena 8 goes on to a 13th arena against Midnight Wesker; otherwise it ends on arena 12 (the user, 2026-11-08: "you get 1 on 4 one on 8 and one on 12, except if you take both alternate routes, then you get 13th stage"). Each boss has its own length's health wherever it is fought. **Endless** (RUN LENGTH's last row) opens once you beat an 8-arena run: no last arena, the run ends when you fall; a boss every 4 arenas, the bosses of the run lengths you have opened in turn (the 4-arena run's choices, the 8's, the 12's, then Midnight Wesker once you have won the 13th arena), 25 percent more health each time round; rewards and refines come as in a 12-arena run, and a boss beaten there counts for what it unlocks, not for the ladder (`economy.json` `ladder.endless`). The boss names are the game's own (its Library: Uroboros Mkono is the flamethrower room's, Uroboros Aheri Excella's on the ship; before 2026-11-19 the mod had them the other way round and called U-8 a giant Uroboros). From the fifth arena on every arena brings 2 more enemies, hits 8 percent harder and takes 5 percent more to kill than the one before. RUN RESULTS says what a boss win opened. Your profile keeps which bosses you beat (`stats.bosses_beaten`).
   **The bosses**: the Uroboros (Furnace Facility) and U-8 (Experimental Facility) as before; Popokarimu, the flier of the Mining Area (12000 health, it walks, flies, dives and strikes); Jill in her battle suit in the Monarch Room (15000 since 0.2.6: she dashes, rolls and fires her SMGs from any distance and in faster bursts, kicks, sweeps and slides at you up close, sweeps her fire at range, only an explosion makes her flinch, she rolls aside when you hold your aim on her, and she comes down from the balcony when you are below); Wesker in the Hangar (8500): he shrugs off most shots and is open while he winds up an attack; his knife-hand grab, an instant kill in the story, becomes a kick in a run. Since 0.2.6 he has two second winds: at zero health he kneels as if beaten, then gets up with his health back and calls in Majini, two Executioners and a Chainsaw Majini; the second time two Executioners and an Uroboros Mkono; the third time he staggers instead of dying (his death is a story scene). Uroboros Wesker on the Volcano (9000 since 0.2.6): his whole body takes gunfire in a run (in the story only his open core does), half of it outside the core; he rages every minute and throws in his ground slam, javelins and arm dance; at zero health he reels at his core and takes his second form with his health back, the drill grab, Tornado and the rest of his moves (his drill grab takes a third of your health, never your last point); at zero again he reels in his last fall. Uroboros Mkono in the Uroboros Research Facility (10000): it waits in its room where the story sets it (the HUD says FIND UROBOROS MKONO) and comes out once you are within 15 m (or after 90 s); it walks, runs, beats you down, punches and grabs; in the story its health refills every moment, in a run gunfire wears it down (80 a pistol hit, 200 on a weak spot) and at zero it dies with its own death. The grabs of both Uroboros end in a kill if you do not break free: in a run you are let go after 4 s (Chris's Iron Grip lets go at once). Every boss falls for 3 s before the arena's screens.
   The last arena is a boss: the Uroboros in the Furnace Facility, U-8 in the Experimental Facility or Popokarimu in the Mining Area, alone, with its health in the big digits (in percent) and its name under them; it dies to gunfire (the Uroboros' glowing weak spots take more; 15000 health, its own) and the run is complete. U-8 comes 8 s after the arena begins, counted down on screen. The Uroboros is where the story puts it: the HUD says FIND UROBOROS, it appears when you walk into its room (or after 2 minutes), roars, and fights with its own moves: both slams, near and far grabs, running at you, staggering when you hit its arms and head. If you leave its room and it loses sight of you, it goes under the floor and comes up closer. Campaign areas always start you at the same spot (the game would pick one of the area's entrances at random).
   Arenas are also campaign areas (Kijuju's streets and slums, the marsh village, the port, caves and ruins, the facility, the freighter, Spencer's mansion, Desperate Escape's last platform; the volcano is Uroboros Wesker's, the Uroboros Research Facility Uroboros Mkono's; each with the game's own area name, from its save screen, and "(Campaign)" where a Mercenaries stage has the same name): the area loads in place of a Mercenaries stage, starting where the story enters it but without its cutscenes or story enemies, and the director spawns a similar stage's Mercenaries enemies at the area's own AI waypoints around you. The story's camera zones are off in a run, so no scripted camera takes the view from you. `"campaign_arenas": false` keeps runs on the Mercenaries stages.
5. When the target is reached the world pauses and the mod's own screens appear in the game's menu font (no overlay):
   - **ARENA N CLEARED** shows the rating, time, kills, supplies and credits; **Enter** continues;
   - **CHOOSE A REWARD** lists the offers (items go straight into your inventory; if nothing fits you get the item's value in supplies); a reroll row appears when you hold a token;
   - **SHOP** lists heal, a reward reroll (a token for the reward choice after an arena), an upgrade for a weapon you hold (its help says what its level adds now and after), a weapon you don't have yet, and items (grenades, sprays, green, red and mixed herbs, ammo bundles) with their prices, coloured by rarity; bought weapons and items go straight into your inventory (a purchase that does not fit is refunded). **NEW STOCK** draws every offer again, a fresh weapon upgrade among them, for 15 supplies, 10 more each time at the same shop (Clubbin' Sheva's free one comes first). **ORGANIZE INVENTORY** opens the game's own Organize/Buy screen with the run in it: your nine cells as the first grid, under your character's name (the second grid, a partner's, stays empty), your supplies as money (100 per supply) and the run's storage. Move, combine and sell items (a sale pays half the SHOP's price in supplies, whatever the game's money shows); what you put in the storage leaves your grid, so its cell is free for a purchase. Buying and the game's own upgrades stay in the SHOP: during a run the screen's cursor skips **Buy** and an **Upgrade** goes back to the grid. **Esc** then **Enter** ("A punto") returns to the SHOP. Your campaign save is not changed: its data is copied before the screen opens and written back when it closes. Where the screen cannot load, the row shows the mod's own nine cells instead (pick one, then another, to swap them; `"native_item_box": false` in `adapter.json` always uses them). The layout is kept on the next stage. The Px4 and the Samurai Edge, guns the campaign never stocks, have their pictures there too (the Samurai Edge shows the M92F's, its base gun). The last row leaves;
   - **NEXT ROUTE** lists the arenas the run map leads to next (before the last one: the bosses, e.g. `UROBOROS   BOSS`), with their tier (tier 2 in amber, tier 3 in red); the line below names the route (enemy count, rewards, a curse when it carries one) and the arenas that choice leads on to, so a pick also decides what can follow. The pick ends the match, skips the game's results screen and restarts in that arena with your inventory and health carried over (anything the new arena's grid has no room for waits in the storage of **ORGANIZE INVENTORY**, and the screen says so; a gun that went there stays named on the SHOP's top line and its ORGANIZE row until you open it, and a gun offered while your grid is full says `TO STORAGE`); the top line names the arena and route as it begins (set `"stage_per_arena": false` in `adapter.json` to stay on one map).
   Arrow keys move, **Enter** confirms. Each arena places three **chests** (six for Jill's BSAA outfit): the clock statues Mercenaries uses for time bonuses, at spots where the game itself puts them (the stage's own statues stay away during a run). Shoot one or press the action key at it; it gives supplies and often an item (ammo, grenades, herbs, a spray) or, rarely, a gun; the line at the top shows what you got. (`"native_match_ui": false` brings the overlay windows back.)
6. After the boss there is no reward to choose (nothing is left to use it on): **ARENA N CLEARED**'s **Enter** goes to a **RUN COMPLETE** screen with the credits you banked; leaving it ends the match. If you die instead, the game shows its own "Has muerto" and the mod answers its *Continue?* with No by itself (Yes would restart the stage as plain Mercenaries). Either way the game's Mercenaries results and its record save are skipped (quitting a run from the pause menu too), so a run never writes to the game's save, and the game goes back to its Play menu, where the mod menu opens by itself on the run's results: **RUN COMPLETE** or **RUN OVER** with your character's portrait, the arenas cleared (or where you fell), kills, time, supplies earned, the credits banked, a new best run, any character or skin the run unlocked, and each arena with its rating. **Enter** or **Esc** goes on to the mod's main screen; **CREDITS → LAST RUN** shows them again until the game closes. Character perks (for example Chris's *Pocket Grenades*: a hand grenade every four kills) trigger during the arenas; a grant that does not fit shows "inventory full".

While the menu is open the game receives no input. The mouse works too, and so do the arrow keys with Space.

About the Play menu row:
- it appears only with Latin-alphabet game languages; with Japanese, Chinese or Russian text use F7;
- point at it with the mouse and click, or reach it with the keyboard or a gamepad;
- set `"native_menu": false` in `adapter.json` to turn it off.

The mouse works on the mod's own screens (the mod menu, and the reward, SHOP, NEXT ROUTE and ORGANIZE screens in a run): pointing at a row moves the cursor there, a click chooses it, the right button goes back and the wheel scrolls a long list. While a screen is up in a match, the game gets no mouse (no camera turn, no shot under it).

### Two players (split screen)

New in 0.2.0-alpha.28, made playable for both players in alpha.29 and alpha.30, its menus per player in alpha.31 (ROADMAP M10; played with two controllers on 2026-11-18). The Play menu's **PLAYERS** row switches between 1 and **2, SPLIT SCREEN**; with two, **PLAYER 1** and **PLAYER 2** take the place of CHARACTERS: each opens the same CHARACTERS and SKINS screens for that player (the header says whose; player 2 starts on another character than player 1's), and **PERKS** asks which player first. An outfit that would share the other player's costume slot while one of them is a mod's is red there, with the reason: the slot is one file of the game. **START RUN** goes through the game's own split-screen mode, the match with player 1 on the top half and player 2 on the bottom. Since 0.2.6 the game's READY lobby is skipped when two controllers are connected: player 2 is the controller connected last, player 1 the other one or the keyboard. With fewer controllers the lobby appears as before (player 2 joins with **START** on their controller, both press **Ready**). Player 1 can use the keyboard.
- Each player is a seat of the run with their own character, skin build, perks, weapons, inventory, stats and health; supplies, keys, chests, the run map and route are shared. Hits, kills, damage and heals count for the player who made or took them.
- An arena has 1.5 times the enemies, 1.3 times as many alive at once, and a boss 1.6 times the health (`economy.json` `coop`).
- The pads are learned in the lobby (player 2's START, player 1's Ready, or the keyboard). Each skin special is on its player's own pad (LB+RB, both sticks), its rings on that player's half; the run's shared texts (arena, supplies, keys, notices such as PLAYER 2 IS DOWN) show once at the top, as in solo; each half has its own enemies-left count, under the game's counter.
- Dying (crawling) is not down: the other player can help them up the game's way. A player who dies is down: the game would end the match for both (and freeze the other player, their weapon rings and their inventory), the mod holds that back while the other player stands, who plays on as before. The fallen player lies there until the next arena, which they start with half their health; the run ends when both are down, and the game goes back to its Play menu with RUN OVER.
- A chest is the nearest player's. The enemies are shared evenly (since 0.2.6): each one the director spawns goes to the standing player who has fewer of them, the other's turn when even, at a spawn point near that player and not right next to the other, and starts out after that player. Before, a whole wave could come around one player while the other had none.
- Leon and Ada (characters from mods, in Chris's and Sheva's costume slots) have their own names on the weapon rings and in the inventory, in the game's style, while a run plays them (since 0.2.6; two players on one base character keep the game's name).
- Between arenas each player has their own screens at once (since 0.2.6): player 1's in the top half in the game's font, player 2's in the bottom half, each on their own controller (the keyboard works player 1's); a player who is done sees WAITING FOR PLAYER 2 (or 1). Without a controller for player 2 the screens take turns as before, in the top half. **NEXT ROUTE** is a vote: the same pick goes; different picks, one of the two at random, with a notice. **ORGANIZE INVENTORY** shows the mod's nine cells as a three-by-three grid for each player (the game's Organize/Buy screen in a split match is its two-panel co-op one): pick one, then another, to swap them, or send the picked one **TO THE STORAGE** or **SELL** it (half its SHOP price, to the shared supplies; a sold gun leaves that player's run). The storage is the run's, both players': its items are listed under the cells; pick one, then a cell, to take it into the grid (a gun keeps the rounds it went in with; a gun the player already holds stays there). The stage change carries each player's inventory and health. After a split-screen match the game comes back to its lobby; the mod skips it from the second arena on.
- Each player's flamethrower fires its element's recoloured stream, its hits and arcs theirs; each player's sniper scope is on their own pad (the right stick's click while aiming; a gun model scoped by one player is scoped for both while both hold it, the game keeping one record per model).
- **CONTINUE RUN** goes on with a two-player run too: the resume point saved between arenas keeps both players (the core's seats, player 2's carried inventory, health and costume). Continuing shows the game's lobby again (the pads are learned there), player 2 joins with START, both ready, and the arena that was left starts again with each player's items.

### Style perks

The PERKS menu's first slot is the character's way of playing: one free trait each, and more to buy with Credits. Most
of the newer ones are a rule with a cost. Written 2026-11-04 and waiting for a playtest (all but the defaults).

| Character | Perk | What it does | Cost |
|---|---|---|---|
| Chris | B.S.A.A. Veteran | Clearing an arena restocks a first aid spray. | free, the default |
| Chris | Pocket Grenades | Every 4 kills put a hand grenade in your pocket. | 120 credits |
| Chris | Boulder Puncher | Melee hits deal two and a half times the damage, handguns 25 percent less. | 150 credits |
| Chris | Iron Grip | Throws off a Majini that grabs him at once, but every heal is 25 percent smaller. | 200 credits |
| Sheva | Field Training | Each kill heals 2 percent of max health. | free, the default |
| Sheva | Flash Tactics | Every 3 kills hand you a flash grenade. | 120 credits |
| Sheva | Scavenger | Ammo and herbs she picks up become ammo for the gun in her hands. | 150 credits |
| Sheva | Sharpshooter | Rifles hit 35 percent harder, rifle rounds stack twice as high. Machine guns 20 percent less. | 150 credits |
| Jill | Battle Instinct | 15 percent of hits are critical and deal double damage. | free, the default |
| Jill | Weapon Expert | Handguns hit 30 percent harder, handgun rounds stack twice as high. Shotguns 20 percent less. | 150 credits |
| Jill | Finisher | Shots deal 60 percent more to enemies under a third of their health, 10 percent less otherwise. | 150 credits |
| Albert | Firestarter | Wesker. Every 5 kills hand you an incendiary grenade. | free, the default |
| Albert | Superior Being | Wesker. He takes 30 percent less damage, but heals are half as strong. | 200 credits |
| Albert | Predator | Wesker. Each kill within 5 seconds of the last adds 10 percent damage, up to 50. 85 percent health. | 180 credits |
| Josh | Squad Tactics | Every kill gives 1 extra supply. | free, the default |
| Josh | Field Commander | Every arena clear hands him a hand grenade and a flash grenade. 20 percent fewer supplies. | 150 credits |
| Josh | Point Man | Machine guns hit 25 percent harder, their rounds stack 50 percent higher. No magnums offered. | 150 credits |
| Barry | Demolition Man | Every 5 kills hand you a grenade, and an arena clear two more. | free, the default |
| Barry | Magnum Specialist | Magnums hit 30 percent harder, magnum rounds stack twice as high. Machine guns 25 percent less. | 150 credits |
| Barry | Trap Setter | Every 4 kills hand him a proximity bomb. | 150 credits |
| Rebecca | Combat Medic | Each kill heals 2 percent of max health, an arena clear restocks a spray. | free, the default |
| Rebecca | Chemist | Green herbs she picks up become mixed herbs. Every heal is 20 percent smaller. | 180 credits |
| Rebecca | Steady Nerves | Rifles hit 30 percent harder, shotguns 20 percent less. | 120 credits |
| Excella | Executive Bonus | 8 extra supplies for every arena cleared. | free, the default |
| Excella | Hostile Takeover | Every melee kill pays 1 supply, but guns deal 15 percent less. | 150 credits |
| Excella | Golden Parachute | Once per arena she survives a lethal hit with 25 percent health. 20 percent fewer supplies. | 200 credits |

### Skin builds

Every skin except Chris's and Sheva's BSAA outfits plays by its own rule. The first seven were checked in game;
the others are written and wait for a playtest (2026-11-03).

| Character | Skin | Build | What it does |
|---|---|---|---|
| Chris | Safari | Mine Layer | Chests and enemy drops are proximity bombs (an ammo, herb or grenade pickup becomes one); they stack to 10. Health x1.25. |
| Chris | S.T.A.R.S. | Field Kit | A first aid spray on every arena clear. |
| Chris | Warrior | Brute Force | Only shotguns, handguns and magnums are offered; they hit 25 percent harder. |
| Chris | Heavy Metal | Heavy Gunner | Starts with the pistol only; every shop sells the Gatling gun for 100 supplies until he has it (no unlock); ammo stacks three times as high. |
| Sheva | Clubbin' | Night Shopper | Supplies x1.2; one free NEW STOCK at every shop. |
| Sheva | Tribal | Hunter | Starts with her longbow and 20 arrows (10 more per arena clear); arrows hit 50 percent harder; each kill heals 1.5 percent of her health. |
| Sheva | Business | Expense Account | Better rarities in rewards, shop and chests; SMGs hit 30 percent harder and upgrade without limit. |
| Sheva | Fairy Tale | Enchanted Arms | A legendary M3 that upgrades without limit. |
| Jill | BSAA | Locksmith | Opens every chest without a key; every arena places three more chests (six). |
| Jill | Battle Suit | Twin Fury | Special: 1.5 s of fire from two SMGs on everything in a 40 degree cone up to 14 m ahead; 25 s to recover. |
| Josh | BSAA | Call for Backup | Special: Sheva joins as a computer partner for the rest of the arena, once every two arenas: the game's own partner, who follows you, fights, heals herself and revives you when you are bleeding out. Her kills count as yours, and ammo the game drops for her guns is yours: you pick it up as ammo for the gun in your hands. If she cannot join (the stage did not load her), covering fire on the nearest enemy instead. |
| Barry | S.T.A.R.S. | Old Reliable | An S and W M29 that upgrades without limit; health x1.5. |
| Rebecca | S.T.A.R.S. | Field Surgeon | Heals x1.3. A big heal (at least 60 percent of her health: a spray, a mixed herb) makes her untouchable for 4 s and 30 percent stronger for 12 s. |
| Excella | Dress | Ambition | Every melee kill raises one stat for the rest of the run, up to its cap: damage (to +90 percent), defence (to +63), health (to +90), critical chance (to +27), movement speed (to +54), healing (to +45) or supplies (to +20). Every stat is full after 54 melee kills on a short run, 142 on a long one and 249 on a marathon, so each gain is bigger on a shorter run. |
| Wesker | Midnight | Uroboros Speed | Special: a dash, or a lunge at an enemy ahead (below); a second special, the burst. Untouchable while they act. 70 percent of his health. |
| Wesker | S.T.A.R.S. | Iron Fist | Melee hits three times as hard. Only his own Samurai Edge, Lightning Hawk and Hydra can be bought or used. |
| Wesker | RE4 (mod) | Mastermind | Starts with a rare Lightning Hawk. Magnums hit 30 percent harder, melee twice as hard. Only his own Samurai Edge, Lightning Hawk and Hydra. |

**Unlocks.** Lifetime progress, kept in your profile. A character's first skin comes with the character.

| Character or skin | How it opens |
|---|---|
| Jill | Complete a run as Chris |
| Josh | Complete a run as Sheva |
| Barry | Kill 500 enemies in total |
| Rebecca | Clear 5 arenas without taking damage |
| Excella | Earn 1500 credits in total |
| Leon (needs his mod) | Beat Uroboros Mkono |
| Wesker | Secret: his two skins open with bosses of the ladder, and he with the first of them |
| Chris: Safari, Warrior | Complete 2, 5 runs as Chris |
| Chris: S.T.A.R.S. | Earn an S rating in 3 arenas |
| Chris: Heavy Metal | Kill 1500 enemies in total |
| Sheva: Clubbin', Fairy Tale | Complete 2, 5 runs as Sheva |
| Sheva: Tribal | Kill 250 enemies in total |
| Sheva: Business | Earn 750 credits in total |
| Sheva: Hound Wolf Squad (mod) | Clear 8 arenas in one run |
| Jill: Battle Suit | Complete a run as Jill |
| Jill: Classic (mod) | Beat U-8 |
| Excella: Experimenter (mod) | Complete 3 runs as Excella |
| Leon: Jacket (mod) | Complete 3 runs as Leon |
| Wesker: RE4 (mod) | Complete 3 runs as Wesker |
| Ada Wong (mod) | Beat Jill |
| Ada: Assignment, Red Dress (mod) | Complete 2, 4 runs as Ada |

**Mod skins (optional).** Some skins wear a costume mod made by someone else, from its creator's page on Nexus Mods.
None of it comes with ROGUE MERCENARIES: without the mod the game plays as always, and SKINS lists the skin in red,
saying whose mod it is; pressing it opens the mod's page (get it there, and endorse it). Unzip the download into
`Resident Evil 5\RogueMercenaries\mods` (any folder below it, as it unpacks); the skin can then be worn, with its
own build, and SKINS shows the mod's own picture (its character select bust, or its preview). The mod's files are read from that folder only while you wear its skin: nothing
in the game's own folders is changed, the original outfit keeps its look, and deleting the folder removes the mod.
Use this folder, not a mod manager: one that installs the mod into the game replaces the original outfit everywhere.

| Character | Skin | Mod (creator) | Build | What it does |
|---|---|---|---|---|
| Excella | Experimenter | [Excella the Experimenter](https://www.nexusmods.com/residentevil5goldedition/mods/387) (TheGrimrose) | Field Trials | Every arena all her guns carry one random element (the arena's name is followed by it). Every shop offers a refine, at half price. Health x0.8. |
| Jill | Classic | [Jill HDR Casual](https://www.nexusmods.com/residentevil5goldedition/mods/135) | Last Escape | Starts with the grenade launcher. Every gun she holds carries Incendiary, Cryo and Toxic (its flame, freeze and acid rounds). Only handguns and launchers are hers. Health x0.85. The mod comes in four variants (gear or not, gloves or not): keep the one you like in the mods folder (with several, the first by folder name is worn). |
| Sheva | Hound Wolf Squad | [Hound Wolf Squad Sheva](https://www.nexusmods.com/residentevil5goldedition/mods/1177) (Angeltitilxyz) | Scorched Earth | Starts with a pistol in hand (not her costume's M3) and a rare flamethrower, whose fuel comes back 50 percent faster. Every gun she holds is Incendiary. Supplies x0.75. |

| Wesker | RE4 | [RE4 Wesker Reimagined](https://www.nexusmods.com/residentevil5goldedition/mods/1019) (izacky, kreed, Freyr95, Cixon64, DimensionalBoy, nealktorce) | Mastermind | RE4's Mercenaries Wesker: starts with a rare Lightning Hawk (his silenced magnum), magnums hit 30 percent harder and his melee twice as hard. It wears S.T.A.R.S. Wesker's slot (that outfit keeps its look), so only his own Samurai Edge, Lightning Hawk and Hydra are for him. Unzip all of it: his new voice lines too. |

Only the costume file of a mod is used: the Excella mod's other files (her weapons, items, menus and music) are not.

**Ada Wong** (a character of her own, from the [Ada Bundle](https://www.nexusmods.com/residentevil5goldedition/mods/1091)
by Cond3mned, Kreed, Nealktorce, Yumiko, Freyr95 and Cixon). Unzip all of it (her voice lines and its Px4 file too).
She wears Sheva's Default, Clubbin and Tribal slots (those outfits stay Sheva's) and moves like Sheva. The mod's Px4
file is the game's Px4 (the same model, sounds and effects) with new hand movements for it: while she is played her
Px4 is held its way. The mod says its hair is off in two of the outfits. Without the mod CHARACTERS lists her
in red, and pressing her opens the mod's page; it has no preview, so SKINS shows Sheva's bust unless a picture is put
in the mod's folder as `ada_agent.png`, `ada_assignment.png` or `ada_red_dress.png` (a square bust; transparency is
kept). Her own perks: **Butterfly**
(her trait: shots deal 30 percent more to enemies under half their health), **Grapple Gun** (she slips every grab, a
melee kill pays 1 supply, heals 20 percent smaller), **Double Agent** (10 more supplies an arena, 15 percent more
damage taken), **Assassin** (15 percent of hits critical for two and a half times, 85 percent health), and the
supply **Bowgun** (the longbow and 20 arrows). Her skins: **Agent** (Intel Network: her Px4, a free NEW STOCK and
better rarities in every shop, handguns 25 percent harder, supplies x0.85), **Assignment** (a rare MP5, machine guns
and rifles 25 percent harder, health x0.9) and **Red Dress** (Separate Ways: a rare longbow, bows 40 percent harder,
every pickup ammo for the gun in hand).

**Leon S. Kennedy** (a character of his own, from [Leon Definitive Edition](https://www.nexusmods.com/residentevil5goldedition/mods/223)
by kreed and izacky). Unzip all of it, its *Main Files* too (his voice lines): without them he cannot be picked. He
wears Chris's two outfits' slots (the outfits themselves stay Chris's) and moves like Chris; without the mod CHARACTERS
lists him in red, and pressing him opens the mod's page. His own perks: **Roundhouse** (his trait: for 4 s after a melee
hit his shots deal 40 percent more), **Suplex** (melee hits twice as hard, every melee kill pays 1 supply, rifles 25
percent less), **Agent Training** (20 percent of hits critical for double damage, heals 25 percent smaller), and the
supply **Matilda** (an M93R and 30 rounds). His skins: **Darkside Chronicles** (Lucky Rookie: once per arena he survives
a lethal hit at 30 percent health, handguns 25 percent harder, health x0.85) and **Jacket** (Best Customer: every shop
has a refine at half price and a free NEW STOCK, better rarities, supplies x0.8).

**Keys.** Chests need a key (Jill's BSAA outfit excepted). A kill of a Majini drops one 8 percent of the time; you
hold up to 9 and the HUD's right side shows how many. A chest opened without a key says *LOCKED* and stands again at
the same spot a moment later.

**Specials** (Twin Fury, Call for Backup, Uroboros Speed): press **Q** on the keyboard, or **LB and RB together** on a
controller; a build's second special (Wesker's burst) is **X**, or **both sticks pressed together**. While a build with
specials fights, those keys and buttons are the mod's: the game does not see them (no knife from LB), and a
controller combo's first button waits a tenth of a second for the other before it reaches the game. A ring at the
bottom left shows each special's icon and name with the binding under it (the second ring to the right): a dark
sweep and the seconds left while it recovers (or *NEXT ARENA* for Josh), gold and pulsing when it is ready, green
while it acts. Change the bindings in `RogueMercenaries\adapter.json`: `"special_key_scancode": 16` and
`"special2_key_scancode": 45` (DirectInput scan codes; 16 is Q, 45 is X), `"special_pad": ["LB", "RB"]` and
`"special2_pad": ["LS", "RS"]` (buttons held together: A, B, X, Y, LB, RB, BACK, START, LS, RS; XInput pads and,
through the names in the game's own `config.ini` `[JOYPAD]`, DirectInput pads, where LT and RT work too). Twin Fury
deals damage without shots on screen. Its kills count as yours.

**Weapons.** Switching guns during an arena shows a card at the bottom right for a few seconds: the gun, its rarity
and level, its damage factor, what its level adds, and its traits.

**Weapon rarity.** A gun's rarity sets how many bonuses it rolls and how big they are: Uncommon one, Rare two,
Epic and Legendary three, each rarity's bigger than the one below (a Legendary's twice the base roll). A gun carries
one **element** from **Epic** on, evolved from **Legendary** on; an **Exotic** one (cyan) has four bonuses, its evolved
element and one **legendary effect**. A gun found or bought at Epic or above comes with its element (the better ones
turn up less often: Incendiary and Toxic 30 in 100, Shock 20, Cryo 13, Explosive 7). A gun you **refine to Epic** asks
you to choose: two elements, drawn the same way, or **RANDOM ELEMENT**, any of the five, each as likely (the rare ones
too), and final. The choice comes right after the reward or the purchase; a chest's refine asks after the arena. Refined
to **Legendary**, its element evolves:

| Element | What it does | Evolved (Legendary) |
|---|---|---|
| Incendiary | Hits set the enemy on fire: a quarter of the hit's damage every second for 3 s. | **Inferno**: 35 percent for 5 s, and the fire spreads to the enemies within 3 m. |
| Shock | One hit in four stuns the enemy and arcs to two more within 5 m for 60 percent of the hit. | **Storm**: two hits in five, arcs to four within 8 m for the full hit. |
| Cryo | One hit in ten freezes the enemy for 3 s. A frozen Majini dies to the next hit (the game's own shatter); other enemies take 50 percent more while frozen. A frozen one that dies shatters: 40 percent of its health (up to 1200) to everyone within 3 m. | **Absolute Zero**: one hit in five freezes for 4 s; the shatter (50 percent, up to 1500, 3.5 m) freezes those it hits for 3 s. |
| Explosive | Kills blow up: half the victim's health (up to 1500) to everyone within 3.5 m, who are thrown. | **Chain Reaction**: 60 percent (up to 1800) within 4.5 m, and an enemy the blast kills blows up too (two links). |
| Toxic | Hits poison the enemy for 5 s, 12 percent of the hit every second; a poisoned one takes 25 percent more from everything. | **Plague**: 15 percent for 6 s, and a poisoned enemy that dies, however, poisons everyone within 4 m (3 percent of its health a second). |

The **flamethrower** carries its elements in the flame: Shock arcs from the nearest enemy in it every 0.6 s (0.35 s
evolved), Cryo freezes an enemy after 2 s in the flame (1.2 s evolved), Toxic poisons and Incendiary burns everything
in it. A Shock flamethrower fires sparks (the stun rod's) instead of a flame: everything within 5 m ahead takes 300
damage a second, scaled like the flamethrower's hits, and is stunned. The other elements still fire the flame until
an effect is picked for them.

**The mod's own effects.** The elements are colour coded with effects the mod makes from the game's own: the
flamethrower's flame recoloured cyan for Cryo, green for Toxic and violet for Shock, and its small flame in those
colours (orange for fire) on the enemies an element reaches; Shock's hits keep the stun rod's sparks. A Cryo, Toxic or
Shock flamethrower fires its coloured flame instead of the fire one, and what it reaches takes that element (frozen,
poisoned, stunned) instead of catching fire, 300 damage a second within 5 m ahead, scaled like its hits. The mod
builds them at start from your own game files (`uWp21.arc`, `coreEff.arc`, read only) into
`%LOCALAPPDATA%\RogueMercenaries\generated\rmEffects.arc` (about 17 MB), and a run's loads bring it. Nothing of the
game's is changed. `"custom_effects": false` in `adapter.json` turns them off (the game's own looks only). The
coloured flame is drawn at 60 percent of the fire one's size (`"flame_stream_scale"`), without the fire's second
effect (`"flame_stream_companion": true` brings it).

**Lightning.** Shock's jumps are drawn as lightning between the enemies it reaches (a chain for the evolved
Storm), and a Shock flamethrower throws bolts from the gun to the nearest enemy in its flame. They are drawn over the
picture where the game's camera shows those enemies, so a wall between does not hide them. `"element_arcs": false` in
`adapter.json` turns them off.

**Picking the elements' looks (the effect gallery).** In an arena, press **Numpad \*** to turn the gallery on or
off. Every 3 seconds the next effect in memory plays on the nearest enemy, its name at the top of the screen.
**Numpad +** and **Numpad -** step to the next or the previous one at once, **Numpad 0** holds the one shown (it plays
again every 3 seconds). **Numpad 1** to **5** make the one shown the look of an element: **1** Incendiary, **2**
Shock, **3** Cryo, **4** Toxic, **5** Explosive (for Shock, Cryo and Toxic also what the flamethrower fires in place of
its flame). It takes effect at once and is kept in `%LOCALAPPDATA%\RogueMercenaries\element_looks.json` for every
later game; delete that file to go back to the defaults. The game's effects carry their own colours, so a colour is
picked by picking an effect of that colour (an icy one for Cryo, a green one for Toxic). `"effect_gallery": true` in
`adapter.json` starts it on.

Town, village and soldier Majini play the game's own reactions to these (on fire, frozen, acid, the flash stun,
blown away, the head burst); other enemies take the damage alone, and a boss takes 15 to 20 percent more damage from
the gun instead of a status. Legendary effects (an Exotic gun's): **Executioner Rounds** (finishes off any enemy below 20 percent
health; a boss takes double damage there), **Bottomless** (kills
refill the magazine), **Overkill** (damage past a kill hits the nearest enemy within 6 m), **Last Word** (the last
round of a magazine kills any enemy; a boss takes triple damage), **Bloodthirst** (kills heal 3 percent of your
health). Dragons Breath and Chain Lightning are Inferno and Storm now.

**Refining** raises a gun one rarity: it keeps everything it has, its bonuses grow to the new rarity's size, and it
gains what that rarity adds (its help line lists it). A reward's or the shop's refine is for any of your guns that can
still go up, each as likely (NEW STOCK can bring another), so every gun gets its chances. A refine is one of the reward
choices now and then (about one arena in seven), and always after an arena rated A or S; the shop sells one now and then (40 supplies to reach
Uncommon, up to 240 for Exotic); and one chest in about sixteen refines the gun in your hands. Legendary is the top
until you beat U-8: from then on a Legendary refines to Exotic (the run results say so).

**Wesker's special** picks its move, and its ring shows which one the button would make: **LUNGE**, his
long-range melee, at an enemy up to 10 m away in a wide cone ahead that is reeling from a hit (shot in
the head, a knee, a stagger): he turns to face it the way the game turns characters and closes in at
Uroboros speed, ending in front of it, facing it, while it still reels. The screen says *MELEE*: the
game's own melee button finishes it with his own melee move. (Should he end up not facing it, he
strikes it for 650 instead and a Majini is blown away.) Or at an enemy lined up within 12 m: hit for
300 and left reeling from a head hit, the state the game's melee prompt follows (*MELEE* again); 4.2 s
to recover. Else **DASH** (5 m ahead,
200 damage and a stagger to everyone he runs through; 3.5 s). Both play his own forward dodge, started
the way the game starts its dodges. His second special, **BURST** (X or both sticks): 350 damage to
everyone within 3.5 m on his level, a Majini is blown away; 7 s. Every move is checked against the
level with the game's own collision query, the one its dodges use: he stops short of walls and lands
on a floor within a step of his own, or he does not move. Specials only work while he is on his feet
and in control: not while grabbed, hit, down, using an item, in a story action, with the game paused
or in a menu.

**Stuck enemies.** An enemy is moved only where no player can see it: not near a player with no wall
between, and not inside any game camera's view (each split-screen half has its own), so turning the
camera never shows one vanish. One that has not moved for 30 s while far from every player is moved to
a point within reach, but only while the fight is quiet (nothing died for 20 s), one at a time and some
seconds apart; a boss after 8 s. The spot it stood at is not used again that arena (doors the story
opens, ledges). One of the last three, when nothing has died for 25 s and it is far, comes nearer the
same way. The very last enemy, when nothing has hurt it for 20 s and it is far (40 s if it is near),
is pulled to a point nearer you but at least 12 m away, at most twice, 30 s apart; a Majini that never
saw you walks back to its own spot, so after that it is finished and the arena ends. (Before alpha.32
idle enemies were moved after 12 s, which a player saw as enemies teleporting all over the map.)

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
