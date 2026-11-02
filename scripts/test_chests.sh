#!/usr/bin/env bash
# Developer check (ROADMAP M4.1): a run brings its own chests. The stage's time-bonus statues
# are not created; arena 1 and, after a stage change, arena 2 each place the chests their
# director picked (economy.json chests.per_arena) at the map's chest positions. Needs the game
# at the Play menu with a dev adapter config (build/restart_to_play_menu.sh
# configs/adapter.director.json). Saves and restores the profile; closes the game at the end.
#   STAGE (first stage, default 0)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-chests"
STAGE=${STAGE:-0}
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(since | grep -cE -e "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
cp "$D/profile.json" "$D/profile.chests-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
wait_for "chests: [0-9]+ of" 1 90 || echo "no chests placed in arena 1"
sleep 3
cap 01-arena1
for _ in $(seq 1 60); do since | grep -q -- "-> arena_results" && break; dev killall; sleep 4; done
sleep 3
keys "enter+1500"; keys "enter+1500"; keys "up+700,enter+1500"; keys "enter+800"
wait_for "chests: [0-9]+ of" 2 120 || echo "no chests placed in arena 2"
sleep 3
cap 02-arena2
since | grep -E "time bonuses (blocked|allowed)|time bonus \(set|chests: |world: chest|stage change: stage|phase .*arena_active" | cut -c40-200
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.chests-backup.json" "$D/profile.json" && rm -f "$D/profile.chests-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
