#!/usr/bin/env bash
# Developer check (ROADMAP M5.2): runs scripts/test_campaign_arena.sh on every campaign map of
# maps.json (or STAGES), one game launch each, and prints one line per stage: director spawns,
# failures, unloaded classes, fatal errors, chests, the mean distance to the player first and last, and the
# clear. DLL=<path> deploys a fixed build.
cd "$(dirname "$0")/.."
STAGES=${STAGES:-$(python -c "import json;print(' '.join(str(m['stage']) for m in json.load(open('configs/maps.json'))['maps'] if m['stage']>=100))")}
for stage in $STAGES; do
    DLL=${DLL:-} bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    out=$(NUMBER=$stage OBSERVE=15 bash scripts/test_campaign_arena.sh 2>&1)
    spawned=$(echo "$out" | grep -oE "^spawned: .*fatal: [0-9]+")
    dist=$(echo "$out" | grep -oE "mean distance to the player [0-9]+" | awk '{print $NF}' | sed -n '1p;$p' | tr '\n' ' ')
    chests=$(echo "$out" | grep -oE "chests: [0-9]+ of [0-9]+" | head -1)
    cleared=$(echo "$out" | grep -oE "arena cleared in [0-9]+ s" | head -1)
    echo "s$stage: $spawned | $chests | distance $dist| ${cleared:-NOT CLEARED} | $(echo "$out" | grep -oE 'captures in .*')"
done
