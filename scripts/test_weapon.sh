#!/usr/bin/env bash
# Developer check (ROADMAP M3.1 / E3): a preloaded weapon outside the costume's loadout is
# created, equipped and fired. Needs the game at the Play menu with a dev adapter config
# (build/restart_to_play_menu.sh configs/adapter.director.json). Quiets the arena (no refills),
# creates the gun with ROUNDS rounds, tries the candidate weapon keys until the gun is the
# equipped record, then aims and fires; the record's rounds must go down. Saves and restores
# the profile; closes the game at the end.
#   STAGE (default 2)  WEAPON (hex item, default 011A L.Hawk)  CLASS (foreign enemy, default uEm11)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-weapon"
STAGE=${STAGE:-2}
WEAPON=${WEAPON:-011A}
CLASS=${CLASS:-uEm11}
ROUNDS=${ROUNDS:-5}
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
# The weapon's inventory line from the latest dumpinv: "<rounds> <equipped>".
weapon_state() {
    dev dumpinv
    since | grep "dev inv slot" | grep "item $WEAPON " | tail -1 | awk '{c=$0; sub(/.*count /,"",c); split(c,a," "); n=split($0,w," "); print a[1], w[n-4]}'
}
cp "$D/profile.json" "$D/profile.weapon-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "preload $CLASS $WEAPON"
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
sleep 3
dev "quiet on"
dev killall; sleep 4; dev killall; sleep 3
dev "spawn $CLASS -1 1 600"
sleep 1; cap 01-foreign-enemy
dev "create $WEAPON $ROUNDS"
sleep 1
echo "after create: rounds/equipped = $(weapon_state)"
for k in 1 2 3 4 up down left right; do
    keys "$k+1200"
    state=$(weapon_state)
    echo "key $k: rounds/equipped = $state"
    [ "${state#* }" = "00000001" ] && { echo "equipped with key $k"; break; }
done
cap 02-equipped
keys "rdown+900,lclick+700,lclick+700,rup+300"
sleep 1
cap 03-after-fire
echo "after firing: rounds/equipped = $(weapon_state)"
since | grep -E "resources:|dev create|dev spawn|$CLASS" | grep -v "list [0-9]" | cut -c1-180 | tail -12
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.weapon-backup.json" "$D/profile.json" && rm -f "$D/profile.weapon-backup.json"
echo "done (game closed, profile restored); captures in $OUT"
