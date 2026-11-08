#!/usr/bin/env bash
# Developer check (2026-11-08): the supported mods' skins with their own builds, icons and
# pictures, one pass, with the mods as the player installed them in <game>\RogueMercenaries\mods
# (the folder is not touched). Restarts with configs/adapter.director.json (developer mode,
# invulnerable, muted). Captures: Sheva's and Excella's SKINS on the mod rows (the mod's picture,
# the build's icon), then a run as the Experimenter (Field Trials: the arena's element named), then
# in the same session a run as Hound Wolf Squad (Scorched Earth: the flamethrower held from the
# start, fired at a Majini: Incendiary burns; its fuel back). Saves and restores the profile; checks
# the save and the game's files.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
GAME="/g/SteamLibrary/steamapps/common/Resident Evil 5"
ARC="$GAME/nativePC_MT/Image/Archive"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-modbuilds"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.6; }
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
# The mod menu opens by itself at a Play menu (CONTINUE RUN, RUN RESULTS): closed first.
# closed_one=1 when one was: the Play menu's cursor is then on the mod's row (no "up" to reach it).
closed_one=0
close_menus() {
    closed_one=0
    for _ in 1 2 3; do
        [ "$(grep -c 'native menu: mod menu opened' "$LOG")" -gt "$(grep -c 'native menu: mod menu closed' "$LOG")" ] || return 0
        keys "esc+2000"; closed_one=1
    done
}
# SKINS of SKIN's character, the cursor DOWN rows down, captured as NAME.
skins_shot() {
    close_menus
    dev "select $1"
    local m; m=$(mark)
    dev "rogue skins"
    if wait_after "$m" "opened on screen 4 by itself" 45; then
        sleep 1
        for _ in $(seq 1 "$2"); do keys "down+500"; done
        sleep 1.5; cap "$3"
    else
        echo "SKINS did not open ($1)"
    fi
}
game_hashes() { for f in uPl05ExcellaNormal.arc uPl01ShebaCos4.arc; do echo "$f $(sha256sum < "$ARC/$f" | cut -c1-16)"; done; }
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
hashes_before=$(game_hashes)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done

# 1. The SKINS rows (a fresh game for each: a mod menu closed by Esc does not open by itself again).
fresh_game
skins_shot sheva_bsaa 5 01-skins-sheva-hound-wolf
echo "sheva: $(grep -E 'mods: .*(installed|missing)' "$LOG" | tail -1 | cut -c40-260)"
grep -E "portraits: mod picture|icons: (wolf|syringe)" "$LOG" | cut -c40-200
fresh_game
skins_shot excella_dress 1 02-skins-excella-experimenter
grep -E "portraits: mod picture|icons: (wolf|syringe)" "$LOG" | cut -c40-200

# 2. A run as the Experimenter, 3. then as Hound Wolf Squad (same session).
fresh_game
m=$(mark)
close_menus
dev "select excella_experimenter"
if [ "$closed_one" = 1 ]; then keys "enter+2500,enter+2500"; else keys "up+900,enter+2500,enter+2500"; fi
if wait_after "$m" "phase idle -> arena_active" 120; then
    sleep 6; cap 03-experimenter-arena
    echo "experimenter: $(after "$m" | grep -E "the build's element is|wears its mod" | cut -c40-200 | tr '\n' ' ')"
    echo "   $(after "$m" | grep -oE 'build: maximum health.*' | tail -1)"
    sleep 4; dev killall; sleep 2; cap 04-experimenter
    dev abandon; sleep 2
    m2=$(mark)
    dev quitmatch
    if wait_after "$m2" "opened on screen 12 by itself" 60; then
        sleep 2
        dev "select sheva_hound_wolf"
        keys "esc+2500"
        keys "enter+2500"
        if wait_after "$m2" "phase idle -> arena_active" 120; then
            sleep 8
            dev killall; sleep 2
            dev dumpinv
            echo "hound wolf: flamethrower in the inventory: $(after "$m2" | grep -c 'dev inv slot.*item 0121 ')"
            echo "   $(after "$m2" | grep -E "wears its mod|starting weapon|flamethrower" | cut -c40-200 | head -4 | tr '\n' ' ')"
            # The flamethrower in hand (the weapon keys in turn), then held on a Majini.
            for k in 1 2 3 4 up down left right; do
                keys "$k+1200"; dev dumpinv
                # The record's +0x1C (the 8th word after "|") is 1 for the gun in hand.
                [ "$(tail -n 40 "$LOG" | grep "dev inv slot" | grep "item 0121 " | tail -1 | awk -F'|' '{split($2, w, " "); print w[8]}')" = 00000001 ] &&
                    { echo "   equipped with key $k"; break; }
            done
            m3=$(mark)
            keys "rdown+900"
            for _ in 1 2 3; do quick "bring uEm10 400 60 1.0"; keys "lclick:1500+100"; done
            keys "rup+300"
            sleep 0.5; cap 05-hound-wolf-firing
            sleep 4
            echo "   fire: $(after "$m3" | grep -oE 'elements: [a-z_]+ \([A-Za-z_ ]+\)' | sort | uniq -c | tr '\n' ';' | tr -s ' '), kills: $(after "$m3" | grep -cE 'killed=1|died \(')"
            sleep 8
            echo "   fuel: $(after "$m3" | grep -E 'flamethrower: fuel' | tail -1 | cut -c40-160)"
            cap 06-hound-wolf
        else
            echo "the Hound Wolf Squad run did not start"
        fi
    else
        echo "RUN RESULTS did not open after the first run"
    fi
else
    echo "the Experimenter run did not start"
fi
grep -E "game fatal|UNHANDLED|exception" "$LOG" | head -3 | cut -c12-23,25-200
cp "$LOG" "$OUT/log.txt"
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
[ "$(game_hashes)" = "$hashes_before" ] && echo "game files unchanged" || echo "GAME FILES CHANGED"
save_after=$(sha256sum < "$SAVE" | cut -c1-16)
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum < "$SAVE" | cut -c1-16)"
fi
echo "done; $OUT"
