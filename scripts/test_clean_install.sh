#!/usr/bin/env bash
# Developer check (2026-11-08). A friend's PC on this PC: no mods folder, no mod data, no leftover install; the real ZIP and its
# real installer. Everything set aside is put back at the end (trap), whatever happens.
#   ZIP=dist/x.zip PHASES="A B C" bash cleanmachine.sh
# A: the player's install as it is: does the mod load?
# B: a dev adapter.json over that install: fresh profile, START RUN, arena 1 cleared, arena 2 begins.
# C: every character and skin earned but no mods: the menus, and a missing mod's skin chosen.
# D: Gold Edition on a save with The Mercenaries closed: the row, by hand, and START RUN.
# E: the same without Extra Content.
cd /g/Projects/re5mod || exit 1
ZIP=${ZIP:?}
PHASES=${PHASES:-A}
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAMP=$(date +%Y%m%d-%H%M%S)
GAME="/g/SteamLibrary/steamapps/common/Resident Evil 5"
DATA_WIN="$LOCALAPPDATA/RogueMercenaries"
DATA=$(cygpath -u "$DATA_WIN")
DATA_ASIDE="$DATA.cleanmachine-$STAMP"
OUT="build/runs/$STAMP-cleanmachine"
AS="build/backups/cleanmachine-$STAMP"
UNZIPPED="build/package-test/friend download $STAMP"
SAVE="/c/Program Files (x86)/Steam/userdata/80327605/21690/remote/savedata.bin"
CONFIG="$(cygpath -u "$(powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')" | tr -d '\r')")/CAPCOM/RESIDENT EVIL 5/config.ini"
LOG="$DATA/logs/adapter.log"
mkdir -p "$OUT" "$AS" || exit 1

alive() { tasklist | grep -qi re5dx9; }
if alive; then echo "RE5 is running; nothing done"; exit 1; fi
save_before=$(sha256sum "$SAVE" | cut -c1-16)
config_before=$(sha256sum "$CONFIG" | cut -c1-16)
profile_before=$(sha256sum "$DATA/profile.json" | cut -c1-16)
exe_before=$(sha256sum "$GAME/re5dx9.exe" | cut -c1-16)
cp "$SAVE" "$AS/savedata.bin" || exit 1
cp "$CONFIG" "$AS/config.ini" || exit 1
echo "before: save $save_before config $config_before profile $profile_before exe $exe_before"

close_game() {
    alive || { $PS scripts/gamectl.ps1 -Action reap >/dev/null 2>&1; return; }
    $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1
    for _ in $(seq 1 25); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null 2>&1
    sleep 2
    $PS scripts/gamectl.ps1 -Action reap >/dev/null 2>&1
}

moved_data=0
moved_mods=0
restore() {
    trap - EXIT
    echo "== restoring"
    close_game
    [ -d "$DATA" ] && [ "$moved_data" = 1 ] && { mkdir -p "$OUT/sandbox"; cp -r "$DATA/logs" "$OUT/sandbox/" 2>/dev/null; cp "$DATA/profile.json" "$OUT/sandbox/" 2>/dev/null; }
    if [ -f "$GAME/RogueMercenaries/uninstall.ps1" ]; then
        $PS "$(cygpath -w "$GAME/RogueMercenaries/uninstall.ps1")" -GameDir "$(cygpath -w "$GAME")" -KeepData 2>&1 | tail -2
    fi
    if [ "$moved_data" = 1 ]; then rm -rf "$DATA"; mv "$DATA_ASIDE" "$DATA" && echo "data folder back"; fi
    if [ "$moved_mods" = 1 ]; then mkdir -p "$GAME/RogueMercenaries"; mv "$AS/mods" "$GAME/RogueMercenaries/mods" && echo "mods folder back"; fi
    rm -rf "$UNZIPPED"
    $PS scripts/deploy.ps1 -Mode install 2>&1 | tail -1
    echo "after: save $(sha256sum "$SAVE" | cut -c1-16) config $(sha256sum "$CONFIG" | cut -c1-16) profile $(sha256sum "$DATA/profile.json" | cut -c1-16) exe $(sha256sum "$GAME/re5dx9.exe" | cut -c1-16)"
    echo "output: $OUT"
}
trap restore EXIT

# The friend's state.
$PS scripts/deploy.ps1 -Mode uninstall 2>&1 | tail -1
mv "$GAME/RogueMercenaries/mods" "$AS/mods" && moved_mods=1 || { echo "mods not moved"; exit 1; }
rmdir "$GAME/RogueMercenaries" 2>/dev/null
mv "$DATA" "$DATA_ASIDE" && moved_data=1 || { echo "data not moved"; exit 1; }
ls -A "$GAME" | grep -i -E "dinput8|RogueMercenaries" && { echo "game folder not clean"; exit 1; }
[ -e "$DATA" ] && { echo "data folder not clean"; exit 1; }
echo "clean: no dinput8.dll, no RogueMercenaries in the game folder, no data folder"

mkdir -p "$UNZIPPED"
powershell -NoProfile -Command "Expand-Archive -LiteralPath '$(cygpath -w "$ZIP")' -DestinationPath '$(cygpath -w "$UNZIPPED")'" || exit 1
PKG=$(ls -d "$UNZIPPED"/RogueMercenaries-*)
echo "== installer ($PKG)"
$PS "$(cygpath -w "$PKG/installer/install.ps1")" -Yes 2>&1 | grep -v "^$" | head -14
ls -la "$GAME/dinput8.dll" | cut -c20-
[ -d "$GAME/RogueMercenaries/mods" ] && echo "UNEXPECTED: a mods folder" || echo "no mods folder (as on the friend's PC)"

keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
dev() { printf '%s' "$1" > "$DATA/dev.command"; sleep 1.5; }
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $1" || echo "NO CAPTURE ($1)"; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; alive || return 1; sleep 1; done; return 1; }
report() {  # the lines that say whether it works
    grep -E "starting$|game dir|data dir|verification|signature .*(FAIL|MISMATCH|BAD)|build identity|content|initialization complete|all queued hooks|mods: |ERROR|\[WARN \] .*(fail|missing|not )" "$LOG" | cut -c25-210 | head -${1:-30}
}

if [[ " $PHASES " == *" A "* ]]; then
    echo "== A: the player's install, no input"
    $PS scripts/gamectl.ps1 -Action launch
    before=1
    for _ in $(seq 1 30); do [ -f "$LOG" ] && break; sleep 2; done
    if [ ! -f "$LOG" ]; then
        echo "A: NO LOG after 60 s: the DLL did not start (alive: $(alive && echo yes || echo no))"
    else
        wait_for "initialization complete" 1 150 && echo "A: initialization complete" || echo "A: NOT complete"
        sleep 45
        alive && echo "A: game still running after the title settled" || echo "A: GAME EXITED"
        $PS scripts/gamectl.ps1 -Action status | cut -c1-120
        report 40
        grep -cE "signature .* OK" "$LOG" | sed 's/^/A: signatures OK: /'
    fi
    close_game
    cp "$LOG" "$OUT/A-adapter.log" 2>/dev/null
fi

to_play_menu() {
    $PS scripts/gamectl.ps1 -Action launch
    sleep 42
    keys "enter+2500,enter+5000,enter+4000,enter+2500"
    sleep 3
    before=$(grep -c "" "$LOG")
}
open_menu() {
    dev rogue
    wait_for "opened on screen 0 by itself" 1 30 || { echo "the mod menu did not open"; return 1; }
    sleep 1
}

if [[ " $PHASES " == *" B "* ]]; then
    echo "== B: fresh profile, a real run, no mods"
    cp configs/adapter.director.json "$GAME/RogueMercenaries/adapter.json" || exit 1
    to_play_menu
    if open_menu; then
        cap B1-menu
        keys "enter+2500"
        if wait_for "phase idle -> arena_active" 1 90; then
            echo "B: $(since | grep -E 'start run \(' | tail -1 | cut -c25-160)"
            since | grep -E "ladder: |arena 1: " | cut -c25-160 | head -3
            sleep 12; cap B2-arena1
            echo "B: spawned so far: $(count 'director: .* -> [0-9A-F]+; ')"
            for _ in $(seq 1 60); do
                [ "$(count 'phase arena_active -> arena_results')" -ge 1 ] && break
                alive || break
                dev killall; sleep 3
            done
            [ "$(count 'phase arena_active -> arena_results')" -ge 1 ] && echo "B: arena 1 cleared" || echo "B: ARENA 1 NOT CLEARED"
            for _ in $(seq 1 25); do
                [ "$(count 'stage change: arena 2 started')" -ge 1 ] && break
                alive || break
                last=$(since | grep -oE "phase [a-z_]+ -> [a-z_]+" | tail -1 | awk '{print $4}')
                case "$last" in
                    arena_results|reward_selection) keys "enter+2000" ;;
                    shop) keys "up+500,enter+2500" ;;
                    route_selection) keys "enter+2500" ;;
                    *) sleep 2 ;;
                esac
            done
            [ "$(count 'stage change: arena 2 started')" -ge 1 ] && echo "B: arena 2 started" || echo "B: ARENA 2 NOT STARTED"
            sleep 8; cap B3-arena2
        else
            echo "B: THE RUN DID NOT START"
        fi
    fi
    alive && echo "B: game still running" || echo "B: GAME EXITED"
    since | grep -E "ERROR|CRASH|crash|exception" | cut -c25-200 | head
    close_game
    cp "$LOG" "$OUT/B-adapter.log"
    python -c "
import json,sys
p=json.load(open(sys.argv[1]));print('B: fresh profile unlocked:',p.get('unlocked_characters'))" "$DATA/profile.json"
fi

if [[ " $PHASES " == *" C "* ]]; then
    echo "== C: everything earned, no mods installed"
    cp configs/adapter.director.json "$GAME/RogueMercenaries/adapter.json" || exit 1
    python - "$(cygpath -w "$DATA/profile.json")" <<'PY'
import json,sys
sys.path.insert(0,'scripts')
import profile_edit
f=sys.argv[1]
p=json.load(open(f,encoding='utf-8'))
s=p['stats']
s.update(total_kills=9000,credits_earned=9000,no_damage_arenas=99,s_rank_arenas=99,best_arena=13,runs_completed=99)
s['runs_completed_as']={c:9 for c in ('chris','sheva','jill','josh','barry','rebecca','excella','wesker','leon')}
s['bosses_beaten']=['boss:uEm40','boss:uEm43','boss:uEm41','boss:uEm81','boss:uEm82','short:easy','short:hard','long:easy','long:hard']
p['active_run']=None
body={k:v for k,v in p.items() if k!='checksum'}
p['checksum']=profile_edit.checksum(body)
json.dump(p,open(f,'w',encoding='utf-8'),indent=2,sort_keys=True)
PY
    python scripts/profile_edit.py "$(cygpath -w "$DATA/profile.json")" --check && echo "C: profile checksum ok"
    to_play_menu
    grep -E "mods: " "$LOG" | tail -1 | cut -c25-400
    if open_menu; then
        keys "down+800,enter+2500"
        cap C1-characters
        for k in 1 2 3 4 5 6 7 8 9; do keys "down+500"; done
        sleep 1; cap C2-characters-bottom
        since | grep -E "dev |ERROR" | cut -c25-160 | tail -5
        keys "esc+2000"
        sleep 1
        dev "select sheva_hound_wolf"
        keys "up+800"
        cap C3-menu-back
        keys "enter+2500"
        # With longer runs open START RUN asks for the length first: its first row.
        [ "$(count 'START RUN chosen')" -ge 1 ] || keys "enter+2500"
        if wait_for "phase idle -> arena_active" 1 90; then
            echo "C: missing-mod skin chosen, the run started: $(since | grep -E 'start run \(' | tail -1 | cut -c25-160)"
            since | grep -E "inventory: stripped|skin|costume slot" | cut -c25-150 | head -4
            sleep 10; cap C4-arena
        else
            echo "C: THE RUN DID NOT START"
            since | tail -15 | cut -c25-170
        fi
    fi
    alive && echo "C: game still running" || echo "C: GAME EXITED"
    since | grep -E "ERROR|CRASH|crash|exception" | cut -c25-200 | head
    close_game
    cp "$LOG" "$OUT/C-adapter.log"
fi

# The Play menu by hand on a fresh profile: LABEL, the game's own rows, and whether it is told there is no DLC.
menu_phase() {
    local L=$1 ROWS=$2 NODLC=$3 TOTAL=$(( $2 + 1 ))
    echo "== $L: The Mercenaries locked, $ROWS game rows: our row by hand, then START RUN"
    rm -f "$DATA/profile.json" "$DATA/profile.json.bak"
    python -c "
import json
a=json.load(open('configs/adapter.director.json'))
a['dev']['no_extra_content']=$NODLC
a['dev']['mercenaries_locked']=True
json.dump(a,open(r'$(cygpath -w "$GAME/RogueMercenaries/adapter.json")','w'),indent=2)"
    to_play_menu
    grep -E "the Play menu has" "$LOG" | tail -1 | cut -c25-200
    if grep -q "the Play menu has $ROWS rows" "$LOG"; then
        sleep 2; cap $L-1-play-menu
        keys "up+1200"
        if wait_for "cursor on row $ROWS of $TOTAL" 1 8; then
            echo "$L: the cursor reached our row by the game's own navigation"
            sleep 1; cap $L-2-our-row
            keys "enter+2500"
            if wait_for "mod menu opened" 1 10; then
                echo "$L: $(since | grep -E 'decide on row' | tail -1 | cut -c25-120)"
                sleep 1; cap $L-3-mod-menu
                keys "enter+2500"
                if wait_for "phase idle -> arena_active" 1 90; then
                    since | grep -E "has not opened The Mercenaries|START RUN chosen|start run \(" | cut -c25-160
                    echo "$L: THE RUN STARTED"
                    sleep 12; cap $L-4-arena
                    echo "$L: spawned so far: $(count 'director: .* -> [0-9A-F]+; ')"
                else
                    echo "$L: THE RUN DID NOT START"; since | tail -18 | cut -c25-170
                fi
            else
                echo "$L: THE ROW DID NOT OPEN THE MENU"; since | tail -8 | cut -c25-170
            fi
        else
            echo "$L: the cursor never reached row $ROWS; no Enter pressed"; since | grep "cursor on row" | tail -5 | cut -c25-120
        fi
    else
        echo "$L: NOT THE $ROWS-ROW MENU; nothing pressed"
    fi
    alive && echo "$L: game still running" || echo "$L: GAME EXITED"
    since | grep -E "ERROR|CRASH|crash|exception" | cut -c25-200 | head
    close_game
    cp "$LOG" "$OUT/$L-adapter.log"
}
[[ " $PHASES " == *" D "* ]] && menu_phase D 8 False
[[ " $PHASES " == *" E "* ]] && menu_phase E 7 True
true
