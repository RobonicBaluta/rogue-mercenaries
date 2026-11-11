#!/usr/bin/env python3
"""The run economy as arithmetic: what a run earns in supplies, best and worst case, by run length,
and what a fully built gun costs. Reads configs/ (economy, routes, perks, characters, weapons) and
follows the core's rules (core/runs/run_controller.cpp: kills, arena clears, chests, rewards, perks,
gun affixes). Run from the repository root: python scripts/economy_math.py [--markdown | --check]
(--check: exits 1 when a run length pays for more guns than the user's targets, CHECK_MAX_GUNS; a ctest)

Best case: every route the richest one the run map offers (its higher tier, the bigger budget), every
enemy killed, three chests every arena (keys for all), a supplies reward after every arena that has
one (rerolls), S ratings. Worst case: the safest route, one chest an arena, no supplies reward.
Generators: a build or perk stack on top of the best case (all kills melee where it pays more)."""
import json
import math
import os
import sys

CFG = os.environ.get('RM_ECONOMY_CONFIG', 'configs')  # another config folder (a what-if)


def load(name):
    return json.load(open(f'{CFG}/{name}.json', encoding='utf-8'))


ECO, ROUTES, PERKS, CHARS, WEAPONS = (load(n) for n in ('economy', 'routes', 'perks', 'characters', 'weapons'))
RARITIES = ['common', 'uncommon', 'rare', 'epic', 'legendary', 'exotic']


def rnd(x):
    """RoundAmount: llround, halves away from zero."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def playable():
    return [r for r in ROUTES['routes'] if r.get('enabled', True) and r['enemy_budget'] > 0]


def route_for(arena, best, boss):
    """routes.cpp: layer d (arena d+1) offers tiers[d-1] and tiers[d]; a boss layer the highest tier."""
    rs = playable()
    tiers = sorted({r['difficulty_tier'] for r in rs})
    if arena == 1:
        return next(r for r in rs if r['id'] == ECO['run']['starting_route'])
    if boss:
        tier = tiers[-1]
    else:
        d = arena - 1
        tier = tiers[min(d if best else d - 1, len(tiers) - 1)]
    pool = [r for r in rs if r['difficulty_tier'] == tier]
    return max(pool, key=lambda r: (r['enemy_budget'], r['reward_mult'])) if best else min(pool, key=lambda r: (r['enemy_budget'], r['reward_mult']))


def ladder():
    return ECO['ladder']


def steps(arena):
    lad = ladder()
    return arena - lad['growth_from_arena'] + 1 if lad.get('enabled') and arena >= lad['growth_from_arena'] else 0


def boss_arenas(length_id):
    out = set()
    for l in ladder()['lengths']:
        out.add(l['arenas'])
        if l['id'] == length_id:
            break
    return out


def reward_tier_scale(rating_shift):
    """E[1 + 0.5 * rarity index] of a supplies reward under the shifted rarity weights."""
    w = ECO['rewards']['rarity_weights']
    factor = max(0.1, 1.0 + rating_shift)
    weights = [w.get(r, 0) * factor ** i for i, r in enumerate(RARITIES[:5])]
    total = sum(weights)
    return sum((1 + 0.5 * i) * x for i, x in enumerate(weights)) / total


def perk(pid):
    return next(p for p in PERKS['perks'] if p['id'] == pid)


def build(skin_id):
    for c in CHARS['characters']:
        for s in c['skins']:
            if s['id'] == skin_id:
                return s.get('build', {})
    raise KeyError(skin_id)


def run_income(length_id, best=True, stack=None):
    """Supplies a run of `length_id` earns, by source. `stack`: {'perks': [...], 'skin': id,
    'melee': fraction of kills by melee, 'affix': supplies per kill of a gun used for 'affix_share'
    of the kills, 'no_damage': fraction of clears without damage}."""
    stack = stack or {}
    sup = ECO['supplies']
    arenas = next(l['arenas'] for l in ladder()['lengths'] if l['id'] == length_id)
    bosses = boss_arenas(length_id)
    b = build(stack['skin']) if stack.get('skin') else {}
    supplies_mult = b.get('supplies_mult', 1.0)
    ramp = None  # Excella's supplies stat: cap reached after `full` melee kills, linearly
    if 'supplies' in b.get('melee_kill_caps', {}):
        gains = b.get('melee_kill_gains_by_length', {}).get(length_id, {})
        total_gains = sum(gains.values()) if gains else None
        ramp = (b['melee_kill_caps']['supplies'], total_gains)
    melee = stack.get('melee', 0.0)
    perks = [perk(p) for p in stack.get('perks', [])]
    for p in perks:
        supplies_mult *= p.get('build', {}).get('supplies_mult', 1.0)
    kill_perk = sum(e['amount'] for p in perks for e in p['effects'] if e['kind'] == 'supplies_on_kill' and not e.get('melee_only'))
    melee_perk = sum(e['amount'] for p in perks for e in p['effects'] if e['kind'] == 'supplies_on_kill' and e.get('melee_only'))
    clear_perk = sum(e['amount'] for p in perks for e in p['effects'] if e['kind'] == 'supplies_on_arena_clear' and not e.get('require_no_damage'))
    clean_perk = sum(e['amount'] for p in perks for e in p['effects'] if e['kind'] == 'supplies_on_arena_clear' and e.get('require_no_damage'))
    tier_scale = reward_tier_scale(ECO['rewards']['rating_rarity_bonus']['S'] if best else 0.0)
    out = {'start': ECO['run']['starting_supplies'], 'kills': 0, 'clears': 0, 'chests': 0, 'rewards': 0, 'perks': 0, 'gun affix': 0}
    melee_kills = 0
    for a in range(1, arenas + 1):
        boss = a in bosses
        r = route_for(a, best, boss)
        kills = 1 if boss else r['enemy_budget'] + ladder()['budget_per_arena'] * steps(a)
        stat = 0.0
        if ramp and ramp[1]:
            mid = melee_kills + melee * kills / 2
            stat = ramp[0] * min(1.0, mid / ramp[1])
        mult = supplies_mult * (1.0 + stat)
        # A kill's supplies add up exactly (run_controller.cpp KillShare): an arena pays the whole part.
        out['kills'] += math.floor(kills * sup['per_kill'] * r['reward_mult'] * mult + 1e-9)
        out['perks'] += math.floor(kills * kill_perk * mult + 1e-9) + math.floor(kills * melee * melee_perk * mult + 1e-9)
        out['gun affix'] += math.floor(kills * stack.get('affix_share', 0.0) * stack.get('affix', 0) * mult + 1e-9)
        out['clears'] += rnd(rnd(sup['per_arena_clear'] * r['reward_mult']) * mult)
        out['perks'] += rnd(clear_perk * mult) + (rnd(clean_perk * mult) if best and stack.get('no_damage', 0) >= 1 else 0)
        chests = ECO['chests'].get('per_arena', 3) + b.get('extra_chests', 0) if best else 1
        out['chests'] += chests * rnd((ECO['chests']['supplies_base'] + ECO['chests']['supplies_per_arena'] * (a - 1)) * mult)
        if best and a < arenas:  # no reward after the run's last arena
            out['rewards'] += rnd(ECO['rewards']['supplies_bundle'] * r['reward_mult'] * supplies_mult * tier_scale)
        melee_kills += melee * kills
    out['total'] = sum(out.values())
    return out


def gun_cost():
    """A gun from common to legendary at the top level: bought common, every refine and upgrade bought."""
    shop, refine, up = ECO['shop'], ECO['refine'], ECO['upgrades']
    buy = shop['weapon_price']['common']
    refines = sum(refine['price'][r] for r in RARITIES[1:5])
    upgrades = sum(up['cost_to_next'][: up['max_level'] - 1])
    return buy, refines, upgrades


STACKS = {
    'no generators': {},
    'Josh (Squad Tactics) + Clean Sweep': {'perks': ['josh_squad_tactics', 'clean_sweep'], 'no_damage': 1},
    'Excella Dress, Executive Bonus + Hostile Takeover + Clean Sweep, all melee': {
        'skin': 'excella_dress', 'perks': ['excella_executive_bonus', 'excella_hostile_takeover', 'clean_sweep'], 'melee': 1.0, 'no_damage': 1},
    'Leon DSC, Suplex + Clean Sweep, half melee': {'skin': 'leon_dsc', 'perks': ['leon_suplex', 'clean_sweep'], 'melee': 0.5, 'no_damage': 1},
    'Sheva Clubbin (Night Shopper) + Clean Sweep': {'skin': 'sheva_clubbin', 'perks': ['clean_sweep'], 'no_damage': 1},
    'a legendary +supplies gun for half the kills': {'affix': None, 'affix_share': 0.5},
}


def affix_legendary():
    a = next(x for x in WEAPONS['affixes'] if x['kind'] == 'supplies_on_kill')
    m = ECO['weapon_rarity']['legendary']['magnitude_mult']
    return (a['min'] + a['max']) / 2 * m


# The user's targets (2026-11-09 and 11-11): guns built all the way that a run's best case pays for.
# A short run one (two with luck), a long run two or three (four with the supply perks), a marathon
# four or five. With no supply perk or build, then with the strongest stack; and a marathon's worst
# case still pays for more than one.
CHECK_MAX_GUNS = {'short': (1.5, 1.75), 'long': (3.0, 3.75), 'marathon': (4.5, 5.75)}
CHECK_MIN_WORST_GUNS = {'marathon': 1.5}


def check(rows, full):
    failed = []
    for name, lid, worst, best, _ in rows:
        base_max, stack_max = CHECK_MAX_GUNS.get(lid, (None, None))
        limit = base_max if name == 'no generators' else stack_max
        if limit is not None and best / full > limit:
            failed.append(f'{name} {lid}: best {best} = {best / full:.2f} guns, over {limit}')
        if name == 'no generators' and lid in CHECK_MIN_WORST_GUNS and worst / full < CHECK_MIN_WORST_GUNS[lid]:
            failed.append(f'{lid}: worst {worst} = {worst / full:.2f} guns, under {CHECK_MIN_WORST_GUNS[lid]}')
    for f in failed:
        print('FAIL', f)
    print('economy check:', 'failed' if failed else 'ok')
    return 1 if failed else 0


def main():
    md = '--markdown' in sys.argv
    buy, refines, upgrades = gun_cost()
    full = buy + refines + upgrades
    print(f'A gun built all the way: bought common {buy}, refines to legendary {refines}, levels 1 to '
          f'{ECO["upgrades"]["max_level"]} {upgrades}: {full} supplies.')
    lengths = [l['id'] for l in ladder()['lengths']]
    rows = []
    for name, stack in STACKS.items():
        s = dict(stack)
        if 'affix' in s and s['affix'] is None:
            s['affix'] = affix_legendary()
        for lid in lengths:
            best = run_income(lid, True, s)
            worst = run_income(lid, False, {k: v for k, v in s.items() if k not in ('no_damage',)})
            rows.append((name, lid, worst['total'], best['total'], best))
    if '--check' in sys.argv:
        return check(rows, full)
    if md:
        print('| Stack | Length | Worst | Best | Guns (best) |')
        print('|---|---|---|---|---|')
    for name, lid, worst, best, parts in rows:
        guns = best / full
        if md:
            print(f'| {name} | {lid} | {worst} | {best} | {guns:.1f} |')
        else:
            detail = ', '.join(f'{k} {v}' for k, v in parts.items() if k != 'total' and v)
            print(f'{name:<78} {lid:<9} worst {worst:>5}  best {best:>5}  ({guns:.1f} guns)  [{detail}]')


if __name__ == '__main__':
    sys.exit(main())
