ROGUE MERCENARIES {VERSION}
A roguelite mod for Resident Evil 5 (Steam). This is an ALPHA: expect bugs and crashes,
and please send your logs (see "When something breaks").

WHAT IT IS
  A run is a chain of arenas: Mercenaries stages and campaign areas, picked on a
  branching run map, with a boss every fourth arena. The mod spawns the enemies and
  there is no clock; from the second arena on, enemies from other stages join in, and
  later arenas get harder. After each arena you pick a reward, shop with the run's
  supplies, and choose the next arena. A run banks Credits, which unlock perks and
  weapons in the mod's menu. Every character and skin is open from the start except
  Wesker: his two skins are secret unlocks.

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
  Classic, Sheva Hound Wolf Squad), and Leon S. Kennedy is a character from a mod. None
  of them come with this package, and the mod plays fine without them: SKINS lists such
  a skin in red, and pressing it opens the mod's page on Nexus Mods. Download it there
  (and endorse its creator), then unzip it into
    Resident Evil 5\RogueMercenaries\mods
  Only the costume is used, only while you wear that skin; nothing in the game's own
  folders changes. For Leon, unzip his Main Files too. Do not install these with a mod
  manager: that replaces the original outfit everywhere.

PLAY
  Start Resident Evil 5 from Steam. "Rogue Mercenaries" appears at the top right.
  In the Play menu choose ROGUE MERCENARIES: it is the last row (press Up from the first
  row). Then:
    CONTINUE RUN - only after a run was interrupted (a quit or a crash): the arena you
                   left starts again.
    START RUN    - starts a run with the selected character.
    CHARACTERS   - pick a character and a skin. Most skins change how the run plays
                   (the line under each skin explains it).
    PERKS        - three slots: Style, Supply, Support. Locked perks are red; buy them
                   with Credits.
    CREDITS      - your Credits, records, the last run and weapon unlocks.
  Arrow keys move, Enter confirms, Esc goes back (on a controller: the game's own menu
  buttons). F7 opens the mod's overlay at any time.

  In a run:
    - The big orange number at the top is how many enemies are left in the arena (in a
      boss arena: the boss's health).
    - Chests (clock statues) give supplies and items. Most need a key; Majini
      sometimes drop keys.
    - Keys 1 to 4 pick the first four inventory cells.
    - Skins with a special: Q on the keyboard, or LB+RB together on a controller. A
      second special: X, or both sticks pressed together.
    - Between arenas: ARENA CLEARED, then CHOOSE A REWARD, then SHOP (ORGANIZE INVENTORY
      opens the game's own inventory screen), then NEXT ROUTE.
    - What does not fit in your nine cells waits in the storage of ORGANIZE INVENTORY.
      A gun that went there stays named at the top of the SHOP until you open ORGANIZE,
      and a gun offered while your grid is full says TO STORAGE.
    - Dying or beating the run's last boss ends the run and banks your Credits.

KNOWN ISSUES (ALPHA)
  - Only tested on one game save so far. The mod starts its matches itself, so it should
    not need The Mercenaries or its stages unlocked in your save; if START RUN does
    nothing on your save, please say so.
  - New in this version and not played yet: the bosses on arenas 4 and 8 of longer runs
    and the 13th arena.
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
