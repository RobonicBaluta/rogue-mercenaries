#!/usr/bin/env bash
# Developer survey (ROADMAP M5.2): which campaign stages make arenas. For each stage: load it
# through the host (dev "stage"), quiet, capture the start view at 20 s and log where the
# player stands (dev "setdump"). One game launch per stage; DLL=<path> deploys a fixed build
# (so the tree can be rebuilt meanwhile). Saves and restores the profile; closes the game.
#   STAGES (list of stage numbers)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-vet"
mkdir -p "$OUT"
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
cp "$D/profile.json" "$D/profile.vet-backup.json" || exit 1
for stage in $STAGES; do
    DLL=${DLL:-} bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
    before=$(grep -c "" "$LOG")
    dev "stage $stage"; dev "quiet on"; dev "arm 0 0"
    $PS scripts/gamectl.ps1 -Action keys -Keys "up+900,enter+2500,enter+2500" >/dev/null
    for _ in $(seq 1 60); do tail -n +"$before" "$LOG" | grep -q "phase idle -> arena_active" && break; sleep 1; done
    sleep 15
    shot=no
    timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/s$stage.png" >/dev/null 2>&1 && shot=yes
    dev setdump
    player=$(tail -n +"$before" "$LOG" | grep -oE "setdump: player at [-0-9. ]+" | tail -1 | cut -d' ' -f4-)
    active=$(tail -n +"$before" "$LOG" | grep -c "phase idle -> arena_active")
    echo "stage $stage: capture $shot, arena $active, player $player" | tee -a "$OUT/vet.txt"
    cp "$D/profile.vet-backup.json" "$D/profile.json"
done
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.vet-backup.json" "$D/profile.json" && rm -f "$D/profile.vet-backup.json"
echo "done; $OUT"
