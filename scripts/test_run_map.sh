#!/usr/bin/env bash
# Developer check (ROADMAP M5.3, M6): a whole run through the run map. START RUN; every arena is
# cleared (dev killall), its results, reward and shop are passed with the menu keys (each step
# repeated until the log shows the phase change), the NEXT ROUTE menu is captured and a route is
# chosen (after arena 1 the last option, then the first). Checks that each stage change loads
# the chosen run map node's arena and, with a boss arena in the pool, that the run ends on the
# boss; prints the run map, the choices and the arenas played.
# Needs the game at the Play menu with a dev adapter config. Saves and restores the profile;
# closes the game at the end.
cd "$(dirname "$0")/.."
D="$LOCALAPPDATA/RogueMercenaries"
LOG="$D/logs/adapter.log"
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
OUT="build/runs/$(date +%Y%m%d-%H%M%S)-runmap"
mkdir -p "$OUT"
cap() { timeout 20 $PS scripts/gamectl.ps1 -Action capture -Out "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png" || echo "NO CAPTURE ($1)"; }
dev() { printf '%s' "$1" > "$D/dev.command"; sleep 1.5; }
keys() { $PS scripts/gamectl.ps1 -Action keys -Keys "$1" >/dev/null; }
since() { tail -n +"$before" "$LOG"; }
count() { since | grep -cE -e "$1"; }
wait_for() { for _ in $(seq 1 "$3"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 1; done; return 1; }
# press KEYS until PATTERN has been logged N times (up to 5 tries)
press_until() { for _ in 1 2 3 4 5; do [ "$(count "$2")" -ge "$3" ] && return 0; keys "$1"; sleep 2; done; [ "$(count "$2")" -ge "$3" ]; }
cp "$D/profile.json" "$D/profile.runmap-backup.json" || exit 1
before=$(grep -c "" "$LOG")
keys "up+900,enter+2500,enter+2500"
wait_for "run map: arena 1:" 1 60
sleep 1
ARENAS=$(count "run map: arena [0-9]+:")
echo "run of $ARENAS arenas"
for arena in $(seq 1 "$ARENAS"); do
    wait_for "phase .* -> arena_active" "$arena" 150 || { echo "arena $arena did not start"; break; }
    wait_for "\] arena [0-9]: " "$arena" 10 && { sleep 1; cap "notice$arena"; }
    wait_for "director: uEm" 1 30
    sleep 5; cap "arena$arena"
    for _ in $(seq 1 60); do [ "$(count "-> arena_results")" -ge "$arena" ] && break; dev killall; sleep 4; done
    sleep 3
    press_until "enter+1500" "arena_results -> reward_selection" "$arena" || { echo "results screen stuck after arena $arena"; break; }
    sleep 1
    press_until "enter+1500" "reward_selection -> (shop|run_complete)" "$arena" || { echo "reward screen stuck after arena $arena"; break; }
    [ "$arena" -eq "$ARENAS" ] && break
    sleep 2
    press_until "up+700,enter+1500" "shop -> route_selection" "$arena" || { echo "shop stuck after arena $arena"; break; }
    sleep 2; cap "route$arena"
    if [ "$arena" -eq 1 ]; then pick="up+700,enter+1500"; else pick="enter+1500"; fi
    press_until "$pick" "route: choice" "$arena" || { echo "route not chosen after arena $arena"; break; }
done
wait_for "phase .*run_complete" 1 20 && echo "run complete" || echo "RUN NOT COMPLETE"
sleep 2; cap run-complete
since | grep -E "run map: arena|route: choice|stage change: stage|stage change: arena|\] arena [0-9]: " | cut -c40-200 > "$OUT/trace.txt"
cat "$OUT/trace.txt"
python - "$OUT/trace.txt" <<'PY'
import re, sys
lines = open(sys.argv[1], encoding='utf-8').read().splitlines()
layers = {}
for l in lines:
    m = re.search(r'run map: arena (\d+): (.*)', l)
    if m:
        layers[int(m.group(1)) - 1] = [int(n.split()[0]) for n in m.group(2).split(' | ')]
ok = True
start = re.search(r'\] arena 1: .*\(stage (-?\d+)\)', '\n'.join(lines))
if not start or layers.get(0, [None])[0] != int(start.group(1)):
    ok = False
    print('FAIL: arena 1 is not on the run map start')
for c in (m for m in (re.search(r'route: choice \d+ of \d+, \S+ on stage (-?\d+) \(layer (\d+) node (\d+)\)', l) for l in lines) if m):
    stage, layer, node = int(c.group(1)), int(c.group(2)), int(c.group(3))
    expected = layers.get(layer, [])[node] if node < len(layers.get(layer, [])) else None
    played = re.search(rf'\] arena {layer + 1}: .*\(stage (-?\d+)\)', '\n'.join(lines))
    good = expected == stage and played is not None and int(played.group(1)) == stage
    ok &= good
    print(f"{'ok' if good else 'FAIL'}: arena {layer + 1} chosen node {node} = stage {expected}, played on stage {played.group(1) if played else '?'}")
boss_last = re.search(rf'\] arena {len(layers)}: .*BOSS', '\n'.join(lines)) is not None
print(f"last arena {'is' if boss_last else 'is not'} a boss arena")
print('RUN MAP CHECK', 'PASS' if ok and len(layers) >= 3 else 'FAIL')
PY
$PS scripts/gamectl.ps1 -Action close >/dev/null
for _ in $(seq 1 30); do tasklist | grep -qi re5dx9 || break; sleep 1; done
tasklist | grep -qi re5dx9 && $PS scripts/gamectl.ps1 -Action kill >/dev/null
$PS scripts/gamectl.ps1 -Action reap >/dev/null
cp "$D/profile.runmap-backup.json" "$D/profile.json" && rm -f "$D/profile.runmap-backup.json"
echo "done; captures in $OUT"
