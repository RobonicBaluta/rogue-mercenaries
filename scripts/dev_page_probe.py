#!/usr/bin/env python3
"""Developer: find which GUI page draws the Mercenaries countdown.

Runs against a live match (developer build). Reads the GUI manager's page table
(92 handles at manager+0x20), clears each loaded page's visibility byte (+7 of
the page object) for a moment, captures the timer area and restores it. Writes
build/shots/page_sheet.png and prints the handles tested.
"""
import os
import re
import subprocess
import sys
import time

from PIL import Image, ImageDraw

DATA = os.path.join(os.environ["LOCALAPPDATA"], "RogueMercenaries")
LOG = os.path.join(DATA, "logs", "adapter.log")
GAMECTL = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "scripts/gamectl.ps1"]
MANAGER_PTR = 0x011B2334
TABLE = 0x20
SLOTS = 0x5C


def log_lines():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def dev(command, wait=1.1):
    with open(os.path.join(DATA, "dev.command"), "w") as f:
        f.write(command)
    time.sleep(wait)


def peek(address):
    dev(f"peek 0x{address:08X}")
    for line in reversed(log_lines()):
        m = re.search(r"dev peek %08X = ([0-9A-F]{8})" % address, line)
        if m:
            return int(m.group(1), 16)
    raise RuntimeError(f"peek {address:08X} not answered")


def dump(address, dwords):
    dev(f"dump 0x{address:08X} {dwords}", 1.5)
    lines = log_lines()
    start = max(i for i, l in enumerate(lines) if f"dev dump {address:08X}" in l)
    values = []
    for line in lines[start + 1:start + 1 + (dwords + 3) // 4]:
        m = re.search(r"\+[0-9A-F]{3}: ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})", line)
        if m:
            values += [int(x, 16) for x in m.groups()]
    return values[:dwords]


def capture(name):
    subprocess.run(GAMECTL + ["-Action", "capture", "-Out", f"build/shots/{name}.png"], capture_output=True)


manager = peek(MANAGER_PTR)
print(f"manager {manager:08X}")
handles = dump(manager + TABLE, SLOTS)
loaded = []
for handle, record in enumerate(handles):
    if not record:
        continue
    obj = peek(record + 8)
    if obj:
        loaded.append((handle, record, obj))
print("loaded pages:", [(h, f"{o:08X}") for h, _, o in loaded])
capture("page_base")
names = ["page_base"]
for handle, record, obj in loaded:
    flags = peek(obj + 4)  # dword holding byte +7 in its top byte
    vis = (flags >> 24) & 0xFF
    dev(f"pokeb 0x{obj + 7:08X} 0", 1.6)
    capture(f"page_{handle:02X}")
    dev(f"pokeb 0x{obj + 7:08X} 0x{vis:02X}", 0.8)
    names.append(f"page_{handle:02X}")
    print(f"page 0x{handle:02X} obj {obj:08X} visible byte was 0x{vis:02X}")

box = (600, 120, 2100, 420)
scale = 0.4
tw, th = int((box[2] - box[0]) * scale), int((box[3] - box[1]) * scale)
sheet = Image.new("RGB", (tw, th * len(names)), "black")
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(names):
    path = f"build/shots/{name}.png"
    if os.path.exists(path):
        sheet.paste(Image.open(path).convert("RGB").crop(box).resize((tw, th)), (0, i * th))
    draw.text((4, i * th + 4), name, fill="white")
sheet.save("build/shots/page_sheet.png")
print("sheet", sheet.size)
