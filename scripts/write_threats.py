#!/usr/bin/env python3
"""Writes configs/economy.json "enemy_threat" (ROADMAP M3.4): how dangerous each enemy weapon and
class is, and how an arena's threat level grows through a run.

  python scripts/write_threats.py

A spawn's threat is the higher of its class's and its weapon's (1 a Majini with a blade or bare
hands .. 4 machine guns, rockets, gatlings, the chainsaw). The first arena of a run is level 1;
later arenas are their route's tier plus one. The director fields only spawns at or under the
arena's level, keeps fewer alive at once on low levels, and lets the stage's special enemies in
from specials_from_level on; the run map puts a map on a node only when the map can field
enemies at that node's level.

Weapons: an enemy weapon is the descriptor's arm 0x701NN, the class uWpNN. Its archive
(Image\\Archive\\uWpNN.arc, read only) names the hit effect it plays (effect\\adh\\wp\\wp_xxxx),
which says what it is; a few have none and are named here from their model textures.
"""
import json
import os
import struct

ARCHIVES = 'G:/SteamLibrary/steamapps/common/Resident Evil 5/nativePC_MT/Image/Archive'
SEP = chr(92)

# Hit effect -> threat level.
EFFECT_LEVEL = {
    'wp_blds': 1,  # small blades and clubs (machete, sickle, hatchet, barbed-wire club)
    'wp_bldl': 2,  # big blades (axes, scythes)
    'wp_firl': 2,  # torches
    'wp_firs': 2,  # small fire
    'wp_hand': 2,  # bows, crossbows, handguns
    'wp_expl': 3,  # dynamite and other thrown explosives
    'wp_elec': 3,  # stun rods
    'wp_shot': 3,  # shotguns
    'wp_mach': 4,  # machine guns (AK)
    'wp_rckt': 4,  # rocket launchers (RPG)
    'wp_gtlg': 4,  # gatlings
}
# Weapons whose effect does not tell the story (by their model and texture names).
WEAPON_OVERRIDES = {
    0x5D: 4,  # the Gatling Majini's (uEm23) gun: wp_shot hits, but the player's Gatling model
    0x78: 2,  # a rock (iwa) to throw
    0x79: 3,  # a utility pole (dentyu) swung by a big Majini
    0x7A: 3,  # a red oil drum (explodes)
}
NONE_LEVEL = 1  # no weapon (arm 0x70100 or none)
DEFAULT_LEVEL = 2
# Classes, from their action classes in the DTI tree and the Mercenaries surveys.
CLASSES = {
    'uEm10': 1, 'uEm11': 1, 'uEm12': 1,  # Majini
    'uEm62': 1,                          # the chicken (Egg, Escape)
    'uEm13': 2, 'uEm14': 2,              # Majini variants (uEm14: the base soldiers)
    'uEm0b': 2,                          # a grabbing creature (AtkCatch)
    'uEm20': 3, 'uEm26': 3,              # Executioner Majini (AtkExecCatch) and its variant
    'uEm21': 3, 'uEm24': 3,              # big Majini variants, specials of their stages
    'uEm32': 3,                          # Licker
    'uEm34': 3,                          # Reaper-like (core, crawl)
    'uEm22': 4,                          # Chainsaw Majini (AtkCut, BreakDoor)
    'uEm23': 4,                          # Gatling Majini
}


def weapon_levels():
    files = {f.lower(): f for f in os.listdir(ARCHIVES)}
    out = {}
    for n in range(0x50, 0x100):
        key = f'uwp{n:02x}.arc'
        if key not in files:
            continue
        data = open(os.path.join(ARCHIVES, files[key]), 'rb').read()
        count = struct.unpack('<H', data[6:8])[0]
        off = 8
        effects = set()
        for _ in range(count):
            name = data[off:off + 64].split(b'\0')[0].decode('latin1')
            off += 80
            if SEP + 'adh' + SEP in name:
                effects.add(name.split(SEP)[-1])
        levels = [EFFECT_LEVEL[e] for e in effects if e in EFFECT_LEVEL]
        level = WEAPON_OVERRIDES.get(n, max(levels) if levels else None)
        if level is not None:
            out[f'{n:02x}'] = level
    return out


def main():
    weapons = {'none': NONE_LEVEL}
    weapons.update(weapon_levels())
    e = json.load(open('configs/economy.json', encoding='utf-8'))
    e['enemy_threat'] = {
        '_comment': ('Arena threat (ROADMAP M3.4, written by scripts/write_threats.py). A spawn\'s threat is the higher of its '
                     'class\'s and its weapon\'s (weapons by uWp class, the arm id 0x701NN\'s NN; none = no weapon). The first '
                     'arena is level 1, later arenas their route\'s tier plus later_arena_bonus, up to max_level. The director '
                     'fields only spawns at or under the level (a map without any falls back to its mildest), keeps at most '
                     'max_alive_by_level of them alive, and lets specials in from specials_from_level; the run map puts a map '
                     'on a node only when it can field enemies at that node\'s level.'),
        'enabled': True,
        'later_arena_bonus': 1,
        'max_level': 4,
        'max_alive_by_level': [6, 8, 10, 12],
        'specials_from_level': 3,
        'default': DEFAULT_LEVEL,
        'weapons': weapons,
        'classes': CLASSES,
    }
    with open('configs/economy.json', 'w', encoding='utf-8', newline='\n') as f:
        json.dump(e, f, indent=2)
        f.write('\n')
    by = {}
    for k, v in weapons.items():
        by.setdefault(v, []).append(k)
    for level in sorted(by):
        print(f'level {level}: weapons {" ".join(sorted(by[level]))}')
    print(f'{len(CLASSES)} classes')


if __name__ == '__main__':
    main()
