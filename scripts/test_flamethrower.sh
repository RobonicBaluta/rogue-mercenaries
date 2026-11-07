#!/usr/bin/env bash
# Developer check (2026-11-07, the user's ask): the flamethrower (item 0121) on enemy after enemy.
# A plain match (no run) on Mercenaries stage STAGE, or campaign area NUMBER through the host, with
# the director quiet; every class of CLASSES and the flamethrower preload with the stage. The gun
# is obtained as a purchase hands it over (dev obtain; full fuel) and equipped (the weapon keys tried in turn), then for each class: aim, one
# spawned DISTANCE units ahead, FIRE_MS of fire, the gun's fuel and the log's hits and reactions
# (`enemy_hit`, `enemy states: <class> <action> after a hit`), a capture, then it is killed.
# Needs the game closed or at the Play menu; restarts it with configs/adapter.director.json
# (developer mode, invulnerable, muted). Saves and restores the profile; closes the game at the end.
#   STAGE (default 2)  NUMBER (campaign area, -1 none)  CLASSES  FUEL (100)  FIRE_MS (3500)
#   DISTANCE (450)  EXTRAS (more archives, e.g. "uEm40Dummy uEm40Parts")
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:-2}
NUMBER=${NUMBER:--1}
CLASSES=${CLASSES:-uEm10 uEm12 uEm14 uEm11 uEm13 uEm21 uEm22 uEm23 uEm24 uEm26 uEm32 uEm34 uEm0b}
FUEL=${FUEL:-100}
FIRE_MS=${FIRE_MS:-3500}
DISTANCE=${DISTANCE:-450}
GUN=${GUN:-0121}  # any weapon item (hex): 011E M93R, 0122 stun rod, 0125 grenade launcher, 010D rocket launcher
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-flamethrower"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
# The gun's inventory line from the latest dumpinv: "<count> <equipped>".
gun_state() {
    dev dumpinv
    since | grep "dev inv slot" | grep "item $GUN " | tail -1 | awk '{c=$0; sub(/.*count /,"",c); split(c,a," "); n=split($0,w," "); print a[1], w[n-4]}'
}
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
}
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
cp "$D/profile.json" "$D/profile.flame-backup.json" || exit 1
close_game
bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null
sleep 3
before=1
[ "$NUMBER" -ge 0 ] && dev "stage $NUMBER"
dev "quiet on"
dev "preload $CLASSES ${EXTRAS:-} $GUN"
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "match detected" && break; alive || break; sleep 1; done
sleep 5
dev killall; sleep 3; dev killall; sleep 2
cap 01-match
dev "obtain $GUN"  # as a purchase: a grid cell for it (the game files it outside the grid)
sleep 1
echo "created: count/equipped = $(gun_state)"
for k in 1 2 3 4 up down left right; do
    keys "$k+1200"
    state=$(gun_state)
    [ "${state#* }" = "00000001" ] && { echo "equipped with key $k"; break; }
done
cap 02-equipped
for cls in $CLASSES; do
    alive || break
    mark=$(grep -c "" "$LOG")
    keys "rdown+900"
    # Fire from the moment it appears: one that reached him first grabbed him (2026-11-07: every
    # class after the first met a grabbed Chris, "AYUDA", and was never burnt).
    printf '%s' "spawn $cls -1 1 $DISTANCE" > "$D/dev.command"
    keys "lclick+$FIRE_MS,rup+200" &
    sleep 1.5; cap "03-$cls-firing"
    wait
    cap "03-$cls"
    sleep 2
    st=$(gun_state)
    hits=$(tail -n +"$mark" "$LOG" | grep -c "enemy_hit.*class=$cls ")
    dmg=$(tail -n +"$mark" "$LOG" | grep "enemy_hit.*class=$cls " | sed -E 's/.*applied=([0-9]+).*/\1/' | awk '{s+=$1} END {print s+0}')
    killed=$(tail -n +"$mark" "$LOG" | grep -c "enemy_hit.*class=$cls .*killed=1")
    states=$(tail -n +"$mark" "$LOG" | grep "enemy states: $cls " | sed -E 's/.*enemy states: [^ ]+ ([^ ]+) after.*/\1/' | sort -u | tr '\n' ' ')
    echo "$cls: $hits hits, $dmg damage, killed $killed; fuel/equipped now $st; states: $states"
    dev killall; sleep 2
done
# Its fuel comes back by itself in a run (RefuelFlamethrower): after 25 s without firing.
echo "fuel before the wait: $(gun_state)"
sleep 25
echo "fuel after 25 s: $(gun_state)"
since | grep -E "dev create|dev obtain|outside the grid|flamethrower:|dev spawn|refused|not loaded|game fatal|UNHANDLED|exception" | cut -c25-200 | head -30 > "$OUT/notes.txt"
cat "$OUT/notes.txt"
since | grep -E "enemy_hit|enemy states|dev inv slot.*item $GUN " | cut -c25-260 > "$OUT/hits.txt"
alive || echo "THE GAME CLOSED"
close_game
cp "$D/profile.flame-backup.json" "$D/profile.json" && rm -f "$D/profile.flame-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; captures in $OUT"
