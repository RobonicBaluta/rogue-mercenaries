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
import zlib

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
    309: (-55.0, 1.0, 932.0), 108: (-1753.0, 366.0, -1727.0),
}
# Starts moved off the recorded spot. s204: the host puts the player on the boat the story arrives
# on (33885 51 -87306), and the climb to the ledge is a story action that never appears here (user
# playtest 2026-11-03: stuck in the boat; the vetting's nearest enemy never closed in). The start is
# the ledge where that climb ends (its waypoints: boat 41-46, the climb 77, the ledge 113).
# Points below a height are left out: nobody spawned there could leave either.
BELOW = {204: 90.0}

# Boss arenas (ROADMAP M6, scripts/test_boss_spawn.sh): the area the boss is fought in, the boss
# (its class, a name for the menus, its health: the Uroboros's own 15000; 6000 fell in under a
# minute, user playtest 2026-11-03) and the
# archives that come with its class. Their maps hold no roster; runs end on one.
BOSSES = {
    103: {'class': 'uEm40', 'name': 'Uroboros', 'hp': 15000, 'preload': ['uEm40Dummy', 'uEm40Parts']},
    304: {'class': 'uEm43', 'name': 'Giant Uroboros', 'hp': 15000, 'preload': []},
    # Wesker in his black coat (the user's Midnight Wesker): gunfire hurts him only while he winds
    # up an attack (his knee dash: 150, 100 a pistol hit), otherwise he shrugs it off (DmgNot); at
    # zero health he fights on, so the mod counts him dead and plays his own Shrink as his fall
    # (build/runs/20261108-002722-hits-uEm81-s506). His campaign health.
    506: {'class': 'uEm81', 'name': 'Wesker', 'article': '', 'hp': 8500, 'preload': []},
    # Jill in her battle suit (2026-11-08): dashes, rolls, fires her SMGs; a killing shot plays her own
    # DieNormal (build/runs/20261108-010222-hits-uEm84-s309). 2000 her own: 6000 (a pistol hit 250-275).
    309: {'class': 'uEm84', 'name': 'Jill', 'article': '', 'hp': 6000, 'preload': []},
    # The flying creature of s108 (Popokarimu, by its looks and moves: walks, flies, dives, dashes,
    # strikes with its arm); a killing shot plays its own Die (build/runs/20261108-010801-hits-uEm44-s108).
    # 19200 its own, 50 a pistol hit on it: 12000.
    108: {'class': 'uEm44', 'name': 'Popokarimu', 'hp': 12000, 'preload': []},
    # Uroboros Wesker on the Volcano (2026-11-08, the ladder's 12): his own check lets only his open
    # core be hurt (the story opens it with the partner); in a run his body takes gunfire too
    # (adapter hooks_combat.cpp), and at zero health, where he fights on, the mod plays his last
    # fall. His tentacles (uEm8e) and core (uEm8c) come with him.
    # 25 a pistol hit on his body (build/runs/20261108-153415-bossarena-uEm82-s508): 3000, about 120
    # pistol hits (180 at the 12-arena run's 1.5).
    508: {'class': 'uEm82', 'name': 'Uroboros Wesker', 'article': '', 'hp': 3000, 'preload': ['uEm8c', 'uEm8e']},
    # The stronger Uroboros of the flamethrower room (uEm41, 2026-11-08): it walks, runs, beats down
    # and grabs on its own; its arm and core parts come with it. The room's own setup is skipped in a
    # run (adapter stage_fixes.cpp: it crashed with no flamethrower). It waits in its room where the
    # story sets it (the s308 set's position and turn, its story health 15000) and appears once the
    # player comes within 15 m, or after 90 s (the user, 2026-11-08: "spawn ... where they are
    # supposed to be, right now it spawns in the corridor if you don't move"). 80 a pistol hit on its
    # body, 200 on a weak spot (build/runs/20261108-181644-bossarena-uEm41-s308): 10000, 125 body
    # hits (160 at the 8-arena run's 1.3; the first Uroboros is 150 at 100 a hit).
    308: {'class': 'uEm41', 'name': 'Uroboros Aheri', 'hp': 10000, 'preload': ['uEm41Arm', 'uEm41Core', 'uOm0019'],
          'spawn': (-11691.4, 0.0, -18013.9), 'facing': 1.4286, 'trigger': 1500, 'trigger_wait': 90},
    # Excella's Uroboros (uEm42, 2026-11-08): its body, four cores, slime and tentacles. A mass that
    # rises beside the deck: it appears where the story sets it (the s511 set Uro3_Battle's position
    # and turn; at a waypoint it hung 33 m up, the user: "it spawned incorrectly") and never moves.
    511: {'class': 'uEm42', 'name': 'Uroboros Mkono', 'hp': 28000, 'preload': ['uEm42Core', 'uEm42Slime', 'uEm42Tentacle', 'uOm0019'],
          'spawn': (1248.0, 0.0, 21800.0), 'facing': 0.0},
}
BOSS_PLAYER_DISTANCE = (1000, 2500)  # the boss appears within sight, not on top of the player
# Kept for developer tests but out of the run rotation ("rotation": false), with the reason.
NO_ROTATION = {
    # The campaign's opening area: the story keeps the inventory, weapon switching and the HUD off
    # there until an event a run never reaches (user playtest 2026-11-03).
    100: 'the story keeps the inventory, weapons and HUD off',
    # The ground floor (y 0) leads up only by ladders (s505_IDf00_Ins: eight cSetInfoOmLadderV
    # uOmf201, 2-4 m), and one could not be grabbed after a fall (user playtest 2026-11-08: "you
    # can't ever continue and you are stuck there"). Back once ladders are checked in game.
    505: 'a ladder up from the ground floor cannot be grabbed: a fall there strands the player',
}  # 503 was out (its random upper-deck start); the start warp (M6.2) fixed that. 508 was out
# (2026-11-08: a camera zone held the view); a run's campaign area now keeps the player camera
# (adapter area_cameras.cpp).
# Short names from the areas' start views (vetting 2026-11-02); others get "Campaign sNNN".
NAMES = {
    100: 'Kijuju Town', 104: 'Waterfront', 105: 'Back Alleys', 113: 'Warehouse', 114: 'Slums',
    202: 'Marsh Village', 204: 'Port', 300: 'Cave Shrine', 302: 'Ruin Passages', 310: 'Underground Facility',
    314: 'Mine Scaffolds', 500: 'Cargo Hold', 503: 'Freighter Deck', 505: 'Factory Floor', 506: 'Steel Docks',
    508: 'Volcano', 700: 'Spencer Mansion', 103: 'Pump Rooms', 304: 'Capsule Dome', 309: 'Temple Hall',
    108: 'Field Office', 308: 'Tiled Labs', 511: 'Upper Deck',
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
parser.add_argument('--template', type=int, default=0, help='Mercenaries map (stage index) whose roster is copied')
args = parser.parse_args()

doc = json.load(open('configs/maps.json', encoding='utf-8'))
template = next(m for m in doc['maps'] if m['stage'] == args.template)
preload = json.load(open('research/stage_preloads.json', encoding='utf-8'))['stages'][str(args.template)]
preload = [name for name in preload if re.fullmatch(r'\w{1,48}', name)]
for stage in args.stages:
    points = []
    for x, y, z in waypoints(stage):
        if any(math.hypot(x - q[0], z - q[2]) < MERGE_DISTANCE and abs(y - q[1]) < 200 for q in points):
            continue
        points.append([round(x, 1), round(y, 1), round(z, 1)])
    if stage in BELOW:
        points = [p for p in points if p[1] >= BELOW[stage]]
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
        for key in ('facing', 'trigger', 'trigger_wait'):
            if key in boss:
                entry['boss'][key] = boss[key]
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

with open('configs/maps.json', 'w', encoding='utf-8', newline='\n') as f:
    text = json.dumps(doc, indent=2)
    text = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', text)
    text = re.sub(r'"(arms|preload)": \[([^\]]*)\]',
                  lambda m: f'"{m.group(1)}": [' + ', '.join(x.strip() for x in m.group(2).split(',')) + ']', text)
    text = re.sub(r'\{\s+"weight": (\d+),\s+"fields": \{([^}]*)\}\s+\}',
                  lambda m: '{"weight": ' + m.group(1) + ', "fields": {' + ', '.join(x.strip() for x in m.group(2).split(',') if x.strip()) + '}}', text)
    f.write(text + '\n')
