#!/usr/bin/env bash
# Developer check (ROADMAP M5.2): a campaign arena (NUMBER, default 113) where the enemies are
# left alone until they grab the player. With developer_mode the hang watchdog logs where the
# game's main thread is when frames stop. Needs the game at the Play menu with a dev adapter
# config. Saves and restores the profile; closes or kills the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:-113}
WATCH=${WATCH:-90}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-grab$NUMBER"
mkdir -p "$OUT"
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.grab-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "stage $NUMBER"; dev "arm 0 0"
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
for t in $(seq 1 "$WATCH"); do
    sleep 1
    [ "$(since | grep -c 'hang watchdog: no frame')" -ge 4 ] && break
    [ $((t % 20)) = 0 ] && timeout 15 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/t$t.png" >/dev/null 2>&1
done
since | grep -E "hang watchdog|mean distance|UNHANDLED|first-chance" | cut -c40-400 | tail -14
if ! timeout 15 $PS scripts/gamectl.ps1 -Action close >/dev/null 2>&1; then :; fi
for _ in $(seq 1 15); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.grab-backup.json" "$D/profile.json" && rm -f "$D/profile.grab-backup.json"
echo "done; $OUT"
