#!/usr/bin/env python3
"""Writes configs/item_pictures.json: where each gun's inventory picture sits in the game's item
picture sheet (id\\jpn\\cockpit\\cp05_360_NOMIP_BM, 1024x1760 DXT5).

The pictures are 102x102 cuts of that texture, one layout element `item_NNN` each; the item box
layout (id\\jpn\\itembox\\itembox_360 in ShopResource.arc) records each element's rect in pixels
(four u32 just before `01 00 00 00 FF FF FF FF`), and the executable's item box table (0x01128C70,
105 records of 12 bytes: u16 item, u16 picture NNN, ...) says which item takes which element. The
match's inventory, its rings and the item box all draw from that texture (research/re5-engine-map.md
"Item pictures"). The adapter recolours a gun's cut for its element (gun_looks.cpp).

  python scripts/item_pictures.py [--dump build/dump/re5dx9_mem.exe] [--game "<RE5 folder>"]

Reads the game's files only.
"""
import argparse
import json
import os
import re
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE_VA, TABLE_RECORDS, IMAGE_BASE = 0x01128C70, 105, 0x400000
TEXTURE = 'id\\jpn\\cockpit\\cp05_360_NOMIP_BM'
LAYOUT = 'id\\jpn\\itembox\\itembox_360'
# Guns a run hands out that have no record of their own: the pictures itembox_guard.cpp gives them,
# and their models' folders.
EXTRA = {0x011F: 0x13E, 0x0129: 0x124, 0x0112: 0x124, 0x0121: 0x135}
EXTRA_MODELS = {0x011F: 'wp1f', 0x0129: 'wp29', 0x0112: 'wp12', 0x0121: 'wp21'}

parser = argparse.ArgumentParser()
parser.add_argument('--dump', default=os.path.join(ROOT, 'build', 'dump', 're5dx9_mem.exe'))
parser.add_argument('--game', default=r'G:\SteamLibrary\steamapps\common\Resident Evil 5')
parser.add_argument('--out', default=os.path.join(ROOT, 'configs', 'item_pictures.json'))
args = parser.parse_args()


def arc_entry(path, wanted):
    data = open(path, 'rb').read()
    assert data[:4] == b'ARC\0', path
    count = struct.unpack_from('<H', data, 6)[0]
    for i in range(count):
        at = 8 + i * 80
        name = data[at:at + 64].split(b'\0')[0].decode('latin1')
        _, packed, _, start = struct.unpack_from('<IIII', data, at + 64)
        if name == wanted:
            return zlib.decompress(data[start:start + packed])
    raise SystemExit(f'{wanted} not in {path}')


layout = arc_entry(os.path.join(args.game, 'nativePC_MT', 'Image', 'Archive', 'ShopResource.arc'), LAYOUT)
names = [(m.start(), int(m.group(1), 16)) for m in re.finditer(rb'item_([0-9a-f]{3})\0', layout)]
marks = [m.start() for m in re.finditer(rb'\x01\x00\x00\x00\xff\xff\xff\xff', layout)]
rects = {}
for i, (at, number) in enumerate(names):
    end = names[i + 1][0] if i + 1 < len(names) else len(layout)
    for mark in marks:
        if at < mark < end:
            x0, y0, x1, y1 = struct.unpack_from('<4I', layout, mark - 16)
            if 0 <= x0 < x1 <= 1024 and 0 <= y0 < y1 <= 1760:
                rects[number] = [x0, y0, x1, y1]
                break
assert len(rects) == len(names) and len(rects) > 100, (len(rects), len(names))
assert all(r[2] - r[0] == 102 and r[3] - r[1] == 102 for r in rects.values())

exe = open(args.dump, 'rb').read()
table, models = {}, {}
for i in range(TABLE_RECORDS):
    at = TABLE_VA - IMAGE_BASE + i * 12
    item, picture = struct.unpack_from('<HH', exe, at)
    if not item:
        continue
    table[item] = picture
    model_va = struct.unpack_from('<I', exe, at + 8)[0]
    if IMAGE_BASE <= model_va < IMAGE_BASE + len(exe):
        path = exe[model_va - IMAGE_BASE:model_va - IMAGE_BASE + 64].split(b'\0')[0].decode('latin1')
        m = re.match(r'pawn\\wp\\(wp[0-9a-f]{2})\\model\\', path, re.IGNORECASE)  # the P8's is "Model"
        if m:
            models[item] = m.group(1)
assert table.get(0x0102) == 0x124 and table.get(0x011E) == 0x13D, 'not the item box table expected'
for item, picture in EXTRA.items():
    table.setdefault(item, picture)
    models.setdefault(item, EXTRA_MODELS[item])

pictures, folders = {}, {}
for item, picture in sorted(table.items()):
    if item >> 8 != 1 or picture not in rects:
        continue  # guns (and grenades) only: the elements are a gun's
    pictures[f'0x{item:04X}'] = rects[picture]
    if item in models:
        folders[f'0x{item:04X}'] = models[item]
out = {
    '_comment': 'Written by scripts/item_pictures.py: each gun\'s 102x102 picture in the item picture sheet, '
                'as x0 y0 x1 y1 in pixels. gun_looks.cpp recolours it for the gun\'s element.',
    'texture': TEXTURE,
    'width': 1024,
    'height': 1760,
    'pictures': pictures,
    '_models': 'Each gun\'s model folder (pawn\\wp\\<folder>\\model, its unit uWp<NN>), from the same records.',
    'models': folders,
}
with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(out, f, indent=1)
    f.write('\n')
print(len(pictures), 'gun pictures,', len(folders), 'model folders ->', args.out)
