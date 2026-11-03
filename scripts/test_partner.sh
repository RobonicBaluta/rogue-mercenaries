#!/usr/bin/env bash
# Developer check (ROADMAP E22): a computer partner added the game's way. The game is restarted at
# the Play menu with configs/adapter.builds.json (invulnerable, muted, debug log), SKIN is selected
# and START RUN chosen; in arena 1 dev "partner CHARACTER COSTUME" adds the partner beside the
# player, then the player walks forward and back while the probe logs the partner every 5 s
# (distance, action, health, items) and enemy hits name their attacker. Captures in
# build/runs/<time>-partner. Saves and restores the profile, checks the save file's hash; closes
# the game at the end.
#   SKIN=chris_bsaa CHARACTER=0 COSTUME=0 WATCH=40 bash scripts/test_partner.sh
#   SKIN=josh_bsaa TRIGGER=special bash scripts/test_partner.sh   (Josh's Call for Backup: Q)
#   NEXT=1 ...: then clear the arena and go on to arena 2 (the stage change with a partner)
cd "$(dirname "$0")/.."
SKIN=${SKIN:-chris_bsaa}
CHARACTER=${CHARACTER:-0}
COSTUME=${COSTUME:-0}
WATCH=${WATCH:-40}
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-partner"
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
cp "$D/profile.json" "$D/profile.partner-backup.json" || exit 1
close_game
bash build/restart_to_play_menu.sh "${CONFIG:-configs/adapter.builds.json}" >/dev/null
sleep 3
before=1  # the log starts over with each launch; the mod menu may already be open
dev "select $SKIN"
dev rogue
if wait_for "opened on screen 0 by itself" 1 30; then
    sleep 1; keys "enter+2500"
    if wait_for "phase idle -> arena_active" 1 90; then
        wait_for "director: uEm" 1 30
        sleep 2
        cap 01-before
        if [ "${TRIGGER:-dev}" = special ]; then keys "q+500"; else dev "partner $CHARACTER $COSTUME"; fi
        sleep 2
        cap 02-added
        if alive && [ "$(count "game fatal")" -eq 0 ]; then
            # Walk away and back (W, then S), so a follower has to move.
            keys "w:3000+500"; sleep 4; cap 03-walked
            # DIE=1 (with CONFIG=configs/adapter.mortal.json): the player down to 5 HP, so the
            # enemies take him down while the partner lives (does she revive him?).
            [ -n "$DIE" ] && dev "sethp 5"
            # PARTNER_DIES=1: the partner's health to 0 (does her death end a run's match?).
            [ -n "$PARTNER_DIES" ] && dev "partnerhp 0"
            keys "s:3000+500"; sleep "$WATCH"; cap 04-later
            if [ -n "$NEXT" ]; then
                # The arena cleared with the partner alive, then rewards, shop and route, and the next
                # arena's stage loads without her (the match she was added in is over).
                for _ in $(seq 1 40); do [ "$(count "-> arena_results")" -ge 1 ] && break; alive || break; dev killall; sleep 4; done
                press_until "enter+1500" "arena_results -> reward_selection" 1
                press_until "enter+1500" "reward_selection -> shop" 1
                press_until "up+700,enter+1500" "shop -> route_selection" 1
                press_until "up+700,enter+1500" "route: choice" 1
                wait_for "stage change: arena 2 started" 1 90 && sleep 8 && cap 05-arena2 || echo "ARENA 2 DID NOT START"
            fi
        fi
    else
        echo "arena 1 did not start"
    fi
else
    echo "the mod menu did not open"
fi
since | grep -E "dev select|start run \(|partner|dev partner|special:|extras queued|enemy_hit .*player=[0-9] local=0|kill counted|stage change|arena [0-9]:|probe +slot|game fatal|UNHANDLED|exception" |
    sed -E 's/^.{24}//' | cut -c1-240 > "$OUT/partner.txt"
cat "$OUT/partner.txt" | head -60
alive || echo "THE GAME CLOSED"
close_game
cp "$D/profile.partner-backup.json" "$D/profile.json" && rm -f "$D/profile.partner-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; captures in $OUT"
