#!/usr/bin/env bash
# Developer check (2026-11-08): which of an enemy class's own reactions the mod can start, and what
# they look like (the rarity remake's elements: on fire, frozen, acid, flash, blown away, a head
# burst, a burning death). The game restarts with configs/adapter.boss.json (invulnerable); a run's
# first arena starts on STAGE (dev arm); once an enemy of CLASS is out, its action table is logged
# (dev actions), and for each reaction it has (names ending in REACTIONS' suffixes) it is brought in
# front of the player (dev bring: no pin, which would keep it falling from the player's height, and a
# falling one drops a reaction), left LAND seconds to land, the action started (dev act) and three
# frames captured.
# Saves and restores the profile; checks the save. Captures and log.txt in build/runs/<time>-reactions-<class>.
#   STAGE (default 0, Public Assembly)  CLASS (default uEm10)  LAND (default 2.5)
#   REACTIONS (default "DmgFire DmgIce DmgSulfate DmgFlash DmgFlyoff DmgYoroke DeadHeadExplosion DeadArmyBurn")
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:-0}
CLASS=${CLASS:-uEm10}
LAND=${LAND:-2.5}
REACTIONS=${REACTIONS:-"DmgFire DmgIce DmgSulfate DmgFlash DmgFlyoff DmgYoroke DeadHeadExplosion DeadArmyBurn"}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-reactions-$CLASS"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.6; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; alive || return 1; sleep 1; done; return 1; }
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
dev "arm $STAGE 0"
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
if wait_for "phase idle -> arena_active" 1 120; then
    # The first enemy of the class: its table once it is out.
    table=""
    for _ in $(seq 1 30); do
        quick "actions $CLASS"
        if since | grep -qE "dev actions $CLASS [0-9A-F]{8}: [1-9]"; then
            sleep 0.5
            table=$(since | awk "/dev actions $CLASS [0-9A-F]{8}: [1-9]/{f=1;next} f&&/\]   [0-9]/{print;next} f{exit}")
            break
        fi
        sleep 1
    done
    [ -n "$table" ] || echo "NO $CLASS CAME OUT"
    since | grep -E "dev actions $CLASS" | tail -1 | cut -c25-200
    dev "watch $CLASS"
    for suffix in $REACTIONS; do
        id=$(echo "$table" | grep -oE "[0-9]+ cEm[0-9a-fA-F]+Act$suffix\b" | head -1 | cut -d' ' -f1)
        if [ -z "$id" ]; then echo "$suffix: not in the table"; continue; fi
        quick "bring $CLASS 380 50"
        sleep "$LAND"
        quick "act $CLASS $id"
        sleep 0.2; cap "$suffix-a"
        sleep 0.8; cap "$suffix-b"
        sleep 1.2; cap "$suffix-c"
        echo "$suffix ($id): $(since | grep -E "dev act $CLASS $id " | tail -1 | grep -oE 'started|refused'); then $(since | grep "watch: $CLASS" | tail -4 | grep -oE 'action -?[0-9]+ [A-Za-z0-9_]+' | uniq | tr '\n' ' ')"
        sleep 1.5
    done
else
    echo "the arena did not start"
fi
since > "$OUT/log.txt"
grep -E "game fatal|UNHANDLED|exception" "$OUT/log.txt" | head -3 | cut -c12-23,25-200
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
