#!/usr/bin/env bash
# Developer survey (ROADMAP M5): what campaign stages do when a Mercenaries match loads them
# (dev "stage <n>"): whether they keep running, which enemies their own sets spawn and where,
# and which objects they create. One game launch per stage; the director is off on overridden
# stages. Saves and restores the profile; closes the game at the end.
#   STAGES (default "100 102 203 304 503 607")   WATCH (seconds, default 75)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-campaign"
WATCH=${WATCH:-75}
mkdir -p "$OUT"
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
cp "$D/profile.json" "$D/profile.campaign-backup.json" || exit 1
for stage in ${STAGES:-100 102 203 304 503 607}; do
    bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    before=$(grep -c "" "$LOG")
    dev "survey on"; dev "stage $stage"; dev "arm 0 0"
    $PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
    shots=0
    for t in $(seq 25 25 "$WATCH"); do
        sleep 25
        timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/s$stage-t$t.png" >/dev/null 2>&1 && shots=$((shots + 1))
    done
    dev "survey off"
    tail -n +"$before" "$LOG" > "$OUT/s$stage.log"
    enemies=$(grep -c "director survey:" "$OUT/s$stage.log")
    classes=$(grep -oE "director survey: [A-Za-z0-9_]+" "$OUT/s$stage.log" | awk '{print $3}' | sort | uniq -c | tr -s ' ' | tr '\n' ',')
    objects=$(grep -c "world survey:" "$OUT/s$stage.log")
    echo "stage $stage: $shots captures, $enemies enemy spawns ($classes) $objects objects"
    cp "$D/profile.campaign-backup.json" "$D/profile.json"
done
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.campaign-backup.json" "$D/profile.json" && rm -f "$D/profile.campaign-backup.json"
echo "done; logs and captures in $OUT"
