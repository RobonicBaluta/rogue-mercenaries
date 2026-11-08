#!/usr/bin/env bash
# Developer research (2026-11-08): the Midnight Wesker black screen. One boss arena in six went
# black for good (the world black, the HUD drawn, the player still aiming and firing:
# build/runs/20261108-012025-bossarena-uEm81-s506). RUNS times at most (default 6): a run's first
# arena on Steel Docks (stage 506), the boss map; the screen watch on (dev screenwatch: the fader,
# the application's flags, the player camera, the player's action, on every change); a capture at
# the arena's start and every 3 s for WATCH seconds while Wesker fights the (invulnerable) player.
# A capture whose middle is black (no pixel over 12 of 255) stops the loop: then dev fadein, a
# capture, and the log lines around it. Restarts with configs/adapter.boss.json each time. Saves and
# restores the profile; checks the save.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
RUNS=${RUNS:-6}
WATCH=${WATCH:-45}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-blackscreen"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
# The brightest pixel (0..255) of the middle half of a capture (the HUD sits at its edges): a
# black world reads 0 there (build/runs/20261108-012025-...); a dark close-up (Wesker's grab, its
# own event camera) still has highlights.
middle() { python -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]).convert('L'); w, h = im.size; print(im.crop((w // 4, h // 4, 3 * w // 4, 3 * h // 4)).getextrema()[1])" "$OUT/$1.png" 2>/dev/null || echo 999; }
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
black=""
for run in $(seq 1 "$RUNS"); do
    close_game
    bash build/restart_to_play_menu.sh configs/adapter.boss.json >/dev/null 2>&1
    sleep 3
    before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
    dev "screenwatch on"
    dev "stage 506"; dev "arm 0 0"
    keys "up+900,enter+2500,enter+2500"
    for _ in $(seq 1 120); do since | grep -q "phase idle -> arena_active" && break; alive || break; sleep 1; done
    dev "watch uEm81"
    cap "r$run-00-start"
    echo "run $run: start $(middle "r$run-00-start")"
    for t in $(seq 3 3 "$WATCH"); do
        alive || break
        sleep 2
        cap "r$run-t$t"
        b=$(middle "r$run-t$t")
        if [ "$b" -le 12 ]; then
            black="r$run-t$t"
            echo "run $run: BLACK at $t s (middle $b)"
            dev "fadein 10"; sleep 1; cap "r$run-after-fadein"
            echo "   after dev fadein: middle $(middle "r$run-after-fadein")"
            break
        fi
    done
    since > "$OUT/r$run-log.txt"
    echo "   wesker: $(since | grep 'watch: uEm81 hp' | grep -oE 'action [0-9]+ [A-Za-z0-9_]+' | uniq | head -20 | tr '\n' ';' | cut -c1-400)"
    echo "   screen: $(since | grep -c 'screenwatch:') changes; last: $(since | grep 'screenwatch:' | tail -1 | cut -c40-260)"
    [ -n "$black" ] && break
done
if [ -n "$black" ]; then
    echo "the screen watch before and after the black capture:"
    grep -E "screenwatch:|watch: uEm81 hp|dev capture|dev fadein" "$OUT/${black%%-*}-log.txt" | tail -40 | cut -c12-23,40-260
else
    echo "no black screen in $RUNS runs"
fi
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
