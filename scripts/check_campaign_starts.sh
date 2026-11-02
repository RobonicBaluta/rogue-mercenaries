#!/usr/bin/env bash
# Developer check (ROADMAP M5.2): where the player starts on each campaign map when it is a run's
# later arena (after a stage change; scripts/test_rotation.sh with FIRST=3, a Mercenaries stage)
# compared with the start recorded for its first arena (STARTS in scripts/campaign_maps.py, where
# the map's points and chests are), and whether the director's enemies close in. One game launch
# per map.   STAGES (default: every campaign map of maps.json)   DLL=<path> deploys a fixed build.
cd "$(dirname "$0")/.."
STAGES=${STAGES:-$(python -c "import json;print(' '.join(str(m['stage']) for m in json.load(open('configs/maps.json'))['maps'] if m['stage']>=100))")}
for stage in $STAGES; do
    DLL=${DLL:-} bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    out=$(FIRST=3 SECOND=$stage bash scripts/test_rotation.sh 2>&1)
    arena2=$(echo "$out" | sed -n '/route_selection -> arena_active/,$p')
    pos=$(echo "$arena2" | grep -oE "player at [-0-9. ]+" | head -1 | cut -c11-)
    dist=$(echo "$arena2" | grep -oE "mean distance to the player [0-9]+" | awk '{print $NF}' | sed -n '1p;$p' | tr '\n' ' ')
    python - "$stage" "$pos" "$dist" <<'PY'
import math, re, sys
stage, pos, dist = int(sys.argv[1]), sys.argv[2].split(), sys.argv[3]
starts = re.search(r'STARTS = \{(.*?)\n\}', open('scripts/campaign_maps.py', encoding='utf-8').read(), re.S).group(1)
known = {int(k): tuple(map(float, v.split(','))) for k, v in re.findall(r'(\d+): \(([^)]*)\)', starts)}
if len(pos) != 3:
    print(f's{stage}: no position (arena 2 did not start?)')
else:
    p = tuple(map(float, pos))
    k = known.get(stage)
    off = f'{math.dist(p, k) / 100:.0f} m from the recorded start' if k else 'no recorded start'
    print(f's{stage}: later-arena start {p[0]:.0f} {p[1]:.0f} {p[2]:.0f} ({off}) | distance {dist}')
PY
done
