#!/usr/bin/env bash
# Developer check (2026-11-07): a run goes on after a quit and after a crash. Starts a run (dev
# select CHARACTER, START RUN), clears arena 1 (dev killall until it is clear), walks the reward, shop and
# route screens to arena 2 (its stage change saves the resume point), then
#  1. leaves the match for the menus (dev quitmatch, as the pause menu's quit): the run must be
#     suspended, and CONTINUE RUN (the mod menu's first row) must start arena 2 again;
#  2. kills the game (a crash), starts it again: the run must be kept, and CONTINUE RUN must
#     start arena 2 again.
# Restarts the game with configs/adapter.director.json. Saves and restores the profile, checks the
# save file's hash; closes the game at the end. Captures in build/runs/<time>-resume.
#   CHARACTER (default chris)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
CHARACTER=${CHARACTER:-chris}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-resume"
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
    before=1
}
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
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
cp "$D/profile.json" "$D/profile.resume-backup.json" || exit 1
close_game
start_game
dev "select $CHARACTER"
dev rogue
ok=1
if wait_for "opened on screen 0 by itself" 1 30; then
    sleep 1; keys "enter+2500"
    wait_for "phase idle -> arena_active" 1 90 || { echo "arena 1 did not start"; ok=0; }
else
    echo "the mod menu did not open"; ok=0
fi
if [ $ok = 1 ]; then
    sleep 5; cap 01-arena1
    # Real deaths (the director counts its own units; force_complete's synthetic kills do not).
    for _ in $(seq 1 40); do
        [ "$(count "phase arena_active -> arena_results")" -ge 1 ] && break
        alive || break
        dev killall; sleep 1
    done
    if to_next_arena 1 && wait_for "phase route_selection -> arena_active" 1 60; then
        echo "arena 2 reached: $(since | grep -c 'resume point') resume point line(s), $(count 'save_resume_point|resume_point') core"
        sleep 6; cap 02-arena2
        # 1. The pause menu's quit (the same for the mod: the match is left).
        dev quitmatch
        if wait_for "the run is suspended" 1 30; then
            echo "suspended: $(since | grep 'the run is suspended' | tail -1 | cut -c25-200)"
            # The mod backs out of the Mercenaries menus to the Play menu by itself and opens its
            # menu there (CONTINUE RUN at its top).
            wait_for "opened on screen 0 by itself" 2 90 && sleep 2
            cap 03-menu-continue
            keys "enter+2500"
            if wait_for "resume: run" 1 20 && wait_for "stage change: arena 2 started" 2 120; then
                echo "continued: $(since | grep 'resume: run' | tail -1 | cut -c25-200)"
                sleep 6; cap 04-arena2-again
            else
                echo "CONTINUE RUN did not bring arena 2 back"; ok=0
            fi
        else
            echo "THE RUN WAS NOT SUSPENDED"; ok=0
        fi
    else
        echo "arena 2 was not reached"; ok=0
    fi
fi
# 2. A crash: the game killed in arena 2, started again.
if [ $ok = 1 ]; then
    $PS scripts/gamectl.ps1 -Action kill >/dev/null
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
    start_game
    dev rogue
    if wait_for "opened on screen 0 by itself" 1 30; then
        echo "after the crash: $(since | grep -E 'interrupted run' | tail -1 | cut -c25-200)"
        sleep 2; cap 05-menu-after-crash
        keys "enter+2500"
        if wait_for "resume: run" 1 20 && wait_for "stage change: arena 2 started" 1 120; then
            echo "continued after the crash: $(since | grep 'resume: run' | tail -1 | cut -c25-200)"
            sleep 6; cap 06-arena2-after-crash
        else
            echo "CONTINUE RUN did not bring arena 2 back after the crash"
        fi
    else
        echo "the mod menu did not open after the crash"
    fi
fi
since | grep -E "resume|suspended|interrupted run|stage change|carried:|run ended|start run \(|game fatal|UNHANDLED|exception" | cut -c25-220 > "$OUT/resume.txt"
close_game
cp "$D/profile.resume-backup.json" "$D/profile.json" && rm -f "$D/profile.resume-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; log lines in $OUT/resume.txt"
