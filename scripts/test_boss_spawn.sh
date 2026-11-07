#!/usr/bin/env bash
# Developer spike (ROADMAP M6 / E11): one boss candidate in a run's arena. The host loads stage
# NUMBER (the boss's home area; -1: the armed Mercenaries stage 0), the director stays quiet, the
# boss's archives load with the stage (dev "preload CLASS EXTRAS"), then one CLASS is spawned
# DISTANCE units in front of the player and watched: captures, its hp and position over 16 s, a
# kill and whether the run counts it. Needs the game at the Play menu with a dev adapter config.
# Saves and restores the profile; closes the game at the end.
#   NUMBER  CLASS  EXTRAS ("uEm40Dummy uEm40Parts")  DISTANCE (default 1200)  WEAPON (default -1)
#   FIRE (shots fired at it 2 s after the spawn; 0: none)  SETHP (its health before the shots)
# Its action and health are logged on every change (dev watch) into watch.txt, through its zero health
# (dev killall) and 15 s after; its action table (dev actions) is in log.txt. ACT=<id>: that action
# of its own is started 1 s after its health reaches zero (dev act).
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:--1}
CLASS=${CLASS:?CLASS=uEmNN}
DISTANCE=${DISTANCE:-1200}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-boss-$CLASS-s$NUMBER"
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
# The profile (and .bak) are put back at the end; with none before, none is left.
mkdir -p "$OUT/profile-before" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
before=$(grep -c "" "$LOG")
[ "$NUMBER" -ge 0 ] && dev "stage $NUMBER"
dev "quiet on"; dev "preload $CLASS ${EXTRAS:-}"; dev "arm 0 0"
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
for _ in $(seq 1 90); do since | grep -q "phase idle -> arena_active" && break; alive || break; sleep 1; done
sleep 4
dev killall; sleep 1; cap 01-arena
# With FIRE the player aims first: aiming turns them to the camera's direction, so the spawn
# (in front of the player) lands in the line of fire.
[ "${FIRE:-0}" -gt 0 ] && $PS scripts/gamectl.ps1 -Action keys -Keys "rdown+1200" >/dev/null
# Its action and health on every change (dev watch), its action table once it has one.
dev "watch $CLASS"
dev "spawn $CLASS ${WEAPON:--1} 1 $DISTANCE"
# PREACT: an action of its own started right after the spawn (Wesker's Shrink 104 holds him still
# in the line of fire for the shots).
[ -n "${PREACT:-}" ] && dev "act $CLASS $PREACT"
(sleep 3; dev "actions $CLASS") &
if [ "${FIRE:-0}" -gt 0 ]; then
    # Does gunfire hurt it: fire FIRE shots at it, then let go of the aim. SETHP first brings it
    # down to that health (does gunfire kill it).
    sleep 2; [ -n "${SETHP:-}" ] && dev "enemyhp $SETHP"; dev enemies
    # BRING: it is moved that far straight ahead of the aiming player first (dev bring).
    [ -n "${BRING:-}" ] && dev "bring $CLASS $BRING"
    shots=$(printf 'lclick+450,%.0s' $(seq 1 "$FIRE"))
    $PS scripts/gamectl.ps1 -Action keys -Keys "${shots}rup+300" >/dev/null
    cap 02-fired; dev enemies
fi
for t in 2 8 16; do
    sleep $([ "$t" -eq 2 ] && echo 2 || echo 6)
    alive || break
    cap "02-after-${t}s"; dev enemies
done
if alive; then
    # At zero health (dev killall: SubHp to 0), and after: does it die, and what does it do instead.
    dev killall
    # ACT: an action of its own started at zero health (dev act, e.g. Wesker's Shrink 104).
    [ -n "${ACT:-}" ] && { sleep 1; dev "act $CLASS $ACT"; }
    for t in 3 8 15; do
        sleep $([ "$t" -eq 3 ] && echo 3 || echo $((t == 8 ? 5 : 7)))
        alive || break
        cap "03-zero-${t}s"
    done
    dev enemies
fi
since > "$OUT/log.txt"
since | grep -E "watch:|dev act" | cut -c25-260 > "$OUT/watch.txt"
since | grep -E "extras queued|host:|dev spawn|dev enemies|[0-9A-F]{8} uEm[0-9a-zA-Z_]+ hp|killall|enemy killed|game fatal|hang watchdog|UNHANDLED|exception" | cut -c40-230 | head -40
alive && $PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do alive || break; sleep 1; done
alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
echo "done; captures in $OUT"
