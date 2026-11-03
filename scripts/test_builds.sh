#!/usr/bin/env bash
# Developer check (ROADMAP M8): skin builds in game. For every skin in SKINS the game is restarted
# at the Play menu (build/restart_to_play_menu.sh with configs/adapter.builds.json: debug log with
# every hit's damage breakdown, invulnerable), the skin is selected (dev select) and START RUN
# chosen. In arena 1 the inventory is listed (a build's starting weapon), the player aims, an
# enemy of a class the director spawned there is spawned in front and shot three times (the
# hits' build bonus; any other class may not be loaded on the stage), then the arena is
# cleared (dev killall); the rewards and the shop are captured (Clubbin': NEW STOCK is used and
# captured again). Prints per skin the start, the build's health, the inventory's ids, the shots'
# breakdown, the item grants after the clear and the shop rerolls. Saves and restores the
# profile, checks the save file's hash; closes the game at the end.
cd "$(dirname "$0")/.."
SKINS=${SKINS:-"chris_warrior sheva_clubbin sheva_business sheva_fairy_tale wesker_stars barry_stars chris_stars"}
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-builds"
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
cp "$D/profile.json" "$D/profile.builds-backup.json" || exit 1
for skin in $SKINS; do
    echo "== $skin"
    close_game  # the restart's deploy refuses while the game is still exiting
    bash build/restart_to_play_menu.sh configs/adapter.builds.json >/dev/null
    sleep 3
    before=$(grep -c "" "$LOG")
    dev "select $skin"
    # The mod menu opens by itself (dev rogue): no key ever lands on the Play menu's own rows.
    dev rogue
    wait_for "opened on screen 0 by itself" 1 30 || { echo "the mod menu did not open"; close_game; continue; }
    sleep 1; keys "enter+2500"
    wait_for "phase idle -> arena_active" 1 90 || { echo "arena 1 did not start"; close_game; continue; }
    wait_for "director: uEm" 1 30
    sleep 3
    dev dumpinv
    # A class the director spawned here (its archive is loaded on this stage).
    cls=$(since | grep -oE "director: uEm[0-9a-f]+" | head -1 | awk '{print $2}')
    keys "rdown+1200"
    dev "spawn ${cls:-uEm10} -1 1 300"
    sleep 2
    keys "lclick+450,lclick+450,lclick+450,rup+300"
    sleep 1
    if ! alive || [ "$(count "game fatal")" -gt 0 ]; then echo "FATAL ERROR or the game closed: stopping"; close_game; break; fi
    cap "$skin-01-shots"
    for _ in $(seq 1 40); do [ "$(count "-> arena_results")" -ge 1 ] && break; alive || break; dev killall; sleep 4; done
    sleep 3
    press_until "enter+1500" "arena_results -> reward_selection" 1
    sleep 1; cap "$skin-02-rewards"
    press_until "enter+1500" "reward_selection -> shop" 1
    sleep 2; cap "$skin-03-shop"
    if [ "$skin" = sheva_clubbin ]; then
        keys "up+600"; keys "up+600"; keys "up+600"; sleep 1; cap "$skin-04-new-stock-row"
        keys "enter+1500"; sleep 1.5; cap "$skin-05-new-stock"
    fi
    since | grep -E "dev select|start run \(|build: |dev inv slot|dev spawn|outgoing core|reroll shop|created with| stacked,|game fatal|hang watchdog" |
        sed -E 's/^.{24}//' | cut -c1-200 > "$OUT/$skin.txt"
    grep -E "dev select|start run|build: |dev spawn .*(->|refused)|reroll|created with| stacked,|fatal" "$OUT/$skin.txt"
    if [ "$(count "game fatal")" -gt 0 ]; then echo "FATAL ERROR: stopping"; close_game; break; fi
    echo "inventory: $(grep "dev inv slot" "$OUT/$skin.txt" | grep -oE "item [0-9A-F]{4} count -?[0-9]+" | tr '\n' ' ')"
    grep "outgoing core" "$OUT/$skin.txt" | head -3
    close_game
done
cp "$D/profile.builds-backup.json" "$D/profile.json" && rm -f "$D/profile.builds-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; captures in $OUT"
