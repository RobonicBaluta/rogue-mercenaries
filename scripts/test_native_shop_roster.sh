#!/usr/bin/env bash
# Developer check (ROADMAP M7.1): the game's Organize/Buy screen with each character. For every
# costume slot in COSTUMES (default: one per character) the game is restarted at the Play menu
# (build/restart_to_play_menu.sh with configs/adapter.director.json), a run starts in that
# costume, arena 1 is cleared (dev killall), the screen is opened from the SHOP, captured (main
# menu and Organizar) and closed untouched; then the next route is taken. Prints the grid the
# run put in, the grid the screen gave back, the inventory in arena 2 (on map NEXTMAP if set:
# dev nextmap), whether the first aid spray's archive is loaded there, and the save file's hash
# before and after (a copy goes to build/backups/ first; it must not change). Saves and restores
# the profile; closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
COSTUMES=${COSTUMES:-"3 6 9 12 13 16 17"}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-native-shop-roster"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; sleep 1; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
press_until() { for _ in 1 2 3 4 5; do [ "$(count "$2")" -ge "$3" ] && return 0; keys "$1"; sleep 2; done; [ "$(count "$2")" -ge "$3" ]; }
grid() { local mark; mark=$(grep -c "" "$LOG"); dev dumpinv; tail -n +"$((mark + 1))" "$LOG" | grep "dev inv slot" | sed -E 's/.*item ([0-9A-F]{4}) count (-?[0-9]+).*\| ([0-9A-F ]+)$/\1x\2 \3/' | awk '{print $1 "@" $8 "[" $10 " " $11 "]"}' | tr '\n' ' '; }
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null
    for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
    tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
}
save_before=$(sha256sum "$SAVE" | cut -c1-16)
cp "$SAVE" "build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin" || exit 1
cp "$D/profile.json" "$D/profile.roster-backup.json" || exit 1
for costume in $COSTUMES; do
    echo "=== costume slot $costume"
    bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null
    sleep 3
    before=$(grep -c "" "$LOG")
    dev "arm 0 $costume"
    keys "up+900,enter+2500,enter+2500"
    for _ in $(seq 1 90); do since | grep -q "phase idle -> arena_active" && break; sleep 1; done
    since | grep -E "match detected" | tail -1 | cut -c40-200
    sleep 6
    for _ in $(seq 1 60); do [ "$(count "-> arena_results")" -ge 1 ] && break; dev killall; sleep 4; done
    sleep 3
    press_until "enter+1500" "arena_results -> reward_selection" 1
    press_until "enter+1500" "reward_selection -> shop" 1
    sleep 2
    echo "inventory before the screen: $(grid)"
    keys "up+700"; keys "up+700"; keys "enter+1500"; sleep 3; cap "c$costume-1-screen"
    keys "enter+1500"; sleep 1; cap "c$costume-2-organize"
    # Back to the main menu, then "A punto" (Esc moves the cursor there) and Enter.
    keys "esc+1000"; keys "esc+1000"; keys "esc+1000"
    keys "enter+1200"; keys "enter+1200"; sleep 3
    for _ in 1 2 3; do since | grep -q "itembox: closed" && break; keys "esc+800"; keys "enter+1200"; done
    sleep 2
    echo "inventory after the screen: $(grid)"
    [ -n "$NEXTMAP" ] && dev "nextmap $NEXTMAP"
    press_until "up+700,enter+1500" "shop -> route_selection" 1 || echo "shop stuck"
    sleep 2; press_until "enter+1500" "route: choice" 1 || echo "route not chosen"
    for _ in $(seq 1 150); do [ "$(count "phase .* -> arena_active")" -ge 2 ] && break; sleep 1; done
    sleep 6; cap "c$costume-3-arena2"
    echo "inventory in arena 2: $(grid)"
    dev "archive uIt0304"  # the first aid spray some supply perks give: loaded on every stage of a run
    since | grep -E "itembox: (the run|the screen shows|the run's grid|stash|gold|closed|save data restored)|could not go|game fatal|hang watchdog|stage change: arena|dev archive" | cut -c40-300
    close_game
done
cp "$D/profile.roster-backup.json" "$D/profile.json" && rm -f "$D/profile.roster-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save file before $save_before after $save_after: $([ "$save_before" = "$save_after" ] && echo unchanged || echo CHANGED)"
echo "captures in $OUT"
