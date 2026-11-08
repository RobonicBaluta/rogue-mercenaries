"""Rewrites configs/perks.json in the style / supply / support / skin scheme and aligns
characters.json, loadouts.json and weapons.json with it. Run from the repository root (python scripts/write_perks.py)."""
import json

HAND, INCENDIARY, FLASH, SPRAY, HANDGUN_AMMO = 0x0106, 0x0107, 0x0108, 0x0304, 0x0201
M92F, ITHACA, MP5, LHAWK, SHELLS, MG_AMMO, MAGNUM_AMMO = 0x0102, 0x0104, 0x0113, 0x011A, 0x0203, 0x0202, 0x0209
VZ61, M500, M29, PSG1, DRAGUNOV, RIFLE_AMMO = 0x0103, 0x011B, 0x010B, 0x011C, 0x0120, 0x0204
M93R = 0x011E


def perk(pid, name, desc, category, effects, characters=None, cost=0, rarity='common', tags=None, enabled=True, build=None):
    p = {'id': pid, 'name': name, 'description': desc, 'category': category, 'rarity': rarity,
         'tags': tags or [], 'max_stacks': 1, 'effects': effects}
    if characters:
        p['characters'] = characters
    if cost:
        p['unlock_cost_credits'] = cost
    if not enabled:
        p['enabled'] = False
    if build:
        p['build'] = build
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
    perk('wesker_firestarter', 'Firestarter', 'Wesker. Every 5 kills hand you an incendiary grenade.', 'style',
         [{'kind': 'item_on_kill', 'game_item': INCENDIARY, 'amount': 1, 'every_kills': 5}], ['wesker'], tags=['items']),
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
    # Leon S. Kennedy (a mod character, 2026-11-08: Leon Definitive Edition): RE4's shoot, kick, shoot.
    perk('leon_roundhouse', 'Roundhouse', 'Leon. For 4 seconds after a melee hit his shots deal 40 percent more.', 'style',
         [{'kind': 'outgoing_damage_mult', 'value': 0.4, 'condition': {'type': 'after_melee', 'window_seconds': 4}}], ['leon'],
         tags=['damage']),
]
# Two more per character (2026-11-04, the user's go): each a rule with its cost, made of what the
# builds already do in game ('build' adds into the run's build) or the perk effects.
GUNS = ['handgun', 'shotgun', 'smg', 'machine_gun', 'rifle', 'magnum', 'bow', 'launcher', 'flamethrower']
HANDGUN_AMMO_ITEM, MG_AMMO_ITEM, RIFLE_AMMO_ITEM, MAGNUM_AMMO_ITEM = 0x0201, 0x0202, 0x0204, 0x0209
PROXIMITY, GREEN_HERB_GROUND, GREEN_HERB, MIXED_HERB = 0x010A, 0x0301, 0x0305, 0x0309
perks += [
    perk('chris_boulder_puncher', 'Boulder Puncher', 'Chris. Melee hits deal two and a half times the damage, handguns 25 percent less.', 'style',
         [], ['chris'], cost=150, rarity='rare', tags=['melee'], build={'melee_damage': 1.5, 'category_damage': {'handgun': -0.25}}),
    perk('chris_iron_grip', 'Iron Grip', 'Chris. Throws off a Majini that grabs him at once, but every heal is 25 percent smaller.', 'style',
         [], ['chris'], cost=200, rarity='epic', tags=['survival'], build={'break_grabs': True, 'heal_mult': 0.75}),
    perk('sheva_scavenger', 'Scavenger', 'Sheva. Ammo and herbs she picks up become ammo for the gun in her hands.', 'style',
         [], ['sheva'], cost=150, rarity='rare', tags=['items'], build={'drops_as_held_ammo': True}),
    perk('sheva_sharpshooter', 'Sharpshooter', 'Sheva. Rifles hit 35 percent harder, rifle rounds stack twice as high. Machine guns 20 percent less.', 'style',
         [], ['sheva'], cost=150, rarity='rare', tags=['weapon'],
         build={'category_damage': {'rifle': 0.35, 'smg': -0.2, 'machine_gun': -0.2}, 'stack_mult': {hex(RIFLE_AMMO_ITEM): 2}}),
    perk('jill_weapon_expert', 'Weapon Expert', 'Jill. Handguns hit 30 percent harder, handgun rounds stack twice as high. Shotguns 20 percent less.', 'style',
         [], ['jill'], cost=150, rarity='rare', tags=['weapon'],
         build={'category_damage': {'handgun': 0.3, 'shotgun': -0.2}, 'stack_mult': {hex(HANDGUN_AMMO_ITEM): 2}}),
    perk('jill_finisher', 'Finisher', 'Jill. Shots deal 60 percent more to enemies under a third of their health, 10 percent less otherwise.', 'style',
         [{'kind': 'outgoing_damage_mult', 'value': 0.7, 'condition': {'type': 'target_hp_below', 'fraction': 0.34}},
          {'kind': 'outgoing_damage_mult', 'value': -0.1}], ['jill'], cost=150, rarity='rare', tags=['damage']),
    perk('wesker_superior_being', 'Superior Being', 'Wesker. He takes 30 percent less damage, but heals are half as strong.', 'style',
         [{'kind': 'incoming_damage_mult', 'value': -0.3}], ['wesker'], cost=200, rarity='epic', tags=['defense'], build={'heal_mult': 0.5}),
    perk('wesker_predator', 'Predator', 'Wesker. Each kill within 5 seconds of the last adds 10 percent damage, up to 50. 85 percent health.', 'style',
         [{'kind': 'outgoing_damage_mult', 'value': 0.0,
           'condition': {'type': 'kill_streak', 'window_seconds': 5, 'per_kill': 0.1, 'max_bonus': 0.5}}],
         ['wesker'], cost=180, rarity='rare', tags=['damage'], build={'max_health_mult': 0.85}),
    perk('josh_field_commander', 'Field Commander', 'Josh. Every arena clear hands him a hand grenade and a flash grenade. 20 percent fewer supplies.', 'style',
         [{'kind': 'item_on_arena_clear', 'game_item': HAND, 'amount': 1}, {'kind': 'item_on_arena_clear', 'game_item': FLASH, 'amount': 1}],
         ['josh'], cost=150, rarity='rare', tags=['items'], build={'supplies_mult': 0.8}),
    perk('josh_point_man', 'Point Man', 'Josh. Machine guns hit 25 percent harder, their rounds stack 50 percent higher. No magnums offered.', 'style',
         [], ['josh'], cost=150, rarity='rare', tags=['weapon'],
         build={'category_damage': {'smg': 0.25, 'machine_gun': 0.25}, 'stack_mult': {hex(MG_AMMO_ITEM): 1.5},
                'never_offered': {'categories': ['magnum']}}),
    perk('barry_magnum_specialist', 'Magnum Specialist', 'Barry. Magnums hit 30 percent harder, magnum rounds stack twice as high. Machine guns 25 percent less.', 'style',
         [], ['barry'], cost=150, rarity='rare', tags=['weapon'],
         build={'category_damage': {'magnum': 0.3, 'smg': -0.25, 'machine_gun': -0.25}, 'stack_mult': {hex(MAGNUM_AMMO_ITEM): 2}}),
    perk('barry_trap_setter', 'Trap Setter', 'Barry. Every 4 kills hand him a proximity bomb.', 'style',
         [{'kind': 'item_on_kill', 'game_item': PROXIMITY, 'amount': 1, 'every_kills': 4}], ['barry'], cost=150, rarity='rare', tags=['items']),
    perk('rebecca_chemist', 'Chemist', 'Rebecca. Green herbs she picks up become mixed herbs. Every heal is 20 percent smaller.', 'style',
         [], ['rebecca'], cost=180, rarity='rare', tags=['heal'],
         build={'pickup_as': {hex(GREEN_HERB_GROUND): hex(MIXED_HERB), hex(GREEN_HERB): hex(MIXED_HERB)}, 'heal_mult': 0.8}),
    perk('rebecca_steady_nerves', 'Steady Nerves', 'Rebecca. Rifles hit 30 percent harder, shotguns 20 percent less.', 'style',
         [], ['rebecca'], cost=120, rarity='uncommon', tags=['weapon'], build={'category_damage': {'rifle': 0.3, 'shotgun': -0.2}}),
    perk('excella_hostile_takeover', 'Hostile Takeover', 'Excella. Every melee kill pays 15 supplies, but guns deal 15 percent less.', 'style',
         [{'kind': 'supplies_on_kill', 'amount': 15, 'melee_only': True}], ['excella'], cost=150, rarity='rare', tags=['supplies'],
         build={'category_damage': {g: -0.15 for g in GUNS}}),
    perk('leon_suplex', 'Suplex', 'Leon. Melee hits deal twice the damage, every melee kill pays 10 supplies. Rifles 25 percent less.', 'style',
         [{'kind': 'supplies_on_kill', 'amount': 10, 'melee_only': True}], ['leon'], cost=150, rarity='rare', tags=['melee'],
         build={'melee_damage': 1.0, 'category_damage': {'rifle': -0.25}}),
    perk('leon_agent_training', 'Agent Training', 'Leon. 20 percent of his hits are critical and deal double damage. Heals 25 percent smaller.', 'style',
         [{'kind': 'crit', 'chance': 0.2, 'multiplier': 2.0}], ['leon'], cost=200, rarity='epic', tags=['crit'], build={'heal_mult': 0.75}),
    perk('excella_golden_parachute', 'Golden Parachute', 'Excella. Once per arena she survives a lethal hit with 25 percent health. 20 percent fewer supplies.', 'style',
         [{'kind': 'cheat_death', 'per_arena': 1, 'leave_hp_fraction': 0.25}], ['excella'], cost=200, rarity='epic', tags=['survival'],
         build={'supplies_mult': 0.8}),
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
    perk('leon_matilda', 'Matilda', 'Leon starts with an M93R and 30 handgun rounds.', 'supply', [item_start(M93R), item_start(HANDGUN_AMMO, 30)],
         ['leon'], cost=150, rarity='rare', tags=['weapon']),
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
    'leon_dsc_trait': ('Lucky Rookie', 'Once per arena Leon survives a lethal hit with 30 percent health.',
                       [{'kind': 'cheat_death', 'per_arena': 1, 'leave_hp_fraction': 0.3}]),
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
        'summary': 'Every shop sells the Gatling gun for 100 supplies until he has it. Ammo stacks three times as high.',
        'extra_weapons': ['gatling'], 'ammo_stack_mult': 3, 'shop_weapon': 'gatling', 'shop_weapon_price': 100,
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
        # x1.5 (the user, 2026-11-07: 49 melee kills gave +20 percent damage, +60 percent health and
        # felt weak; with these about +30 and +90).
        'melee_kill_stats': {'damage': 0.06, 'defense': 0.045, 'max_health': 0.075, 'crit': 0.03, 'move_speed': 0.045,
                             'heal': 0.075, 'supplies': 0.06},
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
    # The supported mods' skins (MOD_SKINS below), from their looks (the user, 2026-11-08: "make a
    # nice perk for both based on the vibes and the names"): Excella in a lab suit tries a new
    # compound on her guns every arena; Sheva in Hound Wolf Squad gear carries a flamethrower.
    'excella_experimenter': {
        'name': 'Field Trials', 'icon': 'syringe',
        'summary': 'Every arena her guns carry a random element. Every shop refines a gun at half price. Health x0.8.',
        'arena_element': True, 'refine_every_shop': True, 'refine_price_mult': 0.5, 'max_health_mult': 0.8,
    },
    'sheva_hound_wolf': {
        'name': 'Scorched Earth', 'icon': 'wolf',
        'summary': 'Starts with the flamethrower, fuel back twice as fast. Her guns are all Incendiary. Supplies x0.75.',
        'starting_weapon': 'flamethrower', 'fuel_regen_mult': 2.0, 'gun_elements': ['incendiary'], 'supplies_mult': 0.75,
    },
    # Jill in her Raccoon City clothes (Jill HDR Casual): RE3's Last Escape, the grenade launcher
    # and its flame, freeze and acid rounds; RE3's two guns only.
    'jill_classic': {
        'name': 'Last Escape', 'icon': 'launcher',
        'summary': 'Starts with the grenade launcher. Her guns all carry Incendiary, Cryo and Toxic. Handguns, launchers only.',
        'starting_weapon': 'grenade_launcher', 'gun_elements': ['incendiary', 'cryo', 'toxic'],
        'offered_weapons': {'categories': ['handgun', 'launcher']}, 'usable_weapons': {'categories': ['handgun', 'launcher']},
        'max_health_mult': 0.85,
    },
    # Leon's two (Leon Definitive Edition): Darkside Chronicles' rookie, RE4's jacket and its merchant.
    'leon_dsc': {
        'name': 'Lucky Rookie', 'icon': 'badge',
        'summary': 'Survives one lethal hit an arena at 30 percent health. Handguns 25 percent harder. Health x0.85.',
        'category_damage': {'handgun': 0.25}, 'max_health_mult': 0.85,
    },
    'leon_jacket': {
        'name': 'Best Customer', 'icon': 'credits',
        'summary': 'Every shop has a refine at half price and a free NEW STOCK. Better rarities. Supplies x0.8.',
        'refine_every_shop': True, 'refine_price_mult': 0.5, 'free_shop_rerolls': 1, 'rarity_shift': 0.3, 'supplies_mult': 0.8,
    },
    'sheva_fairy_tale': {
        'name': 'Enchanted Arms', 'icon': 'star',
        'summary': 'Starts with a legendary M3 that upgrades without limit.',
        'starting_weapon': 'm3', 'starting_weapon_rarity': 'legendary', 'uncapped_upgrades': {'ids': ['m3']},
    },
    'wesker_stars': {
        'name': 'Iron Fist', 'icon': 'fist',
        # Only his own three guns: the S and W M500 (Midnight Wesker's) sat sideways on his body
        # and fired from there, never in his hand (user playtest 2026-11-07); his model holds
        # what the game gives him.
        'summary': 'His melee hits three times as hard. Only his own Samurai Edge, Lightning Hawk and Hydra are for him.',
        'usable_weapons': {'ids': ['samurai_edge', 'l_hawk', 'hydra']}, 'melee_damage': 2.0,
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
    'chris_boulder_puncher': 'fist', 'chris_iron_grip': 'chain', 'sheva_scavenger': 'ammo', 'sheva_sharpshooter': 'rifle',
    'jill_weapon_expert': 'handgun', 'jill_finisher': 'skull', 'wesker_superior_being': 'shades', 'wesker_predator': 'lunge',
    'josh_field_commander': 'flag', 'josh_point_man': 'smg', 'barry_magnum_specialist': 'magnum', 'barry_trap_setter': 'mine',
    'rebecca_chemist': 'herb', 'rebecca_steady_nerves': 'rifle', 'excella_hostile_takeover': 'credits',
    'excella_golden_parachute': 'parachute',
    'leon_roundhouse': 'boot', 'leon_suplex': 'fist', 'leon_agent_training': 'crosshair', 'leon_matilda': 'handgun',
}
for p in perks:
    if p['id'] in ICONS:
        p['icon'] = ICONS[p['id']]

doc = {
    '_comment': ("Perks are per character and come only from the mod menu. category: style = the character's way of playing "
                 "(no plain stat perks), supply = what you start with (item_on_start puts game_item into the inventory at the first "
                 "arena; generic ones plus one per character), support = bad situations (low health, lethal hits), skin = a perk a "
                 "skin grants through its starting_passive (never listed; most skins are builds, see characters.json), curse = route modifiers. "
                 "A menu perk's 'build' adds into the run's build (characters.h ApplyPerkBuild: damage by weapon category, melee damage, "
                 "health, heals, supplies, stack sizes, weapons never offered, drops as ammo for the gun in hand, items swapped on pickup, "
                 "grabs broken). Not yet possible: move speed. Item ids: 0x0106 hand grenade, "
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
    'chris': ['chris_pocket_grenades', 'chris_boulder_puncher', 'chris_iron_grip', 'chris_shotgun'],
    'sheva': ['sheva_flash_tactics', 'sheva_scavenger', 'sheva_sharpshooter', 'sheva_smg'],
    'jill': ['jill_weapon_expert', 'jill_finisher', 'jill_smg'],
    'wesker': ['wesker_superior_being', 'wesker_predator', 'wesker_magnum'],
    'josh': ['josh_field_commander', 'josh_point_man', 'josh_magnum'],
    'barry': ['barry_magnum_specialist', 'barry_trap_setter', 'barry_smg'],
    'rebecca': ['rebecca_chemist', 'rebecca_steady_nerves', 'rebecca_rifle'],
    'excella': ['excella_hostile_takeover', 'excella_golden_parachute', 'excella_rifle'],
    'leon': ['leon_suplex', 'leon_agent_training', 'leon_matilda'],
}
passives = {'chris': 'chris_veteran', 'sheva': 'sheva_field_training', 'jill': 'jill_battle_instinct', 'wesker': 'wesker_firestarter',
            'josh': 'josh_squad_tactics', 'barry': 'barry_demolition_man', 'rebecca': 'rebecca_combat_medic', 'excella': 'excella_executive_bonus',
            'leon': 'leon_roundhouse'}


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
# The PERKS menu shows a perk's description as its help, in the game font (letters, digits,
# space, comma, period, hyphen: no percent sign, plus, colon or slash).
for p in perks:
    if p['category'] in ('style', 'supply', 'support'):
        assert len(help_lines(p['description'])) <= 2, (p['id'], help_lines(p['description']))
        assert all(ch.isalnum() or ch in ' ,.-' for ch in p['description']), (p['id'], p['description'])

# Midnight Wesker opens with the boss Wesker beaten, S.T.A.R.S. Wesker with Uroboros Wesker (the user's
# ladder, 2026-11-07); until one of them is beaten Wesker himself is locked.
SKIN_BOSS_UNLOCKS = {'wesker_midnight': 'boss:uEm81', 'wesker_stars': 'boss:uEm82'}
# Supported third-party mods (2026-11-08, the user's list: "not mandatory", "you need to go to the
# original creators mod page and get it to endorse it"). Each is a skin that stands in for a costume
# (its build and trait are that skin's); the player unzips the mod into RogueMercenaries\mods and the
# adapter reads the listed archives from there (adapters/re5/src/file_redirect.cpp). Never shipped
# with this mod. Sizes are of the version checked in game (scripts/test_skin_mod.sh).
MOD_SKINS = [
    {'character': 'excella', 'base': 'excella_dress', 'id': 'excella_experimenter', 'name': 'Experimenter',
     'mod': {'name': 'Excella the Experimenter', 'author': 'TheGrimrose',
             'url': 'https://www.nexusmods.com/residentevil5goldedition/mods/387',
             'files': [{'name': 'uPl05ExcellaNormal.arc', 'size': 13847141}],
             # Its own character select bust, in its copy of the game's Mercenaries archive.
             'portrait': {'file': 'Merce2Resource.arc', 'entry': 'id\\jpn\\merce_dc\\versus_22_360_NOMIP_BM'}}},
    {'character': 'sheva', 'base': 'sheva_fairy_tale', 'id': 'sheva_hound_wolf', 'name': 'Hound Wolf Squad',
     'mod': {'name': 'Hound Wolf Squad Sheva', 'author': 'Angeltitilxyz',
             'url': 'https://www.nexusmods.com/residentevil5goldedition/mods/1177',
             'files': [{'name': 'uPl01ShebaCos4.arc', 'size': 66242900}],
             # Its preview picture, the standing Sheva with the flamethrower.
             'portrait': {'file': 'A.jpg', 'crop': [560, 50, 540, 540]}}},
]
MOD_SKINS.append(
    {'character': 'jill', 'base': 'jill_bsaa', 'id': 'jill_classic', 'name': 'Classic',
     'mod': {'name': 'Jill HDR Casual', 'author': '',
             'url': 'https://www.nexusmods.com/residentevil5goldedition/mods/135',
             # Its four variants of her BSAA outfit's file (gear or not, gloves or not): any one.
             'files': [{'name': 'uPl02JillCos1.arc', 'sizes': [35058761, 34773905, 34708493, 34423428]}],
             # Its preview: the "Gear, Gloves" panel, below its title.
             'portrait': {'file': 'preview.jpg', 'crop': [1250, 180, 416, 416]}}})
# Leon S. Kennedy: a character of his own, worn in Chris's costumes (Leon Definitive Edition by
# kreed and izacky, 2026-11-08, the user: "this has to be his own character with his own perks").
# His voice lines are new files the costumes ask for: their folder is read from the mod too.
if not any(x['id'] == 'leon' for x in c['characters']):
    c['characters'].append({'id': 'leon', 'name': 'Leon S. Kennedy', 'enabled': True, 'starting_weapons': ['m92f', 'm3'],
                            'exclusive_mechanic': '', 'unlock_cost_credits': 0, 'costume_index': 0, 'skins': []})
LEON_MOD = {'name': 'Leon Definitive Edition', 'author': 'kreed, izacky',
            'url': 'https://www.nexusmods.com/residentevil5goldedition/mods/223',
            'folders': ['sound\\bgm\\oto\\emo_voice\\mod\\leon']}
MOD_SKINS += [
    {'character': 'leon', 'base': 'chris_bsaa', 'id': 'leon_dsc', 'name': 'Darkside Chronicles',
     'mod': dict(LEON_MOD, files=[{'name': 'uPl00ChrisNormal.arc', 'size': 60420703}],
                 portrait={'file': 'Leon dsc\\kreedpic.jpg', 'crop': [2020, 0, 900, 900]})},
    {'character': 'leon', 'base': 'chris_stars', 'id': 'leon_jacket', 'name': 'Jacket',
     'mod': dict(LEON_MOD, files=[{'name': 'uPl00ChrisCos2.arc', 'size': 90341329}],
                 portrait={'file': 'Leon jacket\\kreedpic.jpg', 'crop': [80, 0, 1200, 1200]})},
]
MOD_BASE = {m['id']: m['base'] for m in MOD_SKINS}
for m in MOD_SKINS:
    ch = next(x for x in c['characters'] if x['id'] == m['character'])
    base = next(s for x in c['characters'] for s in x['skins'] if s['id'] == m['base'])  # Leon's are Chris's
    entry = next((x for x in ch['skins'] if x['id'] == m['id']), None)
    if entry is None:
        entry = {'id': m['id']}
        ch['skins'].append(entry)
    entry['name'] = m['name']
    entry['costume_index'] = base['costume_index']
    entry['mod'] = m['mod']
for ch in c['characters']:
    ch['starting_passive'] = passives[ch['id']]
    ch['skill_pool'] = pools[ch['id']]
    for s in ch.get('skins', []):
        own = s['id'] if s['id'] in BUILDS else MOD_BASE.get(s['id'], s['id'])  # a mod skin without its own build wears its base's
        trait = own + '_trait'
        if trait in skin_traits:
            s['starting_passive'] = trait
        else:
            s.pop('starting_passive', None)
        if own in BUILDS:
            s['build'] = BUILDS[own]
        else:
            s.pop('build', None)
        # Skins a boss unlocks (the user's ladder, 2026-11-07: "once you get midnight wesker you unlock
        # him", the description not saying how). The menus add the full stop.
        for field in ('unlock_kind', 'unlock_boss', 'unlock_text'):
            s.pop(field, None)
        if s['id'] in SKIN_BOSS_UNLOCKS:
            s['unlock_kind'] = 'boss_beaten'
            s['unlock_boss'] = SKIN_BOSS_UNLOCKS[s['id']]
            s['unlock_text'] = 'Locked'
c['_comment'] = ("Playable characters. Every character and skin is available from the start (the challenge fields unlock_kind/unlock_count/"
                 "unlock_text/unlock_character, see core/include/rm/characters.h, are kept for later). starting_weapons: the first entry is "
                 "the pistol every run starts with (the adapter strips the game's own Mercenaries loadout down to it); other weapons come "
                 "from supply perks, rewards and the shop. starting_passive is the character's free style trait; skill_pool lists the perks "
                 "the character can unlock. A skin is a build (ROADMAP M8, written by scripts/write_perks.py): 'build' holds its rules "
                 "(weapon filters, damage by weapon category, supplies, free shop rerolls, rarity, uncapped upgrades, a starting weapon, "
                 "max health), and a skin without one yet keeps a small trait (a 'skin' perk). costume_index is the class and the model each "
                 "slot loads, checked in game: 0..2 and 10 Heavy Metal, 14 Warrior Chris; 3 and 11 Fairy Tale, 15 Business Sheva; 9 Wesker "
                 "S.T.A.R.S.; 12 Josh, 13 Excella, 16 Barry, 17 Rebecca. Text shown by the game font: letters, digits, space, comma and "
                 "period only. A skin with 'mod' wears a third-party mod's look (name, author, url: its page; files: the archives "
                 "read from RogueMercenaries\\mods instead of the game's, with the size checked); without the mod it is listed but "
                 "not selectable.")
json.dump(c, open('configs/characters.json', 'w'), indent=2)
open('configs/characters.json', 'a').write('\n')

# loadouts.json: no roll (pistol only); the equipment list stays the shop/reward catalog.
l = json.load(open('configs/loadouts.json'))
l['budget'] = 0
CATALOG_EXTRA = [
    {'id': 'proximity_bomb', 'name': 'Proximity Bomb', 'cost': 15, 'max': 2, 'game_item': 0x010A, 'price': 20, 'bundle': 2},
    {'id': 'arrows', 'name': 'Arrows', 'cost': 5, 'max': 1, 'game_item': 0x020B, 'price': 0, 'bundle': 10},
    # The grenade launcher's explosive rounds, offered while the run holds it (the launcher is one
    # game item per round type loaded; acid and nitrogen rounds would change it).
    {'id': 'explosive_rounds', 'name': 'Explosive Rounds', 'cost': 10, 'max': 2, 'game_item': 0x0206, 'price': 25, 'bundle': 3,
     'for_weapons': ['grenade_launcher']},
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
       'dragunov': 0x0120, 'samurai_edge': 0x0129, 'gatling': 0x0115, 'longbow': 0x010F,
       'm93r': 0x011E, 'stun_rod': 0x0122, 'grenade_launcher': 0x0125, 'rocket_launcher': 0x010D, 'flamethrower': 0x0121}
WEAPON_EXTRA = [
    # Heavy Metal's own gun: no unlock (every shop of his sells it, user playtest 2026-11-04).
    {'id': 'gatling', 'name': 'Gatling Gun', 'category': 'machine_gun', 'base_damage_mult': 1.0, 'build_only': True},
    {'id': 'longbow', 'name': 'Longbow', 'category': 'bow', 'base_damage_mult': 1.0, 'build_only': True},
    # The rest of RE5's arsenal (2026-11-04, the user's go), unlocked in CREDITS. The grenade
    # launcher is its explosive-round item (0x0125); the rocket launcher holds one rocket and the
    # game has no rockets to sell; the flamethrower is a campaign area's item (no Organize record).
    {'id': 'm93r', 'name': 'M93R', 'category': 'handgun', 'base_damage_mult': 1.0, 'unlock_cost_credits': 150},
    {'id': 'stun_rod', 'name': 'Stun Rod', 'category': 'melee', 'base_damage_mult': 1.0, 'unlock_cost_credits': 100},
    {'id': 'grenade_launcher', 'name': 'Grenade Launcher', 'category': 'launcher', 'base_damage_mult': 1.0, 'unlock_cost_credits': 350},
    {'id': 'rocket_launcher', 'name': 'Rocket Launcher', 'category': 'launcher', 'base_damage_mult': 1.0, 'unlock_cost_credits': 400},
    {'id': 'flamethrower', 'name': 'Flamethrower', 'category': 'flamethrower', 'base_damage_mult': 1.0, 'unlock_cost_credits': 300},
]
for extra in WEAPON_EXTRA:
    known = [wd for wd in w['weapons'] if wd['id'] == extra['id']]
    if not known:
        w['weapons'].append(extra)
    elif 'unlock_cost_credits' not in extra:
        known[0].pop('unlock_cost_credits', None)
# Guns kept out of the arsenal. The flamethrower was (2026-11-07: only its pilot light showed) until
# 2026-11-08 showed the test only tapped the trigger ("lclick+ms" is a tap and a wait): held, it
# flames on a Mercenaries stage and sets Majini burning (scripts/test_flamethrower.sh).
WEAPON_OFF = set()
for wd in w['weapons']:
    if wd['id'] in WEAPON_OFF:
        wd['enabled'] = False
    else:
        wd.pop('enabled', None)
for wd in w['weapons']:
    if wd['id'] in ids:
        wd['game_item'] = ids[wd['id']]
json.dump(w, open('configs/weapons.json', 'w'), indent=2)
open('configs/weapons.json', 'a').write('\n')
# economy.json: chest keys (M8; Jill BSAA's build opens chests without them). 8 percent of Majini
# kills: at 15 two sessions dropped 50 keys for 17 chests (the user, 2026-11-07: too many).
e = json.load(open('configs/economy.json'))
e['keys'] = {'enabled': True, 'drop_chance': 0.08, 'classes': ['uEm1'], 'start': 0, 'max': 9}
# The ladder (the user, 2026-11-07): 4 arenas ending on Uroboros; beating him adds Giant Uroboros
# as a choice; beating that opens 8 arenas, then 12, then the 13th arena against Wesker; later
# arenas harder a step each from the fifth.
e['ladder'] = {
    '_comment': 'Run lengths (EconomyConfig::Ladder, written by scripts/write_perks.py). A length ends on its easy boss until it is beaten, then on a choice of easy or hard; beating a hard one opens the length whose unlocked_by names it. A run also fights every shorter length\'s boss at that length\'s arena. The finale is one more arena after its after length\'s boss in a run that beat a hard boss of every length in needs_hard. From growth_from_arena on, each arena adds budget_per_arena enemies, incoming_per_arena damage taken and toughness_per_arena (the player\'s damage divided by 1 + it) per step.',
    'enabled': True,
    'lengths': [
        {'id': 'short', 'name': 'Short', 'arenas': 4, 'easy_boss': 103, 'hard_boss': 304},
        # The user's ladder (2026-11-08: "4 - uroboros, 4 alternate giant uroboros, 8 - Aheri, 8
        # alternate - Mkono, 12 - uroboros wesker, 13 midnight wesker"; then Jill as the 8's other
        # alternate: "8: Mkono or Jill"). Popokarimu (s108) stays a boss map outside the ladder.
        {'id': 'long', 'name': 'Long', 'arenas': 8, 'easy_boss': 308, 'hard_boss': [511, 309],
         'unlocked_by': 'short:hard', 'boss_hp_mult': 1.3},
        {'id': 'marathon', 'name': 'Marathon', 'arenas': 12, 'easy_boss': 508,
         'unlocked_by': 'long:hard', 'boss_hp_mult': 1.5},
    ],
    # A longer run fights every shorter length's boss on its way (the user, 2026-11-08: "you get 1 on
    # 4 one on 8 and one on 12"). The 13th arena: Midnight Wesker, after the 12's boss in a run that
    # took the alternates on 4 and 8 ("except if you take both alternate routes, then you get 13th
    # stage").
    'finale': {'boss': 506, 'after': 'marathon', 'needs_hard': ['short', 'long'], 'boss_hp_mult': 1.0},
    'growth_from_arena': 5,
    'incoming_per_arena': 0.08,
    'toughness_per_arena': 0.05,
    'budget_per_arena': 2,
}
json.dump(e, open('configs/economy.json', 'w'), indent=2)
open('configs/economy.json', 'a').write('\n')
print('perks', len(perks), 'written; characters, loadouts, weapons, economy aligned')
