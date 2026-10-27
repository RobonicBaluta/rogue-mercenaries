#!/usr/bin/env python3
"""Developer: find the Mercenaries HUD sprites that show the countdown.

Run within the first 30 s of a match (the mod holds the countdown at 90 s after
that, so the digits stop changing). Snapshots the HUD's 97 child sprites, diffs
them a second later, maps the changed words to child slots, then nulls those
slots in the HUD for a moment and captures build/shots/timer_hidden.png.
"""
import os
import re
import subprocess
import time

DATA = os.path.join(os.environ["LOCALAPPDATA"], "RogueMercenaries")
LOG = os.path.join(DATA, "logs", "adapter.log")
GAMECTL = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "scripts/gamectl.ps1"]
CHILDREN = 0x180
COUNT = 97


def log_lines():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def dev(command, wait=1.1):
    with open(os.path.join(DATA, "dev.command"), "w") as f:
        f.write(command)
    time.sleep(wait)


def last_match(pattern):
    for line in reversed(log_lines()):
        m = re.search(pattern, line)
        if m:
            return m
    return None


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


hud = int(last_match(r"menu screen opened: uIdMerce00 obj=([0-9A-F]{8})").group(1), 16)
print(f"hud {hud:08X}")
ptrs = dump(hud + CHILDREN, COUNT + 3)[:COUNT]
live = sorted((p, i) for i, p in enumerate(ptrs) if p)
lo, hi = live[0][0], live[-1][0] + 0x300
dev(f"snap 0x{lo:08X} 0x{hi - lo:X}", 1.2)
time.sleep(1.0)
dev("diff", 1.5)
lines = log_lines()
start = max(i for i, l in enumerate(lines) if f"dev snap {lo:08X}" in l)
changed = {}
for line in lines[start:]:
    m = re.search(r"dev diff \+([0-9A-F]{5}): ([0-9A-F]{8}) -> ([0-9A-F]{8})", line)
    if not m:
        continue
    a = lo + int(m.group(1), 16)
    owner = max((p, i) for p, i in live if p <= a)
    changed.setdefault(owner[1], []).append((a - owner[0], m.group(2), m.group(3)))
print("changed children:", {i: v[:3] for i, v in changed.items()})
slots = sorted(changed)
capture("timer_visible")
saved = {i: ptrs[i] for i in slots}
for i in slots:
    dev(f"poke 0x{hud + CHILDREN + 4 * i:08X} 0", 0.6)
time.sleep(1.2)
capture("timer_hidden")
for i in slots:
    dev(f"poke 0x{hud + CHILDREN + 4 * i:08X} 0x{saved[i]:08X}", 0.6)
print("tested slots:", [f"{i} (+0x{CHILDREN + 4 * i:X})" for i in slots])
