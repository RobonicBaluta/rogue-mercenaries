#!/usr/bin/env python3
"""Draws the menu icons (configs/icons/<name>.png, 128x128 RGBA) from simple shapes.

Original art made from polygons and ellipses: a light metal gradient silhouette with a dark
outline and a soft shadow, in the manner of the game's menu text. The adapter tints each icon
like the row it sits on. Run from the repository root: python scripts/make_icons.py [--sheet]
(--sheet also writes build/icons_sheet.png, every icon on a dark background, for review).
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 128
SS = 4                    # supersampling
CANVAS = SIZE * SS
UNIT = CANVAS / 100.0     # shapes are drawn in a 100 x 100 space
OUT = os.path.join('configs', 'icons')


def P(points):
    return [(x * UNIT, y * UNIT) for x, y in points]


def spline(points, steps=8, closed=True):
    """Catmull-Rom curve through the points (a smooth outline from a few control points)."""
    pts = list(points)
    n = len(pts)
    out = []
    last = n if closed else n - 1
    for i in range(last):
        p0 = pts[(i - 1) % n] if closed or i > 0 else pts[0]
        p1, p2 = pts[i], pts[(i + 1) % n]
        p3 = pts[(i + 2) % n] if closed or i + 2 < n else pts[-1]
        for s in range(steps):
            t = s / steps
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
                                    (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3) for k in range(2)))
    return out


def rotate(points, degrees, cx=50.0, cy=50.0):
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


class Shape:
    """A mask built from filled (add) and cut (cut) primitives in the 100-unit space."""

    def __init__(self):
        self.mask = Image.new('L', (CANVAS, CANVAS), 0)
        self.d = ImageDraw.Draw(self.mask)

    def poly(self, points, cut=False):
        self.d.polygon(P(points), fill=0 if cut else 255)

    def ellipse(self, cx, cy, rx, ry, cut=False):
        self.d.ellipse([(cx - rx) * UNIT, (cy - ry) * UNIT, (cx + rx) * UNIT, (cy + ry) * UNIT], fill=0 if cut else 255)

    def rect(self, x0, y0, x1, y1, r=0.0, cut=False):
        self.d.rounded_rectangle([x0 * UNIT, y0 * UNIT, x1 * UNIT, y1 * UNIT], radius=r * UNIT, fill=0 if cut else 255)

    def ring(self, cx, cy, r, width, cut=False):
        self.d.ellipse([(cx - r) * UNIT, (cy - r) * UNIT, (cx + r) * UNIT, (cy + r) * UNIT], outline=0 if cut else 255,
                       width=max(1, int(width * UNIT)))

    def line(self, points, width, cut=False):
        self.d.line(P(points), fill=0 if cut else 255, width=max(1, int(width * UNIT)), joint='curve')
        for x, y in (points[0], points[-1]):  # round caps
            self.ellipse(x, y, width / 2, width / 2, cut)


def render(shape):
    mask = shape.mask
    outline_px = int(3.2 * UNIT)
    grown = mask.filter(ImageFilter.MaxFilter(outline_px | 1))
    # Shadow: the outlined shape, blurred, a little down and right.
    shadow = grown.filter(ImageFilter.GaussianBlur(2.5 * UNIT))
    shadow = ImageChops.offset(shadow, int(1.2 * UNIT), int(2.0 * UNIT))
    img = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    img.paste((0, 0, 0, 255), (0, 0), shadow.point(lambda v: int(v * 0.65)))
    img.paste((14, 14, 14, 255), (0, 0), grown)
    # Fill: a vertical metal gradient with a faint bright band near the top.
    grad = Image.new('RGBA', (CANVAS, CANVAS))
    gd = ImageDraw.Draw(grad)
    for y in range(CANVAS):
        t = y / (CANVAS - 1)
        v = 246 - 92 * t + (14 if 0.18 < t < 0.30 else 0)
        v = max(0, min(255, int(v)))
        gd.line([(0, y), (CANVAS, y)], fill=(v, v, int(v * 0.97), 255))
    img.paste(grad, (0, 0), mask)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


# ---- the icons ------------------------------------------------------------------------------

def grenade():
    s = Shape()
    s.ellipse(48, 62, 25, 29)
    s.rect(38, 24, 58, 36, 2)
    # grooves of the body
    for y in (52, 64, 76):
        s.line([(24, y), (72, y)], 2.4, cut=True)
    s.line([(48, 34), (48, 92)], 2.4, cut=True)
    # spoon along the right side and the pin ring
    s.poly([(56, 22), (66, 22), (74, 30), (73, 60), (67, 60), (67, 33), (60, 28)])
    s.ring(31, 24, 8, 3.4)
    return s


def flash():
    s = Shape()
    s.rect(37, 42, 63, 94, 4)
    s.rect(41, 34, 59, 43, 2)
    s.poly([(55, 32), (64, 32), (70, 39), (69, 64), (64, 64), (64, 41), (58, 37)])
    s.line([(37, 56), (63, 56)], 2.2, cut=True)
    s.line([(37, 80), (63, 80)], 2.2, cut=True)
    for a in (-162, -126, -90, -54, -18):
        r = math.radians(a)
        s.line([(50 + 18 * math.cos(r), 36 + 18 * math.sin(r)), (50 + 31 * math.cos(r), 36 + 31 * math.sin(r))], 4.4)
    return s


def fire():
    s = Shape()
    outer = [(50, 6), (60, 22), (70, 30), (76, 46), (78, 60), (72, 80), (58, 93), (42, 93), (28, 82), (22, 64),
             (26, 46), (34, 34), (38, 46), (42, 30)]
    s.poly(spline(outer, 10))
    inner = [(52, 44), (60, 58), (63, 72), (57, 85), (47, 87), (39, 78), (40, 64), (46, 56)]
    s.poly(spline(inner, 10), cut=True)
    s.poly(spline([(50, 60), (56, 70), (55, 80), (49, 83), (44, 77), (45, 68)], 10))
    return s


def medkit():
    s = Shape()
    s.rect(33, 30, 67, 92, 6)
    s.rect(39, 18, 61, 30, 3)
    s.rect(58, 18, 74, 25, 2)
    s.rect(46, 50, 54, 78, 0, cut=True)
    s.rect(36, 60, 64, 68, 0, cut=True)
    return s


def heart_points():
    pts = []
    for i in range(72):
        t = i / 72 * 2 * math.pi
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((50 + x * 2.65, 52 - y * 2.65))
    return pts


def heal():
    s = Shape()
    s.poly(heart_points())
    s.rect(45, 30, 55, 66, 0, cut=True)
    s.rect(32, 43, 68, 53, 0, cut=True)
    return s


def second_wind():
    s = Shape()
    s.poly(heart_points())
    s.line([(14, 50), (34, 50), (41, 36), (49, 66), (57, 30), (63, 50), (86, 50)], 5.5, cut=True)
    return s


def ammo():
    s = Shape()
    for x in (26, 50, 74):
        s.rect(x - 9, 46, x + 9, 92, 1.5)
        s.poly(spline([(x - 9, 47), (x - 8, 34), (x - 4, 22), (x, 16), (x + 4, 22), (x + 8, 34), (x + 9, 47)], 6, closed=False)
               + [(x + 9, 48), (x - 9, 48)])
        s.line([(x - 9, 50), (x + 9, 50)], 2.2, cut=True)
        s.line([(x - 9, 84), (x + 9, 84)], 2.2, cut=True)
    return s


def gun(points, extra=(), cuts=(), angle=-32):
    s = Shape()
    s.poly(rotate(points, angle))
    for e in extra:
        s.poly(rotate(e, angle))
    for c in cuts:
        s.poly(rotate(c, angle), cut=True)
    return s


def shotgun():
    body = [(2, 46), (10, 42), (30, 40), (36, 43), (98, 43), (98, 49), (62, 49), (62, 52), (40, 52), (38, 56),
            (34, 63), (28, 63), (31, 55), (24, 53), (4, 60)]
    pump = [(56, 49), (84, 49), (84, 55), (56, 55)]
    return gun(body, [pump], [[(29, 50), (35, 50), (33, 57), (30, 57)]])


def smg():
    body = [(14, 40), (24, 38), (30, 34), (78, 34), (78, 38), (96, 38), (96, 43), (78, 43), (76, 48), (60, 48),
            (58, 52), (50, 52), (46, 46), (40, 48), (36, 58), (28, 58), (30, 48), (14, 50)]
    mag = [(48, 50), (56, 50), (60, 76), (52, 78)]
    stock = [(4, 38), (14, 38), (14, 41), (8, 41), (8, 47), (14, 47), (14, 50), (4, 50)]
    return gun(body, [mag, stock], [[(40, 47), (44, 47), (42, 52)]], angle=-28)


def magnum():
    s = Shape()
    a = -22
    s.poly(rotate([(54, 36), (97, 36), (97, 43), (54, 43)], a))                      # barrel
    s.poly(rotate([(92, 32), (96, 32), (96, 36), (92, 36)], a))                      # front sight
    s.poly(rotate(spline([(38, 33), (58, 32), (60, 41), (58, 50), (38, 50)], 6), a))  # frame and cylinder
    s.poly(rotate([(33, 28), (39, 28), (42, 35), (36, 36)], a))                      # hammer
    s.poly(rotate(spline([(36, 47), (47, 50), (45, 58), (41, 70), (38, 80), (26, 82), (24, 74), (29, 58)], 6), a))  # grip
    s.poly(rotate([(47, 50), (54, 50), (55, 55), (50, 60), (44, 60), (45, 56)], a))  # trigger guard
    s.poly(rotate([(47, 52), (51, 52), (49, 57)], a), cut=True)
    for x in (44, 50):
        s.line(rotate([(x, 35), (x, 48)], a), 2.0, cut=True)                        # cylinder flutes
    return s


def rifle():
    body = [(2, 44), (8, 40), (32, 42), (40, 42), (40, 40), (98, 40), (98, 44), (60, 44), (58, 48), (40, 48),
            (36, 56), (31, 56), (33, 48), (26, 48), (4, 56)]
    scope = [(42, 30), (68, 30), (70, 33), (66, 35), (60, 35), (60, 39), (52, 39), (52, 35), (44, 35), (40, 33)]
    mag = [(44, 47), (50, 47), (52, 58), (46, 58)]
    return gun(body, [scope, mag], [[(32, 49), (36, 49), (34, 54)]], angle=-30)


def handgun():
    s = Shape()
    a = -18
    s.poly(rotate([(30, 34), (90, 34), (90, 46), (30, 46)], a))                      # slide
    s.poly(rotate([(84, 30), (88, 30), (88, 34), (84, 34)], a))                      # front sight
    s.poly(rotate([(30, 46), (64, 46), (64, 50), (30, 50)], a))                      # frame
    s.poly(rotate(spline([(30, 48), (46, 48), (44, 60), (40, 76), (24, 78), (22, 70), (28, 56)], 6), a))  # grip
    s.poly(rotate([(46, 48), (60, 48), (58, 56), (52, 62), (44, 62)], a))            # trigger guard
    s.poly(rotate([(48, 51), (55, 51), (52, 58), (47, 58)], a), cut=True)
    for x in (38, 44, 50):
        s.line(rotate([(x, 37), (x, 43)], a), 2.0, cut=True)                        # slide serrations
    return s


def credits():
    s = Shape()
    w = max(1, int(3.0 * UNIT))
    for y in (78, 64, 50, 36):                                                       # a stack of coins, bottom up
        s.rect(18, y, 82, y + 10, 0)
        s.ellipse(50, y + 10, 32, 8)
        s.ellipse(50, y, 32, 8)
        # The rim of this coin's top face; the next coin covers its upper half.
        s.d.ellipse([18 * UNIT, (y - 8) * UNIT, 82 * UNIT, (y + 8) * UNIT], outline=0, width=w)
    s.ellipse(50, 36, 30, 6.5)
    s.ellipse(50, 36, 20, 4, cut=True)
    s.ellipse(50, 36, 13, 2.2)
    return s


def clock():
    s = Shape()
    s.rect(43, 6, 57, 14, 2)                                                         # stopwatch button
    s.rect(47, 12, 53, 22, 0)
    s.ellipse(50, 56, 38, 38)
    s.ellipse(50, 56, 30, 30, cut=True)
    s.line([(50, 56), (50, 34)], 5.0)                                                # hands
    s.line([(50, 56), (66, 64)], 5.0)
    s.ellipse(50, 56, 5, 5)
    for k in range(12):
        ang = math.radians(k * 30)
        r0, r1 = (22, 27) if k % 3 else (19, 27)
        s.line([(50 + r0 * math.sin(ang), 56 - r0 * math.cos(ang)), (50 + r1 * math.sin(ang), 56 - r1 * math.cos(ang))], 2.4)
    return s


def flag():
    s = Shape()
    s.rect(18, 8, 25, 94, 2)                                                         # pole
    s.ellipse(21.5, 8, 6, 6)
    cloth = [(25, 14), (40, 10), (58, 16), (76, 12), (88, 14), (84, 34), (88, 54), (72, 52), (56, 58), (40, 52), (25, 56)]
    s.poly(spline(cloth, 6))
    s.poly(star(54, 34, 11, 4.5), cut=True)
    return s


def trophy():
    s = Shape()
    s.poly(spline([(24, 10), (76, 10), (74, 34), (64, 52), (50, 58), (36, 52), (26, 34)], 6))  # cup
    s.ring(22, 26, 12, 5.0)                                                          # handles
    s.ring(78, 26, 12, 5.0)
    s.rect(24, 10, 76, 14, 0)
    s.rect(45, 56, 55, 72, 0)                                                        # stem
    s.rect(32, 72, 68, 80, 2)                                                        # base
    s.rect(26, 80, 74, 90, 2)
    s.poly(star(50, 30, 12, 5), cut=True)
    return s


def person():
    s = Shape()
    s.ellipse(50, 32, 18, 21)                                                        # head
    s.poly(spline([(14, 94), (16, 72), (30, 60), (50, 56), (70, 60), (84, 72), (86, 94)], 6))  # shoulders
    s.ellipse(50, 56, 20, 6, cut=True)                                               # neck gap
    s.ellipse(50, 32, 18, 21)
    return s


def mine():
    s = Shape()
    s.ellipse(50, 66, 40, 16)                                                        # base disc
    s.rect(10, 56, 90, 66, 0)
    s.ellipse(50, 56, 40, 14)
    s.ellipse(50, 56, 30, 9, cut=True)
    s.ellipse(50, 52, 22, 14)                                                        # dome
    s.rect(47, 22, 53, 42, 1)                                                        # sensor
    s.ellipse(50, 20, 7, 7)
    s.ellipse(50, 20, 3, 3, cut=True)
    for x in (22, 78):
        s.line([(x, 70), (x, 76)], 2.6, cut=True)
    return s


def gatling():
    s = Shape()
    a = -24
    for dy in (-6, 0, 6):                                                            # barrels
        s.line(rotate([(46, 46 + dy), (96, 46 + dy)], a), 4.0)
    s.poly(rotate([(84, 36), (90, 36), (90, 56), (84, 56)], a))                      # muzzle ring
    s.poly(rotate(spline([(14, 34), (48, 34), (52, 46), (48, 58), (14, 58), (8, 46)], 6), a))  # body
    s.poly(rotate([(22, 58), (32, 58), (30, 80), (20, 80)], a))                      # grip
    s.poly(rotate([(32, 26), (44, 26), (44, 34), (32, 34)], a))                      # carry handle
    s.line(rotate([(20, 46), (40, 46)], a), 2.4, cut=True)
    return s


def bow():
    s = Shape()
    arc = spline([(30, 8), (52, 22), (60, 50), (52, 78), (30, 92)], 8, closed=False)
    s.line(arc, 6.0)
    s.line([(30, 8), (30, 92)], 1.8)                                                 # string
    s.line([(12, 50), (86, 50)], 3.0)                                                # arrow
    s.poly([(96, 50), (84, 43), (84, 57)])                                           # head
    s.poly([(8, 44), (18, 50), (8, 56), (14, 50)])                                   # fletching
    return s


def key():
    s = Shape()
    s.ring(28, 50, 17, 7.0)                                                          # bow of the key
    s.rect(42, 45, 92, 55, 2)                                                        # shaft
    s.rect(70, 55, 78, 70, 1)                                                        # bit
    s.rect(82, 55, 90, 66, 1)
    return s


def dash():
    s = Shape()
    s.poly([(44, 18), (88, 50), (44, 82), (56, 50)])                                 # chevron
    s.poly([(20, 26), (54, 50), (20, 74), (30, 50)])
    for y, x0 in ((30, 4), (50, 0), (70, 4)):                                        # speed lines
        s.line([(x0, y), (x0 + 12, y)], 3.0)
    return s


def dual_smg():
    gun = smg().mask.resize((int(CANVAS * 0.82), int(CANVAS * 0.82)), Image.LANCZOS)
    s = Shape()
    back = Image.new('L', (CANVAS, CANVAS), 0)
    back.paste(gun, (int(-6 * UNIT), int(-4 * UNIT)))
    front = Image.new('L', (CANVAS, CANVAS), 0)
    front.paste(gun, (int(22 * UNIT), int(26 * UNIT)))
    # The front gun cuts a gap into the back one so the two read apart.
    gap = front.filter(ImageFilter.MaxFilter(int(4 * UNIT) | 1))
    s.mask = ImageChops.lighter(ImageChops.subtract(back, gap), front)
    s.d = ImageDraw.Draw(s.mask)
    return s


def radio():
    s = Shape()
    s.rect(30, 30, 72, 92, 6)                                                        # body
    s.rect(58, 6, 64, 32, 2)                                                         # antenna
    s.rect(38, 40, 64, 60, 3, cut=True)                                              # screen
    for y in (68, 76, 84):                                                           # speaker grille
        s.line([(40, y), (62, y)], 2.4, cut=True)
    s.rect(24, 50, 30, 64, 1)                                                        # side button
    return s


def fist():
    s = Shape()
    s.rect(22, 34, 80, 80, 10)                                                       # back of the hand
    for x in (30, 44, 58, 72):                                                       # knuckles
        s.ellipse(x, 34, 8, 9)
    for x in (37, 51, 65):                                                           # finger gaps
        s.line([(x, 30), (x, 52)], 2.4, cut=True)
    s.poly(spline([(16, 52), (30, 50), (44, 58), (36, 70), (18, 66)], 6))           # thumb
    s.rect(34, 80, 70, 94, 3)                                                        # wrist
    return s


def crown():
    s = Shape()
    s.poly([(10, 34), (30, 56), (50, 18), (70, 56), (90, 34), (82, 78), (18, 78)])
    s.rect(16, 80, 84, 90, 2)
    for x, y in ((10, 32), (50, 16), (90, 32)):                                      # jewels on the points
        s.ellipse(x, y, 6, 6)
    s.ellipse(50, 62, 6, 6, cut=True)
    return s


def big_star():
    s = Shape()
    s.poly(star(50, 54, 44, 18))
    s.poly(star(50, 54, 26, 11), cut=True)
    s.poly(star(50, 54, 14, 6))
    return s


def crosshair():
    s = Shape()
    s.ring(50, 50, 30, 5.5)
    for (x0, y0, x1, y1) in ((50, 6, 50, 30), (50, 70, 50, 94), (6, 50, 30, 50), (70, 50, 94, 50)):
        s.line([(x0, y0), (x1, y1)], 5.5)
    s.ellipse(50, 50, 5.5, 5.5)
    return s


def supplies():
    s = Shape()
    s.rect(14, 30, 86, 88, 2.5)
    s.rect(10, 22, 90, 34, 2)
    s.line([(10, 34), (90, 34)], 2.6, cut=True)
    for y in (52, 70):
        s.line([(14, y), (86, y)], 2.4, cut=True)
    for x in (24, 76):
        s.line([(x, 36), (x, 86)], 2.4, cut=True)
    s.rect(43, 38, 57, 48, 1.5, cut=True)
    s.rect(46, 41, 54, 45, 1)
    return s


def last_stand():
    s = Shape()
    s.ellipse(50, 42, 30, 30)
    s.rect(32, 56, 68, 82, 6)
    s.ellipse(38, 46, 8, 9, cut=True)
    s.ellipse(62, 46, 8, 9, cut=True)
    s.poly([(50, 56), (45, 66), (55, 66)], cut=True)
    for x in (40, 50, 60):
        s.line([(x, 72), (x, 84)], 2.6, cut=True)
    return s


def shield():
    s = Shape()
    outline = [(50, 8), (66, 14), (84, 16), (84, 46), (76, 68), (62, 84), (50, 92), (38, 84), (24, 68), (16, 46),
               (16, 16), (34, 14)]
    s.poly(spline(outline, 6))
    inner = [(50, 18), (63, 23), (76, 24), (76, 46), (70, 64), (59, 77), (50, 83), (41, 77), (30, 64), (24, 46),
             (24, 24), (37, 23)]
    s.poly(spline(inner, 6), cut=True)
    s.poly([(28, 34), (50, 52), (72, 34), (72, 46), (50, 64), (28, 46)])            # chevron
    s.poly([(34, 58), (50, 71), (66, 58), (64, 66), (50, 77), (36, 66)])
    return s


def star(cx, cy, r_out, r_in, points=5, rot=-90):
    pts = []
    for i in range(points * 2):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rot + i * 180 / points)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def medal():
    s = Shape()
    s.poly([(30, 6), (46, 6), (56, 40), (44, 46)])
    s.poly([(54, 6), (70, 6), (56, 46), (44, 40)])
    s.line([(46, 6), (46, 20)], 2.0, cut=True)
    s.ellipse(50, 64, 28, 28)
    s.ellipse(50, 64, 22, 22, cut=True)
    s.poly(star(50, 65, 18, 7.5))
    return s


def burst(cx, cy, r_out, r_in, points, jitter):
    pts = []
    for i in range(points * 2):
        r = (r_out if i % 2 == 0 else r_in) * (1.0 + jitter[i % len(jitter)])
        a = math.radians(-90 + i * 180 / points)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def explosion():
    s = Shape()
    jitter = [0.0, 0.05, -0.12, 0.0, 0.06, -0.05, -0.08, 0.04, 0.03, -0.1, 0.07, 0.0]
    s.poly(burst(50, 52, 46, 28, 12, jitter))
    s.poly(burst(50, 52, 29, 19, 12, jitter[::-1]), cut=True)
    s.poly(burst(50, 52, 19, 11, 9, jitter))
    return s


ICONS = {
    'grenade': grenade, 'flash': flash, 'fire': fire, 'medkit': medkit, 'heal': heal, 'second_wind': second_wind,
    'ammo': ammo, 'shotgun': shotgun, 'smg': smg, 'magnum': magnum, 'rifle': rifle, 'crosshair': crosshair,
    'supplies': supplies, 'last_stand': last_stand, 'shield': shield, 'medal': medal, 'explosion': explosion,
    'handgun': handgun, 'credits': credits, 'clock': clock, 'flag': flag, 'trophy': trophy, 'person': person,
    'mine': mine, 'gatling': gatling, 'bow': bow, 'key': key, 'dash': dash, 'dual_smg': dual_smg, 'radio': radio,
    'fist': fist, 'crown': crown, 'star': big_star,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    images = {}
    for name, make in ICONS.items():
        images[name] = render(make())
        images[name].save(os.path.join(OUT, name + '.png'), optimize=True)
    print('%d icons in %s' % (len(images), OUT))
    if '--sheet' in sys.argv:
        cols = 6
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new('RGBA', (cols * (SIZE + 16) + 16, rows * (SIZE + 16) + 16), (38, 36, 32, 255))
        for i, (name, img) in enumerate(images.items()):
            x = 16 + (i % cols) * (SIZE + 16)
            y = 16 + (i // cols) * (SIZE + 16)
            sheet.alpha_composite(img, (x, y))
        os.makedirs('build', exist_ok=True)
        sheet.save(os.path.join('build', 'icons_sheet.png'))
        print('sheet: build/icons_sheet.png')


if __name__ == '__main__':
    main()
