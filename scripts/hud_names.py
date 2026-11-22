#!/usr/bin/env python3
"""The HUD's character names (the weapon rings and the inventory tabs) for characters that wear
another one's costume slots: the game draws "Chris" and "Sheva" from the cockpit texture
id\\eng\\cockpit\\cp03 (top left strip), so Leon and Ada need their own art in the same style
(a flared serif in brushed silver, bevelled, with a dark outline). The adapter writes a name's
tile over the game's while a run plays as that character (hud_names.cpp).

  python scripts/hud_names.py compare CP03_PNG OUT_PNG      fonts side by side with the original
  python scripts/hud_names.py build TEX_DIR OUT_DIR          the tiles and fingerprints in configs/hud_names
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = r'C:\Windows\Fonts'
# The game's two names in cp03 (pixels, 4-aligned so a tile is whole DXT blocks): "Chris" is
# the first player's base character, "Sheva" the second's.
SLOTS = {
    'chris': (0, 0, 128, 56),  # the game's glyphs: x 4..123, y 5..48 ("HELP" starts at y 59)
    'sheva': (128, 0, 264, 56),  # x 133..258 (the orange digits start at x 266)
}
# Characters that wear another one's costume slots (configs/characters.json), and their names.
NAMES = {'leon': 'Leon', 'ada': 'Ada'}
FONT = 'pala.ttf'
SIZE = 55          # Palatino's ascenders then stand as tall as the game's (about 40 px)
CONDENSE = 0.85    # the game's face is narrower
BASELINE = 47
SCALE = 4  # supersampling


def render(text, size, font_file, box_w, box_h, baseline, condense=CONDENSE):
    """The name as RGBA in the game's style, its baseline at `baseline` px, left-aligned with a
    4 px margin like the game's, condensed like its face and shrunk to fit the box."""
    S = SCALE
    font = ImageFont.truetype(os.path.join(FONTS, font_file), size * S)
    left, top, right, bottom = font.getbbox(text, anchor='ls')
    width = (right - left) * condense
    room = (box_w - 10) * S
    if width > room:
        font = ImageFont.truetype(os.path.join(FONTS, font_file), int(size * S * room / width))
        left, top, right, bottom = font.getbbox(text, anchor='ls')
    # Drawn wide, then squeezed to the box: the glyphs come out `condense` as wide.
    wide = Image.new('L', (int(box_w * S / condense), box_h * S), 0)
    ImageDraw.Draw(wide).text((5 * S / condense - left, baseline * S), text, fill=255, font=font, anchor='ls')
    mask = wide.resize((box_w * S, box_h * S), Image.LANCZOS)
    m = np.asarray(mask, dtype=np.float32) / 255.0
    # Bevel: light from the upper left on the blurred glyphs' slope.
    blur = np.asarray(mask.filter(ImageFilter.GaussianBlur(1.6 * S)), dtype=np.float32) / 255.0
    gy, gx = np.gradient(blur)
    light = (-gx - gy) * 6.0 * S
    # Brushed silver: mid grey, lighter at the top of the glyphs, darker below the middle.
    rows = np.linspace(0.0, 1.0, box_h * S)[:, None]
    base = 166.0 - 34.0 * np.clip((rows - 0.45) * 2.2, 0.0, 1.0) + 18.0 * np.clip(0.45 - rows, 0.0, 1.0) * 2.0
    grey = np.clip(base + light * 70.0, 60.0, 235.0)
    fill = np.stack([grey - 3.0, grey - 1.0, grey + 5.0], axis=-1)
    # The outline: the glyphs grown by about 2 px, near black.
    grow = 2 * S
    outline = np.asarray(mask.filter(ImageFilter.MaxFilter(grow * 2 + 1)), dtype=np.float32) / 255.0
    outline = np.asarray(Image.fromarray((outline * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7 * S)),
                         dtype=np.float32) / 255.0
    dark = np.array([18.0, 18.0, 22.0])
    rgb = fill * m[..., None] + dark * (1.0 - m[..., None])
    alpha = np.clip(np.maximum(m, outline), 0.0, 1.0)
    img = np.concatenate([rgb, alpha[..., None] * 255.0], axis=-1)
    big = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGBA')
    return big.resize((box_w, box_h), Image.LANCZOS)


def on_bg(img):
    bg = Image.new('RGBA', img.size, (70, 40, 40, 255))
    bg.alpha_composite(img)
    return bg.convert('RGB')


def measure(cp03):
    """The original names' cap height and baseline, to size ours alike."""
    a = np.asarray(cp03)[:, :, 3]
    rows = np.where(a[0:64, 0:120].max(axis=1) > 64)[0]
    return rows.min(), rows.max()


def compare(cp03_path, out):
    """The game's names, ours for Chris and Sheva in the same boxes (to match the style), and the
    new names."""
    cp03 = Image.open(cp03_path).convert('RGBA')
    width = SLOTS['sheva'][2]
    height = SLOTS['chris'][3]
    rows = [on_bg(cp03.crop((0, 0, width, height)))]
    for pair in (('Chris', 'Sheva'), ('Leon', 'Ada')):
        row = Image.new('RGBA', (width, height))
        for text, (x0, y0, x1, y1) in zip(pair, SLOTS.values()):
            row.alpha_composite(render(text, SIZE, FONT, x1 - x0, y1 - y0, BASELINE), (x0, y0))
        rows.append(on_bg(row))
    sheet = Image.new('RGB', (width, height * len(rows)))
    for i, r in enumerate(rows):
        sheet.paste(r, (0, height * i))
    sheet.resize((width * 3, height * 3 * len(rows)), Image.NEAREST).save(out)
    print(out)


def fingerprint(tex):
    """FNV-1a 64 of the raw DXT5 blocks under both name boxes (block rows by block rows), as the
    adapter reads them from the loaded texture to know it is a cp03 and which language's."""
    width = struct.unpack_from('<H', tex, 0x0C)[0]
    x1, y1 = SLOTS['sheva'][2], SLOTS['chris'][3]
    pitch = width // 4 * 16
    h = 0xcbf29ce484222325
    for by in range(y1 // 4):
        row = tex[0x2C + by * pitch: 0x2C + by * pitch + x1 // 4 * 16]
        for b in row:
            h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f'{h:016x}'


# The Russian HUD names its characters in Cyrillic.
NAMES_RUS = {'leon': 'Леон', 'ada': 'Ада'}


def build(tex_dir, out_dir):
    """tex_dir: every language's cp03 extracted by arc_extract.py (id_<lang>_cockpit_cp03_...)."""
    os.makedirs(out_dir, exist_ok=True)
    fingerprints = {}
    for f in sorted(os.listdir(tex_dir)):
        if 'cockpit_cp03_360' not in f:
            continue
        lang = f.split('_')[1]
        fingerprints[fingerprint(open(os.path.join(tex_dir, f), 'rb').read())] = 'rus' if lang == 'rus' else 'latin'
    names = {}
    for script, table in (('latin', NAMES), ('rus', NAMES_RUS)):
        for cid, name in table.items():
            for slot, (x0, y0, x1, y1) in SLOTS.items():
                tile = render(name, SIZE, FONT, x1 - x0, y1 - y0, BASELINE)
                file = f'{cid}_{slot}' + ('' if script == 'latin' else f'_{script}') + '.png'
                tile.save(os.path.join(out_dir, file))
                names.setdefault(cid, {}).setdefault(script, {})[slot] = file
    meta = {
        'about': 'The HUD names of characters that wear another one\'s costume slots, written over the game\'s '
                 '"Chris" and "Sheva" in the cockpit texture cp03 (scripts/hud_names.py).',
        'boxes': {k: list(v) for k, v in SLOTS.items()},
        'fingerprints': fingerprints,
        'names': names,
        # The inventory tabs and a cockpit label print the name as text (the executable's ASCII tables
        # at 0x011341E8 and 0x01123460); these stand in for "Chris" and "Sheva" there.
        'labels': dict(NAMES),
    }
    open(os.path.join(out_dir, 'hud_names.json'), 'w', encoding='utf-8', newline='\n').write(
        json.dumps(meta, indent=2, ensure_ascii=False) + '\n')
    print('built', sorted(names), 'fingerprints', len(fingerprints))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'compare':
        compare(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'build':
        build(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
