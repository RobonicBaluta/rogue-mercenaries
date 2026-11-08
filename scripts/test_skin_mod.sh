#!/usr/bin/env bash
# Developer check (2026-11-08): a third-party skin read from its own folder (the supported-mods
# plan): the game's file stays as it is, the mod's copy is opened in its place (dev redirect, the
# CreateFile hook in file_redirect.cpp). The game restarts with configs/adapter.director.json
# (developer mode, invulnerable, muted); every .arc the game opens is logged (dev openlog); the
# FILES of MODDIR are redirected (none with CONTROL=1: the game's own look, to compare); a plain
# Mercenaries match starts on STAGE with costume COSTUME (dev arm); frames are captured. Nothing in
# the game folder is written. Saves and restores the profile; checks the save. Captures and log.txt
# in build/runs/<time>-skinmod-<costume>.
#   COSTUME (13 Excella, 3 Sheva BSAA, 11 Sheva Fairy Tale)  MODDIR (the mod's nativePC_MT folder)
#   FILES (archive names under Image\Archive, space separated)  STAGE (default 0)  CONTROL (1: no redirect)
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
COSTUME=${COSTUME:?COSTUME=<costume index>}
STAGE=${STAGE:-0}
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-skinmod-$COSTUME${CONTROL:+-control}"
mkdir -p "$OUT/profile-before" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
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
G="/g/SteamLibrary/steamapps/common/Resident Evil 5/nativePC_MT/Image/Archive"
game_hashes() { for f in $FILES; do echo "$f $(sha256sum < "$G/$f" | cut -c1-16)"; done; }
save_before=$(sha256sum < "$SAVE" | cut -c1-16)
hashes_before=$(game_hashes)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null 2>&1
sleep 3
before=$(grep -c "" "$LOG"); [ "$before" -gt 0 ] || before=1
dev "openlog on"
if [ -z "${CONTROL:-}" ]; then
    for f in $FILES; do
        dev "redirect Image\\Archive\\$f $(cygpath -wa "$MODDIR")\\Image\\Archive\\$f"
    done
    since | grep -E "dev redirect" | cut -c40-260
fi
dev "quiet on"
dev "arm $STAGE $COSTUME"
keys "up+900,enter+2500,enter+2500"
if wait_for "match detected" 1 120; then
    sleep 6
    dev killall
    sleep 2; cap 01-match
    keys "rdown+1500"; cap 02-aiming; keys "rup+300"
    keys "s:1200+300"; sleep 1; cap 03-walked
else
    echo "the match did not start"
fi
echo "opens of these files: $(for f in $FILES; do echo -n "$f $(since | grep -c "files: the game opens .*\\\\$f") "; done)"
echo "redirected: $(since | grep -c 'files: .* read from ')"
since | grep -E "files: .* read from " | head -6 | cut -c25-260
since > "$OUT/log.txt"
grep -E "game fatal|UNHANDLED|exception" "$OUT/log.txt" | head -3 | cut -c12-23,25-200
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
[ "$(game_hashes)" = "$hashes_before" ] && echo "game files unchanged" || echo "GAME FILES CHANGED"
save_after=$(sha256sum < "$SAVE" | cut -c1-16)
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum < "$SAVE" | cut -c1-16)"
fi
echo "done; $OUT"
