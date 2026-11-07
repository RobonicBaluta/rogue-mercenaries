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

## Playing

1. Launch RE5 from Steam. Top right shows *Rogue Mercenaries – Play menu or F7*.
2. In the game's **Play** menu choose **ROGUE MERCENARIES**, the last row (Up from the first row gets there). It opens the mod's own menu, drawn by the game itself:
   - **START RUN**: Solo, a random arena (one of the eight Mercenaries stages or one of the campaign areas in `configs/maps.json`) and your character and skin are handed to the game's own menu script, so no setup screen appears; the screen fades to black and the match loads about 1.5 s later. The run starts with it. Once a longer run is open (see **The ladder** under step 4), START RUN first asks for the **RUN LENGTH**: each length with the boss it ends on; a locked one is red and says which boss opens it.
   - **CHARACTERS**: pick who you play. All eight characters and every skin are available for now (the challenge unlocks come back once the roster is settled), whatever the game's own Mercenaries unlocks: the run starts the match with the costume directly. The character under the cursor is shown on the left, with the bust portrait the game's own character select uses (read from your game's files at runtime). Choosing a character opens its **SKINS**, each skin with its own portrait: most skins are a *build*, a strong rule with a cost (see **Skin builds** below), named in the row with its icon; the line below says what it does.
   - **PERKS**: three slots for the selected character, one per kind. *Style* defines how the character plays (Chris restocks a spray per arena, Sheva heals on kills); *Supply* is what a run starts with (grenade pouch, medkit, a shotgun for Chris); *Support* helps in bad situations (Second Wind, Last Stand below 30% health). Each slot opens its list, every perk with its icon: pick one (it shows SELECTED), press it again to clear the slot, and buy locked ones with Credits. The style slot falls back to the character's own trait. The fourth row, **SKIN**, names the selected skin's build and opens SKINS.
   - **CREDITS**: your balance, lifetime records, the last run's results and weapon unlocks. Every RE5 gun is in the pool; the M93R (150), stun rod (100), grenade launcher (350, its explosive rounds then turn up in the shop and rewards), rocket launcher (400, one rocket) and flamethrower (300) are bought here first (added 2026-11-04, not yet played).
   Every row of the main screen, CREDITS and the run's results has its icon next to it.
   Esc goes back one screen; locked entries are red, like the game's own disabled rows.
3. Every run starts with one gun: the game's Mercenaries loadout is cut down to the weapon in your hands, its ammo and the knife when the first arena starts (for most costumes that is the pistol; Sheva keeps her VZ61), and your Supply perks then add their items and weapons (a weapon arrives with a full magazine). Everything else comes from rewards and the shop, which can offer any gun of `configs/weapons.json`: during a run every stage loads all of their models along with yours (about 20 MB), and what you buy comes with you to the next stage. The game's grid has nine slots; the number keys 1 to 4 pick its first four cells. (`configs/loadouts.json` still holds the item catalog and an optional point-budget roll, off by default.)
   *Without the menu row:* go to **The Mercenaries → Solo**, press **F7**, select the same character and press **Enter** (Start run). Set `"direct_start": false` in `adapter.json` to go through the game's own setup screens yourself.
4. The run starts with the match. There is no clock and no score: the Mercenaries countdown, score and combo panels stay off screen for the whole run, and the game can no longer end the match by itself (the countdown is held underneath and its 150-kill limit is off), so the kill target decides the arena. In the clock's place the same big orange digits count the *enemies left*; the arena number is on the left and your supplies on the right, in the game's font. (`"merc_hud": true` in `adapter.json` shows the game's clock and score again, and the count moves into the label.) The mod spawns the arena's enemies itself (`configs/maps.json`: the spawn points and enemy types each stage uses in the original game): the game's own waves stay off for the whole run, up to 12 enemies are alive at once, and the arena ends when exactly its kill target has died, however they died. Enemies are spawned the way the game's own Mercenaries spawns them (weapons, plagas and looks as surveyed per stage) and already know where you are, so they come for you from the start. Enemies get harder as the run goes on: the first arena has only Majini with bare hands or small blades, at most 6 at once and no special enemies; later arenas add axes, torches, bows and handguns, then dynamite, stun rods, shotguns and the big special enemies (a Licker, the Executioner), and the hardest routes machine guns, rocket launchers, gatlings and the chainsaw, up to 12 at once (`economy.json` `enemy_threat`; the arena line in `adapter.log` names its level). Each stage's special enemies join once a quarter of the arena is dead: one at a time from threat level 3, one more at level 4. From the second arena on, each arena also brings *guests*: enemies of another stage, planned when the run starts (`economy.json` `run_rosters`): one more kind of Majini and, from threat level 3, another stage's special enemy, never more than five kinds in one arena, and preferably kinds you have not met yet on the way there. Only those load with the stage. NEXT ROUTE says where they come from when the help line has room, and the arena's name on screen is followed by "WITH ENEMIES FROM ...". The first enemies come 3 s after an arena starts. (`"director": false` in `adapter.json` brings back the game's own waves.)
   **The ladder** (`economy.json` `ladder`): the first run is 4 arenas and ends on the Uroboros. Beat him and the next short run ends on a choice in the last NEXT ROUTE: the Uroboros again or the giant Uroboros. Beat the giant Uroboros and the 8-arena run opens: the same two bosses, the Uroboros alone until you beat him there, both at 1.3 times their health, and from its fifth arena on every arena brings 3 more enemies, hits 8 percent harder and takes 5 percent more to kill than the one before. Longer lengths (12 arenas, ending on Uroboros Wesker, then a 13th arena against Midnight Wesker) come once those fights work in the mod. Your profile keeps which bosses you beat (`stats.bosses_beaten`).
   The last arena is a boss: the Uroboros in the pump rooms or the giant Uroboros in the capsule dome, alone, with its health in the big digits (in percent) and its name under them; it dies to gunfire (the Uroboros' glowing weak spots take more; 15000 health, its own) and the run is complete. It comes 8 s after the arena begins, counted down on screen, from a point on your own level. Campaign areas always start you at the same spot (the game would pick one of the area's entrances at random).
   Arenas are also campaign areas (Kijuju's streets and slums, the marsh village, the port, caves and ruins, the facility, the freighter, the volcano, Spencer's mansion): the area loads in place of a Mercenaries stage, starting where the story enters it but without its cutscenes or story enemies, and the director spawns a similar stage's Mercenaries enemies at the area's own AI waypoints around you. `"campaign_arenas": false` keeps runs on the Mercenaries stages.
5. When the target is reached the world pauses and the mod's own screens appear in the game's menu font (no overlay):
   - **ARENA N CLEARED** shows the rating, time, kills, supplies and credits; **Enter** continues;
   - **CHOOSE A REWARD** lists the offers (items go straight into your inventory; if nothing fits you get the item's value in supplies); a reroll row appears when you hold a token;
   - **SHOP** lists heal, a reward reroll (a token for the reward choice after an arena), an upgrade for a weapon you hold (its help says what its level adds now and after), a weapon you don't have yet, and items (grenades, sprays, green, red and mixed herbs, ammo bundles) with their prices, coloured by rarity; bought weapons and items go straight into your inventory (a purchase that does not fit is refunded). **NEW STOCK** draws every offer again, a fresh weapon upgrade among them, for 15 supplies, 10 more each time at the same shop (Clubbin' Sheva's free one comes first). **ORGANIZE INVENTORY** opens the game's own Organize/Buy screen with the run in it: your nine cells as the first grid, under your character's name (the second grid, a partner's, stays empty), your supplies as money (100 per supply) and the run's storage. Move, combine and sell items (the money comes back as supplies); what you put in the storage leaves your grid, so its cell is free for a purchase. Buying and the game's own upgrades stay in the SHOP: during a run the screen's cursor skips **Buy** and an **Upgrade** goes back to the grid. **Esc** then **Enter** ("A punto") returns to the SHOP. Your campaign save is not changed: its data is copied before the screen opens and written back when it closes. Where the screen cannot load, the row shows the mod's own nine cells instead (pick one, then another, to swap them; `"native_item_box": false` in `adapter.json` always uses them). The layout is kept on the next stage. The Px4 and the Samurai Edge, guns the campaign never stocks, have their pictures there too (the Samurai Edge shows the M92F's, its base gun). The last row leaves;
   - **NEXT ROUTE** lists the arenas the run map leads to next (before the last one: the bosses, e.g. `UROBOROS   BOSS`), with their tier (tier 2 in amber, tier 3 in red); the line below names the route (enemy count, rewards, a curse when it carries one) and the arenas that choice leads on to, so a pick also decides what can follow. The pick ends the match, skips the game's results screen and restarts in that arena with your inventory and health carried over (anything the new arena's grid has no room for waits in the storage of **ORGANIZE INVENTORY**, and the screen says so); the top line names the arena and route as it begins (set `"stage_per_arena": false` in `adapter.json` to stay on one map).
   Arrow keys move, **Enter** confirms. Each arena places three **chests** (six for Jill's BSAA outfit): the clock statues Mercenaries uses for time bonuses, at spots where the game itself puts them (the stage's own statues stay away during a run). Shoot one or press the action key at it; it gives supplies and often an item (ammo, grenades, herbs, a spray) or, rarely, a gun; the line at the top shows what you got. (`"native_match_ui": false` brings the overlay windows back.)
6. After the boss there is no reward to choose (nothing is left to use it on): **ARENA N CLEARED**'s **Enter** goes to a **RUN COMPLETE** screen with the credits you banked; leaving it ends the match. If you die instead, the game shows its own "Has muerto" and the mod answers its *Continue?* with No by itself (Yes would restart the stage as plain Mercenaries). Either way the game's Mercenaries results and its record save are skipped (quitting a run from the pause menu too), so a run never writes to the game's save, and the game goes back to its Play menu, where the mod menu opens by itself on the run's results: **RUN COMPLETE** or **RUN OVER** with your character's portrait, the arenas cleared (or where you fell), kills, time, supplies earned, the credits banked, a new best run, any character or skin the run unlocked, and each arena with its rating. **Enter** or **Esc** goes on to the mod's main screen; **CREDITS → LAST RUN** shows them again until the game closes. Character perks (for example Chris's *Pocket Grenades*: a hand grenade every four kills) trigger during the arenas; a grant that does not fit shows "inventory full".

While the menu is open the game receives no input. The mouse works too, and so do the arrow keys with Space.

About the Play menu row:
- it appears only with Latin-alphabet game languages; with Japanese, Chinese or Russian text use F7;
- reach it with the keyboard or a gamepad, because mouse hover does not select it;
- set `"native_menu": false` in `adapter.json` to turn it off.

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
| Josh | Squad Tactics | Every kill gives 2 extra supplies. | free, the default |
| Josh | Field Commander | Every arena clear hands him a hand grenade and a flash grenade. 20 percent fewer supplies. | 150 credits |
| Josh | Point Man | Machine guns hit 25 percent harder, their rounds stack 50 percent higher. No magnums offered. | 150 credits |
| Barry | Demolition Man | Every 5 kills hand you a grenade, and an arena clear two more. | free, the default |
| Barry | Magnum Specialist | Magnums hit 30 percent harder, magnum rounds stack twice as high. Machine guns 25 percent less. | 150 credits |
| Barry | Trap Setter | Every 4 kills hand him a proximity bomb. | 150 credits |
| Rebecca | Combat Medic | Each kill heals 2 percent of max health, an arena clear restocks a spray. | free, the default |
| Rebecca | Chemist | Green herbs she picks up become mixed herbs. Every heal is 20 percent smaller. | 180 credits |
| Rebecca | Steady Nerves | Rifles hit 30 percent harder, shotguns 20 percent less. | 120 credits |
| Excella | Executive Bonus | 25 extra supplies for every arena cleared. | free, the default |
| Excella | Hostile Takeover | Every melee kill pays 15 supplies, but guns deal 15 percent less. | 150 credits |
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
| Sheva | Clubbin' | Night Shopper | Supplies x1.5; one free NEW STOCK at every shop. |
| Sheva | Tribal | Hunter | Starts with her longbow and 20 arrows (10 more per arena clear); arrows hit 50 percent harder; each kill heals 1.5 percent of her health. |
| Sheva | Business | Expense Account | Better rarities in rewards, shop and chests; SMGs hit 30 percent harder and upgrade without limit. |
| Sheva | Fairy Tale | Enchanted Arms | A legendary M3 that upgrades without limit. |
| Jill | BSAA | Locksmith | Opens every chest without a key; every arena places three more chests (six). |
| Jill | Battle Suit | Twin Fury | Special: 1.5 s of fire from two SMGs on everything in a 40 degree cone up to 14 m ahead; 25 s to recover. |
| Josh | BSAA | Call for Backup | Special: Sheva joins as a computer partner for the rest of the arena, once every two arenas: the game's own partner, who follows you, fights, heals herself and revives you when you are bleeding out. Her kills count as yours. If she cannot join (the stage did not load her), covering fire on the nearest enemy instead. |
| Barry | S.T.A.R.S. | Old Reliable | An S and W M29 that upgrades without limit; health x1.5. |
| Rebecca | S.T.A.R.S. | Field Surgeon | Heals x1.3. A big heal (at least 60 percent of her health: a spray, a mixed herb) makes her untouchable for 4 s and 30 percent stronger for 12 s. |
| Excella | Dress | Ambition | Every melee kill raises one stat for the rest of the run, up to its cap: damage (+4 percent, to +100), defence (+3, to +70), health (+5, to +100), critical chance (+2, to +30), movement speed (+3, to +60), healing (+5, to +50) or supplies (+4, to +50). |
| Wesker | Midnight | Uroboros Speed | Special: a dash, or a lunge at an enemy ahead (below); a second special, the burst. Untouchable while they act. 70 percent of his health. |
| Wesker | S.T.A.R.S. | Iron Fist | Melee hits three times as hard. Only his own Samurai Edge, Lightning Hawk and Hydra can be bought or used. |

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

**Stuck enemies.** An enemy that has not moved for 12 s while far from you (8 s for a boss), or one of
the last three when nothing has died for 25 s and it is far, is moved to a point within your reach and
the spot it was stuck at is not used again that arena (doors the story opens, ledges).

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
