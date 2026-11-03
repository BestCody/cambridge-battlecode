#!/bin/bash
# submit v111 (v110 + the siege pricing), test it on the monolith block against Tokenmaxxing, Ding and Jupiter, re-pin v110
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_siege.txt; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submit bots/baseline -n "v111 = v110 + siege pricing for single monoliths (measured cost, no trips)" </dev/null 2>&1 | tail -1
waitlive 111 && mk v111s
cambc submissions activate v110 </dev/null 2>&1 | tail -1; echo "repinned v110 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v111s.txt analysis/pg10/results_v111s.txt
