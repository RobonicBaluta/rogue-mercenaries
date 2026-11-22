ROGUE MERCENARIES {VERSION}
A roguelite mod for Resident Evil 5 (Steam). This is an ALPHA: expect bugs and crashes,
and please send your logs (see "When something breaks").

WHAT IT IS
  A run is a chain of arenas: Mercenaries stages and campaign areas, picked on a
  branching run map, with a boss every fourth arena. The mod spawns the enemies and
  there is no clock; from the second arena on, enemies from other stages join in, and
  later arenas get harder. After each arena you pick a reward, shop with the run's
  supplies, and choose the next arena. A run banks Credits, which unlock perks and
  weapons in the mod's menu. You start with Chris and Sheva; every other character and
  skin is earned, and its locked row says how (some are secret).

THE LADDER
  Your first runs are 4 arenas long and end on a boss. Beating bosses opens harder
  alternate bosses and longer runs (8 and 12 arenas, with a boss on every fourth arena).
  A 12-arena run that takes the alternate boss on both arena 4 and arena 8 gets a 13th
  arena. Beating an 8-arena run also opens ENDLESS: no last arena, the run ends when you
  fall, with a boss every 4 arenas, stronger each time round. START RUN asks for the run
  length once a longer run is open, and the results screen after a run says what a boss
  win opened. The bosses and campaign areas carry the game's own names (until this
  version two Uroboros had each other's names, and U-8 was called a giant Uroboros).

REQUIREMENTS
  - Resident Evil 5 on Steam, the current build (Steam build 11465250, re5dx9.exe 1.0.0.129).
    On any other build the mod switches itself off and the game runs as usual.
  - Windows 10 or 11.
  - A keyboard, a controller or a mouse (the mouse works on the mod's menus and screens).

INSTALL
  1. Close Resident Evil 5.
  2. Extract this whole ZIP to a folder (do not run it from inside the ZIP).
  3. Double-click Install.bat. It finds the game through Steam and asks before doing
     anything unusual. If Windows warns about an unknown file, choose "More info" then
     "Run anyway". The scripts are plain text in the "installer" folder if you want to
     check them first.
  The installer also copies your RE5 save to %LOCALAPPDATA%\RogueMercenaries\save-backups
  before anything else. The mod is designed never to write your game save during a run.
  Installing over an earlier version is fine, and your progress (Credits, unlocks) carries over.
  If the game folder already has a dinput8.dll from another mod with a plugins folder next
  to it (input fixes and Enhanced Movement come that way), do not run Install.bat: use the
  Plugin version of Rogue Mercenaries instead (the download with plugins\RogueMercenaries.dll
  in it), which keeps that mod. Only one dinput8.dll can be in the game folder, and two
  cannot be merged into one; Install.bat would set the other mod's aside, and that mod
  with it.

OPTIONAL: COSTUME MODS
  A few skins wear a costume mod made by someone else (Excella Experimenter, Jill
  Classic, Sheva Hound Wolf Squad, Rebecca Winter, Wesker RE4), and Leon S. Kennedy and
  Ada Wong are characters from mods. None
  of them come with this package, and the mod plays fine without them: SKINS lists such
  a skin in red, and pressing it opens the mod's page on Nexus Mods. Download it there
  (and endorse its creator), then unzip it into
    Resident Evil 5\RogueMercenaries\mods
  Only the costume is used, only while you wear that skin; nothing in the game's own
  folders changes. For Leon, unzip his Main Files too; for Wesker RE4 and Ada, all of
  their files (their voice lines). Do not install these with a mod
  manager: that replaces the original outfit everywhere.

PLAY
  Start Resident Evil 5 from Steam. "Rogue Mercenaries" appears at the top right.
  In the Play menu choose ROGUE MERCENARIES: it is the last row (press Up from the first
  row). Then:
    CONTINUE RUN - only after a run was interrupted (a quit or a crash): the arena you
                   left starts again.
    START RUN    - starts a run with the selected character.
    PLAYERS      - 1, or 2 on a split screen (see TWO PLAYERS below).
    PLAYER 1     - with 2 players, in place of CHARACTERS: each player picks a character
    PLAYER 2       and a skin on the same screens. PERKS then asks which player first.
    CHARACTERS   - pick a character and a skin. Most skins change how the run plays
                   (the line under each skin explains it).
    PERKS        - three slots: Style, Supply, Support. Locked perks are red and show
                   their price in Credits; press one to buy it.
    CREDITS      - your Credits, records, the last run and weapon unlocks.
  Arrow keys move, Enter confirms, Esc goes back (on a controller: the game's own menu
  buttons). F7 opens the mod's overlay at any time.

  In a run:
    - The big orange number at the top is how many enemies are left in the arena (in a
      boss arena: the boss's health).
    - New in 0.2.6, solo: the ARENA INTEL panel at the top right shows the time, kills,
      damage dealt and taken, supplies, keys, credits, your kill chain, the boss's health
      and the rating so far. "info_panel": false in adapter.json turns it off.
    - Chests (clock statues) give supplies and items. Most need a key; Majini
      sometimes drop keys.
    - Keys 1 to 4 pick the first four inventory cells.
    - Sniper rifles aim over the shoulder like any gun. While aiming, press Z (the right
      stick's click on a controller) to look through the scope, and again to leave it.
    - Skins with a special: Q on the keyboard, or LB+RB together on a controller. A
      second special: X, or both sticks pressed together.
    - Between arenas: ARENA CLEARED, then CHOOSE A REWARD, then SHOP (ORGANIZE INVENTORY
      opens the game's own inventory screen), then NEXT ROUTE.
    - A weapon upgrade adds damage, and more rounds and a faster reload as far as that gun
      has levels left (a few guns, like the Px4 and the launchers, have none). Upgrades are
      only offered for guns you still have.
    - A rocket launcher, once bought, comes back at the start of every arena (into the
      storage if your grid is full). Selling it gives it up for good.
    - What does not fit in your nine cells waits in the storage of ORGANIZE INVENTORY.
      A gun that went there stays named at the top of the SHOP until you open ORGANIZE,
      and a gun offered while your grid is full says TO STORAGE.
    - Dying or beating the run's last boss ends the run and banks your Credits.

TWO PLAYERS (SPLIT SCREEN) - NEW IN ALPHA.28, MADE PLAYABLE IN ALPHA.29 AND ALPHA.30
  Two players on one PC, the screen split in two: the game's own split screen. Player 2
  needs a controller; player 1 can use the keyboard or another controller.
  In the mod's menu set PLAYERS to 2. PLAYER 1 and PLAYER 2 each open the CHARACTERS
  and SKINS screens for that player (an outfit in red is the other player's costume
  slot, which a mod outfit cannot share), and PERKS asks whose perks first. Then
  START RUN. With two controllers connected the match starts right away (new in 0.2.6):
  player 2 is the controller connected last. Otherwise the game's split-screen lobby
  appears: player 2 presses START on their controller to join, then both press Ready.
  Player 1 plays on the top half, player 2 on the bottom half.
    - Each player has their own character, skin build, perks, weapons, inventory and
      health. The supplies are shared.
    - An arena has half again as many enemies, more of them at once, and a boss has more
      health.
    - Each player's skin special is on their own controller (LB+RB, both sticks), with
      its rings on their half. The arena, supplies and keys show once, at the top as in
      solo; each half shows how many enemies are left.
    - A player who is dying (crawling) can still be helped up by the other, the game's
      way. A player who dies stays down until the next arena, which they start with half
      their health; the other plays on (moving, aiming, weapon rings and inventory). The
      run ends when both have fallen.
    - A chest is the nearest player's. The enemies are shared evenly between the two of
      you (new in 0.2.6): each comes for the player who has fewer, from near them.
    - Leon and Ada have their own names on the weapon rings and in the inventory.
    - Between arenas each player has their own screens at once (new in 0.2.6): player 1's
      in the top half, player 2's in the bottom half, each on their own controller (the
      keyboard works player 1's). Without a controller for player 2 the screens take turns
      in the top half. NEXT ROUTE is a vote: the same pick goes, different picks one of
      the two at random.
    - ORGANIZE INVENTORY shows your nine cells as a grid: pick one, then another, to swap
      them. A picked item can also go TO THE STORAGE or be SOLD
      (half its SHOP price, in supplies). The storage is the run's, both players': pick
      an item there, then a cell, to take it into your grid. A gun keeps its rounds.
    - Each player's flamethrower shows its element's colours, and each player's sniper
      rifle has its scope on their own controller (right stick click while aiming).
    - The lobby, when it appears, only does so before the first arena. After the run the
      game goes back to the Play menu with the run's results.
    - An interrupted two-player run (the game closed or a match quit) is saved like a
      solo one: CONTINUE RUN shows the lobby again, player 2 presses START to join, both
      press Ready, and the arena that was left starts again with each player's items.

KNOWN ISSUES (ALPHA)
  - The mod does not need the Extra Content DLC or The Mercenaries unlocked, but it has
    only been played on one full save, with those simulated for the rest. If "Rogue
    Mercenaries" shows at the top right but something is missing, send your logs.
  - New in 0.2.1: ENDLESS (it has had one short test run: please report what goes wrong
    deep into one) and Popokarimu as an alternate boss of arena 4.
  - Two players (split screen) is new and has had one short session with two real
    controllers: please report what goes wrong.
  - Uroboros Aheri (Excella, on the Bridge Deck), one of the alternate bosses of arena 8,
    may not be beatable yet: pick another boss there.
  - New in 0.2.6, harder bosses: Jill (more health, attacks from any distance, kicks up
    close, dodges your aim, flinches only from explosions), Wesker (gets back up twice,
    calling in enemies each time) and Uroboros Wesker (more health, rage, his second form
    when he first falls). Please report a fight that cannot be won or never ends.
  - The campaign areas' ladders can be climbed since this version (before, they were
    only scenery, and Factory Floor was left out of the runs for it). If a ladder still
    ignores you, please report the arena.
  - An enemy that stands far away out of sight while nothing dies is moved nearer
    after a while (never where you can see it). The last enemy of an arena is pulled
    nearer twice at most; if it is still not found, it is finished and the arena ends.
  - The Play menu row only shows with Latin-alphabet game languages. With Japanese,
    Chinese or Russian text, go to The Mercenaries > Solo, press F7 and start the run there.
  - Trainers: cheats that freeze or change the character, the stage, the Mercenaries
    timer, kills or score, health or the inventory fight the mod, which sets those itself
    during a run. Turn them off before a run (a trainer's saved settings come back when it
    starts), and use the mod's skins instead of a character swap.

WHEN SOMETHING BREAKS
  If "Rogue Mercenaries" never shows at the top right of the game, check that dinput8.dll
  is still in the game folder: an antivirus may have removed it (it hooks the game, which
  looks suspicious). If it is gone, allow it and run Install.bat again.
  If the game stops with "inflate: incorrect data check", one of the game's files is
  damaged: in Steam, right-click Resident Evil 5, Properties, Installed Files, "Verify
  integrity of game files". If it happens again, send your logs: they list the archives
  the game was reading and check each one.
  Please send:
    - what you were doing, the character and skin, and the stage;
    - the folder %LOCALAPPDATA%\RogueMercenaries\logs (paste that path into Explorer's
      address bar, then zip the folder). adapter.log has the details; after a crash also
      send adapter.prev.log.

START OVER
  Your progress is in %LOCALAPPDATA%\RogueMercenaries\profile.json. Delete that file
  (with the game closed) to start again from the first 4-arena run, with everything
  locked.

UNINSTALL
  Double-click Uninstall.bat (in this folder, or in the game folder's RogueMercenaries
  folder). It removes exactly what was installed. If another mod's dinput8.dll was there
  before, the uninstaller puts it back. It asks before deleting your progress and logs.
  Costume mods you put in RogueMercenaries\mods stay; delete that folder yourself.
  Optional: Steam > Resident Evil 5 > Properties > Installed Files > Verify integrity.

LICENSES
  The mod uses MinHook, Dear ImGui and nlohmann/json; their licenses are in "licenses".
  No game files are included in this package.
