#!/usr/bin/env bash
# Developer survey (ROADMAP M4.1): where each Mercenaries stage places its time-bonus statues
# (uOm0100), one game launch per stage. Writes research/chest_survey.json and closes the game.
#   STAGES (default "0 1 2 3 4 5 6 7")
cd "$(dirname "$0")/.."
LOG="$LOCALAPPDATA/RogueMercenaries/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT=build/runs/chest-survey.txt
: > "$OUT"
for stage in ${STAGES:-0 1 2 3 4 5 6 7}; do
    bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    before=$(grep -c "" "$LOG")
    STAGE=$stage WAIT=4 bash scripts/survey_objects.sh >/dev/null 2>&1
    tail -n +"$before" "$LOG" | grep -E "world survey: uOm0100 " | sed "s/^/stage $stage: /" >> "$OUT"
    echo "stage $stage: $(grep -c "^stage $stage:" "$OUT") statues"
done
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
$PS scripts/gamectl.ps1 -Action reap >/dev/null
python - "$OUT" <<'PY'
import json, re, sys
points = {}
for line in open(sys.argv[1], encoding='utf-8'):
    m = re.match(r'stage (\d+): .*world survey: uOm0100 \(sObjModel\) set \d+ id \d+ at ([-\d.]+) ([-\d.]+) ([-\d.]+) .*\+1364 (\d+)', line)
    if m:
        points.setdefault(m.group(1), []).append({"pos": [float(m.group(2)), float(m.group(3)), float(m.group(4))], "seconds": int(m.group(5))})
json.dump({"source": "scripts/survey_chests.sh (uOm0100 set spawns at match start)", "stages": points},
          open('research/chest_survey.json', 'w'), indent=1)
print({k: len(v) for k, v in points.items()})
PY
echo "done (game closed)"
