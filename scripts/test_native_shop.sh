#!/usr/bin/env bash
# Developer check (ROADMAP M7.1): the SHOP's ORGANIZE row opens the game's Organize/Buy screen
# with the run in it. Arena 1 is cleared (dev killall), the screen is opened from the SHOP, the
# run's M92F (Chris, costume slot 0: grid cell 3) gets a reload upgrade, the screen is closed;
# then the next route is taken. Prints the M92F's slot (upgrade levels at +0x20) after the screen
# and in arena 2, and the save file's hash before and after (the campaign save must not change;
# a copy goes to build/backups/ first). Needs the game at the Play menu with a dev adapter config
# (configs/adapter.director.json). Saves and restores the profile; closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
SAVE=$(ls "/c/Program Files (x86)/Steam/userdata/"*/21690/remote/savedata.bin 2>/dev/null | head -1)
[ -f "$SAVE" ] || { echo "savedata.bin not found"; exit 1; }
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-native-shop"
mkdir -p "$OUT" build/backups
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; sleep 1; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
# press KEYS until PATTERN has been logged N times (up to 5 tries)
press_until() { for _ in 1 2 3 4 5; do [ "$(count "$2")" -ge "$3" ] && return 0; keys "$1"; sleep 2; done; [ "$(count "$2")" -ge "$3" ]; }
m92f() { dev dumpinv; since | grep "dev inv slot" | grep "item 0102" | tail -1 | sed -E 's/.*\| ([0-9A-F ]+)$/\1/' | cut -d' ' -f9-10; }
save_before=$(sha256sum "$SAVE" | cut -c1-16)
cp "$SAVE" "build/backups/savedata-$(date +%Y%m%d-%H%M%S).bin" || exit 1
cp "$D/profile.json" "$D/profile.shop-backup.json" || exit 1
before=$(grep -c "" "$LOG")
dev "arm 0 0"
keys "up+900,enter+2500,enter+2500"
for _ in $(seq 1 90); do since | grep -q "phase idle -> arena_active" && break; sleep 1; done
sleep 6
for _ in $(seq 1 60); do [ "$(count "-> arena_results")" -ge 1 ] && break; dev killall; sleep 4; done
sleep 3
press_until "enter+1500" "arena_results -> reward_selection" 1
press_until "enter+1500" "reward_selection -> shop" 1
sleep 2; cap 01-shop
echo "M92F levels before the screen: $(m92f)"
# SHOP: ORGANIZE INVENTORY is two rows up. In the screen: Organizar, the M92F (left of the first
# cell), V (Mejorar), down to the reload row, Enter (prompt), Enter (Mejorar), Enter (the notice).
keys "up+700"; keys "up+700"; keys "enter+1500"; sleep 3; cap 02-screen
keys "enter+1200"; keys "left+700"; keys "v+1200"; keys "down+600"; cap 03-reload
keys "enter+1200"; keys "enter+1500"; cap 04-bought
# Back to the main menu, then "A punto" (Esc moves the cursor there) and Enter.
keys "enter+1200"; keys "esc+1000"; keys "esc+1000"; keys "esc+1000"; keys "esc+1000"
keys "enter+1200"; keys "enter+1200"; sleep 3
for _ in 1 2 3; do since | grep -q "itembox: closed" && break; keys "esc+800"; keys "enter+1200"; done
sleep 2; cap 05-back-in-shop
echo "M92F levels after the screen: $(m92f)"
press_until "up+700,enter+1500" "shop -> route_selection" 1 || echo "shop stuck"
sleep 2; press_until "enter+1500" "route: choice" 1 || echo "route not chosen"
for _ in $(seq 1 150); do [ "$(count "phase .* -> arena_active")" -ge 2 ] && break; sleep 1; done
sleep 6; cap 06-arena2
echo "M92F levels in arena 2: $(m92f)"
since | grep -E "itembox: (the run|gold|closed|save data restored)|upgrades:|native shop|game fatal|hang watchdog|stage change: arena" | cut -c40-260
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.shop-backup.json" "$D/profile.json" && rm -f "$D/profile.shop-backup.json"
save_after=$(sha256sum "$SAVE" | cut -c1-16)
echo "save file before $save_before after $save_after: $([ "$save_before" = "$save_after" ] && echo unchanged || echo CHANGED)"
echo "captures in $OUT"
