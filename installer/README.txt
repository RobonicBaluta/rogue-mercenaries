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
  arena. START RUN asks for the run length once a longer run is open, and the results
  screen after a run says what a boss win opened.

REQUIREMENTS
  - Resident Evil 5 on Steam, the current build (Steam build 11465250, re5dx9.exe 1.0.0.129).
    On any other build the mod switches itself off and the game runs as usual.
  - Windows 10 or 11.
  - A keyboard or a controller. Mouse hover does not work in the game's menus.

INSTALL
  1. Close Resident Evil 5.
  2. Extract this whole ZIP to a folder (do not run it from inside the ZIP).
  3. Double-click Install.bat. It finds the game through Steam and asks before doing
     anything unusual. If Windows warns about an unknown file, choose "More info" then
     "Run anyway". The scripts are plain text in the "installer" folder if you want to
     check them first.
  The installer also copies your RE5 save to %LOCALAPPDATA%\RogueMercenaries\save-backups
  before anything else. The mod is designed never to write your game save during a run.
  Installing over alpha.1 is fine, and your progress (Credits, unlocks) carries over.

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
    PLAYER 2     - with 2 players: player 2's character and outfit.
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

TWO PLAYERS (SPLIT SCREEN) - NEW IN ALPHA.28
  Two players on one PC, the screen split in two: the game's own split screen. Player 2
  needs a controller; player 1 can use the keyboard or another controller.
  In the mod's menu set PLAYERS to 2, choose PLAYER 2's character and outfit, then
  START RUN. The game's split-screen lobby appears: player 2 presses START on their
  controller to join, then both press Ready. Player 1 plays on the top half, player 2
  on the bottom half.
    - Each player has their own character, skin build, perks, weapons, inventory and
      health. The supplies are shared.
    - An arena has half again as many enemies, and a boss has more health.
    - A player who falls stays down until the next arena, which they start with half
      their health. The run ends when both have fallen.
    - Between arenas player 1 picks a reward and shops first; then the screens say
      PLAYER 2 and player 2 does. For now any controller or the keyboard works these
      screens.
    - The lobby only appears before the first arena.
  Not there yet: player 2's skin specials, the mod's numbers on player 2's half,
  ORGANIZE INVENTORY for player 2 (it shows player 1's items), and continuing an
  interrupted two-player run (it cannot be continued).

KNOWN ISSUES (ALPHA)
  - The mod does not need the Extra Content DLC or The Mercenaries unlocked, but it has
    only been played on one full save, with those simulated for the rest. If "Rogue
    Mercenaries" shows at the top right but something is missing, send your logs.
  - New in this version and not played yet: the bosses on arenas 4 and 8 of longer runs
    and the 13th arena.
  - Two players (split screen) has only been tried with a simulated second controller:
    please report what goes wrong with two real ones.
  - Uroboros Mkono, one of the alternate bosses of arena 8, may not be beatable yet:
    pick another boss there.
  - In one campaign area a ladder could not be climbed, and a fall left the player
    stuck; that area is out of the runs for now. If a ladder ignores you anywhere else,
    please report the arena.
  - Some campaign areas may still have spots where enemies get stuck; the mod moves
    them after a while.
  - The Play menu row only shows with Latin-alphabet game languages. With Japanese,
    Chinese or Russian text, go to The Mercenaries > Solo, press F7 and start the run there.

WHEN SOMETHING BREAKS
  If "Rogue Mercenaries" never shows at the top right of the game, check that dinput8.dll
  is still in the game folder: an antivirus may have removed it (it hooks the game, which
  looks suspicious). If it is gone, allow it and run Install.bat again.
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
