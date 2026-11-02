#!/usr/bin/env bash
# Developer check (ROADMAP M3.2): in a run every gun of the content is loaded with each stage,
# so the shop sells a gun outside the costume's loadout; it is bought, carried into arena 2 on
# another stage, equipped and fired there. Needs the game at the Play menu with a dev adapter
# config (build/restart_to_play_menu.sh configs/adapter.director.json). Saves and restores the
# profile; closes the game at the end.
#   STAGE (first stage, default 0)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-shopgun"
STAGE=${STAGE:-0}
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(since | grep -cE -e "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
weapon_state() {  # "<rounds> <equipped>" of item $1 from a fresh dumpinv
    dev dumpinv
    since | grep "dev inv slot" | grep "item $1 " | tail -1 | awk '{c=$0; sub(/.*count /,"",c); split(c,a," "); n=split($0,w," "); print a[1], w[n-4]}'
}
cp "$D/profile.json" "$D/profile.shopgun-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
wait_for "director: uEm" 1 90 || echo "no director spawn seen"
sleep 3
since | grep -E "resources: (run|list)|weapons placeable" | cut -c40-230
for _ in $(seq 1 60); do since | grep -q -- "-> arena_results" && break; dev killall; sleep 4; done
sleep 3
keys "enter+1500"   # results -> rewards
keys "enter+1500"   # first reward -> shop
dev "supplies 500"
dev "buy weapon"
since | grep -E "dev buy|weapon [0-9A-F]{4} created|not loaded" | cut -c40-200
ITEM=$(since | grep -oE "weapon [0-9A-F]{4} created" | tail -1 | cut -d' ' -f2)
echo "bought item: ${ITEM:-none}"
cap 01-shop
keys "up+700,enter+1500"  # leave the shop
keys "enter+800"          # first route
wait_for "-> arena_active" 2 120 || echo "arena 2 did not start"
sleep 6
since | grep -E "stage change|carried|weapons placeable|resources: (run|list)" | cut -c40-230 | tail -12
[ -z "$ITEM" ] && ITEM=011A
dev "quiet on"; dev killall; sleep 4; dev killall; sleep 2
echo "arena 2, $ITEM: rounds/equipped = $(weapon_state $ITEM)"
# Quick keys 1..4 pick grid cells 1..4: move the gun to cell 2 (as ORGANIZE would) and press 2.
CELL=$(since | grep "dev inv slot" | grep "item $ITEM " | tail -1 | awk '{n=split($0,w," "); print strtonum("0x" w[n-5]) + 1}')
echo "$ITEM is in cell $CELL"
[ "$CELL" != 2 ] && dev "cell $CELL 2"
keys "2+1200"
echo "key 2: rounds/equipped = $(weapon_state $ITEM)"
cap 02-arena2-equipped
keys "rdown+900,lclick+700,lclick+700,rup+300"
sleep 1
cap 03-arena2-fired
echo "after firing: rounds/equipped = $(weapon_state $ITEM)"
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.shopgun-backup.json" "$D/profile.json" && rm -f "$D/profile.shopgun-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
