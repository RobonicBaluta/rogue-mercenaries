#!/usr/bin/env bash
# Developer spike (ROADMAP M4.1 / E10): places a time-bonus statue (uOm0100, a chest in a run)
# in front of the player in an emptied arena, shoots it and checks that the chest opens through
# the game's own pickup. Needs the game at the Play menu with a dev adapter config
# (build/restart_to_play_menu.sh configs/adapter.director.json). Saves and restores the
# profile; closes the game at the end.   STAGE (default 1)  DISTANCE (default 250)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-chest"
STAGE=${STAGE:-1}
DISTANCE=${DISTANCE:-250}
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.chest-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
sleep 3
dev "quiet on"; dev killall; sleep 4; dev killall; sleep 2
dev "placechest $DISTANCE 30"
sleep 1; cap 01-placed
for _ in 1 2 3; do keys "rdown+800,lclick+500,lclick+500,lclick+500,rup+300"; sleep 1; since | grep -q "time bonus picked up" && break; done
sleep 2; cap 02-after-shots
since | grep -E "world: chest|time bonus picked up|chest [0-9]+:|placechest" | cut -c40-230
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.chest-backup.json" "$D/profile.json" && rm -f "$D/profile.chest-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
