#!/usr/bin/env bash
# Developer research (2026-11-08, the bosses round: Uroboros Wesker as the Volcano's boss). Where
# can gunfire hurt Uroboros Wesker (uEm82)? 2026-11-08 tests shot him only from the front and none
# registered. A run's first arena on the Volcano (stage 508, dev stage), the director quiet; he is
# spawned (dev spawn), made to hunt the player (dev target) and his hit check logged (dev hitprobe:
# every pending hit, what the check reads and answers). Sweeps, each shot with him pinned 4 m
# ahead in the line of fire: facing the player, then his back to the player (bring ... away),
# then both again after his core reveal (CoreRev, action 54); then a free fight (his attacks, held
# fire). Restarts with configs/adapter.boss.json (invulnerable, every hit logged). Saves and
# restores the profile; checks the save.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SIDES=${SIDES:-0 30 60 90}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-uroboros-hits"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.7; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
mark() { grep -c "" "$LOG"; }
after() { tail -n +"$(($1 + 1))" "$LOG"; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null 2>&1
    $PS scripts/gamectl.ps1 -Action reap >/dev/null 2>&1
}
# A sweep: NAME, then extra bring words ("" or "away"); each side shot once, twice over.
sweep() {
    local name=$1 extra=$2 m n=0
    m=$(mark)
    keys "rdown+1000"
    for pass in 1 2; do
        for side in $SIDES; do
            n=$((n + 1))
            quick "bring uEm82 400 $side 2.0 $extra"
            keys "lclick+350"
            [ "$n" = 2 ] && cap "$name"
        done
        sleep 2.5
    done
    keys "rup+300"
    echo "$name: pending hits $(after "$m" | grep -c 'hitprobe: uEm82'), passed $(after "$m" | grep 'hitprobe: uEm82' | grep -c -- '-> 1$'), enemy_hit $(after "$m" | grep -c 'enemy_hit .*class=uEm82'), health $(after "$m" | grep -oE 'watch: uEm82 hp [0-9]+' | tail -1 | awk '{print $4}')"
    after "$m" | grep 'hitprobe: uEm82' | cut -c40-260 | sort | uniq -c | sort -rn | head -4
}
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.boss.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "stage 508"; dev "quiet on"; dev "preload uEm82 uEm8c uEm8e"; dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 120); do since | grep -q "phase idle -> arena_active" && break; alive || break; sleep 1; done
sleep 4
dev killall
dev "watch uEm82"
dev "spawn uEm82 -1 1 600"
sleep 2
dev "target uEm82"
dev "hitprobe uEm82"
dev "actions uEm82"
cap 01-spawned
sweep 02-front ""
sweep 03-back away
dev "act uEm82 54"
sleep 1
sweep 04-corerev-back away
dev "act uEm82 54"
sleep 1
sweep 05-corerev-front ""
# A free fight: he comes and attacks; fire held toward him whenever he is near (no pin).
m=$(mark)
for _ in 1 2 3 4 5 6; do
    quick "bring uEm82 500 60 0"
    keys "rdown+800"; keys "lclick:1500+100"; keys "rup+200"
    sleep 2
done
cap 06-free
echo "free fight: pending hits $(after "$m" | grep -c 'hitprobe: uEm82'), passed $(after "$m" | grep 'hitprobe: uEm82' | grep -c -- '-> 1$'), enemy_hit $(after "$m" | grep -c 'enemy_hit .*class=uEm82')"
dev enemies
echo "his actions: $(since | grep 'watch: uEm82 hp' | grep -oE 'action [0-9]+ [A-Za-z0-9_]+' | sort | uniq -c | sort -rn | head -12 | tr '\n' ';')"
echo "hits on others: $(since | grep -oE 'enemy_hit enemy=[0-9A-F]+ class=[A-Za-z0-9]+' | awk '{print $3}' | sort | uniq -c | tr '\n' ' ')"
since > "$OUT/log.txt"
since | grep -E "game fatal|UNHANDLED|exception" | head -3
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
save_after=$(sha256sum < "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum < "$SAVE" | cut -c1-16)"
fi
echo "done; $OUT"
