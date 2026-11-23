"""The M9 test matrix from the mod's own telemetry (the user's item 9, 2026-11-24: "every character x
host type x a full run, soak runs, save corruption").

Reads every rm_events*.jsonl in the given log folders and prints:
  1. characters x host x run length: what was played and how far (each cell: runs done, or the best arena),
     plus co-op and the plugin loader,
  2. soak: the longest runs (arenas, time) and the longest play session,
  3. saves: where each profile load came from (main, backup, default) and any load with errors,
  4. the cells still to play.

Usage (from the repository root):
  python scripts/test_matrix.py                         the PC's logs (%LOCALAPPDATA%\\RogueMercenaries\\logs)
  python scripts/test_matrix.py DIR=proton ...          more folders; =host names their host for runs from
                                                        0.3.0 and before (whose run_started has no "host")
  python scripts/test_matrix.py --since 2026-11-24      only runs started that day or later
  python scripts/test_matrix.py --version X.Y.Z         only runs of that release (after 0.3.0: the release's own number)
"""
import argparse
import glob
import json
import os
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LENGTHS = [('short', 'S'), ('long', 'L'), ('marathon', 'M'), ('endless', 'E')]
HOSTS = ['windows', 'proton']


def load_characters():
    with open(os.path.join(ROOT, 'configs', 'characters.json'), encoding='utf-8') as f:
        data = json.load(f)
    chars = data['characters'] if isinstance(data, dict) else data
    return [(c['id'], c.get('name', c['id']), bool(c.get('tester_only'))) for c in chars]


def read_events(folder):
    """The folder's events in time order (rotated files first: rm_events.5 .. rm_events.1, then rm_events)."""
    files = glob.glob(os.path.join(folder, 'rm_events*.jsonl'))
    lines = []
    for path in files:
        with open(path, encoding='utf-8', errors='replace') as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    lines.append(json.loads(raw))
                except json.JSONDecodeError:
                    continue
    lines.sort(key=lambda e: e.get('ts', ''))
    return lines


def parse_ts(ts):
    # "2026-11-22T16:12:52.592Z" -> seconds (good enough for differences within the logs)
    import datetime
    try:
        return datetime.datetime.strptime(ts[:19], '%Y-%m-%dT%H:%M:%S').timestamp()
    except (ValueError, TypeError):
        return None


def collect(folders, since, version):
    runs = []
    loads = []
    errors = defaultdict(int)
    sessions = []
    for folder, default_host in folders:
        open_runs = {}
        session_start = None
        last_ts = None
        for e in read_events(folder):
            ev = e.get('event')
            ts = e.get('ts', '')
            if since and ts[:10] < since:
                continue
            if version and e.get('mod_version') != version:
                continue
            t = parse_ts(ts)
            if ev == 'profile_loaded':
                loads.append((folder, ts, e.get('source', '?'), e.get('errors', 0)))
                # A profile load starts a game session (one per launch).
                if session_start is not None and last_ts is not None:
                    sessions.append((last_ts - session_start, folder, session_start))
                session_start = t
            if t is not None:
                last_ts = t
            if ev == 'error':
                errors[(e.get('operation') or e.get('code') or '?', str(e.get('message', ''))[:90])] += 1
            rid = e.get('run_id', '')
            if ev == 'run_started':
                r = {
                    'folder': folder, 'id': rid, 'character': e.get('character', '?'), 'skin': e.get('skin', ''),
                    'length': e.get('length', '?'), 'host': e.get('host') or default_host,
                    'loader': e.get('loader', '?'), 'plugins': e.get('plugins', ''), 'unlocked': e.get('unlocked', '?'),
                    'version': e.get('mod_version', '?'), 'start': t, 'end': None, 'ts': ts, 'players': [e.get('character', '?')],
                    'arenas': 0, 'completed': False, 'reason': 'open', 'bosses': 0,
                }
                runs.append(r)
                open_runs[rid] = r
            elif rid in open_runs:
                r = open_runs[rid]
                if ev == 'player_joined':
                    r['players'].append(e.get('character', '?'))
                elif ev == 'arena_completed':
                    r['arenas'] = max(r['arenas'], int(e.get('arena', 0) or 0))
                elif ev == 'boss_phase':
                    r['bosses'] += 1
                elif ev == 'run_ended':
                    r['completed'] = bool(e.get('completed'))
                    r['reason'] = e.get('reason', '?')
                    r['arenas'] = max(r['arenas'], int(e.get('arenas_cleared', 0) or 0))
                    r['end'] = t
                    del open_runs[rid]
        if session_start is not None and last_ts is not None:
            sessions.append((last_ts - session_start, folder, session_start))
    return runs, loads, errors, sessions


def cell(runs):
    if not runs:
        return '-'
    done = sum(1 for r in runs if r['completed'])
    if done:
        return f'{done}done'
    return f'a{max(r["arenas"] for r in runs)}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folders', nargs='*', help='log folders, each optionally DIR=host')
    ap.add_argument('--since', help='YYYY-MM-DD: only runs from that day')
    ap.add_argument('--version', help='only events of that mod_version')
    args = ap.parse_args()
    folders = []
    for spec in args.folders or [os.path.join(os.environ.get('LOCALAPPDATA', ''), 'RogueMercenaries', 'logs') + '=windows']:
        path, _, host = spec.partition('=')
        if not os.path.isdir(path):
            print(f'no folder {path}', file=sys.stderr)
            continue
        folders.append((path, host or '?'))
    if not folders:
        return 1
    runs, loads, errors, sessions = collect(folders, args.since, args.version)
    chars = load_characters()

    print(f'{len(runs)} runs in {len(folders)} folder(s)' + (f' since {args.since}' if args.since else '') +
          (f', version {args.version}' if args.version else ''))
    print('cells: Ndone = N runs finished; aN = the best arena reached (none finished); - = not played\n')

    # 1. The matrix.
    hosts = HOSTS + sorted({r['host'] for r in runs} - set(HOSTS))
    head = f'{"character":<18}' + ''.join(f'{h[:7] + " " + code:>11}' for h in hosts for _, code in LENGTHS) + f'{"co-op":>9}{"plugin":>9}'
    print(head)
    print('-' * len(head))
    missing = []
    for cid, name, tester in chars:
        mine = [r for r in runs if r['character'] == cid]
        row = f'{(name + (" (t)" if tester else ""))[:17]:<18}'
        for h in hosts:
            for length, code in LENGTHS:
                c = cell([r for r in mine if r['host'] == h and r['length'] == length])
                row += f'{c:>11}'
                if not c.endswith('done') and not tester and h in HOSTS and length != 'endless':
                    missing.append(f'{name} {length} on {h}' + ('' if c == '-' else f' (best: arena {c[1:]})'))
        coop = [r for r in runs if len(r['players']) > 1 and cid in r['players']]
        plugin = [r for r in mine if r['loader'] == 'plugin']
        row += f'{cell(coop):>9}{cell(plugin):>9}'
        print(row)

    # 2. Soak.
    print('\nlongest runs:')
    timed = [r for r in runs if r['start'] is not None and r['end'] is not None]
    for r in sorted(timed, key=lambda r: (r['arenas'], r['end'] - r['start']), reverse=True)[:5]:
        print(f'  {r["ts"][:16]} {r["character"]:<8} {r["length"]:<9} {r["arenas"]:>3} arenas, {(r["end"] - r["start"]) / 60:5.1f} min, '
              f'{r["reason"]}{" (co-op)" if len(r["players"]) > 1 else ""} [{r["host"]}]')
    if sessions:
        longest = max(sessions)
        print(f'longest session: {longest[0] / 3600:.1f} h ({longest[1]})')

    # 3. Saves.
    print('\nprofile loads:', ', '.join(f'{k} {v}' for k, v in sorted(_count(l[2] for l in loads).items())))
    bad = [l for l in loads if l[3] or l[2] not in ('main', 'default')]
    for folder, ts, source, errs in bad[-10:]:
        print(f'  {ts[:16]} from {source}, {errs} error(s) ({folder})')
    if errors:
        print('error events (most first):')
        for (op, msg), n in sorted(errors.items(), key=lambda kv: -kv[1])[:8]:
            print(f'  {n:>4} x {op}: {msg}')

    # 4. Still to play.
    print(f'\nno full run yet ({len(missing)} cells; endless and the tester left out):')
    for m in missing:
        print('  ' + m)
    return 0


def _count(items):
    out = defaultdict(int)
    for i in items:
        out[i] += 1
    return out


if __name__ == '__main__':
    sys.exit(main())
