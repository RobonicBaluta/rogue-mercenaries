#!/usr/bin/env bash
# Developer check (2026-11-08): a boss map's arena left to the boss for SECONDS (no shots): its
# actions and the player's (dev watch, watchplayer), a capture every 5 s (a black screen after
# Wesker's knee strike once). CONFIG (default configs/adapter.boss.json: invulnerable; the mortal
# one, configs/adapter.mortal.json, lets the boss kill the player). Saves and restores the
# profile; checks the save.
#   STAGE  CLASS  SECONDS (default 45)  CONFIG
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:?STAGE=<boss map stage>}
CLASS=${CLASS:?CLASS=uEmNN}
SECONDS_LEFT=${SECONDS:-45}
CONFIG=${CONFIG:-configs/adapter.boss.json}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-bossidle-$CLASS-s$STAGE"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
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
bash build/restart_to_play_menu.sh "$CONFIG" >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "stage $STAGE"; dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
if wait_for "phase idle -> arena_active" 1 120; then
    dev "watch $CLASS"; dev "watchplayer on"
    for t in $(seq 5 5 "$SECONDS_LEFT"); do
        sleep 5
        alive || break
        cap "t$(printf '%02d' "$t")"
        [ "$(count 'phase arena_active -> (arena_results|run_failed)')" -ge 1 ] && break
    done
else
    echo "the arena did not start"
fi
since > "$OUT/log.txt"
echo "player actions: $(grep -oE 'watch: player action -?[0-9]+ [A-Za-z0-9_]*' "$OUT/log.txt" | cut -d' ' -f5 | uniq | tr '\n' ' ' | cut -c1-600)"
echo "boss actions: $(grep -oE "watch: $CLASS hp [0-9]+/[0-9]+ action -?[0-9]+ [A-Za-z0-9_]*" "$OUT/log.txt" | awk '{print $NF}' | uniq | tr '\n' ' ' | cut -c1-500)"
grep -E "player died|run_failed|game fatal|UNHANDLED" "$OUT/log.txt" | head -3 | cut -c12-23,25-200
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
