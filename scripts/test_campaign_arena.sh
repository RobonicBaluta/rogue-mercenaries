#!/usr/bin/env bash
# Developer check (ROADMAP M5.1 / E6): a run's arena in a campaign area. The host loads stage
# NUMBER in place of the Mercenaries stage, the director spawns its map (scripts/campaign_maps.py:
# AI waypoints, a Mercenaries roster preloaded with the run), chests are placed, the enemies come
# for the player and the arena clears at its budget. Needs the game at the Play menu with a dev
# adapter config (build/restart_to_play_menu.sh configs/adapter.director.json). Saves and restores
# the profile; closes the game at the end.   NUMBER (default 503)  OBSERVE (default 20)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
NUMBER=${NUMBER:-503}
OBSERVE=${OBSERVE:-20}
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-arena$NUMBER"
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.arena-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "stage $NUMBER"; dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
sleep 3; cap 01-spawned
sleep "$OBSERVE"; cap 02-after-$OBSERVE-s
for _ in $(seq 1 60); do since | grep -q -- "-> arena_results" && break; dev killall; sleep 4; done
sleep 2; cap 03-cleared
echo "spawned: $(since | grep -cE 'director: uEm[0-9a-f]+ \(arm') units; failed: $(since | grep -c 'director: .* failed'); not loaded: $(since | grep -c 'is not loaded')"
since | grep -E "host:|resources: run|chests: |mean distance|arena cleared|phase .*arena" | cut -c40-170 | head -20
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.arena-backup.json" "$D/profile.json" && rm -f "$D/profile.arena-backup.json"
echo "done; captures in $OUT"
