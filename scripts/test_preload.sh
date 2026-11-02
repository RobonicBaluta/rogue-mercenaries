#!/usr/bin/env bash
# Developer spike (ROADMAP M3.1 / E3): preloads one foreign enemy class and one weapon outside
# the costume's loadout with the next Mercenaries load, then spawns the enemy and creates the
# gun. Without the preload both hang the game. Needs the game at the Play menu with a dev
# adapter config (build/restart_to_play_menu.sh configs/adapter.director.json). Saves the
# profile first; closes (or, if hung, kills) the game and restores the profile at the end.
#   STAGE (default 2, Village)  CLASS (default uEm11)  WEAPON (hex item, default 011A L.Hawk)
#   PRELOAD=0 skips the preload (expect a hang).
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-preload"
STAGE=${STAGE:-2}
CLASS=${CLASS:-uEm11}
WEAPON=${WEAPON:-011A}
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1): game not presenting"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.preload-backup.json" || exit 1
before=$(grep -c "" "$LOG")
[ "${PRELOAD:-1}" = 1 ] && dev "preload $CLASS $WEAPON"
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
sleep 3
dev loadlists
dev "archive $CLASS"
dev "archive uWp${WEAPON:2:2}"
cap 01-arena
dev "spawn $CLASS"
sleep 4
dev enemies
cap 02-after-spawn
dev "create $WEAPON 1"
sleep 3
dev dumpinv
cap 03-after-create
sleep 5
cap 04-still-running
since | grep -E "resources:|dev preload|dev archive|dev spawn|spawned|dev create|dev inv slot|dev enemies|$CLASS" | grep -v "list [0-9] .*state" | cut -c1-200 | tail -40
echo "--- loader lists (extras and weapons)"
since | grep -E "resources: list" | grep -iE "$CLASS|uWp|uEm" | cut -c40-160 | head -40
if timeout 15 $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1; then
    for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
fi
tasklist | grep -qi re5dx9 && { echo "game did not close: killing it"; $PS scripts/gamectl.ps1 -Action kill >/dev/null; sleep 3; }
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.preload-backup.json" "$D/profile.json" && rm -f "$D/profile.preload-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
