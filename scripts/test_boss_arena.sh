#!/usr/bin/env bash
# Developer check (2026-11-08): a boss map's arena end to end. The game restarts with
# configs/adapter.boss.json (invulnerable, every hit logged); a run's first arena is loaded on
# boss map STAGE (dev stage), so the director spawns its boss with the map's health. The player
# aims; the boss is brought into his line of fire (dev bring, pinned; SIDE units right of his
# facing) for SHOTS shots; then its health goes to 1 (dev enemyhp) and it is shot until a hit kills
# it (KILLSHOTS tries; then dev killall, as a last resort). Captures: the boss on the HUD, its fall
# 1.5 s after its death, the arena's screens. Reports the spawn and health lines, the hits, its
# death (its actions after zero health), the fall and the arena's end. Saves and restores the
# profile; checks the save.
#   PRELOAD (archives loaded with the stage too)  STAGE  CLASS  SIDE (default 60)  DISTANCE (default 400)  SHOTS (default 6)  KILLSHOTS (default 20)
#   FREE (seconds the boss fights the invulnerable player first, a capture every 5 s; default 0)
#   ACTS (its own actions by id, one per free-fight capture)  PARTS (dev enemies and actions)  NOBRING (shots where the
#   player aims: the boss is never moved; Uroboros Mkono stays at its story spot)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:?STAGE=<boss map stage>}
CLASS=${CLASS:?CLASS=uEmNN}
SIDE=${SIDE:-60}
DISTANCE=${DISTANCE:-400}
SHOTS=${SHOTS:-6}
KILLSHOTS=${KILLSHOTS:-20}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-bossarena-$CLASS-s$STAGE"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
quick() { printf '%s' "$1" > "$D/dev.command"; sleep 0.7; }
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
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.boss.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "stage $STAGE"
# PRELOAD: more archives loaded with the stage (dev preload), e.g. the area's own story enemies'.
[ -n "${PRELOAD:-}" ] && dev "preload $PRELOAD"
dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
ok=1
wait_for "phase idle -> arena_active" 1 120 || { echo "the arena did not start"; ok=0; }
if [ $ok = 1 ]; then
    dev "watch $CLASS"
    sleep 2; cap 00-start  # before the boss comes (8 s after the arena begins)
    # WARP "x y z": a boss that waits at its story spot (maps.json boss trigger) must still be waiting
    # past the arena's 8 s; then the player is moved there and it appears.
    if [ -n "${WARP:-}" ]; then
        sleep 10
        echo "before the warp: $(count "director: boss $CLASS .*spawned") boss spawn(s) (0 expected)"
        cap 00-waiting; dev "warp $WARP"
    fi
    if wait_for "director: boss $CLASS .*spawned" 1 40; then
        echo "spawn: $(since | grep -E "director: boss $CLASS .*spawned" | tail -1 | cut -c25-200)"
        wait_for "director: boss health" 1 8 && echo "health: $(since | grep 'director: boss health' | tail -1 | cut -c25-200)"
        sleep 3; cap 01-boss
        # PARTS: every enemy unit (dev enemies) and the boss's action table (dev actions) once it is in.
        [ -n "${PARTS:-}" ] && { dev enemies; dev "actions $CLASS"; }
        if [ "${FREE:-0}" -gt 0 ]; then
            # ACTS: the boss's own actions by id, one started at each capture of the free fight.
            set -- ${ACTS:-}
            for t in $(seq 5 5 "$FREE"); do
                sleep 4; cap "01-free-$t"
                if [ $# -gt 0 ]; then dev "act $CLASS $1"; shift; [ -n "${PARTS:-}" ] && dev enemies; fi
            done
            echo "free fight: $(since | grep "watch: $CLASS hp" | grep -oE 'action [0-9]+ [A-Za-z0-9_]+' | uniq | head -30 | tr '\n' ';' | cut -c1-500)"
            echo "   player: $(since | grep -oE 'c[A-Za-z0-9]+_PL' | sort | uniq -c | tr '\n' ' ')"
            since | grep -E "boss: |camera: |player died" | cut -c25-200 | head -6
        fi
        keys "rdown+1200"
        for _ in $(seq 1 "$SHOTS"); do [ -z "${NOBRING:-}" ] && quick "bring $CLASS $DISTANCE $SIDE 2.0"; keys "lclick+350"; done
        sleep 1; cap 02-hud-after-shots
        echo "hits at full health: $(since | grep -c "enemy_hit .*class=$CLASS")"
        since | grep -E "enemy_hit .*class=$CLASS" | head -3 | cut -c25-200
        dev "enemyhp 1"
        for i in $(seq 1 "$KILLSHOTS"); do
            [ "$(count "enemy_hit .*class=$CLASS.*killed=1|died \(")" -ge 1 ] && break
            [ -z "${NOBRING:-}" ] && quick "bring $CLASS $DISTANCE $SIDE 2.0"; keys "lclick+350"
            [ $((i % 8)) = 0 ] && sleep 2.5  # a reload
        done
        keys "rup+300"
        if [ "$(count "enemy_hit .*class=$CLASS.*killed=1")" -ge 1 ]; then
            echo "killed by a shot: $(since | grep -E "enemy_hit .*class=$CLASS.*killed=1" | head -1 | cut -c25-200)"
        else
            echo "NO KILLING SHOT in $KILLSHOTS tries: dev killall"
            dev killall
        fi
        sleep 0.3; cap 03-fall
        if wait_for "phase arena_active -> arena_results" 1 15; then
            sleep 1; cap 04-screens
        else
            echo "THE ARENA DID NOT END"
        fi
        echo "death: $(since | grep "watch: $CLASS" | grep -A12 "hp 0/" | grep -oE "action [0-9]+ [A-Za-z0-9_]+|gone" | uniq | head -6 | tr '\n' ' ')"
        since | grep -E "died \(|boss fall|phase arena_active -> arena_results" | head -4 | cut -c12-23,25-200
    else
        echo "THE BOSS DID NOT SPAWN"
        cap 01-no-boss
    fi
fi
since > "$OUT/log.txt"
since | grep -E "game fatal|UNHANDLED|exception" | head -3
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
