#!/usr/bin/env bash
# Developer check (2026-11-08): does gunfire register on a unit at all, misses ruled out. The game
# restarts with configs/adapter.boss.json; the host loads stage NUMBER, the director stays quiet,
# CLASS is spawned and watched (dev watch), the player aims, and for each of SIDES (units right of
# his facing; the aiming camera looks past his shoulder) the unit is brought DISTANCE ahead (dev
# bring) and shot once; PASSES sweeps. A human-sized control (CLASS=uEm10, a Majini) tells which
# sides are in the line of fire; a unit with no hit at any of those sides takes none.
# Every `enemy_hit` line is counted per side. Saves and restores the profile; checks the save.
#   NUMBER  CLASS  EXTRAS  DISTANCE (default 400)  SIDES (default "-90 -60 -30 0 30 60 90")
#   PASSES (default 2)  HOLDHP (health given first, so a control survives; default none)
#   PREACT (an action of its own started after the spawn: Wesker's Shrink 104 holds him still)
#   LETHAL=1 (health 1 before the sweep: the first hit kills)  TARGET=1 (the player made its target: dev target)  ZERO (default 1: its health to zero after the
#   sweep, watched 10 s: does it die)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:?NUMBER=<stage>}
CLASS=${CLASS:?CLASS=uEmNN}
DISTANCE=${DISTANCE:-400}
SIDES=${SIDES:--90 -60 -30 0 30 60 90}
PASSES=${PASSES:-2}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-hits-$CLASS-s$NUMBER"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.7; }  # picked up within 0.5 s
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null 2>&1
    $PS scripts/gamectl.ps1 -Action reap >/dev/null 2>&1
}
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.boss.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "stage $NUMBER"; dev "quiet on"; dev "preload $CLASS ${EXTRAS:-}"; dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "phase idle -> arena_active" && break; alive || break; sleep 1; done
sleep 4
dev killall
dev "watch $CLASS"
dev "spawn $CLASS -1 1 600"
sleep 2
dev "actions $CLASS"
[ -n "${TARGET:-}" ] && dev "target $CLASS"
cap 01-spawned
[ -n "${HOLDHP:-}" ] && dev "enemyhp $HOLDHP"
# LETHAL=1: its health 1 first, so the first hit that registers kills it (a death a hit starts,
# which dev killall's SubHp never does).
[ "${LETHAL:-}" = 1 ] && dev "enemyhp 1"
[ -n "${PREACT:-}" ] && dev "act $CLASS $PREACT"
keys "rdown+1200"
n=0
for pass in $(seq 1 "$PASSES"); do
    for side in $SIDES; do
        n=$((n + 1))
        quick "bring $CLASS $DISTANCE $side 2.0"  # pinned there 2 s: the shot finds it in place
        keys "lclick+350"
        [ "$n" = 3 ] && cap "shot-side$side"
    done
    sleep 2.5  # a reload, if the magazine ran dry
done
keys "rup+300"
sleep 1; dev enemies
# Its health to zero (SubHp, dev killall): does it die, and how, over 10 s.
if [ "${ZERO:-1}" = 1 ] && alive; then
    dev killall
    sleep 2; cap 02-zero-2s
    sleep 5; cap 03-zero-7s
    sleep 3; dev enemies
fi
since > "$OUT/log.txt"
# Hits per side: each enemy_hit line counts for the last bring before it.
awk '/dev bring/ { for (i = 1; i <= NF; ++i) if ($i == "side") side = $(i + 1); shots[side]++ }
     /enemy_hit/ && side != "" { hits[side]++ }
     END { for (s in shots) printf "side %s: %d hit(s) of %d shot(s)\n", s, hits[s] + 0, shots[s] }' "$OUT/log.txt" | sort -t' ' -k2 -n
echo "$CLASS hits in all: $(grep -c enemy_hit "$OUT/log.txt"); its actions during the sweep:"
grep "watch: $CLASS hp" "$OUT/log.txt" | sed -n '3,60p' | grep -oE "action [0-9]+ [A-Za-z0-9_]+" | sort | uniq -c | sort -rn | head -10
echo "hit classes: $(grep -oE 'enemy_hit enemy=[0-9A-F]+ class=[A-Za-z0-9]+' "$OUT/log.txt" | awk '{print $3}' | sort | uniq -c | tr '
' ' ')"
echo "after zero health: $(grep "watch: $CLASS" "$OUT/log.txt" | grep -A30 "hp 0/" | grep -oE "action [0-9]+ [A-Za-z0-9_]+|gone" | uniq | head -8 | tr '
' ' ')"
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
save_after=$(sha256sum < "$SAVE" | cut -c1-16)
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum < "$SAVE" | cut -c1-16)"
fi
echo "done; $OUT"
