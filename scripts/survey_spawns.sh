#!/usr/bin/env bash
# Developer survey (roadmap M1.2/M1.3): loads each Mercenaries stage and logs every native
# spawn (class, weapon id, set record, position). Needs configs/adapter.survey.json and the game
# at the Play menu (build/restart_to_play_menu.sh). Parse with scripts/survey_spawns.py.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
KEYS="powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gamectl.ps1 -Action keys -Keys"
SECONDS_PER_STAGE=${SECONDS_PER_STAGE:-64}
KILL_EVERY=${KILL_EVERY:-4}
STAGES=${STAGES:-"0 1 2 3 4 5 6 7"}
# START RUN is the only way into a match from the mod's menu, so the survey starts a run and
# abandons it; the profile is saved now and put back after the game is closed at the end.
cp "$D/profile.json" "$D/profile.survey-backup.json" || exit 1
first=1
for stage in $STAGES; do
    before=$(grep -c "" "$LOG")
    # On before the stage loads: the first wave spawns with the match.
    printf 'survey on' > "$D/dev.command"; sleep 1.5
    printf 'arm %s 0' "$stage" > "$D/dev.command"; sleep 1.5
    if [ $first = 1 ]; then
        $KEYS "up+900,enter+2500,enter+2500" >/dev/null
        first=0
    elif grep -E "match detected|match ended" "$LOG" | tail -1 | grep -q "match detected"; then
        # Only a live match is ended: on the results screen the armed autopilot already
        # starts the next stage, and a time-up would end that new match at once.
        printf 'timeup' > "$D/dev.command"
    fi
    for i in $(seq 1 60); do
        tail -n +"$before" "$LOG" | grep -q "mercenaries match object" && break
        sleep 1
    done
    # The run START RUN began would hold the countdown and clear arenas: drop it at once, the
    # match goes on as plain Mercenaries.
    printf 'abandon' > "$D/dev.command"; sleep 1.5
    echo "stage $stage: surveying ${SECONDS_PER_STAGE}s"
    # Mercenaries only spawns when enemies die: kill everything every few seconds.
    for i in $(seq 1 $((SECONDS_PER_STAGE / KILL_EVERY))); do
        sleep "$KILL_EVERY"
        printf 'killall' > "$D/dev.command"
    done
    sleep 2
done
printf 'survey off' > "$D/dev.command"; sleep 1.5
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gamectl.ps1 -Action close >/dev/null
for i in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.survey-backup.json" "$D/profile.json" && rm -f "$D/profile.survey-backup.json"
echo "done (game closed, profile restored)"
