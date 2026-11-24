#!/usr/bin/env python3
"""Draws the charms' pictures (configs/charms/<id>.png, 102x102 RGBA, the size of an item's picture in
the game's item sheet) for gun_looks.cpp to paint over the treasure each charm stands for.

  python scripts/charm_pictures.py [--game "<RE5 folder>"] [--sheet]

- fidelity_card: a golden card, drawn here in the sheet's rendered style (the door keycards are key
  items, whose pictures no Mercenaries archive loads).
- uroboros_virus: the game's own herb vial (item 0x0305's picture) with its liquid turned to the
  virus's black and violet.
- grapple_gun: the game's own M93R (0x011E's picture) with a grappling hook and its cable.
- reloading_tool: the game's own grenade launcher rounds, flame (0x020E), acid (0x0207) and freeze (0x0208), fanned out
  over a small press, Classic Jill's RE3 gunpowder.

The game's pictures come from the item sheet id\\jpn\\cockpit\\cp05_360_NOMIP_BM in ShopResource.arc,
read only; configs/item_pictures.json says where each sits. --sheet also writes build/charms_sheet.png.
"""
import argparse
import json
import math
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re5tex  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'configs', 'charms')
SIZE = 102
SS = 4
SHEET = 'id\\jpn\\cockpit\\cp05_360_NOMIP_BM'
# Item 0x0305 (green herb) and 0x011E (M93R): their pictures' rects (item box layout, scripts/item_pictures.py).
HERB_PICTURE = 'item_15c'
# The launcher's flame, acid and nitrogen rounds (the item box table, 0x01128C70: 0x020E, 0x0207, 0x0208).
ROUND_PICTURES = ('item_157', 'item_154', 'item_155')

parser = argparse.ArgumentParser()
parser.add_argument('--game', default=r'G:\SteamLibrary\steamapps\common\Resident Evil 5')
parser.add_argument('--sheet', action='store_true')
args = parser.parse_args()


def arc_entry(path, wanted):
    data = open(path, 'rb').read()
    count = struct.unpack_from('<H', data, 6)[0]
    for i in range(count):
        at = 8 + i * 80
        name = data[at:at + 64].split(b'\0')[0].decode('latin1')
        _, packed, _, start = struct.unpack_from('<IIII', data, at + 64)
        if name == wanted:
            return zlib.decompress(data[start:start + packed])
    raise SystemExit(f'{wanted} not in {path}')


def layout_rect(layout, element):
    import re
    names = [(m.start(), m.group(1).decode()) for m in re.finditer(rb'(item_[0-9a-f]{3})\0', layout)]
    marks = [m.start() for m in re.finditer(rb'\x01\x00\x00\x00\xff\xff\xff\xff', layout)]
    for i, (at, name) in enumerate(names):
        if name != element:
            continue
        end = names[i + 1][0] if i + 1 < len(names) else len(layout)
        for mark in marks:
            if at < mark < end:
                return struct.unpack_from('<4I', layout, mark - 16)
    raise SystemExit(f'{element} not in the layout')


archive = os.path.join(args.game, 'nativePC_MT', 'Image', 'Archive', 'ShopResource.arc')
sheet = Image.fromarray(re5tex.to_rgba(arc_entry(archive, SHEET)), 'RGBA')
layout = arc_entry(archive, 'id\\jpn\\itembox\\itembox_360')
pictures = json.load(open(os.path.join(ROOT, 'configs', 'item_pictures.json'), encoding='utf-8'))['pictures']


def game_picture(rect):
    return sheet.crop(tuple(rect)).copy()


def big():
    return Image.new('RGBA', (SIZE * SS, SIZE * SS), (0, 0, 0, 0))


def shadowed(img):
    """A soft dark shadow under the drawing, as the sheet's pictures have."""
    alpha = img.split()[3]
    shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 150), (0, 0), alpha.filter(ImageFilter.GaussianBlur(3 * SS)))
    shadow = ImageChops.offset(shadow, 2 * SS, 3 * SS)
    shadow.alpha_composite(img)
    return shadow


def fidelity_card():
    img = big()
    w, h = 78 * SS, 50 * SS
    card = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=6 * SS, fill=255)
    # Brushed gold: a diagonal light, darker edges, fine horizontal streaks.
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    t = (xx / w * 0.6 + yy / h * 0.4)
    light = 0.55 + 0.45 * np.cos((t - 0.35) * math.pi * 1.6)
    streak = 0.06 * np.sin(yy / (1.3 * SS) * 1.7) + 0.04 * np.sin(yy / (0.6 * SS))
    v = np.clip(light + streak, 0.15, 1.1)
    gold = np.stack([np.clip(240 * v, 0, 255), np.clip(190 * v, 0, 255), np.clip(80 * v, 0, 255), np.full_like(v, 255)], axis=-1)
    card = Image.fromarray(gold.astype(np.uint8), 'RGBA')
    card.putalpha(mask)
    d = ImageDraw.Draw(card)
    d.rounded_rectangle([1.5 * SS, 1.5 * SS, w - 1.5 * SS, h - 1.5 * SS], radius=5 * SS, outline=(255, 238, 170, 230), width=int(1.2 * SS))
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=6 * SS, outline=(110, 70, 20, 255), width=int(1.0 * SS))
    # The chip.
    cx, cy = 10 * SS, 16 * SS
    d.rounded_rectangle([cx, cy, cx + 14 * SS, cy + 11 * SS], radius=2 * SS, fill=(200, 160, 70, 255), outline=(120, 85, 30, 255), width=SS)
    for k in range(1, 3):
        d.line([cx + k * 14 * SS / 3, cy, cx + k * 14 * SS / 3, cy + 11 * SS], fill=(130, 95, 35, 255), width=SS)
    d.line([cx, cy + 5.5 * SS, cx + 14 * SS, cy + 5.5 * SS], fill=(130, 95, 35, 255), width=SS)
    # A five-pointed star stamped in the corner, and the number embossed along the bottom.
    sx, sy, r_out, r_in = 60 * SS, 15 * SS, 8 * SS, 3.4 * SS
    star = [(sx + (r_out if i % 2 == 0 else r_in) * math.cos(math.radians(-90 + i * 36)),
             sy + (r_out if i % 2 == 0 else r_in) * math.sin(math.radians(-90 + i * 36))) for i in range(10)]
    d.polygon([(x + 0.6 * SS, y + 0.8 * SS) for x, y in star], fill=(120, 80, 25, 200))
    d.polygon(star, fill=(255, 236, 160, 255))
    for k in range(4):
        x0 = 9 * SS + k * 16 * SS
        for j in range(4):
            px = x0 + j * 3.2 * SS
            d.ellipse([px, 37 * SS, px + 1.8 * SS, 38.8 * SS], fill=(120, 80, 25, 230))
    card = card.rotate(14, resample=Image.BICUBIC, expand=True)
    img.alpha_composite(card, ((SIZE * SS - card.width) // 2, (SIZE * SS - card.height) // 2))
    return shadowed(img).resize((SIZE, SIZE), Image.LANCZOS)


def uroboros_virus():
    rect = layout_rect(layout, HERB_PICTURE)
    vial = np.asarray(game_picture(rect)).astype(np.float32)
    r, g, b, a = vial[..., 0], vial[..., 1], vial[..., 2], vial[..., 3]
    # The green liquid (green above red and blue) becomes near black with a violet sheen on its highlights.
    green = np.clip((g - np.maximum(r, b) - 6.0) / 25.0, 0.0, 1.0)
    lum = (0.3 * r + 0.59 * g + 0.11 * b) / 255.0
    out = vial.copy()
    out[..., 0] = r * (1 - green) + (18 + 150 * lum ** 2.2) * green
    out[..., 1] = g * (1 - green) + (6 + 35 * lum ** 2.2) * green
    out[..., 2] = b * (1 - green) + (28 + 200 * lum ** 2.2) * green
    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), 'RGBA')
    # Black tendrils wrapping out of the liquid, across the vial's lower half (it runs from its cap at the top
    # right to its bottom at the bottom left).
    over = Image.new('RGBA', (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    a0, a1 = (50.0, 60.0), (34.0, 90.0)
    normal = (0.87, 0.48)
    for k, (t0, side, length) in enumerate(((0.1, 1, 20), (0.35, -1, 18), (0.6, 1, 16), (0.85, -1, 15))):
        sx, sy = a0[0] + (a1[0] - a0[0]) * t0, a0[1] + (a1[1] - a0[1]) * t0
        pts = []
        for i in range(14):
            t = i / 13
            wiggle = 3.5 * math.sin(t * math.pi * 2.2 + k)
            x = sx + side * normal[0] * length * t - normal[1] * wiggle
            y = sy + side * normal[1] * length * t + normal[0] * wiggle
            pts.append((x * SS, y * SS))
        for i in range(len(pts) - 1):  # tapering
            wdt = (3.2 - 2.4 * i / (len(pts) - 1)) * SS
            d.line([pts[i], pts[i + 1]], fill=(14, 4, 22, 245), width=int(wdt))
            d.line([pts[i], pts[i + 1]], fill=(90, 40, 120, 120), width=max(1, int(wdt * 0.25)))
    over = over.resize((SIZE, SIZE), Image.LANCZOS)
    img.alpha_composite(over)
    return img


def grapple_gun():
    gun = game_picture(pictures['0x011E'])
    img = Image.new('RGBA', (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # The cable from the muzzle (the picture's left), coiled below, and the hook's three prongs at its end.
    hx, hy = 11 * SS, 21 * SS
    cable = [(30 * SS, 34 * SS), (22 * SS, 30 * SS), (15 * SS, 26 * SS), (hx, hy)]
    d.line(cable, fill=(30, 30, 34, 255), width=int(2.2 * SS), joint='curve')
    for k, ang in enumerate((-150, -95, -40)):
        a = math.radians(ang)
        tip = (hx + 13 * SS * math.cos(a), hy + 13 * SS * math.sin(a))
        mid = (hx + 8 * SS * math.cos(a - 0.5), hy + 8 * SS * math.sin(a - 0.5))
        d.line([(hx, hy), mid, tip], fill=(196, 198, 204, 255), width=int(3.0 * SS), joint='curve')
        d.line([(hx, hy), mid, tip], fill=(60, 60, 66, 255), width=int(0.8 * SS), joint='curve')
    d.ellipse([hx - 2.6 * SS, hy - 2.6 * SS, hx + 2.6 * SS, hy + 2.6 * SS], fill=(150, 152, 158, 255), outline=(50, 50, 55, 255), width=SS)
    hook = img.resize((SIZE, SIZE), Image.LANCZOS)
    out = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(gun.resize((90, 90), Image.LANCZOS), (10, 12))
    out.alpha_composite(hook)
    return out


def reloading_tool():
    out = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    # A small press under them: a dark steel base and a lever.
    press = big()
    d = ImageDraw.Draw(press)
    d.rounded_rectangle([18 * SS, 74 * SS, 86 * SS, 88 * SS], radius=3 * SS, fill=(58, 60, 66, 255), outline=(25, 25, 28, 255), width=SS)
    d.rectangle([47 * SS, 46 * SS, 55 * SS, 76 * SS], fill=(84, 86, 94, 255), outline=(30, 30, 34, 255), width=SS)
    d.line([(51 * SS, 50 * SS), (82 * SS, 38 * SS)], fill=(150, 152, 160, 255), width=int(3 * SS))
    d.ellipse([79 * SS, 34 * SS, 87 * SS, 42 * SS], fill=(190, 40, 40, 255), outline=(80, 15, 15, 255), width=SS)
    out.alpha_composite(shadowed(press).resize((SIZE, SIZE), Image.LANCZOS))
    # The three rounds' own pictures, fanned out.
    for k, (element, ang, at) in enumerate(zip(ROUND_PICTURES, (18, 0, -18), ((2, 6), (20, 0), (38, 6)))):
        pic = game_picture(layout_rect(layout, element)).resize((62, 62), Image.LANCZOS).rotate(ang, resample=Image.BICUBIC, expand=True)
        out.alpha_composite(pic, at)
    return out


os.makedirs(OUT, exist_ok=True)
made = {'fidelity_card': fidelity_card(), 'uroboros_virus': uroboros_virus(), 'grapple_gun': grapple_gun(),
        'reloading_tool': reloading_tool()}
for name, im in made.items():
    assert im.size == (SIZE, SIZE)
    im.save(os.path.join(OUT, name + '.png'), optimize=True)
print(len(made), 'charm pictures in', OUT)
if args.sheet:
    board = Image.new('RGBA', (len(made) * (SIZE * 2 + 12) + 12, SIZE * 2 + 24), (35, 35, 35, 255))
    for i, im in enumerate(made.values()):
        board.alpha_composite(im.resize((SIZE * 2, SIZE * 2), Image.LANCZOS), (12 + i * (SIZE * 2 + 12), 12))
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    board.save(os.path.join(ROOT, 'build', 'charms_sheet.png'))
    print('sheet: build/charms_sheet.png')
