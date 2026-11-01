#!/usr/bin/env python3
"""Turns survey lines of the adapter log into research/spawn_survey.json and prints a summary.

  python scripts/survey_spawns.py [adapter.log ...]

Survey lines count while the survey is on ("dev survey on" .. "dev survey off"); the stage is the
one the autopilot armed last ("autopilot: armed (stage N"; the game work stage field reads 0 in
Mercenaries). Every "director survey: <class> arm <hex> num <n> set <g> id <i> at <x> <y> <z>" line
is one native spawn. Every "director descriptor native <class>: | <dwords>" line is the game's own
descriptor of a Majini-family spawn (+0x00..+0x11F); its behaviour and look fields become the
class's profiles (counted, so the director can pick them with the game's own odds).
"""
import json
import os
import re
import struct
import sys

ON = re.compile(r'dev survey on')
ARMED = re.compile(r'autopilot: armed \(stage (\d+)')
OFF = re.compile(r'dev survey off')
LINE = re.compile(r'director survey: (\S+) arm ([0-9A-F]+) num (-?\d+) set (\d+) id (\d+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')
DESC = re.compile(r'director descriptor native (\S+):((?: \|?[0-9A-F]{8})+)')

# Descriptor fields kept in a profile: name -> (offset, kind). Offsets from the property lists
# FUN_00CCE070 (cSetInfoEnemy) and FUN_00CC6C20 (cSetInfoEm10); names match the adapter's
# kSpawnFields. Placement (position, angle, goto) is the director's business and left out.
FIELDS = {
    'search_type': (0xB0, 'u32'), 'search_forward': (0xB4, 'f32'), 'search_circle': (0xB8, 'f32'),
    'area_keep_type': (0xC0, 'u32'), 'area_keep_dist': (0xC4, 'f32'),
    'is_extra': (0xE0, 'u8'), 'extra_no': (0xE4, 'u32'), 'is_acting': (0xE8, 'u8'), 'armed': (0xE9, 'u8'),
    'arm_num': (0xEC, 'u32'), 'arm': (0xF0, 'u32'), 'arm_set_joint': (0xF4, 'u32'), 'attribute': (0xF8, 'u32'),
    'plaga': (0xFC, 'u32'), 'sub_bomb': (0x100, 'u32'), 'attack': (0x104, 'u32'), 'mask': (0x108, 'u32'),
    'lefty': (0x10C, 'u8'), 'neck_event': (0x10D, 'u8'),
}


def profile_of(dwords):
    raw = b''.join(struct.pack('<I', d) for d in dwords)
    out = {}
    for name, (off, kind) in FIELDS.items():
        if off + 4 > len(raw):
            continue
        if kind == 'u8':
            out[name] = raw[off]
        elif kind == 'f32':
            out[name] = round(struct.unpack_from('<f', raw, off)[0], 3)
        else:
            out[name] = struct.unpack_from('<I', raw, off)[0]
    return out


paths = sys.argv[1:] or [os.path.expandvars(r'%LOCALAPPDATA%\RogueMercenaries\logs\adapter.log')]
stages = {}
for path in paths:
    stage = None
    surveying = False
    for line in open(path, encoding='utf-8', errors='replace'):
        if 'survey' not in line and 'armed' not in line and 'descriptor' not in line:
            continue
        m = ARMED.search(line)
        if m:
            stage = int(m.group(1))
            continue
        if ON.search(line):
            surveying = True
            continue
        if OFF.search(line):
            surveying = False
            continue
        if not surveying or stage is None:
            continue
        m = DESC.search(line)
        if m:
            cls = m.group(1)
            dwords = [int(x.strip('|'), 16) for x in m.group(2).split()]
            entry = stages.setdefault(str(stage), {'points': {}, 'classes': {}})
            c = entry['classes'].setdefault(cls, {'count': 0, 'arms': []})
            key = json.dumps(profile_of(dwords), sort_keys=True)
            c.setdefault('profile_counts', {})
            c['profile_counts'][key] = c['profile_counts'].get(key, 0) + 1
            continue
        m = LINE.search(line)
        if m:
            cls, arm, num, group, ident, x, y, z = m.groups()
            entry = stages.setdefault(str(stage), {'points': {}, 'classes': {}})
            key = f'{group}:{ident}:{round(float(x))}:{round(float(z))}'
            entry['points'][key] = {'set': int(group), 'id': int(ident), 'class': cls, 'arm': int(arm, 16),
                                    'pos': [round(float(x), 1), round(float(y), 1), round(float(z), 1)]}
            c = entry['classes'].setdefault(cls, {'count': 0, 'arms': []})
            c['count'] += 1
            if int(arm, 16) not in c['arms']:
                c['arms'].append(int(arm, 16))

out = {}
for s, v in sorted(stages.items(), key=lambda kv: int(kv[0])):
    classes = {}
    for cls, c in v['classes'].items():
        entry = {'count': c['count'], 'arms': c['arms']}
        if c.get('profile_counts'):
            entry['profiles'] = sorted(({'count': n, 'fields': json.loads(k)} for k, n in c['profile_counts'].items()),
                                       key=lambda p: -p['count'])
        classes[cls] = entry
    out[s] = {'points': sorted(v['points'].values(), key=lambda p: (p['set'], p['id'])), 'classes': classes}
os.makedirs('research', exist_ok=True)
json.dump({'_comment': 'Native spawns observed per Mercenaries stage (scripts/survey_spawns.sh). pos = where the unit '
                       'stood right after spawning; arm = descriptor weapon id; set/id = the stage set record; '
                       'profiles = the game\'s own descriptor fields for Majini-family spawns, with counts.',
           'stages': out}, open('research/spawn_survey.json', 'w'), indent=1)
for s, v in out.items():
    classes = ', '.join(f"{c} x{d['count']} ({len(d.get('profiles', []))} profiles)" for c, d in v['classes'].items())
    print(f'stage {s}: {len(v["points"])} distinct spawn points; {classes}')
