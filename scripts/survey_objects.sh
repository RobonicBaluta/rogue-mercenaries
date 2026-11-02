#!/usr/bin/env bash
# Developer survey (ROADMAP M4 / E10): which objects and items a Mercenaries stage creates at
# match start, through which manager, with which descriptor; and who constructs the
# time-bonus statues (uOmHourGlass). Needs the game at the Play menu with a dev adapter config
# (build/restart_to_play_menu.sh configs/adapter.director.json). Saves and restores the
# profile; leaves the game running in the match (close it or run another check).
#   STAGE (default 1)  WAIT (seconds after the first director spawn, default 6)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:-1}
WAIT=${WAIT:-6}
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
since() { tail -n +"$before" "$LOG"; }
cp "$D/profile.json" "$D/profile.objects-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "survey on"
dev "arm $STAGE 0"
$PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
for _ in $(seq 1 90); do since | grep -q "director: uEm" && break; sleep 1; done
sleep "$WAIT"
dev "survey off"
dev abandon
cp "$D/profile.objects-backup.json" "$D/profile.json" && rm -f "$D/profile.objects-backup.json"
echo "objects by class (manager):"
since | grep -oE "world survey: [A-Za-z0-9_]+ \([A-Za-z]+\)" | sort | uniq -c | sort -rn
echo "creates by class:"
since | grep -oE "world create: [A-Za-z0-9_]+ \([A-Za-z]+\) group [0-9]+ mode [0-9]+" | sort | uniq -c | sort -rn | head -30
echo "time-bonus statues:"
since | grep -E "HourGlass" | cut -c40-330 | head -20
