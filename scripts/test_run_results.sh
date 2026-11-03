#!/usr/bin/env bash
# Developer check (ROADMAP M7.3): the end of a run. MODE=die: arena 1 starts, the player's health
# is set to 1 and the director's enemies finish the run; the game-over screen must leave for the
# Mercenaries menu by itself. MODE=finish: every arena is cleared (dev killall), the first route
# taken each time, FINISH on RUN COMPLETE; the game's Mercenaries results must be skipped. In
# both, the Mercenaries menu script must answer "back" twice (stage screen, Iniciar) so the game
# returns to its Play menu, where the mod menu opens by itself on RUN RESULTS: captured, scrolled,
# then back to the main screen and through CREDITS to LAST RUN. The save file must not change
# (a copy goes to build/backups/ first and is put back if it did). The game is restarted at the
# Play menu (build/restart_to_play_menu.sh with configs/adapter.mortal.json for MODE=die: not
# invulnerable; configs/adapter.director.json otherwise). Saves and restores the profile;
# closes the game at the end.
cd "$(dirname "$0")/.."
MODE=${MODE:-die}
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-run-results-$MODE"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
press_until() { for _ in 1 2 3 4 5; do [ "$(count "$2")" -ge "$3" ] && return 0; keys "$1"; sleep 2; done; [ "$(count "$2")" -ge "$3" ]; }
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
cp "$D/profile.json" "$D/profile.results-backup.json" || exit 1
CONFIG=configs/adapter.mortal.json
[ "$MODE" = die ] || CONFIG=configs/adapter.director.json  # invulnerable: the run must reach its end
bash build/restart_to_play_menu.sh "$CONFIG" >/dev/null
sleep 3
before=$(grep -c "" "$LOG")
keys "up+900,enter+2500,enter+2500"
wait_for "phase .* -> arena_active" 1 90 || echo "arena 1 did not start"
if [ "$MODE" = die ]; then
    wait_for "director: uEm" 1 30
    sleep 3; dev "sethp 1"
    wait_for "player died" 1 120 || echo "NO DEATH"
    sleep 4; cap 00-dead
else
    ARENAS=$(count "run map: arena [0-9]+:")
    for arena in $(seq 1 "$ARENAS"); do
        [ "$(count "run ended")" -ge 1 ] && { echo "THE RUN ENDED EARLY"; break; }
        wait_for "phase .* -> arena_active" "$arena" 150 || { echo "arena $arena did not start"; break; }
        wait_for "director: uEm" 1 30; sleep 4
        for _ in $(seq 1 60); do [ "$(count "-> arena_results")" -ge "$arena" ] && break; dev killall; sleep 4; done
        sleep 3
        press_until "enter+1500" "arena_results -> reward_selection" "$arena" || { echo "results screen stuck after arena $arena"; break; }
        sleep 1
        press_until "enter+1500" "reward_selection -> (shop|run_complete)" "$arena" || { echo "reward screen stuck after arena $arena"; break; }
        [ "$arena" -eq "$ARENAS" ] && break
        sleep 2
        press_until "up+700,enter+1500" "shop -> route_selection" "$arena" || { echo "shop stuck after arena $arena"; break; }
        sleep 2
        press_until "enter+1500" "route: choice" "$arena" || { echo "route not chosen after arena $arena"; break; }
    done
    if wait_for "phase .*run_complete" 1 20; then
        sleep 2; cap 00-run-complete
        keys "enter+1500"
    else
        echo "RUN NOT COMPLETE"
    fi
fi
if wait_for "native menu: opened on screen" 1 120; then
    sleep 2; cap 01-results
    for i in 1 2 3 4 5 6 7 8 9 10; do keys "down+500"; sleep 0.4; done
    sleep 1; cap 02-results-bottom
    keys "enter+1500"; sleep 1.5; cap 03-main
    keys "up+600"; sleep 0.8; cap 04-main-credits-row
    keys "enter+1500"; sleep 2; cap 05-credits
    for i in 1 2 3 4 5 6; do keys "down+500"; sleep 0.8; done
    sleep 1; cap 06-credits-last-run
    keys "enter+1500"; sleep 1.5; cap 07-last-run
    keys "esc+1200"; keys "esc+1200"; keys "esc+1200"
else
    echo "THE MOD MENU DID NOT OPEN ON THE RESULTS"; cap 01-stuck
fi
since | grep -E "player died|run ended|results screen|game over screen|answered with back|back to the title|native menu: (opened|mod menu)|scene request|match (ended|detected)|game fatal|hang watchdog|portraits:|icons:" | cut -c12-230 > "$OUT/trace.txt"
cat "$OUT/trace.txt"
fatal=$(count "game fatal|hang watchdog")
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.results-backup.json" "$D/profile.json" && rm -f "$D/profile.results-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "fatal errors: $fatal"
echo "done; captures in $OUT"
