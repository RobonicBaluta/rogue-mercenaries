#!/usr/bin/env bash
# Developer check (2026-11-08): supported third-party mods as a player meets them. The Excella mod
# (build/thirdparty/excella387, unpacked from third_party/nexusmods) is put in
# <game>\RogueMercenaries\mods as a player would unzip it; the Sheva mod is not. The game restarts
# with configs/adapter.director.json (developer mode, invulnerable, muted). Captured: Sheva's SKINS
# with the Hound Wolf Squad row not installed (its help), then a run as Excella in her own dress, then
# (same session, after leaving that match) a run as Excella the Experimenter: the mod's look must
# replace the dress although the game held the dress's archive a match before. With OPEN=1 the
# not-installed row is pressed (the mod's page opens in the browser). The mods put in are removed
# again; the game folder's own files are hash-checked; the profile and save are restored or checked.
#   OPEN (default 0)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
GAME="/g/SteamLibrary/steamapps/common/Resident Evil 5"
MODS="$GAME/RogueMercenaries/mods"
EXCELLA="build/thirdparty/excella387/Excella Experimenter v1.0"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
[ -d "$EXCELLA" ] || { echo "unpack the Excella mod into $EXCELLA first"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-modskins"
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
ARC="$GAME/nativePC_MT/Image/Archive"
game_hashes() { for f in uPl05ExcellaNormal.arc uPl01ShebaCos4.arc; do echo "$f $(sha256sum < "$ARC/$f" | cut -c1-16)"; done; }
# The mod menu opens by itself at a Play menu (CONTINUE RUN, RUN RESULTS): a request to open it on
# a screen waits for it to be closed. Counted over the session's whole log.
menu_open() { [ "$(grep -c 'native menu: mod menu opened' "$LOG")" -gt "$(grep -c 'native menu: mod menu closed' "$LOG")" ]; }
close_menus() { for _ in 1 2 3 4; do menu_open || return 0; keys "esc+2000"; done; }
# Opens the mod menu on SCREEN (dev command, "rogue" or "rogue skins") and waits for screen N.
open_menu() {
    close_menus
    local mark; mark=$(grep -c "" "$LOG")
    dev "$1"
    for _ in $(seq 1 45); do
        tail -n +"$((mark + 1))" "$LOG" | grep -q "opened on screen $2 by itself" && return 0
        alive || return 1; sleep 1
    done
    return 1
}
# A run as SKIN from the mod menu's START RUN; captures NAME once its arena runs.
run_as() {
    close_menus
    dev "select $1"
    # The Play menu's own last row (the mod's), its menu, START RUN: as the boss tests start runs.
    # Once the mod menu has closed, the cursor is still on that row: no "up" then.
    if grep -q "native menu: mod menu closed (row 8)" "$LOG"; then keys "enter+2500,enter+2500"; else keys "up+900,enter+2500,enter+2500"; fi
    if ! wait_for "phase idle -> arena_active" "$3" 120; then echo "the run did not start ($1)"; return 1; fi
    sleep 8
    dev killall
    sleep 2; cap "$4"
    keys "s:1000+300"; sleep 1; cap "$4-b"
}
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
hashes_before=$(game_hashes)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
mods_existed=no; [ -d "$MODS" ] && mods_existed=yes
close_game
bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
# Installed as a player would: the mod's own folder, unzipped into mods (after the redeploy).
mkdir -p "$MODS" && cp -r "$EXCELLA" "$MODS/" || { echo "could not put the mod in"; exit 1; }
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "openlog on"
# 1. Sheva's skins: the Hound Wolf Squad row, not installed.
close_menus
dev "select sheva_bsaa"
if open_menu "rogue skins" 4; then
    sleep 1; cap 01-skins-sheva
    for _ in 1 2 3 4 5; do keys "down+500"; done
    sleep 1; cap 02-skins-sheva-mod
    if [ "${OPEN:-0}" = 1 ]; then keys "enter+2500"; echo "open: $(since | grep -E 'mods: opening' | tail -1 | cut -c40-200)"; fi
    keys "esc+2000"  # SKINS opened by itself: one Esc is the Play menu (a second leaves it for the title)
else
    echo "SKINS did not open"
fi
echo "scan: $(since | grep -E 'mods: .*(installed|missing)' | tail -1 | cut -c40-300)"
since > "$OUT/log-skins.txt"
# A fresh game for the runs: the mod menu opens by itself only when a Play menu comes up, and the
# one SKINS closed back to was already up. (The mods folder stays: the redeploy leaves it.)
close_game
bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "openlog on"
# 2. Excella in her dress, then 3. the mod's Experimenter, in the same session.
if run_as excella_dress 1 1 03-excella-dress; then
    dev abandon; sleep 2; dev quitmatch
    # Back at the Play menu the mod menu opens by itself on RUN RESULTS, over its main screen: one
    # Esc is the main screen, whose first row is START RUN (the skin chosen meanwhile).
    if wait_for "opened on screen 12 by itself" 1 60; then
        sleep 2
        dev "select excella_experimenter"
        keys "esc+2500"
        keys "enter+2500"
        if wait_for "phase idle -> arena_active" 2 120; then
            sleep 8; dev killall
            sleep 2; cap 04-excella-mod
            keys "s:1000+300"; sleep 1; cap 04-excella-mod-b
        else
            echo "the second run did not start"
        fi
    else
        echo "RUN RESULTS did not open after the first run"
    fi
fi
echo "wears: $(since | grep -E 'mods: .* wears its mod' | tail -1 | cut -c40-200)"
echo "opens of Excella's archive: $(since | grep -c 'files: the game opens .*uPl05ExcellaNormal'), read from the mod: $(since | grep -c 'uPl05ExcellaNormal.arc read from')"
since > "$OUT/log.txt"
grep -E "game fatal|UNHANDLED|exception" "$OUT/log.txt" | head -3 | cut -c12-23,25-200
close_game
rm -rf "$MODS/$(basename "$EXCELLA")"
[ "$mods_existed" = no ] && rmdir "$MODS" 2>/dev/null
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
