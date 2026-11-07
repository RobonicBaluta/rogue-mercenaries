#!/usr/bin/env python3
"""Developer helper: edits a Rogue Mercenaries profile.json and writes it back with a valid checksum
(core/persistence/profile.cpp: "fnv1a64:" + FNV-1a 64 of the body's compact dump, keys sorted).

  python scripts/profile_edit.py PROFILE --check            the stored checksum matches (exit 0) or not (1)
  python scripts/profile_edit.py PROFILE --bosses a,b,c     stats.bosses_beaten = [a, b, c] ("" clears it)
  python scripts/profile_edit.py PROFILE --no-active-run    active_run = null

Only for tests on a backed-up profile: the result is checked against the original's format first.
"""
import argparse
import json
import sys


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def checksum(body: dict) -> str:
    text = json.dumps(body, sort_keys=True, separators=(',', ':'), ensure_ascii=False)
    return 'fnv1a64:%016x' % fnv1a64(text.encode('utf-8'))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--bosses')
    ap.add_argument('--no-active-run', action='store_true')
    a = ap.parse_args()
    with open(a.profile, encoding='utf-8') as f:
        doc = json.load(f)
    body = {k: v for k, v in doc.items() if k != 'checksum'}
    if checksum(body) != doc.get('checksum'):
        print('the stored checksum does not match this script\'s; not editing', file=sys.stderr)
        return 1
    if a.check:
        print('checksum ok')
        return 0
    if a.bosses is not None:
        body.setdefault('stats', {})['bosses_beaten'] = sorted(x for x in a.bosses.split(',') if x)
    if a.no_active_run:
        body['active_run'] = None
    body['checksum'] = checksum(body)
    with open(a.profile, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(body, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print('written', a.profile, body['checksum'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
