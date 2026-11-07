#!/usr/bin/env bash
# Developer check (2026-11-07): the ladder (economy.json "ladder").
#  1. No boss beaten: START RUN starts the short length at once (no RUN LENGTH screen), 4 arenas,
#     the last one Uroboros (103) alone; every arena is cleared (dev killall, the boss too); the run
#     completes and the profile records short:easy and boss:uEm40.
#  2. short:easy and short:hard beaten (scripts/profile_edit.py): START RUN opens RUN LENGTH
#     (captured); its second row starts the long length: 8 arenas, the last one Uroboros alone again
#     (long:easy not beaten yet).
# Restarts the game with configs/adapter.director.json (invulnerable). Saves and restores the
# profile, checks the save file's hash; closes the game at the end. Captures in build/runs/<time>-ladder.
#   CHARACTER (default chris), PART (1, 2 or both: default both)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
CHARACTER=${CHARACTER:-chris}
PART=${PART:-both}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-ladder"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; alive || return 1; sleep 1; done; return 1; }
alive() { tasklist | grep -qi re5dx9; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
}
start_game() {
    bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null
    sleep 3
    before=$(grep -c "" "$LOG")
    [ "$before" -gt 0 ] || before=1
}
# The run map's line for arena $1 (its nodes: "stage route ...").
map_line() { since | grep -oE "run map: arena $1: .*" | tail -1 | cut -d' ' -f5-; }
# Walks the screens after an arena until the next arena begins (or gives up).
to_next_arena() {
    for _ in $(seq 1 25); do
        [ "$(count "stage change: arena [0-9]+ started")" -ge "$1" ] && return 0
        last=$(since | grep -oE "phase [a-z_]+ -> [a-z_]+" | tail -1 | awk '{print $4}')
        case "$last" in
            arena_results|reward_selection) keys "enter+2000" ;;
            shop) keys "up+500,enter+2500" ;;
            route_selection) keys "enter+2500" ;;
            *) sleep 2 ;;
        esac
    done
    return 1
}
bosses() { python -c "import json,sys; print(','.join(json.load(open(sys.argv[1]))['stats'].get('bosses_beaten', [])))" "$D/profile.json"; }
open_menu() {
    dev "select $CHARACTER"
    dev rogue
    wait_for "opened on screen 0 by itself" 1 30 || { echo "the mod menu did not open"; return 1; }
    sleep 1
}
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
cp "$D/profile.json" "$D/profile.ladder-backup.json" || exit 1
cp "$D/profile.json" "$OUT/profile-before.json"
close_game

if [ "$PART" != 2 ]; then
    echo "== 1. no boss beaten"
    python scripts/profile_edit.py "$D/profile.json" --bosses "" --no-active-run || exit 1
    start_game
    if open_menu; then
        keys "enter+2500"
        if wait_for "phase idle -> arena_active" 1 90; then
            echo "run: $(since | grep -E 'start run \(' | tail -1 | cut -c25-200)"
            echo "arenas: $(count 'run map: arena [0-9]+:'); arena 4: $(map_line 4)"
            sleep 5; cap 01-short-arena1
            for arena in 1 2 3 4; do
                for _ in $(seq 1 60); do
                    [ "$(count "phase arena_active -> arena_results")" -ge "$arena" ] && break
                    alive || break
                    dev killall; sleep 3
                done
                [ "$(count "phase arena_active -> arena_results")" -ge "$arena" ] || { echo "arena $arena not cleared"; break; }
                echo "arena $arena cleared: $(since | grep -oE 'stage change: arena [0-9]+ started.*' | tail -1 | cut -c1-80)"
                [ "$arena" = 4 ] && break
                to_next_arena "$arena" && wait_for "phase route_selection -> arena_active" "$arena" 90 || { echo "arena $((arena + 1)) not reached"; break; }
                sleep 4
                [ "$arena" = 3 ] && { sleep 4; cap 02-short-boss; }
            done
            # The last arena's results, then the native RUN COMPLETE screen (a decide ends the run).
            for _ in $(seq 1 15); do
                [ "$(count '-> run_complete')" -ge 1 ] && break
                keys "enter+1500"; sleep 2
            done
            sleep 2; cap 03-run-complete
            for _ in 1 2 3 4 5; do
                [ "$(count 'run ended')" -ge 1 ] && break
                keys "enter+1500"; sleep 3
            done
            echo "end: $(since | grep -E 'run ended|-> run_complete' | tail -2 | cut -c25-200 | tr '\n' ' ')"
            wait_for "native menu: opened on screen" 2 90 && { sleep 2; cap 04-results; } || echo "the mod menu did not open on the results"
        else
            echo "arena 1 did not start"
        fi
    fi
    since | grep -E "run map|start run \(|stage change|boss|run ended|run_complete|game fatal|UNHANDLED|exception" | cut -c25-220 > "$OUT/part1.txt"
    close_game
    echo "profile bosses_beaten after part 1: [$(bosses)]"
fi

if [ "$PART" != 1 ]; then
    echo "== 2. the short hard boss beaten"
    cp "$D/profile.ladder-backup.json" "$D/profile.json"
    python scripts/profile_edit.py "$D/profile.json" --bosses "short:easy,short:hard" --no-active-run || exit 1
    start_game
    if open_menu; then
        keys "enter+2500"
        sleep 2
        [ "$(count 'start run \(')" -eq 0 ] && echo "START RUN did not start a run (RUN LENGTH expected)" || echo "A RUN STARTED WITHOUT RUN LENGTH"
        cap 05-run-length
        keys "down+700"; sleep 1; cap 06-run-length-long
        keys "enter+2500"
        if wait_for "phase idle -> arena_active" 1 90; then
            echo "run: $(since | grep -E 'start run \(' | tail -1 | cut -c25-200)"
            echo "arenas: $(count 'run map: arena [0-9]+:'); arena 8: $(map_line 8)"
            sleep 6; cap 07-long-arena1
        else
            echo "the long run did not start"
        fi
    fi
    since | grep -E "run map|start run \(|stage change|native menu|game fatal|UNHANDLED|exception" | cut -c25-220 > "$OUT/part2.txt"
    close_game
fi

cp "$D/profile.ladder-backup.json" "$D/profile.json" && rm -f "$D/profile.ladder-backup.json"
echo "profile restored: $(sha256sum "$D/profile.json" | cut -c1-16) (before $(sha256sum "$OUT/profile-before.json" | cut -c1-16))"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; log lines in $OUT"
