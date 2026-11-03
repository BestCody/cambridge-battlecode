#!/bin/bash
# after v114's block (results_v114g.txt) and v112 live again: submit v115 (the general outnumbered rule), fight block, re-pin v112
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
for i in $(seq 1 200); do [ -s analysis/pg10/results_v114g.txt ] && break; sleep 10; done
[ -s analysis/pg10/results_v114g.txt ] || { echo "no v114 results"; exit 1; }
cambc submit bots/baseline -n "v115 = v112 + leave when outnumbered (enemies in 10 at 2x us and allies in 4)" </dev/null 2>&1 | tail -1
waitlive 115 || exit 1
mk v115g analysis/pg10/opponents_fight.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v115g.txt analysis/pg10/results_v115g.txt
