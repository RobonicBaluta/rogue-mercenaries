#!/usr/bin/env bash
# Developer spike (ROADMAP M5.1 / E6): a Mercenaries match loads another stage number (a
# campaign area, s1xx..s6xx) through the match setup's stage field. Starts a run with the
# director quiet (its spawn points belong to the Mercenaries map), then captures what loads.
# A missing capture means the game stopped presenting (hang). Needs the game at the Play menu
# with a dev adapter config (build/restart_to_play_menu.sh configs/adapter.director.json).
# Saves and restores the profile; closes (or kills) the game at the end.
#   NUMBER (stage number, default 100)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:-100}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-stage$NUMBER"
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1): not presenting"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.stage-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "stage $NUMBER"
dev "quiet on"
dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
for t in 15 30 45 60 90; do
    sleep 15; [ $t = 90 ] && sleep 15
    cap "t$t"
done
since | grep -E "host:|mercenaries match object|phase |resources: list|ERROR|crash|exception|run started|start run" | cut -c40-200 | head -30
if timeout 15 $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1; then
    for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
fi
tasklist | grep -qi re5dx9 && { echo "game did not close: killing it"; $PS scripts/gamectl.ps1 -Action kill >/dev/null; sleep 3; }
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.stage-backup.json" "$D/profile.json" && rm -f "$D/profile.stage-backup.json"
echo "done; captures in $OUT"
