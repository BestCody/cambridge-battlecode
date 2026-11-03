#!/bin/bash
# fight block (membrane, arena, honeycomb, fragment, guardians against Ding, Tokenmaxxing, Jupiter, Nanyang, SkibidiSloppers):
# reference on v112 (live), then v114 = v112 + the carry rule on the same games, then re-pin v112
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
python scripts/evaluate.py --bot baseline --baseline test-bot --fuels 100000 --maps 02arena --out analysis/carry_chk c:carry_ratio=2.0 2>&1 | grep -q "02arena" || { echo "build failed"; exit 1; }
waitlive 112 || exit 1
mk v112g analysis/pg10/opponents_fight.txt
bash analysis/pg10/results.sh analysis/pg10/groups_v112g.txt analysis/pg10/results_v112g.txt
cambc submit bots/baseline -n "v114 = v112 + leave before a fed carry arrives (2x our power within 10, no ally within 4)" </dev/null 2>&1 | tail -1
waitlive 114 || exit 1
mk v114g analysis/pg10/opponents_fight.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v114g.txt analysis/pg10/results_v114g.txt
