#!/usr/bin/env python3
"""Turns survey lines of the adapter log into research/spawn_survey.json and prints a summary.

  python scripts/survey_spawns.py [adapter.log ...]

Survey lines count while the survey is on ("dev survey on" .. "dev survey off"); the stage is the
one the autopilot armed last ("autopilot: armed (stage N"; the game work stage field reads 0 in
Mercenaries). Every "director survey: <class> arm <hex> num <n> set <g> id <i> at <x> <y> <z>" line
is one native spawn.
"""
import json
import os
import re
import sys

ON = re.compile(r'dev survey on')
ARMED = re.compile(r'autopilot: armed \(stage (\d+)')
OFF = re.compile(r'dev survey off')
LINE = re.compile(r'director survey: (\S+) arm ([0-9A-F]+) num (-?\d+) set (\d+) id (\d+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')

paths = sys.argv[1:] or [os.path.expandvars(r'%LOCALAPPDATA%\RogueMercenaries\logs\adapter.log')]
stages = {}
for path in paths:
    stage = None
    surveying = False
    for line in open(path, encoding='utf-8', errors='replace'):
        if 'survey' not in line and 'armed' not in line:
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
        m = LINE.search(line)
        if m and surveying and stage is not None:
            cls, arm, num, group, ident, x, y, z = m.groups()
            entry = stages.setdefault(str(stage), {'points': {}, 'classes': {}})
            key = f'{group}:{ident}:{round(float(x))}:{round(float(z))}'
            entry['points'][key] = {'set': int(group), 'id': int(ident), 'class': cls, 'arm': int(arm, 16),
                                    'pos': [round(float(x), 1), round(float(y), 1), round(float(z), 1)]}
            c = entry['classes'].setdefault(cls, {'count': 0, 'arms': []})
            c['count'] += 1
            if int(arm, 16) not in c['arms']:
                c['arms'].append(int(arm, 16))

out = {s: {'points': sorted(v['points'].values(), key=lambda p: (p['set'], p['id'])), 'classes': v['classes']}
       for s, v in sorted(stages.items(), key=lambda kv: int(kv[0]))}
os.makedirs('research', exist_ok=True)
json.dump({'_comment': 'Native spawns observed per Mercenaries stage (scripts/survey_spawns.sh). pos = where the unit '
                       'stood right after spawning; arm = descriptor weapon id; set/id = the stage set record.',
           'stages': out}, open('research/spawn_survey.json', 'w'), indent=1)
for s, v in out.items():
    classes = ', '.join(f"{c} x{d['count']} arms {','.join(hex(a) for a in d['arms'])}" for c, d in v['classes'].items())
    print(f'stage {s}: {len(v["points"])} distinct spawn points; {classes}')
