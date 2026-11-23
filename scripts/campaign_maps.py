#!/usr/bin/env python3
"""Adds campaign areas to configs/maps.json as director maps (ROADMAP M5).

  python scripts/campaign_maps.py 503 [100 ...] [--template 0]

A campaign stage loaded in a Mercenaries match (adapter dev "stage <n>") has no enemy sets of
its own, but its archive holds the AI waypoints (rAIWayPoint "stage\\sNNN\\soft\\sNNN_g"): points on
walkable ground. They become the map's spawn points (merged within 150 units); chests stand on a
spread-out sample of them. Where the area's player start is known (STARTS, from
scripts/vet_campaign.sh) the arena keeps to its region: points within REGION of the start,
chests within CHEST_REGION, and the director prefers points within max_player_distance. The roster is copied from a Mercenaries map (--template, its stage
index): those classes load with the run (M3 preloads), whatever the area loads itself. So does
everything that stage loads for its enemies ("preload": the weapons they carry, plagas, effects;
research/stage_preloads.json, from scripts/survey_stage_preloads.sh), and every weapon the area's
own story enemies hold (arm ids 0x701NN in its set layouts): a weapon can take the area's model
(a uWp5f made in the s30x caves loads wp6f), and a model the area never loaded ends the game
("Failed open file ... wp6f00.mod").
Campaign maps use the stage number as "stage" (100 and up); Mercenaries maps keep 0..7.
Existing entries for the same stage are replaced; the Mercenaries maps are left as they are.
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xfs_setinfo import parse_bytes  # noqa: E402

ARCHIVES = 'G:/SteamLibrary/steamapps/common/Resident Evil 5/nativePC_MT/Image/Archive'
WAYPOINT_TYPE = 0x5F36B659
LAYOUT_TYPE = 0x15302EF4
MERGE_DISTANCE = 150.0
CHESTS = 8
REGION = 7000.0        # engine units around the player start (70 m)
CHEST_REGION = 4000.0
VERTICAL_REGION = 5000.0  # and within 50 m above or below it (s304's upper level is 110 m up)
MAX_PLAYER_DISTANCE = 3500
# Player starts when the host loads the area (vetting 2026-11-02, dev setdump). The game picks one of
# an area's entrances at random in this mode, so the map's "start" is where the adapter moves the
# player when an arena begins (its points and chests are around it).
STARTS = {
    100: (10492.5, 0.0, 7927.8), 104: (2123.6, -350.0, -10882.3), 105: (44.0, -99.8, -1147.4),
    113: (-92.9, 16.7, -4678.2), 114: (1266.0, 50.0, 1598.8), 202: (-3969.7, 301.2, 7726.8),
    204: (33856.2, 112.7, -87173.7), 300: (-3314.4, 2996.6, 46320.6), 302: (1696.9, 950.0, 14451.7),
    310: (342.5, -1299.5, -23565.0), 314: (-2455.7, 900.0, -22567.5), 500: (529.4, 0.0, 2613.4),
    503: (1718.5, 0.0, 19263.3), 505: (-2253.9, 703.2, -723.6), 506: (-137.7, -700.0, 821.5),
    508: (-897.9, -4.2, 798.2), 700: (0.0, 0.0, 0.0), 103: (-810.0, -1200.0, -1140.0),
    304: (470.0, -10900.0, 524.0),
    # Boss areas (2026-11-08): where a match on the stage put the player, the same in two runs each.
    309: (-55.0, 1.0, 932.0),
    # Popokarimu's ground (2026-11-19). A match put the player in the Field Office (-1753 366 -1727),
    # 3.7 m above the yard and 120 m from where the story fights it, and the boss came at a waypoint near
    # the player: under the office (build/runs/20261108-011728-bossarena-uEm44-s108: 5.4 m away, 3.6 m
    # below), and a player's "popped through the house, I had to use no collision to defeat it". The
    # start is the waypoint on the story's fight floor 15.7 m in front of where it lands (BOSSES).
    108: (-6140.2, 764.3, -12366.2),
    # Uroboros Mkono's lair (2026-11-09). A match puts the player at one of the area's entrances, behind
    # the door the story opens when the area is finished, so there was no way in (the user: "you can't
    # arrive to his lair in any capacity"; the boss test had warped there, which hid it). On the lair's
    # floor, on the line the boss itself ran in build/runs/20261108-181644-bossarena-uEm41-s308, 17.4 m
    # from its story spot: just outside its 15 m trigger.
    308: (-9952.8, -10.0, -17960.7),
    # Desperate Escape's last platform (2026-11-11, the user: "can you add the last platform from desperate
    # escape to the pool of maps?"). s804 was not it ("It's public assembly after the metal fence is
    # broken": s800..s804 are Public Assembly's); Desperate Escape is s702..s704 (Majini, soldiers, the
    # chainsaw, the big man), and s704 its last area: every waypoint 22 to 39 m up, the main deck at 26 m
    # (302 of its 565). Not vetted in game: the start is the waypoint nearest the middle of that deck, so
    # the adapter's start warp brings the player there wherever the host puts them.
    704: (-4.8, 2580.0, -184.6),
    # Four areas a match put outside the waypoints (2026-11-02 vetting), added 2026-11-23 for the user's map editor
    # (the user: "More campaign and DLC arenas: points made with the map editor"): each start is the waypoint with
    # the most others within 15 m, nearest the middle (scripts: the session's area_starts.py), not vetted in game.
    109: (100542.2, 0.0, 221619.7),
    119: (9993.5, 266.1, -9387.6),
    313: (-8744.8, 4102.0, 47538.5),
    702: (10681.2, -603.0, -17684.4),
}
# Starts moved off the recorded spot. s204: the host puts the player on the boat the story arrives
# on (33885 51 -87306), and the climb to the ledge is a story action that never appears here (user
# playtest 2026-11-03: stuck in the boat; the vetting's nearest enemy never closed in). The start is
# the ledge where that climb ends (its waypoints: boat 41-46, the climb 77, the ledge 113).
# Points below a height are left out: nobody spawned there could leave either.
BELOW = {204: 90.0}
# Points above a height are left out. s308: the lair's floor only (its 78 points are one linked floor);
# the corridors above it are past the doors the story keeps closed, so a chest there could not be reached.
# s108: Popokarimu's fight floor (its waypoints within 30 m of the landing are 6 to 13.4 m up); a chest
# went on a ledge 21.5 m up.
ABOVE = {108: 1400.0, 308: 50.0}
# Boxes (x0, z0, x1, z1) whose points and chests are left out. s204: the western walkway (x 27700 to
# 31300, 3 to 4.5 m up) ends 17 m short of the floor the player fights on and 3.4 m above it, with no
# waypoint between: of 7 Majini spawned there in a player's arena (2026-11-19, alpha.33, adapter.log
# 11:27-11:32) 6 stood at its end 64 to 165 s until the mod moved them ("enemies spawning where they
# shouldn't and they need much time to appear near"). And the ledge 3.6 m up at x 35800 to 36200: a
# Majini spawned on it stood there in that arena (47 s) and in the 2026-11-19 test (30 s).
# s108: the way back north to the office (z above -115 m): the fight floor's points and chests only.
LEFT_OUT = {108: [(-1.0e6, -11500.0, 1.0e6, 1.0e6)], 204: [(-1.0e6, -1.0e6, 32000.0, 1.0e6), (35700.0, -87800.0, 36300.0, -87400.0)]}

# Boss arenas (ROADMAP M6, scripts/test_boss_spawn.sh): the area the boss is fought in, the boss
# (its class, a name for the menus, its health: the Uroboros's own 15000; 6000 fell in under a
# minute, user playtest 2026-11-03) and the
# archives that come with its class. Their maps hold no roster; runs end on one.
BOSSES = {
    # Where the story sets it (s103_em00: its position and turn), and it appears when the player walks
    # into the box the story's own script waits for (s103_fsm "uro shtuugen", about 9 by 11 m), or
    # after 2 minutes (the user, 2026-11-08: it "teleports where you are at the start of the stage").
    103: {'class': 'uEm40', 'name': 'Uroboros', 'hp': 15000, 'preload': ['uEm40Dummy', 'uEm40Parts'],
          'spawn': (-3404.9, -1000.0, 962.0), 'facing': 0.0559,
          'trigger_box': [-4162.4, -1023.2, 61.1, -3263.7, -695.9, 1149.5], 'trigger_wait': 120},
    # U-8 (uEm43, the game's name; called a giant Uroboros before 2026-11-19): its flying critters (uEmc5) come with it.
    304: {'class': 'uEm43', 'name': 'U-8', 'article': '', 'hp': 15000, 'preload': []},
    # Wesker in his black coat (the user's Midnight Wesker): gunfire hurts him only while he winds
    # up an attack (his knee dash: 150, 100 a pistol hit), otherwise he shrugs it off (DmgNot); at
    # zero health he fights on, so the mod counts him dead and plays his own Shrink as his fall
    # (build/runs/20261108-002722-hits-uEm81-s506). His campaign health.
    # His second wind (the user, 2026-11-22: "I want him to kneel and that it looks like he's dead, then
    # it he should get back up and play some voiceline and call in some enemies, little amount of small
    # majini, 1 or 2 executioners and some chainsaw guy ... then when you end him for the second time,
    # he's getting a couple of executioners to spawn, an uroboros mkono"): MapBoss phases. The adds
    # (their archives load with the arena) come from their home maps, Uroboros Mkono with its own
    # boss health; the third death is real.
    506: {'class': 'uEm81', 'name': 'Wesker', 'article': '', 'hp': 8500, 'preload': [],
          'phases': [
              {'down_seconds': 4, 'down_action': 'cEm80ActHiza', 'rise_action': 'cEm80ActProduction',
               'adds': [{'class': 'uEm10', 'count': 3}, {'class': 'uEm11', 'count': 2}, {'class': 'uEm20', 'count': 2},
                        {'class': 'uEm22', 'count': 1}]},
              {'down_seconds': 4, 'down_action': 'cEm80ActHiza', 'rise_action': 'cEm80ActProduction',
               'adds': [{'class': 'uEm20', 'count': 2}, {'class': 'uEm41', 'count': 1}]},
          ]},
    # Jill in her battle suit (2026-11-08): dashes, rolls, fires her SMGs; a killing shot plays her own
    # DieNormal (build/runs/20261108-010222-hits-uEm84-s309). 2000 her own: 6000 (a pistol hit 250-275).
    # The user's picks (2026-11-22, the boss tuning board): 15000 health, attacking from any distance,
    # her fire cooldowns at most 1.4 s, close moves every 5 s within 4 m, her wide sweep every 10 s at
    # range, no flinching but to explosions (an evasion instead), a roll aside 60 percent of the time
    # after 1.5 s in a player's aim. Her balcony start went (the user, the same day: "she goes up and
    # uses the smgs but you are below so it doesnt do anything"): she stays on the player's floor.
    309: {'class': 'uEm84', 'name': 'Jill', 'article': '', 'hp': 15000, 'preload': [],
          'tuning': {'attack_chance': [0.5, 0.6, 0.8, 0.9, 1.0, 1.0], 'fire_cooldown_max': 1.4, 'melee_every': 5, 'melee_range': 400,
                     'wide_fire_every': 10, 'flinch_only_explosives': 1, 'dodge_chance': 0.6, 'dodge_aim_seconds': 1.5}},
    # The flying creature of s108 (Popokarimu, by its looks and moves: walks, flies, dives, dashes,
    # strikes with its arm); a killing shot plays its own Die (build/runs/20261108-010801-hits-uEm44-s108).
    # 19200 its own, 50 a pistol hit on it: 12000. It appears at once where the story lands it (s108_em00:
    # it flies in from -4045.5 1629.2 -14310.0 to its mGotoPos, turned -0.733), facing the player's start.
    108: {'class': 'uEm44', 'name': 'Popokarimu', 'article': '', 'hp': 12000, 'preload': [],
          'spawn': (-5069.0, 886.1, -13514.2), 'facing': -0.733},
    # Uroboros Wesker on the Volcano (2026-11-08, the ladder's 12): his own check lets only his open
    # core be hurt (the story opens it with the partner); in a run his body takes gunfire too
    # (adapter hooks_combat.cpp), and at zero health, where he fights on, the mod plays his last
    # fall. His tentacles (uEm8e) and core (uEm8c) come with him.
    # 25 a pistol hit on his body (build/runs/20261108-153415-bossarena-uEm82-s508): 3000, about 120
    # pistol hits (180 at the 12-arena run's 1.5).
    # The user's picks (2026-11-22, the boss tuning board): 9000 health, body hits at half, rage every 60 s,
    # a special attack every 20 s (maps.json boss tuning). His second form (the full move set his arena
    # never switched on: drill grab, ground slam, javelins, Tornado and rage) comes with his first death,
    # with his health back (the user, the same day: "when he dies we bump it to a second phase"): a 2.5 s
    # stagger at his core, then his pose byte alone. Not the story's StageChange: its end applies the giant
    # lava form, whose hit shapes are off ("he's untargetable, no damage").
    508: {'class': 'uEm82', 'name': 'Uroboros Wesker', 'article': '', 'hp': 9000, 'preload': ['uEm8c', 'uEm8e'],
          'tuning': {'body_damage': 0.5, 'rage_every': 60, 'rage_seconds': 15, 'specials_every': 20},
          'phases': [{'down_seconds': 2.5, 'down_action': 'cEm82ActDmgCore', 'transform': True}]},
    # Uroboros Mkono, the stronger Uroboros of the flamethrower room (uEm41, 2026-11-08; called Aheri
    # before 2026-11-19: the game's chapter 5-2 boss): it walks, runs, beats down
    # and grabs on its own; its arm and core parts come with it. The room's own setup is skipped in a
    # run (adapter stage_fixes.cpp: it crashed with no flamethrower). It waits in its room where the
    # story sets it (the s308 set's position and turn, its story health 15000) and appears once the
    # player comes within 15 m, or after 90 s (the user, 2026-11-08: "spawn ... where they are
    # supposed to be, right now it spawns in the corridor if you don't move"). 80 a pistol hit on its
    # body, 200 on a weak spot (build/runs/20261108-181644-bossarena-uEm41-s308): 10000, 125 body
    # hits (160 at the 8-arena run's 1.3; the first Uroboros is 150 at 100 a hit).
    308: {'class': 'uEm41', 'name': 'Uroboros Mkono', 'article': '', 'hp': 10000, 'preload': ['uEm41Arm', 'uEm41Core', 'uOm0019'],
          'spawn': (-11691.4, 0.0, -18013.9), 'facing': 1.4286, 'trigger': 1500, 'trigger_wait': 90},
    # Uroboros Aheri, Excella's Uroboros (uEm42, 2026-11-08; called Mkono before 2026-11-19: the game's
    # chapter 6-2 boss): its body, four cores, slime and tentacles. A mass that
    # rises beside the deck: it appears where the story sets it (the s511 set Uro3_Battle's position
    # and turn; at a waypoint it hung 33 m up, the user: "it spawned incorrectly") and never moves.
    511: {'class': 'uEm42', 'name': 'Uroboros Aheri', 'article': '', 'hp': 28000, 'preload': ['uEm42Core', 'uEm42Slime', 'uEm42Tentacle', 'uOm0019'],
          'spawn': (1248.0, 0.0, 21800.0), 'facing': 0.0},
}
BOSS_PLAYER_DISTANCE = (1000, 2500)  # the boss appears within sight, not on top of the player
# Kept for developer tests but out of the run rotation ("rotation": false), with the reason.
# Objects of an area a run does not build (maps.json "hide_models", set record names or their
# start). Since 2026-11-20 the user sets them, the points and the starts of many campaign areas
# with the map editor (adapter.json "map_editor"); scripts/fold_map_edits.py writes those into
# maps.json and research/map_edits/ keeps each map_edits.json folded. Regenerating an entry here
# loses that hand work: fold the kept files again afterwards. (The Engine Room's big doors,
# om0955/om0959, were tried hidden; the user put them back and hid the lever om08BC_IDo00_U001.)
HIDE_MODELS = {
    505: ['OM08BC_IDo00_U001'],
}

NO_ROTATION = {
    # New (2026-11-23): out of the runs until the user has set their points and starts with the map editor
    # (dev "nextmap <stage>" goes there), then their "rotation": false comes out.
    109: 'new: waits for the map editor',
    119: 'new: waits for the map editor',
    313: 'new: waits for the map editor',
    702: 'new: waits for the map editor',
    # (Civilian Checkpoint, s100, was out from 2026-11-03 to 0.2.5: the story kept the inventory,
    # the weapon and the HUD off there; a run now turns those stage-100 checks off, stage_fixes.h.)
    # Every AI waypoint of the area (276) lies in one 35 x 18 m box in the middle of the deck (y 0 to
    # 2 m); the rest of the deck the player can walk (the upper deck, 50 m off) has none, so the
    # enemies cannot follow there (a player 2026-11-18: "had no pathing to follow me anywhere in the
    # stage except for the wide open area in the middle").
    503: 'the enemies can only walk the middle of the deck',
    # The campaign's Public Assembly (its sets hold the Executioner uEm20 and the helicopter
    # uOm1316): a fence the Executioner breaks and a door the helicopter bursts stay in the level as
    # walls the player cannot see, and enemies got stuck behind one (a player 2026-11-18). The user:
    # "else, if broken, take out of rotation"; the spot was not found in game.
    114: 'an invisible wall where the story breaks a fence; enemies stuck behind it',
    # Lost in Nightmares' mansion: the Majini stop at its doors (the stuck rule moved one three times
    # at om020eL, 1228 0 730). Not building the door models (dev "hidemodels om020 om0210") removes
    # only their look: the player and the Majini still stop at the doorways (2026-11-18), so the
    # doors cannot be taken away (the user's request); out at the user's choice.
    700: 'the Majini stop at its doors, and the doors cannot be removed',
}  # 505 was out (2026-11-08, a ladder up from its ground floor could not be grabbed: a Mercenaries
# match never built the area's ladders; since 2026-11-18 the adapter files its "layouts" and builds
# them, all eight, one climbed in game). 503 was out (its random upper-deck start); the start warp (M6.2) fixed that. 508 was out
# (2026-11-08: a camera zone held the view); a run's campaign area now keeps the player camera
# (adapter area_cameras.cpp).
# The game's own area names (2026-11-19: its stage table at 0x0112B748, stage -> mes_system message,
# the save screen's "Area"); before, short names from the areas' start views (vetting 2026-11-02).
# "(Campaign)" where a Mercenaries stage has the same name; "Oil Field - Drilling Facilities" without
# its "Oil Field - " for the menus' help lines. Not in the game's table (DLC): s700, s704. Others get
# "Campaign sNNN".
NAMES = {
    100: 'Civilian Checkpoint', 104: 'The Bridge', 105: 'Shanty Town', 113: 'The Port',
    114: 'Public Assembly (Campaign)', 202: 'Village (Campaign)', 204: 'Drilling Facilities',
    300: 'Ancient Village', 302: 'Worship Area', 310: 'Power Station', 314: 'Missile Area 2nd Floor',
    500: 'Ship Deck (Campaign)', 503: 'Main Deck', 505: 'Engine Room', 506: 'Hangar', 508: 'Volcano',
    700: 'Spencer Mansion', 103: 'Furnace Facility', 304: 'Experimental Facility (Campaign)',
    309: 'Monarch Room', 108: 'Mining Area', 308: 'Uroboros Research Facility', 511: 'Bridge Deck',
    704: 'Last Platform',
}


def resources(stage, kind_wanted):
    data = open(f'{ARCHIVES}/s{stage:03d}.arc', 'rb').read()
    count = struct.unpack('<H', data[6:8])[0]
    offset = 8
    for _ in range(count):
        name = data[offset:offset + 64].split(b'\0')[0].decode('latin1')
        kind, packed, size, start = struct.unpack('<IIII', data[offset + 64:offset + 80])
        offset += 80
        if kind == kind_wanted:
            yield name, zlib.decompress(data[start:start + packed])


def waypoints(stage):
    for name, body in resources(stage, WAYPOINT_TYPE):
        assert body[:4] == b'WAY\0', name
        n = struct.unpack('<I', body[8:12])[0]
        # 16-byte header, then 28-byte points: x, y, z, flags, two zeros, index.
        return [list(struct.unpack('<3f', body[16 + i * 28:28 + i * 28])) for i in range(n)]
    raise SystemExit(f's{stage:03d}.arc has no AI waypoints')


def area_weapons(stage):
    """uWpNN archives for the Majini arm ids (0x701NN) in the stage's set layouts."""
    present = {f[:-4].lower() for f in os.listdir(ARCHIVES) if f.lower().endswith('.arc')}
    found = set()
    for _, body in resources(stage, LAYOUT_TYPE):
        for at in range(len(body) - 3):
            value = struct.unpack_from('<I', body, at)[0]
            if value & 0xFFFFFF00 == 0x70100 and value != 0x70100 and f'uwp{value & 0xFF:02x}' in present:
                found.add(f'uWp{value & 0xFF:02x}')
    return sorted(found)


LADDER_CLASSES = {'uOmf200', 'uOmf201'}  # cSetInfoOmLadderV


def ladder_layouts(stage):
    """The area's own object layouts (soft\\sNNN_IDfNN_Ins) holding a ladder: a Mercenaries match
    skips them, so the adapter files them and builds their ladders (maps.json "layouts")."""
    names = []
    for name, body in resources(stage, LAYOUT_TYPE):
        path = name.replace('/', '\\').lower()
        if '\\soft\\' not in path or '\\merce' in path or not path.endswith('_ins'):
            continue
        infos = parse_bytes(body)
        if any(isinstance(si, dict) and si.get('mUnitClass') in LADDER_CLASSES for si in infos):
            names.append(name.replace('/', '\\').split('\\')[-1])
    return sorted(names)


def spread(points, count):
    """Farthest-point sample: each next point is the one farthest from those already taken."""
    if not points:
        return []
    chosen = [points[0]]
    while len(chosen) < min(count, len(points)):
        best = max(points, key=lambda p: min(math.hypot(p[0] - c[0], p[2] - c[2]) for c in chosen))
        chosen.append(best)
    return chosen


parser = argparse.ArgumentParser()
parser.add_argument('stages', nargs='+', type=int)
parser.add_argument('--template', default='0',
                    help='Mercenaries map (stage index) whose roster is copied; several, comma separated, are merged '
                         '(their classes, the first one\'s of a class shared, and every one\'s preloads)')
parser.add_argument('--layouts-only', action='store_true',
                    help='only (re)write the "layouts" of the existing entries of these stages')
args = parser.parse_args()

doc = json.load(open('configs/maps.json', encoding='utf-8'))


def with_layouts(entry, layouts):
    """The entry with "layouts" after "preload" (or "chests"), or without it when there are none."""
    out = {}
    for key, value in entry.items():
        if key == 'layouts':
            continue
        out[key] = value
        if key == ('preload' if 'preload' in entry else 'chests') and layouts:
            out['layouts'] = layouts
    if layouts and 'layouts' not in out:
        out['layouts'] = layouts
    return out


def write_doc():
    with open('configs/maps.json', 'w', encoding='utf-8', newline='\n') as f:
        text = json.dumps(doc, indent=2)
        text = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', text)
        text = re.sub(r'"(arms|preload|layouts|hide_models|attack_chance)": \[([^\]]*)\]',
                      lambda m: f'"{m.group(1)}": [' + ', '.join(x.strip() for x in m.group(2).split(',')) + ']', text)
        text = re.sub(r'\{\s+"weight": (\d+),\s+"fields": \{([^}]*)\}\s+\}',
                      lambda m: '{"weight": ' + m.group(1) + ', "fields": {' + ', '.join(x.strip() for x in m.group(2).split(',') if x.strip()) + '}}', text)
        # A boss phase's adds, one per line.
        text = re.sub(r'\{\s+"class": "(uEm\w+)",\s+"count": (\d+)\s+\}', r'{"class": "\1", "count": \2}', text)
        f.write(text + '\n')


if args.layouts_only:
    for stage in args.stages:
        layouts = ladder_layouts(stage)
        doc['maps'] = [with_layouts(m, layouts) if m['stage'] == stage else m for m in doc['maps']]
        print(f's{stage}: layouts {layouts or "none"}')
    write_doc()
    raise SystemExit(0)

template_ids = [int(t) for t in str(args.template).split(',')]
templates = [next(m for m in doc['maps'] if m['stage'] == t) for t in template_ids]
template = dict(templates[0])
template['roster'] = []
for t in templates:
    for entry in t['roster']:
        if all(entry['class'] != e['class'] for e in template['roster']):
            template['roster'].append(entry)
template['name'] = ' and '.join(t['name'] for t in templates)
stage_preloads = json.load(open('research/stage_preloads.json', encoding='utf-8'))['stages']
preload = []
for t in template_ids:
    for name in stage_preloads[str(t)]:
        if re.fullmatch(r'\w{1,48}', name) and name not in preload:
            preload.append(name)
for stage in args.stages:
    points = []
    for x, y, z in waypoints(stage):
        if any(math.hypot(x - q[0], z - q[2]) < MERGE_DISTANCE and abs(y - q[1]) < 200 for q in points):
            continue
        points.append([round(x, 1), round(y, 1), round(z, 1)])
    if stage in BELOW:
        points = [p for p in points if p[1] >= BELOW[stage]]
    if stage in ABOVE:
        points = [p for p in points if p[1] <= ABOVE[stage]]
    for x0, z0, x1, z1 in LEFT_OUT.get(stage, []):
        points = [p for p in points if not (x0 <= p[0] <= x1 and z0 <= p[2] <= z1)]
    chest_points = points
    if stage in STARTS:
        sx, sy, sz = STARTS[stage]
        points = [p for p in points if math.hypot(p[0] - sx, p[2] - sz) <= REGION and abs(p[1] - sy) <= VERTICAL_REGION]
        chest_points = [p for p in points if 500.0 <= math.hypot(p[0] - sx, p[2] - sz) <= CHEST_REGION]
    entry = {
        'id': f'campaign_s{stage}',
        'name': NAMES.get(stage, f'Campaign s{stage}'),
        'stage': stage,
        **({'rotation': False} if stage in NO_ROTATION else {}),
        **({'start': [round(c, 1) for c in STARTS[stage]]} if stage in STARTS else {}),
        'max_alive': template['max_alive'],
        'min_player_distance': template['min_player_distance'],
        'max_player_distance': MAX_PLAYER_DISTANCE,
        'points': points,
        'chests': spread(chest_points or points, CHESTS),
        'preload': preload + [w for w in area_weapons(stage) if w not in preload],
        'roster': template['roster'],
    }
    if stage in BOSSES:
        boss = BOSSES[stage]
        entry = {k: v for k, v in entry.items() if k not in ('roster', 'max_alive', 'preload')}
        entry['min_player_distance'], entry['max_player_distance'] = BOSS_PLAYER_DISTANCE
        entry['preload'] = boss['preload']
        entry['boss'] = {'class': boss['class'], 'name': boss['name'], **({'article': boss['article']} if 'article' in boss else {}),
                         'hp': boss['hp']}
        # Its story spot (the director spawns it there, not at a waypoint) and when it appears.
        if 'spawn' in boss:
            entry['boss']['spawn'] = [round(c, 1) for c in boss['spawn']]
        for key in ('facing', 'trigger', 'trigger_box', 'trigger_wait', 'phases', 'tuning'):
            if key in boss:
                entry['boss'][key] = boss[key]
    entry = with_layouts(entry, ladder_layouts(stage))
    if stage in HIDE_MODELS:
        entry['hide_models'] = HIDE_MODELS[stage]
    # An area made again keeps player 2's start (the map editor's, folded in).
    old = next((m for m in doc['maps'] if m['stage'] == stage), None)
    if old is not None and 'small_only_points' in old:
        entry['small_only_points'] = old['small_only_points']
    if old is not None and 'start2' in old:
        entry = {**{k: v for k, v in entry.items() if k not in ('max_alive', 'min_player_distance', 'max_player_distance', 'points', 'chests', 'preload', 'layouts', 'roster', 'boss', 'hide_models')},
                 'start2': old['start2'], **{k: v for k, v in entry.items() if k in ('max_alive', 'min_player_distance', 'max_player_distance', 'points', 'chests', 'preload', 'layouts', 'roster', 'boss', 'hide_models')}}
    # An area made again keeps its place in the list (the run map draws from it in order).
    if any(m['stage'] == stage for m in doc['maps']):
        doc['maps'] = [entry if m['stage'] == stage else m for m in doc['maps']]
    else:
        doc['maps'].append(entry)
    if stage in BOSSES:
        print(f's{stage}: {len(points)} points, {len(entry["chests"])} chests, boss {entry["boss"]["name"]} ({entry["boss"]["class"]})')
    else:
        print(f's{stage}: {len(points)} points, {len(entry["chests"])} chests, roster and {len(preload)} archives from '
              f'{template["name"]}, {len(entry["preload"]) - len(preload)} more weapons of the area')

write_doc()
