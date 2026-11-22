"""Folds the map editor's changes (map_edits.json) into configs/maps.json, as the mod applies them
(core rm::ApplyMapEdit): `clear` drops the map's own points, `removed` drops every point within half
a metre of one, `added` points come after, `start` replaces the start, `start2` sets player 2's, `hidden`/`shown` go into or
out of "hide_models" (any case). A map left without points keeps its own.

  python scripts/fold_map_edits.py <map_edits.json> [--keep N ...] [--check]

--check only rewrites maps.json unchanged (the formatting round trip) and says whether it is equal.
Afterwards the edits are in maps.json: the game's own map_edits.json must be emptied, or its added
points would come twice.
"""
import argparse
import json
import re
import sys

MAPS = 'configs/maps.json'
MATCH = 50.0  # rm::kMapEditMatchRadius


def write_doc(doc):
    """configs/maps.json's own layout (scripts/campaign_maps.py write_doc)."""
    text = json.dumps(doc, indent=2)
    text = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', text)
    text = re.sub(r'"(arms|preload|layouts|hide_models|attack_chance)": \[([^\]]*)\]',
                  lambda m: f'"{m.group(1)}": [' + ', '.join(x.strip() for x in m.group(2).split(',')) + ']', text)
    text = re.sub(r'\{\s+"weight": (\d+),\s+"fields": \{([^}]*)\}\s+\}',
                  lambda m: '{"weight": ' + m.group(1) + ', "fields": {' + ', '.join(x.strip() for x in m.group(2).split(',') if x.strip()) + '}}', text)
    # A boss phase's adds, one per line.
    text = re.sub(r'\{\s+"class": "(uEm\w+)",\s+"count": (\d+)\s+\}', r'{"class": "\1", "count": \2}', text)
    return text + '\n'


def near(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) <= MATCH * MATCH


def apply(entry, edit):
    """The entry with the edit, and a line on what changed."""
    out = dict(entry)
    own = entry.get('points', [])
    added = [[round(v, 1) for v in p] for p in edit.get('added', [])]
    removed = edit.get('removed', [])
    if edit.get('clear'):
        points = [] if added else list(own)
    else:
        points = [p for p in own if not any(near(p, r) for r in removed)]
    points = points + added
    if points:
        out['points'] = points
    if edit.get('start'):
        out['start'] = [round(v, 1) for v in edit['start']]
    if edit.get('start2'):
        # Player 2's start, right after player 1's (or where it was).
        start2 = [round(v, 1) for v in edit['start2']]
        if 'start2' in out:
            out['start2'] = start2
        else:
            ordered = {}
            for key, value in out.items():
                ordered[key] = value
                if key == 'start':
                    ordered['start2'] = start2
            if 'start2' not in ordered:
                ordered['start2'] = start2
            out = ordered
    hide = list(entry.get('hide_models', []))
    shown = [s.lower() for s in edit.get('shown', [])]
    hide = [h for h in hide if h.lower() not in shown]
    for h in edit.get('hidden', []):
        if h.lower() not in [x.lower() for x in hide]:
            hide.append(h)
    if hide:
        out['hide_models'] = hide
    else:
        out.pop('hide_models', None)
    # Keep the key order: hide_models after layouts (or preload, or chests) when it is new.
    if 'hide_models' in out and 'hide_models' not in entry:
        ordered = {}
        anchor = 'layouts' if 'layouts' in out else ('preload' if 'preload' in out else 'chests')
        for k, v in out.items():
            if k == 'hide_models':
                continue
            ordered[k] = v
            if k == anchor:
                ordered['hide_models'] = out['hide_models']
        if 'hide_models' not in ordered:
            ordered['hide_models'] = out['hide_models']
        out = ordered
    line = (f"s{entry['stage']} {entry['name']}: points {len(own)} -> {len(out.get('points', own))}"
            f"{' (all its own out)' if edit.get('clear') else ''}"
            f"{', start moved' if edit.get('start') else ''}"
            f"{', player 2 start ' + str(out.get('start2')) if edit.get('start2') else ''}"
            f"{', hidden ' + str(out.get('hide_models')) if out.get('hide_models') != entry.get('hide_models') else ''}")
    return out, line


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('edits', nargs='?')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    original = open(MAPS, encoding='utf-8').read()
    doc = json.loads(original)
    if args.check:
        print('round trip', 'equal' if write_doc(doc) == original else 'DIFFERENT')
        return 0
    edits = json.load(open(args.edits, encoding='utf-8')).get('maps', {})
    by_stage = {m['stage']: i for i, m in enumerate(doc['maps'])}
    for key, edit in sorted(edits.items(), key=lambda kv: int(kv[0])):
        stage = int(key)
        if stage not in by_stage:
            print(f'map {stage}: not in maps.json, skipped')
            continue
        entry, line = apply(doc['maps'][by_stage[stage]], edit)
        doc['maps'][by_stage[stage]] = entry
        print(line)
    with open(MAPS, 'w', encoding='utf-8', newline='\n') as f:
        f.write(write_doc(doc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
