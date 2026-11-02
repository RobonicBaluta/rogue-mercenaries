#!/usr/bin/env bash
# Developer check (ROADMAP M2.1): a run shows no Mercenaries clock or score, the game's kill cap
# no longer ends the match, and the stage change into arena 2 keeps both off screen. Needs the
# game at the Play menu with a dev adapter config (build/restart_to_play_menu.sh
# configs/adapter.director.json). Saves the profile first; closes the game and restores it at the end.
#   STAGE=<n> fixes the first stage (default: the menu's random pick).
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-hud"
mkdir -p "$OUT"
cap() { $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
wait_for() {  # pattern count timeout_s
    for _ in $(seq 1 "$3"); do
        [ "$(since | grep -cE -e "$1")" -ge "$2" ] && return 0
        sleep 1
    done
    return 1
}
cp "$D/profile.json" "$D/profile.hud-backup.json" || exit 1
before=$(grep -c "" "$LOG")
[ -n "$STAGE" ] && dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
wait_for "director: uEm" 1 90 || echo "no director spawn seen"
sleep 4
cap 01-arena1-hidden
dev "merchud on"; sleep 1; cap 02-arena1-shown
dev "merchud off"; sleep 1
# The game's own cap (150 kills) must not end a match the run holds.
dev "merckills 160"; sleep 6
cap 03-after-kill-cap
since | grep -E "kill cap held off|countdown forced|merckills|mercenaries kill" | cut -c1-160
since | grep -q "countdown forced" && echo "FAIL: the match ended" || echo "match still running at 160 kills"
dev "merckills 0"
for _ in $(seq 1 60); do
    since | grep -q -- "-> arena_results" && break
    dev killall; sleep 4
done
sleep 3
cap 04-arena1-cleared
keys "enter+1500"        # results -> rewards
keys "enter+1500"        # first reward
keys "up+700,enter+1500" # shop: the last row leaves
cap 05-route
keys "enter+800"         # first route: the match ends behind the fade
cap 06-stage-change-a
sleep 1.5; cap 07-stage-change-b
wait_for "-> arena_active" 2 120 || echo "arena 2 did not start"
sleep 6
cap 08-arena2
since | grep -E "phase |arena clear|countdown forced|kill cap|merc hud|HUD hooks|cockpit" | cut -c1-170 | tail -30
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.hud-backup.json" "$D/profile.json" && rm -f "$D/profile.hud-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
