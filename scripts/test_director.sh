#!/usr/bin/env bash
# Developer check (ROADMAP M1.5/M1.6): starts a run from the Play menu, lets the director fill
# the arena, kills its units every few seconds and waits for the arena to clear. Needs the game
# at the Play menu with a dev adapter config (build/restart_to_play_menu.sh configs/adapter.survey.json)
# and configs/maps.json deployed. Saves the profile first; closes the game and puts it back at the end.
#   OBSERVE=<s> (default 20) watches the enemies come before anything is killed.
#   STAGE=<n> fixes the stage (default: the menu's random pick).
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
TIMEOUT=${TIMEOUT:-240}
KILL_EVERY=${KILL_EVERY:-6}
OBSERVE=${OBSERVE:-20}
cp "$D/profile.json" "$D/profile.director-backup.json" || exit 1
before=$(grep -c "" "$LOG")
if [ -n "$STAGE" ]; then printf 'arm %s 0' "$STAGE" > "$D/dev.command"; sleep 1.5; fi
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
for i in $(seq 1 90); do
    tail -n +"$before" "$LOG" | grep -q "director: uEm" && break
    sleep 1
done
sleep 4
printf 'enemies' > "$D/dev.command"; sleep 1.5
$PS scripts/gamectl.ps1 -Action capture | tail -1
# Do they come? In developer mode the log traces the director units' mean distance to the
# player every 2 s; nothing is killed during this window.
for i in $(seq 1 $((OBSERVE / 10))); do sleep 10; $PS scripts/gamectl.ps1 -Action capture | tail -1; done
start=$(date +%s)
while [ $(( $(date +%s) - start )) -lt "$TIMEOUT" ]; do
    tail -n +"$before" "$LOG" | grep -q "arena cleared" && break
    printf 'killall' > "$D/dev.command"
    sleep "$KILL_EVERY"
done
sleep 3
$PS scripts/gamectl.ps1 -Action capture | tail -1
tail -n +"$before" "$LOG" | grep -E "native spawns|director|counted|arena cleared|dev enemies|killall|phase |untracked" | cut -c1-180 | tail -60
$PS scripts/gamectl.ps1 -Action close >/dev/null
for i in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.director-backup.json" "$D/profile.json" && rm -f "$D/profile.director-backup.json"
echo "done (game closed, profile restored)"
