#!/usr/bin/env bash
# Developer check (2026-11-08, the user's playtest: "flamethrower once you equip it, it shows the
# model on the character at all times, even when it has some other weapon on"). A plain match on
# Mercenaries stage STAGE; the flamethrower (0121) obtained and equipped, a capture; another gun
# equipped, a capture (its tank should be gone); the flamethrower again, a capture and a burst of
# fire (still shown, still flaming). The log's "flamethrower:" lines say what the mod did.
# Needs the game closed or at the Play menu; restarts it with configs/adapter.director.json
# (developer mode, invulnerable, muted). Saves and restores the profile; closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
STAGE=${STAGE:-2}
GUN=0121
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-flame-hide"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
alive() { tasklist | grep -qi re5dx9; }
# The flamethrower's inventory line from the latest dumpinv: "<count> <equipped>".
gun_state() {
    dev dumpinv
    since | grep "dev inv slot" | grep "item $GUN " | tail -1 | awk '{c=$0; sub(/.*count /,"",c); split(c,a," "); n=split($0,w," "); print a[1], w[n-4]}'
}
close_game() {
    $PS scripts/gamectl.ps1 -Action close >/dev/null
    for _ in $(seq 1 30); do alive || break; sleep 1; done
    alive && $PS scripts/gamectl.ps1 -Action kill >/dev/null
    $PS scripts/gamectl.ps1 -Action reap >/dev/null
}
save_before=$(sha256sum "$SAVE" | cut -c1-16)
BACKUP="build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin"
cp "$SAVE" "$BACKUP" || exit 1
mkdir -p "$OUT/profile-before" || exit 1
for f in profile.json profile.json.bak; do [ -f "$D/$f" ] && { cp "$D/$f" "$OUT/profile-before/$f" || exit 1; }; done
close_game
bash build/restart_to_play_menu.sh configs/adapter.director.json >/dev/null
sleep 3
before=1
dev "quiet on"
dev "preload $GUN"
dev "arm $STAGE 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "match detected" && break; alive || break; sleep 1; done
sleep 5
dev killall; sleep 3; dev killall; sleep 2
dev "obtain $GUN"
sleep 1
key=""
for k in 1 2 3 4 up down left right; do
    keys "$k+1200"
    state=$(gun_state)
    [ "${state#* }" = "00000001" ] && { key=$k; echo "flamethrower equipped with key $k"; break; }
done
[ -n "$key" ] || echo "THE FLAMETHROWER WAS NOT EQUIPPED"
sleep 1.5; cap 01-in-hand
other=""
for k in 1 2 3 4 up down left right; do
    [ "$k" = "$key" ] && continue
    keys "$k+1200"
    state=$(gun_state)
    [ "${state#* }" = "00000000" ] && { other=$k; echo "another gun with key $k"; break; }
done
[ -n "$other" ] || echo "NO OTHER GUN WAS EQUIPPED"
sleep 1.5; cap 02-other-gun
keys "rdown+900"; cap 03-other-gun-aiming; keys "rup+200"
[ -n "$key" ] && keys "$key+1200"
echo "back: count/equipped = $(gun_state)"
sleep 1.5; cap 04-in-hand-again
keys "rdown+900"
keys "lclick:1500+100,rup+200" &
sleep 0.8; cap 05-firing
wait
echo "after firing: count/equipped = $(gun_state)"
since | grep -E "flamethrower:|dev obtain|game fatal|UNHANDLED|exception" | cut -c25-220 | head -30 > "$OUT/notes.txt"
cat "$OUT/notes.txt"
alive || echo "THE GAME CLOSED"
close_game
for f in profile.json profile.json.bak; do
    if [ -f "$OUT/profile-before/$f" ]; then cp "$OUT/profile-before/$f" "$D/$f"; else rm -f "$D/$f"; fi
done
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save before $save_before after $save_after"
if [ "$save_after" != "$save_before" ]; then
    cp "$SAVE" "build/backups/savedata-changed-$(date +%Y%m%d-%H%M%S).bin"
    cp "$BACKUP" "$SAVE"
    echo "SAVE CHANGED; restored: $(sha256sum "$SAVE" | cut -c1-16)"
fi
echo "done; captures in $OUT"
