#!/usr/bin/env bash
# Developer check (2026-11-08): the rarity remake's elements and legendary effects in game. The
# game restarts with configs/adapter.boss.json (invulnerable, hits logged); a run's first arena
# starts on STAGE (default 503, the Freighter Deck: flat, Majini). Majini grabs are broken at once
# (dev grabs break), so the shots go out. For each case the gun in hand becomes that gun (dev gun
# held <rarity> <elements> <effects>), the case's set-up runs (dev enemyhp, dev mag), a director
# Majini is brought into the line of fire (dev bring, pinned) for its shots and watched (dev
# watch: its actions show the reactions); the elements' log lines are counted, frames captured.
# Then dev refine, and with REWARDS=1 the arena is cleared (dev killall) to its reward and shop
# screens (captured; their offers logged). Saves and restores the profile; checks the save.
# Captures and log.txt in build/runs/<time>-elements.
#   STAGE (default 503)  CLASS (default uEm10)  CASES (default: all, by name)  REWARDS (default 1)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:-503}
CLASS=${CLASS:-uEm10}
CASES=${CASES:-"burn toxic chain execute lastword overkill freeze shock blast exotic"}
REWARDS=${REWARDS:-1}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-elements"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.6; }
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
# Each case: the gun, its set-up (dev commands, ; separated), its shots.
gun_for() {
    case "$1" in
        burn) echo "epic incendiary -" ;;
        toxic) echo "epic toxic -" ;;
        chain) echo "legendary - chain_lightning" ;;
        execute) echo "legendary - executioner_rounds" ;;
        lastword) echo "legendary - last_word" ;;
        overkill) echo "legendary - overkill,bloodthirst" ;;
        freeze) echo "epic cryo -" ;;
        shock) echo "epic shock -" ;;
        blast) echo "legendary explosive bottomless" ;;
        exotic) echo "exotic shock,incendiary chain_lightning,dragons_breath" ;;
    esac
}
setup_for() {
    case "$1" in
        execute) echo "outgoing 0.1;enemyhp 150" ;;
        lastword) echo "mag 1" ;;
        overkill) echo "enemyhp 40" ;;
        blast) echo "enemyhp 100" ;;
    esac
}
shots_for() {
    case "$1" in
        chain) echo 12 ;;
        lastword) echo 4 ;;
        execute|overkill|blast) echo 3 ;;
        *) echo 6 ;;
    esac
}
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.boss.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "stage $STAGE"
dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
if wait_for "phase idle -> arena_active" 1 120; then
    sleep 8
    dev "grabs break"
    dev "watch $CLASS"
    for c in $CASES; do
        alive || break
        mark=$(since | grep -c "")
        dev "gun held $(gun_for "$c")"
        echo "== $c: $(since | grep -E 'dev gun ' | tail -1 | cut -c40-200)"
        IFS=';' read -ra setup <<< "$(setup_for "$c")"
        for cmd in "${setup[@]}"; do [ -n "$cmd" ] && dev "$cmd"; done
        keys "rdown+1200"
        n=$(shots_for "$c")
        for i in $(seq 1 "$n"); do
            [ "$c" = lastword ] && quick "mag 1"  # every shot the magazine's last
            quick "bring $CLASS 400 60 1.0"
            keys "lclick+350"
            [ $((i % 8)) = 0 ] && sleep 2.5
        done
        keys "rup+300"
        sleep 0.5; cap "$c-a"
        sleep 1.5; cap "$c-b"
        sleep 2.5
        [ "$c" = execute ] && quick "outgoing 1"
        part=$(since | tail -n +"$((mark + 1))")
        echo "   hits: $(echo "$part" | grep -c 'hit: '), procs: $(echo "$part" | grep -oE 'elements: [a-z_]+ \([A-Za-z_ ]+\)' | sort | uniq -c | tr '\n' ';' | tr -s ' ')"
        echo "   own reactions: $(echo "$part" | grep -c 'its own reaction'), kills: $(echo "$part" | grep -cE 'killed=1|died \('), by elements: $(echo "$part" | grep -c 'by the special')"
        echo "   watched: $(echo "$part" | grep "watch: $CLASS hp" | grep -oE 'action -?[0-9]+ [A-Za-z0-9_]+' | awk '{print $3}' | uniq | tr '\n' ' ' | cut -c1-300)"
        echo "$part" | grep -E "elements: |dev (mag|enemyhp)" | head -3 | cut -c25-230
    done
    dev "refine"
    echo "refine: $(since | grep -E 'dev refine' | tail -1 | cut -c40-200)"
    # A Rare gun in hand from here: the refine dev command, then the rewards' refine (an arena rated
    # A or S always offers one) and maybe the shop's.
    dev "gun held rare - -"
    dev "refine"
    echo "refine: $(since | grep -E 'dev refine' | tail -1 | cut -c40-200)"
    if [ "$REWARDS" = 1 ]; then
        dev "grabs game"
        for _ in $(seq 1 40); do [ "$(count 'phase arena_active -> arena_results')" -ge 1 ] && break; dev killall; sleep 3; done
        sleep 2; cap results
        echo "results: $(since | grep -oE 'arena [0-9]+ cleared in .*' | tail -1)"
        for _ in $(seq 1 6); do [ "$(count 'arena_results -> reward_selection')" -ge 1 ] && break; keys "enter+1500"; sleep 1.5; done
        sleep 2; cap rewards
        echo "$(since | grep -E 'rewards offers:' | tail -1 | cut -c25-400)"
        # The refine row (the last offer when there is one): its help, then taken.
        if since | grep -E 'rewards offers:' | tail -1 | grep -q '| refine '; then
            keys "down+600"; keys "down+600"; sleep 1; cap rewards-refine
            keys "enter+2000"
            echo "$(since | grep -E 'reward chosen:' | tail -1 | cut -c25-400)"
        else
            keys "enter+2000"
        fi
        sleep 2; cap shop
        echo "$(since | grep -E 'shop offers:' | tail -1 | cut -c25-400)"
    fi
else
    echo "the arena did not start"
fi
since > "$OUT/log.txt"
grep -E "game fatal|UNHANDLED|exception" "$OUT/log.txt" | head -3 | cut -c12-23,25-200
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
save_after=$(sha256sum < "$SAVE" | cut -c1-16)
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum < "$SAVE" | cut -c1-16)"
fi
echo "done; $OUT"
