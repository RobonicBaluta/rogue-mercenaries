#!/usr/bin/env bash
# Developer automation: plays one Rogue Mercenaries run in The Mercenaries
# (Solo, Public Assembly, Chris) with real kills, using the mod's menu
# hotkeys. Needs an otherwise idle PC (it sends input to the game window).
#
#   scripts/autoplay_run.sh [content_dir] [adapter_json] [arenas] [rounds]
#
# Each arena gets up to <rounds> combat rounds of blind aiming. If the kill
# budget is not met by then, the developer command "force_complete" finishes
# the arena with synthetic kills; that is recorded in steps.txt and the log.
# Artifacts: build/runs/<stamp>-autoplay/.
set -u
cd "$(dirname "$0")/.."
CONTENT=${1:-build/testcontent}
ADAPTER=${2:-configs/adapter.autoplay.json}
ARENAS=${3:-3}
ROUNDS=${4:-15}
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
DATA="$LOCALAPPDATA/RogueMercenaries"
LOG="$DATA/logs/adapter.log"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-autoplay"
mkdir -p "$OUT"

keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
cap() { $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1; }
note() { echo "[$(date +%H:%M:%S)] $*" >> "$OUT/steps.txt"; echo "[$(date +%H:%M:%S)] $*"; }
count_log() { grep -cE -e "$1" "$LOG" 2>/dev/null || true; }
wait_count() {  # pattern minimum timeout_s
    local end=$((SECONDS + $3))
    while [ $SECONDS -lt $end ]; do
        [ "$(count_log "$1")" -ge "$2" ] && return 0
        sleep 1
    done
    return 1
}

$PS scripts/deploy.ps1 -Mode uninstall >/dev/null 2>&1   # previous test install, if any
$PS scripts/deploy.ps1 -Mode install -Dll build/win32/adapters/re5/Release/dinput8.dll \
    -AdapterConfig "$ADAPTER" -ConfigDir "$CONTENT" || exit 1
$PS scripts/gamectl.ps1 -Action launch
sleep 40
[ -n "${PRE_COMMAND:-}" ] && { echo "$PRE_COMMAND" > "$DATA/dev.command"; sleep 2; note "dev command: $PRE_COMMAND -> $(grep -E 'dev command' "$LOG" | tail -1 | cut -c40-)"; }
note "title -> play menu -> Los Mercenarios"
keys "enter+2500,enter+5000,enter+4000,enter+2500,down,down,down,down,down,down+600,enter+3000"
# Safety gate: entering The Mercenaries switches game mode 0 -> 2. If the
# menu path drifted (e.g. an early key press), stop instead of sending more
# keys into unknown menus such as system settings.
if ! wait_count "game work mode 0 -> 2" 1 8; then
    note "ABORT: not in The Mercenaries menu (mode did not switch); no further input sent"
    cap 98-abort
    $PS scripts/gamectl.ps1 -Action close >/dev/null; sleep 7; $PS scripts/gamectl.ps1 -Action reap >/dev/null
    cp "$DATA/logs/"* "$OUT/" 2>/dev/null
    $PS scripts/deploy.ps1 -Mode uninstall
    exit 2
fi
keys "enter+3000,enter+3500,enter+3500"   # Iniciar -> Solo -> stage -> character select
cap 01-charselect
note "start run via F7 + Enter hotkey"
keys "f7+900,enter+1200"
wait_count "ui: start run: ok" 1 10 || { note "start run failed"; cap 99-fail; }
keys "enter+1000"   # confirm Chris -> match loads

burst="rdown+450,lclick+250,lclick+250,lclick+250,lclick+250,lclick+250,lclick+250,rup+150"
sweep="$burst,move=350/0+200,$burst,down=w+900,up=w+100,move=-450/0+200,$burst,r+700,move=600/0+200,$burst"
for arena in $(seq 1 "$ARENAS"); do
    if ! wait_count "-> arena_active" "$arena" 90; then
        note "arena $arena did not start"; cap "a${arena}-nostart"; break
    fi
    note "arena $arena active"
    cap "a${arena}-start"
    for round in $(seq 1 "$ROUNDS"); do
        keys "$sweep"
        [ "$(count_log "-> arena_results")" -ge "$arena" ] && break
    done
    if [ "$(count_log "-> arena_results")" -lt "$arena" ]; then
        note "arena $arena: kill budget not met by blind aiming after $ROUNDS rounds; dev force_complete"
        echo force_complete > "$DATA/dev.command"
        wait_count "-> arena_results" "$arena" 15 || { note "arena $arena did not complete"; cap "a${arena}-stuck"; break; }
    fi
    note "arena $arena cleared: $(grep -E 'ui: arena cleared' "$LOG" | tail -1 | cut -c40-)"
    note "kills so far (real): $(count_log 'kill counted')  forced: $(count_log 'dev command force_complete')"
    sleep 4
    cap "a${arena}-results"
    keys "enter+1000"                # continue to rewards
    cap "a${arena}-rewards"
    keys "1+1000"                    # take reward 1
    if [ "$arena" -lt "$ARENAS" ]; then
        cap "a${arena}-shop"
        keys "1+800,enter+1000"      # buy offer 1, leave shop
        cap "a${arena}-routes"
        keys "2+1500"                # take route 2 (the harder one when offered)
        note "route: $(grep -E 'ui: choose route' "$LOG" | tail -1 | cut -c40-)"
        keys "enter+3000,enter+3500,enter+1000"   # results -> stage -> character -> match
    fi
done
wait_count "run ended" 1 15 && note "$(grep -E 'run ended' "$LOG" | tail -1 | cut -c40-)"
cap 90-final
note "start a second run to check that temporary state resets"
keys "f7+900,enter+1500"
cap 91-second-run
$PS scripts/gamectl.ps1 -Action close >/dev/null; sleep 7
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$DATA/logs/"* "$OUT/" 2>/dev/null
cp "$DATA/profile.json" "$OUT/" 2>/dev/null
$PS scripts/deploy.ps1 -Mode uninstall
note "artifacts in $OUT"
