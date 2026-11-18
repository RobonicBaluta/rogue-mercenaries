#!/usr/bin/env python3
"""Writes configs/maps.json (the director's maps, ROADMAP M1.3) from research/spawn_survey.json.

  python scripts/write_maps.py [--exclude uEmXX,uEmYY]

Per surveyed stage: the spawn points the game itself used (close points merged), and a roster
of the enemy classes it spawned there, weighted by how often; classes other than the stage's main
Majini are its special enemies ("special": true).
max_alive follows the size of the game's own opening wave (set 0). Only classes the stage spawned
are listed: their models are loaded with the stage (other classes need ROADMAP M3).
Majini-family classes get "profiles": the game's own descriptor fields (weapon, plaga, mask, ...)
with their counts as weights, minus what the adapter's template already holds. Other classes keep
"arms" (weapon ids as surveyed; a list that is only the default 0x70100 is left out).
"chests" are where the stage puts its own time-bonus statues (research/chest_survey.json, from
scripts/survey_chests.sh); a run's arena places a few of them as chests (ROADMAP M4.1).
"""
import argparse
import json
import math
import re

# Mercenaries stage order of the game's stage select. 2 (Village) and 6 (Ship Deck) were
# recognised in play; the others follow the game's menu order.
STAGE_NAMES = {
    0: 'Public Assembly', 1: 'The Mines', 2: 'Village', 3: 'Ancient Ruins',
    4: 'Experimental Facility', 5: 'Missile Area', 6: 'Ship Deck', 7: 'Prison',
}
MERGE_DISTANCE = 150.0   # points closer than this (x/z) are one spawn point
MAIN_SHARE = 0.15        # Majini classes with at least this share of a stage's spawns are its main enemies
# Never in a roster: uEm62 is a chicken (seen in game 2026-11-01).
ALWAYS_EXCLUDED = {'uEm62'}
DEFAULT_ARM = 0x70100
# What the adapter's descriptor template already holds (director.cpp): profile fields with these
# values are left out of maps.json.
TEMPLATE = {'search_type': 1, 'search_forward': 0.0, 'search_circle': 1000.0, 'armed': 1, 'mask': 0xFFFFFFFF}


# Classes the survey saw spawn without catching one of their descriptors, with the fields of the
# game's own placements (its rLayout set data): uEm21, the megaphone Majini (model em2100), wears
# montage 0 in all of them (s100, s114, s400) and spawned headless without it (2026-11-18).
KNOWN_PROFILES = {
    'uEm21': [{'weight': 1, 'fields': {'montage': 0, 'armed': 1, 'attribute': 1048576}}],
}


def compact_profiles(profiles):
    merged = {}
    for p in profiles:
        fields = {k: v for k, v in p['fields'].items() if v != TEMPLATE.get(k, 0)}
        key = json.dumps(fields, sort_keys=True)
        merged[key] = merged.get(key, 0) + p['count']
    return [{'weight': n, 'fields': json.loads(k)} for k, n in sorted(merged.items(), key=lambda kv: -kv[1])]

parser = argparse.ArgumentParser()
parser.add_argument('--exclude', default='', help='comma-separated classes to leave out (uEm62, a chicken, always is)')
exclude = {c for c in parser.parse_args().exclude.split(',') if c} | ALWAYS_EXCLUDED

survey = json.load(open('research/spawn_survey.json', encoding='utf-8'))['stages']
try:
    chest_survey = json.load(open('research/chest_survey.json', encoding='utf-8'))['stages']
except FileNotFoundError:
    chest_survey = {}
maps = []
for stage_text, data in survey.items():
    stage = int(stage_text)
    points = []
    for p in data['points']:
        if p['class'] in exclude:
            continue
        x, y, z = p['pos']
        if any(math.hypot(x - q[0], z - q[2]) < MERGE_DISTANCE for q in points):
            continue
        points.append([x, y, z])
    roster = []
    total = sum(info['count'] for cls, info in data['classes'].items() if cls not in exclude)
    for cls, info in sorted(data['classes'].items(), key=lambda kv: -kv[1]['count']):
        if cls in exclude:
            continue
        entry = {'class': cls, 'weight': info['count']}
        # Everything that is not a main Majini class is the stage's special enemy (the big ones
        # the game keeps one of at a time and respawns when killed): the director brings them in
        # once a quarter of the arena is dead, one at a time (more on harder routes).
        majini = bool(info.get('profiles'))
        if not majini or info['count'] < MAIN_SHARE * total:
            entry['special'] = True
        if info.get('profiles'):
            # The game's own descriptors for this class (weapon, plaga, mask, ...), with its odds.
            entry['profiles'] = compact_profiles(info['profiles'])
        elif info['arms'] and info['arms'] != [DEFAULT_ARM]:
            entry['arms'] = sorted(info['arms'])
        if cls in KNOWN_PROFILES and 'profiles' not in entry:
            entry['profiles'] = KNOWN_PROFILES[cls]
        roster.append(entry)
    if not points or not roster:
        continue
    maps.append({
        'id': f'merc_stage_{stage}',
        'name': STAGE_NAMES.get(stage, f'Stage {stage}'),
        'stage': stage,
        # As many alive as the game's own opening wave (set 0), within reason.
        'max_alive': max(6, min(12, sum(1 for p in data['points'] if p['set'] == 0))),
        'min_player_distance': 800,
        'points': points,
        **({'chests': [c['pos'] for c in chest_survey[stage_text]]} if chest_survey.get(stage_text) else {}),
        'roster': roster,
    })

doc = {
    'schema_version': 1,
    '_comment': 'Director maps (ROADMAP M1). Generated by scripts/write_maps.py from research/spawn_survey.json: '
                'points are where the game itself spawned enemies on that Mercenaries stage; the roster lists '
                'the classes it spawned there, weighted by count. stage = Mercenaries stage index.',
    'maps': maps,
}
with open('configs/maps.json', 'w', encoding='utf-8', newline='\n') as f:
    # One point (and one arms list) per line keeps the file reviewable.
    text = json.dumps(doc, indent=2)
    text = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', text)
    text = re.sub(r'"arms": \[([^\]]*)\]',
                  lambda m: '"arms": [' + ', '.join(x.strip() for x in m.group(1).split(',')) + ']', text)
    # One profile per line.
    text = re.sub(r'\{\s+"weight": (\d+),\s+"fields": \{([^}]*)\}\s+\}',
                  lambda m: '{"weight": ' + m.group(1) + ', "fields": {' + ', '.join(x.strip() for x in m.group(2).split(',') if x.strip()) + '}}', text)
    f.write(text + '\n')
for m in maps:
    print(f"{m['stage']} {m['name']}: {len(m['points'])} points, {len(m.get('chests', []))} chests, roster " +
          ', '.join(f"{r['class']} x{r['weight']}" for r in m['roster']))
