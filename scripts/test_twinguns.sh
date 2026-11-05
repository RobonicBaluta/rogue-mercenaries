#!/usr/bin/env bash
# Developer check (ROADMAP E16 spike): the boss Jill's two SMGs (uWp76) on the player. The game is
# restarted at the Play menu with configs/adapter.builds.json (invulnerable, muted, debug log), Jill
# Battle Suit is selected (her runs load uWp76) and START RUN chosen; in arena 1 dev "twinguns"
# attaches the guns to her hands, dev "twinfire N" fires them along her facing, then "twinfire N at" from just in front of the nearest enemy, and the
# log shows whether the shots hit enemies and whom the game credits (enemy_hit ... by=). Captures in
# build/runs/<time>-twinguns. Saves and restores the profile, checks the save file's hash; closes the
# game at the end.
#   bash scripts/test_twinguns.sh            SHOTS=5 ROUNDS=4
cd "$(dirname "$0")/.."
SKIN=${SKIN:-jill_battle_suit}
SHOTS=${SHOTS:-5}
ROUNDS=${ROUNDS:-4}
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-twinguns"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
alive() { tasklist | grep -qi re5dx9; }
press_until() { for _ in 1 2 3 4 5; do [ "$(count "$2")" -ge "$3" ] && return 0; alive || return 1; keys "$1"; sleep 2; done; [ "$(count "$2")" -ge "$3" ]; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null
    for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
    tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
}
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
cp "$D/profile.json" "$D/profile.twinguns-backup.json" || exit 1
close_game
bash build/restart_to_play_menu.sh "${CONFIG:-configs/adapter.builds.json}" >/dev/null
sleep 3
before=1  # the log starts over with each launch; the mod menu may already be open
dev "select $SKIN"
dev rogue
if wait_for "opened on screen 0 by itself" 1 30; then
    sleep 1; keys "enter+2500"
    if wait_for "phase idle -> arena_active" 1 90; then
        wait_for "director: uEm" 1 30
        sleep 2
        cap 01-before
        if [ -n "$SPECIAL" ]; then
            # SPECIAL=1: Twin Fury itself (Q) USES times (default 2), 27 s apart (its cooldown): her
            # two SMGs in her hands for the sweep, firing; captures during it.
            [ -n "$NOGUNS" ] && dev "twinguns special off"  # NOGUNS=1: the same without them (a control)
            for r in $(seq 1 "${USES:-2}"); do
                if [ -n "$DEVSPECIAL" ]; then dev special; else keys "q+300"; fi  # DEVSPECIAL=1: no key press
                cap "02-special-$r-a"
                cap "02-special-$r-b"
                cap "02-special-$r-c"
                [ "$r" -lt "${USES:-2}" ] && sleep 24
            done
            sleep 3
        else
        dev twinguns
        sleep 2
        cap 02-guns
        fi
        if [ -z "$SPECIAL" ] && alive && [ "$(count "game fatal")" -eq 0 ]; then
            # ATTACKS="190003 10018 ...": one burst at the nearest enemy per attack id (the shots'
            # +0x1CA0); without, the first burst along her facing and the rest at the nearest enemy.
            if [ -n "$STREAM" ]; then
                # STREAM=1: both guns fire every 0.1 s for 6 s with no attack (to see the shots);
                # captures during it.
                dev "twinstream 6"
                cap 03-stream-1
                cap 03-stream-2
                keys "a:400+200"
                cap 03-stream-3
                sleep 3
            elif [ -n "$ATTACKS" ]; then
                r=0
                for a in $ATTACKS; do
                    r=$((r + 1))
                    dev "twinfire $SHOTS at attack=$a"
                    sleep 2
                    cap "03-fire-$r-$a"
                done
            else
                for r in $(seq 1 "$ROUNDS"); do
                    if [ "$r" -eq 1 ]; then dev "twinfire $SHOTS"; else dev "twinfire $SHOTS at"; fi
                    sleep 2
                    cap "03-fire-$r"
                    keys "a:600+300"  # turn a little between bursts, toward other enemies
                done
            fi
            sleep 4
            dev "twinguns off"
            sleep 2
            cap 04-off
        fi
    else
        echo "arena 1 did not start"
    fi
else
    echo "the mod menu did not open"
fi
since | grep -E "dev select|start run \(|twin|special: |boss motion|uWp76|extras queued|enemy_hit|player_hit|kill counted|arena [0-9]:|game fatal|UNHANDLED|exception" |
    sed -E 's/^.{24}//' | cut -c1-240 > "$OUT/twinguns.txt"
cat "$OUT/twinguns.txt" | head -80
alive || echo "THE GAME CLOSED"
close_game
cp "$D/profile.twinguns-backup.json" "$D/profile.json" && rm -f "$D/profile.twinguns-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; captures in $OUT"
