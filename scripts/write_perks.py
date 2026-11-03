"""Rewrites configs/perks.json in the style / supply / support / skin scheme and aligns
characters.json, loadouts.json and weapons.json with it. Run from the repository root (python scripts/write_perks.py)."""
import json

HAND, INCENDIARY, FLASH, SPRAY, HANDGUN_AMMO = 0x0106, 0x0107, 0x0108, 0x0304, 0x0201
M92F, ITHACA, MP5, LHAWK, SHELLS, MG_AMMO, MAGNUM_AMMO = 0x0102, 0x0104, 0x0113, 0x011A, 0x0203, 0x0202, 0x0209
VZ61, M500, M29, PSG1, DRAGUNOV, RIFLE_AMMO = 0x0103, 0x011B, 0x010B, 0x011C, 0x0120, 0x0204


def perk(pid, name, desc, category, effects, characters=None, cost=0, rarity='common', tags=None, enabled=True):
    p = {'id': pid, 'name': name, 'description': desc, 'category': category, 'rarity': rarity,
         'tags': tags or [], 'max_stacks': 1, 'effects': effects}
    if characters:
        p['characters'] = characters
    if cost:
        p['unlock_cost_credits'] = cost
    if not enabled:
        p['enabled'] = False
    return p


def item_start(game_item, amount=1):
    return {'kind': 'item_on_start', 'game_item': game_item, 'amount': amount}


perks = []
# ---- Style: the character's way of playing (one free trait each, more to unlock) ----
perks += [
    perk('chris_veteran', 'B.S.A.A. Veteran', 'Chris. Clearing an arena restocks a first aid spray.', 'style',
         [{'kind': 'item_on_arena_clear', 'game_item': SPRAY, 'amount': 1}], ['chris'], tags=['items']),
    perk('chris_pocket_grenades', 'Pocket Grenades', 'Chris. Every 4 kills put a hand grenade in your pocket.', 'style',
         [{'kind': 'item_on_kill', 'game_item': HAND, 'amount': 1, 'every_kills': 4}], ['chris'], cost=120, rarity='rare', tags=['items']),
    perk('sheva_field_training', 'Field Training', 'Sheva. Each kill heals 2 percent of max health.', 'style',
         [{'kind': 'heal_on_kill', 'amount_fraction_of_max': 0.02}], ['sheva'], tags=['heal']),
    perk('sheva_flash_tactics', 'Flash Tactics', 'Sheva. Every 3 kills hand you a flash grenade.', 'style',
         [{'kind': 'item_on_kill', 'game_item': FLASH, 'amount': 1, 'every_kills': 3}], ['sheva'], cost=120, rarity='rare', tags=['items']),
    perk('jill_battle_instinct', 'Battle Instinct', 'Jill. 15 percent of hits are critical and deal double damage.', 'style',
         [{'kind': 'crit', 'chance': 0.15, 'multiplier': 2.0}], ['jill'], tags=['crit']),
    perk('wesker_firestarter', 'Firestarter', 'Wesker. Every 3 kills hand you an incendiary grenade.', 'style',
         [{'kind': 'item_on_kill', 'game_item': INCENDIARY, 'amount': 1, 'every_kills': 3}], ['wesker'], tags=['items']),
    perk('josh_squad_tactics', 'Squad Tactics', 'Josh. Every kill gives 2 extra supplies.', 'style',
         [{'kind': 'supplies_on_kill', 'amount': 2, 'chance': 1.0}], ['josh'], tags=['supplies']),
    perk('barry_demolition_man', 'Demolition Man', 'Barry. Every 5 kills hand you a grenade, and an arena clear two more.', 'style',
         [{'kind': 'item_on_kill', 'game_item': HAND, 'amount': 1, 'every_kills': 5},
          {'kind': 'item_on_arena_clear', 'game_item': HAND, 'amount': 2}], ['barry'], tags=['items']),
    perk('rebecca_combat_medic', 'Combat Medic', 'Rebecca. Each kill heals 2 percent of max health, an arena clear restocks a spray.', 'style',
         [{'kind': 'heal_on_kill', 'amount_fraction_of_max': 0.02},
          {'kind': 'item_on_arena_clear', 'game_item': SPRAY, 'amount': 1}], ['rebecca'], tags=['heal', 'items']),
    perk('excella_executive_bonus', 'Executive Bonus', 'Excella. 25 extra supplies for every arena cleared.', 'style',
         [{'kind': 'supplies_on_arena_clear', 'amount': 25, 'require_no_damage': False}], ['excella'], tags=['supplies']),
]
# ---- Supply: what you start with. Generic ones for everyone, one per character. ----
perks += [
    perk('supply_grenades', 'Grenade Pouch', 'Start every run with 2 hand grenades.', 'supply', [item_start(HAND, 2)], cost=60, tags=['items']),
    perk('supply_flash', 'Flash Pouch', 'Start every run with 2 flash grenades.', 'supply', [item_start(FLASH, 2)], cost=50, tags=['items']),
    perk('supply_incendiary', 'Fire Pouch', 'Start every run with 2 incendiary grenades.', 'supply', [item_start(INCENDIARY, 2)], cost=70, tags=['items']),
    perk('supply_spray', 'Medkit', 'Start every run with a first aid spray.', 'supply', [item_start(SPRAY, 1)], cost=80, tags=['items']),
    perk('supply_ammo', 'Ammo Belt', 'Start every run with 30 extra handgun rounds.', 'supply', [item_start(HANDGUN_AMMO, 30)], cost=40, tags=['items']),
    perk('chris_shotgun', 'Shotgun Licence', 'Chris starts with an Ithaca M37 and 12 shells.', 'supply', [item_start(ITHACA), item_start(SHELLS, 12)], ['chris'], cost=150, rarity='rare', tags=['weapon']),
    perk('sheva_smg', 'Close Support', 'Sheva starts with a VZ61 and 60 rounds.', 'supply', [item_start(VZ61), item_start(MG_AMMO, 60)], ['sheva'], cost=150, rarity='rare', tags=['weapon']),
    perk('jill_smg', 'Rapid Response', 'Jill starts with an MP5 and 60 rounds.', 'supply', [item_start(MP5), item_start(MG_AMMO, 60)], ['jill'], cost=150, rarity='rare', tags=['weapon']),
    perk('wesker_magnum', 'Hand Cannon', 'Wesker starts with an M500 and 6 rounds.', 'supply', [item_start(M500), item_start(MAGNUM_AMMO, 6)], ['wesker'], cost=200, rarity='epic', tags=['weapon']),
    perk('josh_magnum', 'Big Iron', 'Josh starts with an M29 and 6 rounds.', 'supply', [item_start(M29), item_start(MAGNUM_AMMO, 6)], ['josh'], cost=150, rarity='rare', tags=['weapon']),
    perk('barry_smg', 'Old Reliable', 'Barry starts with an MP5 and 60 rounds.', 'supply', [item_start(MP5), item_start(MG_AMMO, 60)], ['barry'], cost=150, rarity='rare', tags=['weapon']),
    perk('rebecca_rifle', 'Rookie Marksman', 'Rebecca starts with a PSG-1 and 10 rounds.', 'supply', [item_start(PSG1), item_start(RIFLE_AMMO, 10)], ['rebecca'], cost=150, rarity='rare', tags=['weapon']),
    perk('excella_rifle', 'Private Security', 'Excella starts with a Dragunov and 10 rounds.', 'supply', [item_start(DRAGUNOV), item_start(RIFLE_AMMO, 10)], ['excella'], cost=150, rarity='rare', tags=['weapon']),
]
# ---- Support: bad situations. ----
perks += [
    perk('second_wind', 'Second Wind', 'Once per arena, survive a lethal hit with 25 percent health.', 'support',
         [{'kind': 'cheat_death', 'per_arena': 1, 'leave_hp_fraction': 0.25}], cost=250, rarity='epic', tags=['survival']),
    perk('last_stand', 'Last Stand', 'Below 30 percent health your shots deal 35 percent more damage.', 'support',
         [{'kind': 'outgoing_damage_mult', 'value': 0.35, 'condition': {'type': 'player_hp_below', 'fraction': 0.30}}], cost=120, rarity='rare', tags=['damage']),
    perk('adrenaline_shield', 'Adrenaline Shield', 'Below 35 percent health you take 30 percent less damage.', 'support',
         [{'kind': 'incoming_damage_mult', 'value': -0.30, 'condition': {'type': 'player_hp_below', 'fraction': 0.35}}], cost=120, rarity='rare', tags=['defense']),
    perk('field_medic', 'Field Medic', 'Each kill heals 2 percent of max health.', 'support',
         [{'kind': 'heal_on_kill', 'amount_fraction_of_max': 0.02}], cost=100, rarity='uncommon', tags=['heal']),
    perk('clean_sweep', 'Clean Sweep', '40 extra supplies for clearing an arena without taking damage.', 'support',
         [{'kind': 'supplies_on_arena_clear', 'amount': 40, 'require_no_damage': True}], cost=90, rarity='uncommon', tags=['supplies']),
]
# ---- Skin traits: a skin's perk (granted by the skin, never listed). Skins with a build (BUILDS below,
# ROADMAP M8) lose their old small trait; the remaining ones go as their builds land. ----
skin_traits = {
    'chris_stars_trait': ('S.T.A.R.S. Restock', 'S.T.A.R.S. Chris restocks a first aid spray on every arena clear.',
                          [{'kind': 'item_on_arena_clear', 'game_item': 0x0304, 'amount': 1}]),
    'sheva_tribal_trait': ('Quiver', 'Tribal Sheva starts with 20 arrows, finds 10 more on every arena clear, and each kill heals 1.5 percent.',
                           [{'kind': 'item_on_start', 'game_item': 0x020B, 'amount': 20},
                            {'kind': 'item_on_arena_clear', 'game_item': 0x020B, 'amount': 10},
                            {'kind': 'heal_on_kill', 'amount_fraction_of_max': 0.015}]),
}
# ---- Skin builds (characters.json skin "build", core/include/rm/characters.h SkinBuild). ----
BUILDS = {
    'chris_safari': {
        'name': 'Mine Layer', 'icon': 'mine',
        'summary': 'Every chest and every enemy drop is a proximity bomb, and they stack up to 10. 25 percent more health.',
        'drops_as': '0x010A', 'stack_max': {'0x010A': 10}, 'max_health_mult': 1.25,
    },
    'chris_stars': {
        'name': 'Field Kit', 'icon': 'medkit',
        'summary': 'A first aid spray on every arena clear.',
    },
    'chris_heavy_metal': {
        'name': 'Heavy Gunner', 'icon': 'gatling',
        'summary': 'Can unlock the Gatling gun in CREDITS and then find it in his runs. Ammo stacks three times as high.',
        'extra_weapons': ['gatling'], 'ammo_stack_mult': 3,
    },
    'sheva_tribal': {
        'name': 'Hunter', 'icon': 'bow',
        'summary': 'Starts with her longbow and a quiver of arrows. Arrows hit 50 percent harder.',
        'starting_weapon': 'longbow', 'extra_weapons': ['longbow'], 'category_damage': {'bow': 0.5},
    },
    'jill_bsaa': {
        'name': 'Locksmith', 'icon': 'key',
        'summary': 'Opens every chest without a key, and finds three more chests in every arena.',
        'chest_keys_exempt': True, 'extra_chests': 3,
    },
    'jill_battle_suit': {
        'name': 'Twin Fury', 'icon': 'dual_smg',
        'summary': 'The special button unloads two submachine guns on everything in front of her. 25 seconds to recover.',
        'special': {'kind': 'dual_smg', 'name': 'Twin Fury', 'icon': 'dual_smg', 'cooldown_seconds': 25,
                    'power': 90, 'duration_seconds': 1.5},
    },
    'wesker_midnight': {
        'name': 'Uroboros Speed', 'icon': 'dash',
        'summary': 'Dash, or lunge at an enemy ahead to finish it in melee; a second button bursts. 70 percent health.',
        'max_health_mult': 0.7,
        'special': {'kind': 'dash', 'name': 'Dash', 'icon': 'dash', 'cooldown_seconds': 3.5, 'power': 5,
                    'duration_seconds': 0.5},
        'special_2': {'kind': 'burst', 'name': 'Burst', 'icon': 'shockwave', 'cooldown_seconds': 7, 'power': 350,
                      'duration_seconds': 0.4},
    },
    'rebecca_stars': {
        'name': 'Field Surgeon', 'icon': 'heal',
        'summary': 'Stronger heals. A spray or mixed herb gives 4 seconds untouchable and 12 seconds of 30 percent more damage.',
        'heal_mult': 1.3, 'heal_buff_fraction': 0.6, 'heal_invulnerable_seconds': 4, 'heal_damage_bonus': 0.3,
        'heal_damage_seconds': 12,
    },
    'josh_bsaa': {
        'name': 'Call for Backup', 'icon': 'radio',
        'summary': 'The special button calls in Sheva to fight by your side for the arena, once every two arenas.',
        'special': {'kind': 'backup', 'name': 'Backup', 'icon': 'radio', 'every_arenas': 2},
    },
    'excella_dress': {
        'name': 'Ambition', 'icon': 'crown',
        'summary': 'Every melee kill raises one of seven stats for the run, each to a cap: damage, speed, healing and more.',
        'melee_kill_stats': {'damage': 0.04, 'defense': 0.03, 'max_health': 0.05, 'crit': 0.02, 'move_speed': 0.03,
                             'heal': 0.05, 'supplies': 0.04},
        'melee_kill_caps': {'damage': 1.0, 'defense': 0.7, 'max_health': 1.0, 'crit': 0.3, 'move_speed': 0.6,
                            'heal': 0.5, 'supplies': 0.5},
    },
    'chris_warrior': {
        'name': 'Brute Force', 'icon': 'shotgun',
        'summary': 'Only shotguns, handguns and magnums come his way, and they hit 25 percent harder.',
        'offered_weapons': {'categories': ['shotgun', 'handgun', 'magnum']},
        'category_damage': {'shotgun': 0.25, 'handgun': 0.25, 'magnum': 0.25},
    },
    'sheva_clubbin': {
        'name': 'Night Shopper', 'icon': 'supplies',
        'summary': 'Half again as many supplies, and one free shop reroll at every shop.',
        'supplies_mult': 1.5, 'free_shop_rerolls': 1,
    },
    'sheva_business': {
        'name': 'Expense Account', 'icon': 'credits',
        'summary': 'Better loot and shop stock. Submachine guns hit 30 percent harder and upgrade without limit.',
        'rarity_shift': 0.8, 'category_damage': {'smg': 0.3}, 'uncapped_upgrades': {'categories': ['smg']},
    },
    'sheva_fairy_tale': {
        'name': 'Enchanted Arms', 'icon': 'star',
        'summary': 'Starts with a legendary M3 that upgrades without limit.',
        'starting_weapon': 'm3', 'starting_weapon_rarity': 'legendary', 'uncapped_upgrades': {'ids': ['m3']},
    },
    'wesker_stars': {
        'name': 'Iron Fist', 'icon': 'fist',
        'summary': 'His melee hits three times as hard. Only his Samurai Edge, magnums and the Hydra are for him.',
        'usable_weapons': {'categories': ['magnum'], 'ids': ['samurai_edge', 'hydra']}, 'melee_damage': 2.0,
    },
    'barry_stars': {
        'name': 'Old Reliable', 'icon': 'magnum',
        'summary': 'Starts with an S and W M29 that upgrades without limit, and half again as much health.',
        'starting_weapon': 'm29', 'uncapped_upgrades': {'ids': ['m29']}, 'max_health_mult': 1.5,
    },
}
for pid, (name, desc, effects) in skin_traits.items():
    perks.append(perk(pid, name, desc, 'skin', effects, tags=['skin']))
# ---- Curses (route modifiers) ----
perks += [
    perk('curse_frailty', 'Frailty', 'Curse. You take 20 percent more damage.', 'curse', [{'kind': 'incoming_damage_mult', 'value': 0.20}], tags=['curse']),
    perk('curse_dull_blades', 'Dull Blades', 'Curse. Your shots deal 15 percent less damage.', 'curse', [{'kind': 'outgoing_damage_mult', 'value': -0.15}], tags=['curse']),
]

# Icons the menus show next to each listed perk (configs/icons/<icon>.png, drawn by scripts/make_icons.py).
ICONS = {
    'chris_veteran': 'medal', 'chris_pocket_grenades': 'grenade', 'sheva_field_training': 'heal',
    'sheva_flash_tactics': 'flash', 'jill_battle_instinct': 'crosshair', 'wesker_firestarter': 'fire',
    'josh_squad_tactics': 'supplies', 'barry_demolition_man': 'explosion', 'rebecca_combat_medic': 'medkit',
    'excella_executive_bonus': 'supplies', 'supply_grenades': 'grenade', 'supply_flash': 'flash',
    'supply_incendiary': 'fire', 'supply_spray': 'medkit', 'supply_ammo': 'ammo', 'chris_shotgun': 'shotgun',
    'sheva_smg': 'smg', 'jill_smg': 'smg', 'wesker_magnum': 'magnum', 'josh_magnum': 'magnum', 'barry_smg': 'smg',
    'rebecca_rifle': 'rifle', 'excella_rifle': 'rifle', 'second_wind': 'second_wind', 'last_stand': 'last_stand',
    'adrenaline_shield': 'shield', 'field_medic': 'heal', 'clean_sweep': 'supplies',
}
for p in perks:
    if p['id'] in ICONS:
        p['icon'] = ICONS[p['id']]

doc = {
    '_comment': ("Perks are per character and come only from the mod menu. category: style = the character's way of playing "
                 "(no plain stat perks), supply = what you start with (item_on_start puts game_item into the inventory at the first "
                 "arena; generic ones plus one per character), support = bad situations (low health, lethal hits), skin = a perk a "
                 "skin grants through its starting_passive (never listed; most skins are builds, see characters.json), curse = route modifiers. Not yet "
                 "possible (engine work pending): grab immunity, move speed (damage by weapon type is a skin build rule). Item ids: 0x0106 hand grenade, "
                 "0x0107 incendiary, 0x0108 flash, 0x0304 first aid spray, 0x0201 handgun ammo, 0x0102 M92F, 0x0104 Ithaca M37, "
                 "0x0113 MP5, 0x011A L.Hawk."),
    'schema_version': 1,
    'perks': perks,
}
json.dump(doc, open('configs/perks.json', 'w'), indent=2)
open('configs/perks.json', 'a').write('\n')

# characters.json: a pistol first for everyone, skins carry only their trait, skill pools list the character's perks.
c = json.load(open('configs/characters.json'))
pools = {
    'chris': ['chris_pocket_grenades', 'chris_shotgun'], 'sheva': ['sheva_flash_tactics', 'sheva_smg'],
    'jill': ['jill_smg'], 'wesker': ['wesker_magnum'], 'josh': ['josh_magnum'], 'barry': ['barry_smg'],
    'rebecca': ['rebecca_rifle'], 'excella': ['excella_rifle'],
}
passives = {'chris': 'chris_veteran', 'sheva': 'sheva_field_training', 'jill': 'jill_battle_instinct', 'wesker': 'wesker_firestarter',
            'josh': 'josh_squad_tactics', 'barry': 'barry_demolition_man', 'rebecca': 'rebecca_combat_medic', 'excella': 'excella_executive_bonus'}


def help_lines(text, width=54):
    # The native menu's help line (adapters/re5/src/native_menu.cpp Wrap): two lines of 54.
    lines, line = [], ''
    for word in text.split(' '):
        if line and len(line) + 1 + len(word) > width:
            lines.append(line)
            line = ''
        line = (line + ' ' if line else '') + word
    return lines + ([line] if line else [])


for key, build in BUILDS.items():
    assert len(help_lines(build['summary'])) <= 2, (key, help_lines(build['summary']))

for ch in c['characters']:
    ch['starting_passive'] = passives[ch['id']]
    ch['skill_pool'] = pools[ch['id']]
    for s in ch.get('skins', []):
        trait = s['id'] + '_trait'
        if trait in skin_traits:
            s['starting_passive'] = trait
        else:
            s.pop('starting_passive', None)
        if s['id'] in BUILDS:
            s['build'] = BUILDS[s['id']]
        else:
            s.pop('build', None)
c['_comment'] = ("Playable characters. Every character and skin is available from the start (the challenge fields unlock_kind/unlock_count/"
                 "unlock_text/unlock_character, see core/include/rm/characters.h, are kept for later). starting_weapons: the first entry is "
                 "the pistol every run starts with (the adapter strips the game's own Mercenaries loadout down to it); other weapons come "
                 "from supply perks, rewards and the shop. starting_passive is the character's free style trait; skill_pool lists the perks "
                 "the character can unlock. A skin is a build (ROADMAP M8, written by scripts/write_perks.py): 'build' holds its rules "
                 "(weapon filters, damage by weapon category, supplies, free shop rerolls, rarity, uncapped upgrades, a starting weapon, "
                 "max health), and a skin without one yet keeps a small trait (a 'skin' perk). costume_index is the class and the model each "
                 "slot loads, checked in game: 0..2 and 10 Heavy Metal, 14 Warrior Chris; 3 and 11 Fairy Tale, 15 Business Sheva; 9 Wesker "
                 "S.T.A.R.S.; 12 Josh, 13 Excella, 16 Barry, 17 Rebecca. Text shown by the game font: letters, digits, space, comma and "
                 "period only.")
json.dump(c, open('configs/characters.json', 'w'), indent=2)
open('configs/characters.json', 'a').write('\n')

# loadouts.json: no roll (pistol only); the equipment list stays the shop/reward catalog.
l = json.load(open('configs/loadouts.json'))
l['budget'] = 0
CATALOG_EXTRA = [
    {'id': 'proximity_bomb', 'name': 'Proximity Bomb', 'cost': 15, 'max': 2, 'game_item': 0x010A, 'price': 20, 'bundle': 2},
    {'id': 'arrows', 'name': 'Arrows', 'cost': 5, 'max': 1, 'game_item': 0x020B, 'price': 0, 'bundle': 10},
]
for extra in CATALOG_EXTRA:
    if not any(e['id'] == extra['id'] for e in l['equipment']):
        l['equipment'].append(extra)
l['_comment'] = ("Item catalog for the shop and the rewards (price in supplies, bundle = units per reward) and the starting loadout roll. "
                 "budget is 0: every run starts with the character's pistol only (supply perks add items and weapons through "
                 "item_on_start); set a budget to roll equipment from this list again (max_items caps distinct entries, the RE5 "
                 "inventory has nine slots). game_item is the RE5 inventory id ((category << 8) | index).")
json.dump(l, open('configs/loadouts.json', 'w'), indent=2)
open('configs/loadouts.json', 'a').write('\n')

# weapons.json: the game item id where known (the adapter creates/keeps the weapon in the inventory).
w = json.load(open('configs/weapons.json'))
ids = {'m92f': 0x0102, 'vz61': 0x0103, 'ithaca_m37': 0x0104, 's75': 0x0105, 'sig556': 0x0109, 'm29': 0x010B,
       'h_and_k_p8': 0x0110, 'sig_p226': 0x0111, 'h_and_k_mp5': 0x0113, 'm3': 0x0116, 'jail_breaker': 0x0117,
       'hydra': 0x0119, 'l_hawk': 0x011A, 'm500': 0x011B, 'psg1': 0x011C, 'ak74': 0x011D, 'px4': 0x011F,
       'dragunov': 0x0120, 'samurai_edge': 0x0129, 'gatling': 0x0115, 'longbow': 0x010F}
WEAPON_EXTRA = [
    {'id': 'gatling', 'name': 'Gatling Gun', 'category': 'machine_gun', 'base_damage_mult': 1.0, 'unlock_cost_credits': 600,
     'build_only': True},
    {'id': 'longbow', 'name': 'Longbow', 'category': 'bow', 'base_damage_mult': 1.0, 'build_only': True},
]
for extra in WEAPON_EXTRA:
    if not any(wd['id'] == extra['id'] for wd in w['weapons']):
        w['weapons'].append(extra)
for wd in w['weapons']:
    if wd['id'] in ids:
        wd['game_item'] = ids[wd['id']]
json.dump(w, open('configs/weapons.json', 'w'), indent=2)
open('configs/weapons.json', 'a').write('\n')
# economy.json: chest keys (M8; Jill BSAA's build opens chests without them).
e = json.load(open('configs/economy.json'))
e['keys'] = {'enabled': True, 'drop_chance': 0.15, 'classes': ['uEm1'], 'start': 0, 'max': 9}
json.dump(e, open('configs/economy.json', 'w'), indent=2)
open('configs/economy.json', 'a').write('\n')
print('perks', len(perks), 'written; characters, loadouts, weapons, economy aligned')
