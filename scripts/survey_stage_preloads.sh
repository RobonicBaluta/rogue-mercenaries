#!/usr/bin/env bash
# Developer survey (ROADMAP M5.2): the archives each Mercenaries stage loads for its enemies
# (loader list 4: classes, the weapons they carry, plagas, effects), so a campaign area that
# borrows the stage's roster can load the same (maps.json "preload"). One launch per stage;
# writes research/stage_preloads.json. DLL=<path> deploys a fixed build. Saves and restores the
# profile; closes the game at the end.   STAGES (default "0 1 2 3 4 5 6 7")
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT=build/runs/stage-preloads.txt
: > "$OUT"
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
cp "$D/profile.json" "$D/profile.preloads-backup.json" || exit 1
for stage in ${STAGES:-0 1 2 3 4 5 6 7}; do
    DLL=${DLL:-} bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    before=$(grep -c "" "$LOG")
    dev "quiet on"; dev "arm $stage 0"
    $PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
    for _ in $(seq 1 60); do tail -n +"$before" "$LOG" | grep -q "phase idle -> arena_active" && break; sleep 1; done
    sleep 3
    dev loadlists
    tail -n +"$before" "$LOG" | grep -E "resources: list 4 " | grep -v "resource 00000000" | sed "s/^/stage $stage: /" >> "$OUT"
    echo "stage $stage: $(grep -c "^stage $stage:" "$OUT") enemy archives"
    cp "$D/profile.preloads-backup.json" "$D/profile.json"
done
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.preloads-backup.json" "$D/profile.json" && rm -f "$D/profile.preloads-backup.json"
python - "$OUT" <<'PY'
import json, re, sys
stages = {}
for line in open(sys.argv[1], encoding='utf-8'):
    m = re.match(r'stage (\d+): .*resources: list 4 Image.Archive.(\S+)', line)
    if m:
        names = stages.setdefault(m.group(1), [])
        if m.group(2) not in names:
            names.append(m.group(2))
json.dump({'source': 'scripts/survey_stage_preloads.sh (loader list 4 in a run on each Mercenaries stage)', 'stages': stages},
          open('research/stage_preloads.json', 'w', newline='\n'), indent=1)
print({k: len(v) for k, v in stages.items()})
PY
echo done
