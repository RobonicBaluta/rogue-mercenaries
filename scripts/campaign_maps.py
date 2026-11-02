#!/usr/bin/env python3
"""Adds campaign areas to configs/maps.json as director maps (ROADMAP M5).

  python scripts/campaign_maps.py 503 [100 ...] [--template 0]

A campaign stage loaded in a Mercenaries match (adapter dev "stage <n>") has no enemy sets of
its own, but its archive holds the AI waypoints (rAIWayPoint "stage\\sNNN\\soft\\sNNN_g"): points on
walkable ground. They become the map's spawn points (merged within 150 units); chests stand on a
spread-out sample of them. The roster is copied from a Mercenaries map (--template, its stage
index): those classes load with the run (M3 preloads), whatever the area loads itself.
Campaign maps use the stage number as "stage" (100 and up); Mercenaries maps keep 0..7.
Existing entries for the same stage are replaced; the Mercenaries maps are left as they are.
"""
import argparse
import json
import math
import re
import struct
import zlib

ARCHIVES = 'G:/SteamLibrary/steamapps/common/Resident Evil 5/nativePC_MT/Image/Archive'
WAYPOINT_TYPE = 0x5F36B659
MERGE_DISTANCE = 150.0
CHESTS = 8


def waypoints(stage):
    data = open(f'{ARCHIVES}/s{stage:03d}.arc', 'rb').read()
    count = struct.unpack('<H', data[6:8])[0]
    offset = 8
    for _ in range(count):
        name = data[offset:offset + 64].split(b'\0')[0].decode('latin1')
        kind, packed, size, start = struct.unpack('<IIII', data[offset + 64:offset + 80])
        offset += 80
        if kind != WAYPOINT_TYPE:
            continue
        body = zlib.decompress(data[start:start + packed])
        assert body[:4] == b'WAY\0', name
        n = struct.unpack('<I', body[8:12])[0]
        # 16-byte header, then 28-byte points: x, y, z, flags, two zeros, index.
        return [list(struct.unpack('<3f', body[16 + i * 28:28 + i * 28])) for i in range(n)]
    raise SystemExit(f's{stage:03d}.arc has no AI waypoints')


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
for stage in args.stages:
    points = []
    for x, y, z in waypoints(stage):
        if any(math.hypot(x - q[0], z - q[2]) < MERGE_DISTANCE and abs(y - q[1]) < 200 for q in points):
            continue
        points.append([round(x, 1), round(y, 1), round(z, 1)])
    entry = {
        'id': f'campaign_s{stage}',
        'name': f'Campaign s{stage}',
        'stage': stage,
        'max_alive': template['max_alive'],
        'min_player_distance': template['min_player_distance'],
        'points': points,
        'chests': spread(points, CHESTS),
        'roster': template['roster'],
    }
    doc['maps'] = [m for m in doc['maps'] if m['stage'] != stage] + [entry]
    print(f's{stage}: {len(points)} points, {len(entry["chests"])} chests, roster from {template["name"]}')

with open('configs/maps.json', 'w', encoding='utf-8', newline='\n') as f:
    text = json.dumps(doc, indent=2)
    text = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', text)
    text = re.sub(r'"arms": \[([^\]]*)\]',
                  lambda m: '"arms": [' + ', '.join(x.strip() for x in m.group(1).split(',')) + ']', text)
    text = re.sub(r'\{\s+"weight": (\d+),\s+"fields": \{([^}]*)\}\s+\}',
                  lambda m: '{"weight": ' + m.group(1) + ', "fields": {' + ', '.join(x.strip() for x in m.group(2).split(',') if x.strip()) + '}}', text)
    f.write(text + '\n')
