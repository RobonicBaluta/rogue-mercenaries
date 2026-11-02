#!/usr/bin/env bash
# Developer check (ROADMAP M5.2): runs pick campaign areas and Mercenaries stages. START RUN
# (no dev arm) takes the forced first map FIRST (default 508, a campaign area, loaded through
# the host); after arena 1, its screens and the route pick, the stage change takes SECOND
# (default 3, a Mercenaries stage: the host leaves it alone). Both arenas are run by the
# director; the player's position is logged in both. Needs the game at the Play menu with a
# dev adapter config. Saves and restores the profile; closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
FIRST=${FIRST:-508}
SECOND=${SECOND:-3}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-rotation"
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(since | grep -cE -e "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
cp "$D/profile.json" "$D/profile.rotation-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "nextmap $FIRST"
keys "up+900,enter+2500,enter+2500"
wait_for "director: uEm" 1 90 || echo "no director spawn in arena 1"
sleep 6; cap 01-arena1; dev setdump
for _ in $(seq 1 60); do since | grep -q -- "-> arena_results" && break; dev killall; sleep 4; done
sleep 3
keys "enter+1500"; keys "enter+1500"; keys "up+700,enter+1500"
dev "nextmap $SECOND"
keys "enter+800"
wait_for "-> arena_active" 2 120 || echo "arena 2 did not start"
wait_for "director: uEm" 2 30
sleep 6; cap 02-arena2; dev setdump
since | grep -E "setdump: player at|director: [0-9]+ units, mean distance|dev next map|host:|stage change: stage|resources: run|chests: |phase .*arena_active|arena cleared" | cut -c40-170
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.rotation-backup.json" "$D/profile.json" && rm -f "$D/profile.rotation-backup.json"
echo "done; captures in $OUT"
