#!/usr/bin/env python3
"""Draws the element badges (configs/element_badges/<element>[_evolved].png, 36x36 RGBA) that the
adapter puts in the corner of an element gun's inventory picture (gun_looks.cpp).

A dark disc with the element's ring and glyph in its colour (the colours of the mod's element text,
mod.cpp ElementColour); an evolved element's has a wider, brighter ring and a glow. The glyphs are
make_icons.py's shapes. Run from the repository root: python scripts/element_badges.py [--sheet]
(--sheet also writes build/element_badges_sheet.png on a grey ground, for review).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_icons  # noqa: E402

SIZE = 36
SS = 8
BIG = SIZE * SS
OUT = os.path.join('configs', 'element_badges')
ELEMENTS = {
    'incendiary': ((255, 140, 60), make_icons.fire),
    'shock': ((255, 230, 90), make_icons.bolt),
    'cryo': ((140, 215, 255), make_icons.snowflake),
    'explosive': ((255, 90, 80), make_icons.explosion),
    'toxic': ((140, 240, 100), make_icons.drop),
}


def badge(colour, shape, evolved):
    img = Image.new('RGBA', (BIG, BIG), (0, 0, 0, 0))
    c = BIG / 2
    r = BIG / 2 - SS * (2.5 if evolved else 1.5)
    if evolved:  # a glow of the element's colour around the disc
        glow = Image.new('RGBA', (BIG, BIG), (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse([c - r - SS * 2, c - r - SS * 2, c + r + SS * 2, c + r + SS * 2], fill=colour + (200,))
        img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(SS * 1.6)))
    d = ImageDraw.Draw(img)
    d.ellipse([c - r, c - r, c + r, c + r], fill=(10, 10, 12, 235))
    ring = SS * (2.6 if evolved else 1.6)
    d.ellipse([c - r, c - r, c + r, c + r], outline=colour + (255,), width=int(ring))
    # The glyph: the icon's shape in the element's colour, lighter at the top.
    glyph_size = int(r * 1.3)
    mask = shape().mask.resize((glyph_size, glyph_size), Image.LANCZOS)
    fill = Image.new('RGBA', (glyph_size, glyph_size))
    fd = ImageDraw.Draw(fill)
    for y in range(glyph_size):
        t = y / max(1, glyph_size - 1)
        k = 1.25 - 0.45 * t
        fd.line([(0, y), (glyph_size, y)], fill=tuple(min(255, int(v * k)) for v in colour) + (255,))
    at = (int(c - glyph_size / 2), int(c - glyph_size / 2))
    img.paste(fill, at, mask)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    made = {}
    for name, (colour, shape) in ELEMENTS.items():
        for evolved in (False, True):
            file = name + ('_evolved' if evolved else '') + '.png'
            made[file] = badge(colour, shape, evolved)
            made[file].save(os.path.join(OUT, file), optimize=True)
    print('%d badges in %s' % (len(made), OUT))
    if '--sheet' in sys.argv:
        sheet = Image.new('RGBA', (len(ELEMENTS) * 2 * (SIZE * 3 + 8) + 8, SIZE * 3 + 16), (90, 90, 90, 255))
        for i, img in enumerate(made.values()):
            sheet.alpha_composite(img.resize((SIZE * 3, SIZE * 3), Image.NEAREST), (8 + i * (SIZE * 3 + 8), 8))
        os.makedirs('build', exist_ok=True)
        sheet.save(os.path.join('build', 'element_badges_sheet.png'))
        print('sheet: build/element_badges_sheet.png')


if __name__ == '__main__':
    main()
