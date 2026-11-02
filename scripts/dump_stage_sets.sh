#!/usr/bin/env bash
# Developer survey (ROADMAP M5): loads stage NUMBER through the host (dev "stage"), quiet, and
# logs every set record of the stage (dev "setdump": enemies, objects, items) plus where the
# player stands. Needs the game at the Play menu with a dev adapter config. Saves and restores
# the profile; closes the game at the end. Output: build/runs/<stamp>-sets<NUMBER>/sets.log
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:-503}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-sets$NUMBER"
mkdir -p "$OUT"
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
cp "$D/profile.json" "$D/profile.sets-backup.json" || exit 1
before=$(grep -c "" "$LOG")
[ "$NUMBER" -ge 0 ] && dev "stage $NUMBER"
dev "quiet on"; dev "arm ${CHOICE:-0} 0"
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
for _ in $(seq 1 60); do tail -n +"$before" "$LOG" | grep -q "phase idle -> arena_active" && break; sleep 1; done
sleep 5
dev setdump
sleep 2
timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/start.png" >/dev/null 2>&1
tail -n +"$before" "$LOG" | grep "setdump:" | cut -c40- > "$OUT/sets.log"
grep "setdump: player\|records" "$OUT/sets.log"
grep -oE "setdump: [A-Za-z]+ set [0-9]+ id [0-9]+ [A-Za-z0-9_]+ \([A-Za-z0-9_]+\)" "$OUT/sets.log" | awk '{print $2, $8, $9}' | sort | uniq -c | sort -rn | head -25
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.sets-backup.json" "$D/profile.json" && rm -f "$D/profile.sets-backup.json"
echo "done; $OUT"
