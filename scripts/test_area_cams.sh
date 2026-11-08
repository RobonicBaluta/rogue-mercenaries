#!/usr/bin/env bash
# Developer check (2026-11-08, the user's playtest: the Volcano's "boulder scene camera ... doesn't
# let go until you go off a ledge"). The camera zones of a campaign area (rAHCamera) and its
# scripted zone cameras, off while a run plays one (adapter area_cameras.cpp).
#  1. A plain match on the Volcano (stage 508), director quiet: dev "areacams game" (the game's
#     own, the control), the player warped into each camera zone and the boulder script's trigger,
#     a capture at each; then dev "areacams off" and the same spots again.
#  2. A run whose first arena is the Volcano (dev nextmap 508), the mode left on auto: the same
#     spots; no zone camera may take over.
# The log's "area cameras:" lines say what the game asked and what the mod did. Restarts with
# configs/adapter.director.json (developer mode, invulnerable, muted). Saves and restores the
# profile; checks the save; closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-areacams"
mkdir -p "$OUT/profile-before" build/backups
# Walkable spots (s508's AI waypoints) inside each camera zone of stage\s508\soft\s508 (rAHCamera)
# and the boulder script's trigger (s508_fsm "iwacam"), and the map's start.
SPOTS="start:-898,-4,798 basin:1549,-299,-2018 highrock:1527,297,-3037 farledge:4555,303,-3737 boulderledge:6377,-3,-2471 eastbasin:7017,-343,-965 iwacam:5629,185,-3325"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
alive() { tasklist | grep -qi re5dx9; }
mark() { grep -c "" "$LOG"; }
after() { tail -n +"$(($1 + 1))" "$LOG"; }
wait_after() { for _ in $(seq 1 "$3"); do after "$1" | grep -qE -e "$2" && return 0; alive || return 1; sleep 1; done; return 1; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null 2>&1
    $PS scripts/gamectl.ps1 -Action reap >/dev/null 2>&1
}
fresh_game() { close_game; bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1; sleep 3; }
closed_one=0
close_menus() {
    closed_one=0
    for _ in 1 2 3; do
        [ "$(grep -c 'native menu: mod menu opened' "$LOG")" -gt "$(grep -c 'native menu: mod menu closed' "$LOG")" ] || return 0
        keys "esc+2000"; closed_one=1
    done
}
# Each spot: warped there, 3 s, a capture PREFIX-name, the "area cameras:" lines it brought.
visit() {
    local prefix=$1 spot name xyz m
    for spot in $SPOTS; do
        alive || return 1
        name=${spot%%:*}; xyz=${spot#*:}
        m=$(mark)
        dev "warp ${xyz//,/ }"
        sleep 3
        cap "$prefix-$name"
        echo "   $name: $(after "$m" | grep -E 'area cameras:|dev warp' | cut -c40-170 | tr '\n' ';')"
    done
}
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done

# 1. A plain match on the Volcano.
fresh_game
m=$(mark)
grep -E "area cameras:|signature.*area_camera" "$LOG" | cut -c40-200 | head -4
dev "stage 508"
dev "quiet on"
dev "arm 2 0"
keys "up+900,enter+2500,enter+2500"
if wait_after "$m" "match detected" 120; then
    sleep 5
    dev killall; sleep 2; dev killall
    dev "areacams game"
    echo "the game's own:"
    visit game
    dev "areacams off"
    echo "off:"
    visit off
else
    echo "THE MATCH DID NOT START"
fi
dev "areacams auto"
dev "stage -1"

# 2. A run on the Volcano (auto).
fresh_game
m=$(mark)
close_menus
dev "select chris_bsaa"
dev "nextmap 508"
if [ "$closed_one" = 1 ]; then keys "enter+2500,enter+2500"; else keys "up+900,enter+2500,enter+2500"; fi
if wait_after "$m" "phase idle -> arena_active" 150; then
    sleep 4
    echo "run: $(after "$m" | grep -E 'arena 1: |area cameras: (off|the game)' | cut -c40-170 | tr '\n' ';')"
    dev "quiet on"; dev killall; sleep 2; dev killall
    echo "in a run (auto):"
    visit run
    dev abandon; sleep 2
else
    echo "THE RUN DID NOT START"
fi
grep -E "game fatal|UNHANDLED|exception" "$LOG" | head -3 | cut -c12-23,25-200
cp "$LOG" "$OUT/log.txt"
alive || echo "THE GAME CLOSED"
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
echo "done; captures in $OUT"
